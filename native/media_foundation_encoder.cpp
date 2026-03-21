/**
 * Windows Media Foundation video encoder — hardware H.264 via MFSinkWriter.
 *
 * BGRA input (MFVideoFormat_RGB32) → MF handles color conversion internally.
 * Same C API as AVAssetWriterEncoder: videoEncoderInit/AddFrame/Finish/Dispose/GetError.
 *
 * GPU path: two runtime strategies —
 *   1. D3D11 interop (WGL_NV_DX_interop2) — zero-copy GL→D3D11→MF (NVIDIA, some AMD)
 *   2. Fallback — glBlitFramebuffer → glReadPixels → existing CPU submission
 */

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

// GL types and constants — resolved dynamically via wglGetProcAddress
#include <GL/gl.h>

// ---------------------------------------------------------------------------
// GL extension constants (not in gl.h)
// ---------------------------------------------------------------------------

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif

// GL types missing from Windows gl.h (OpenGL 1.1 only)
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef struct __GLsync* GLsync;
typedef unsigned __int64 GLuint64;

// PBO constants
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif

// WGL_NV_DX_interop2 constants
#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

// ---------------------------------------------------------------------------
// GL/WGL function pointer typedefs
// ---------------------------------------------------------------------------

typedef void(APIENTRY* PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void(APIENTRY* PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void(APIENTRY* PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef GLsync(APIENTRY* PFNGLFENCESYNCPROC)(GLenum, GLbitfield);
typedef GLenum(APIENTRY* PFNGLCLIENTWAITSYNCPROC)(GLsync, GLbitfield, GLuint64);
typedef void(APIENTRY* PFNGLDELETESYNCPROC)(GLsync);
typedef void(APIENTRY* PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLBUFFERDATAPROC)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void*(APIENTRY* PFNGLMAPBUFFERPROC)(GLenum, GLenum);
typedef GLboolean(APIENTRY* PFNGLUNMAPBUFFERPROC)(GLenum);

// WGL_NV_DX_interop2 function typedefs
typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENVPROC)(void*);
typedef BOOL(WINAPI* PFNWGLDXCLOSEDEVICENVPROC)(HANDLE);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE, GLint, HANDLE*);
typedef BOOL(WINAPI* PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE, GLint, HANDLE*);
typedef const char*(WINAPI* PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC);

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;					 // BGRA
static const LONGLONG MF_TICKS_PER_SECOND = 10000000LL;	 // Media Foundation 100-nanosecond units

// Media Foundation state (shared between CPU and GPU paths)
static IMFSinkWriter* writer_ = NULL;
static IMFMediaBuffer* buffer_ = NULL;
static IMFSample* sample_ = NULL;
static DWORD stream_index_ = 0;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;
static int buffer_size_ = 0;
static BOOL com_initialized_ = FALSE;
static BOOL mf_started_ = FALSE;
static char error_buf_[ERROR_BUF_SIZE] = {0};

// GPU path state
static bool gpu_mode_ = false;
static bool interop_available_ = false;

// D3D11 interop state
static ID3D11Device* d3d_device_ = NULL;
static ID3D11DeviceContext* d3d_context_ = NULL;
static ID3D11Texture2D* d3d_texture_ = NULL;
static HANDLE interop_device_ = NULL;
static HANDLE interop_object_ = NULL;
static GLuint interop_tex_ = 0;
static GLuint interop_fbo_ = 0;
static IMFDXGIDeviceManager* dxgi_manager_ = NULL;

// D3D11 texture pool for B-frame safety (encoder holds refs to previous frames)
static const int POOL_SIZE = 3;
static ID3D11Texture2D* pool_textures_[POOL_SIZE] = {NULL, NULL, NULL};
static int pool_index_ = 0;
static UINT dxgi_reset_token_ = 0;
static GLsync blit_fence_ = NULL;

// Fallback GPU state
static GLuint fallback_fbo_ = 0;
static GLuint fallback_rbo_ = 0;
static unsigned char* readback_buf_ = NULL;

// PBO double-buffer for async readback (fallback path)
static const int PBO_COUNT = 2;
static GLuint pbo_ids_[PBO_COUNT] = {0, 0};
static int pbo_index_ = 0;
static bool pbo_pending_ = false;
static bool pbo_active_ = false;

// Resolved GL function pointers
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_ = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers_ = NULL;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_ = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_ = NULL;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_ = NULL;
static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer_ = NULL;
static PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers_ = NULL;
static PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers_ = NULL;
static PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_ = NULL;
static PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage_ = NULL;
static PFNGLFENCESYNCPROC glFenceSync_ = NULL;
static PFNGLCLIENTWAITSYNCPROC glClientWaitSync_ = NULL;
static PFNGLDELETESYNCPROC glDeleteSync_ = NULL;
static PFNGLGENBUFFERSPROC glGenBuffers_ = NULL;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers_ = NULL;
static PFNGLBINDBUFFERPROC glBindBuffer_ = NULL;
static PFNGLBUFFERDATAPROC glBufferData_ = NULL;
static PFNGLMAPBUFFERPROC glMapBuffer_ = NULL;
static PFNGLUNMAPBUFFERPROC glUnmapBuffer_ = NULL;
static bool gl_functions_resolved_ = false;

// Resolved WGL_NV_DX_interop2 function pointers
static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV_ = NULL;
static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV_ = NULL;
static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV_ = NULL;
static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV_ = NULL;
static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV_ = NULL;
static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV_ = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(error_buf_, ERROR_BUF_SIZE, fmt, args);
	va_end(args);
}

static void setErrorHR(const char* context, HRESULT hr) {
	snprintf(error_buf_, ERROR_BUF_SIZE, "%s failed: HRESULT 0x%08lX", context, (unsigned long)hr);
}

static void clearError(void) {
	error_buf_[0] = '\0';
}

template <class T>
static void safeRelease(T** ppT) {
	if (*ppT) {
		(*ppT)->Release();
		*ppT = NULL;
	}
}

// ---------------------------------------------------------------------------
// GL function resolution
// ---------------------------------------------------------------------------

static bool resolveGlFunctions(void) {
	if (gl_functions_resolved_) return true;

	// Verify we have a native WGL context (not ANGLE/EGL)
	HGLRC ctx = wglGetCurrentContext();
	if (!ctx) return false;

#define RESOLVE_GL(name, type)                \
	name##_ = (type)wglGetProcAddress(#name); \
	if (!name##_) return false

	RESOLVE_GL(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
	RESOLVE_GL(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);
	RESOLVE_GL(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
	RESOLVE_GL(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
	RESOLVE_GL(glFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC);
	RESOLVE_GL(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC);
	RESOLVE_GL(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
	RESOLVE_GL(glGenRenderbuffers, PFNGLGENRENDERBUFFERSPROC);
	RESOLVE_GL(glDeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC);
	RESOLVE_GL(glBindRenderbuffer, PFNGLBINDRENDERBUFFERPROC);
	RESOLVE_GL(glRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC);
	RESOLVE_GL(glFenceSync, PFNGLFENCESYNCPROC);
	RESOLVE_GL(glClientWaitSync, PFNGLCLIENTWAITSYNCPROC);
	RESOLVE_GL(glDeleteSync, PFNGLDELETESYNCPROC);
	RESOLVE_GL(glGenBuffers, PFNGLGENBUFFERSPROC);
	RESOLVE_GL(glDeleteBuffers, PFNGLDELETEBUFFERSPROC);
	RESOLVE_GL(glBindBuffer, PFNGLBINDBUFFERPROC);
	RESOLVE_GL(glBufferData, PFNGLBUFFERDATAPROC);
	RESOLVE_GL(glMapBuffer, PFNGLMAPBUFFERPROC);
	RESOLVE_GL(glUnmapBuffer, PFNGLUNMAPBUFFERPROC);

#undef RESOLVE_GL

	gl_functions_resolved_ = true;
	return true;
}

// ---------------------------------------------------------------------------
// WGL_NV_DX_interop2 detection and resolution
// ---------------------------------------------------------------------------

static bool checkInteropSupport(void) {
	auto wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
	if (!wglGetExtensionsStringARB) return false;

	HDC hdc = wglGetCurrentDC();
	if (!hdc) return false;

	const char* extensions = wglGetExtensionsStringARB(hdc);
	if (!extensions || !strstr(extensions, "WGL_NV_DX_interop2")) return false;

#define RESOLVE_WGL(name, type)               \
	name##_ = (type)wglGetProcAddress(#name); \
	if (!name##_) return false

	RESOLVE_WGL(wglDXOpenDeviceNV, PFNWGLDXOPENDEVICENVPROC);
	RESOLVE_WGL(wglDXCloseDeviceNV, PFNWGLDXCLOSEDEVICENVPROC);
	RESOLVE_WGL(wglDXRegisterObjectNV, PFNWGLDXREGISTEROBJECTNVPROC);
	RESOLVE_WGL(wglDXUnregisterObjectNV, PFNWGLDXUNREGISTEROBJECTNVPROC);
	RESOLVE_WGL(wglDXLockObjectsNV, PFNWGLDXLOCKOBJECTSNVPROC);
	RESOLVE_WGL(wglDXUnlockObjectsNV, PFNWGLDXUNLOCKOBJECTSNVPROC);

#undef RESOLVE_WGL

	return true;
}

// ---------------------------------------------------------------------------
// D3D11 interop initialization
// ---------------------------------------------------------------------------

static bool initD3D11Interop(int width, int height) {
	// Create D3D11 device (multi-threaded by default — MF may use background threads)
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d3d_device_, NULL, &d3d_context_);
	if (FAILED(hr)) return false;

	// Create shared texture — BGRA format matches GL and MF expectations
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = (UINT)width;
	desc.Height = (UINT)height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	hr = d3d_device_->CreateTexture2D(&desc, NULL, &d3d_texture_);
	if (FAILED(hr)) return false;

	// Register D3D11 device with WGL interop
	interop_device_ = wglDXOpenDeviceNV_(d3d_device_);
	if (!interop_device_) return false;

	// Create GL texture and FBO for the interop
	glGenTextures(1, &interop_tex_);
	glGenFramebuffers_(1, &interop_fbo_);

	// Register D3D11 texture as GL texture
	interop_object_ = wglDXRegisterObjectNV_(interop_device_, d3d_texture_, interop_tex_, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
	if (!interop_object_) return false;

	// Verify FBO completeness with interop texture attached
	wglDXLockObjectsNV_(interop_device_, 1, &interop_object_);
	glBindFramebuffer_(GL_FRAMEBUFFER, interop_fbo_);
	glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, interop_tex_, 0);
	GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
	glBindFramebuffer_(GL_FRAMEBUFFER, 0);
	wglDXUnlockObjectsNV_(interop_device_, 1, &interop_object_);

	if (status != GL_FRAMEBUFFER_COMPLETE) return false;

	// Create DXGI device manager for D3D11-aware SinkWriter
	hr = MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_);
	if (FAILED(hr)) return false;

	hr = dxgi_manager_->ResetDevice(d3d_device_, dxgi_reset_token_);
	if (FAILED(hr)) return false;

	// Create texture pool for B-frame safety.
	// H.264 encoder holds refs to previous textures during B-frame reordering.
	// CopyResource from interop texture to a fresh pool texture each frame,
	// same approach as macOS Metal copy to fresh pooled CVPixelBuffer.
	D3D11_TEXTURE2D_DESC poolDesc = desc;
	poolDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // encoder only reads
	for (int i = 0; i < POOL_SIZE; i++) {
		hr = d3d_device_->CreateTexture2D(&poolDesc, NULL, &pool_textures_[i]);
		if (FAILED(hr)) return false;
	}
	pool_index_ = 0;

	return true;
}

// ---------------------------------------------------------------------------
// Fallback GPU initialization (internal FBO + readback buffer)
// ---------------------------------------------------------------------------

static bool initFallbackGpu(int width, int height) {
	glGenFramebuffers_(1, &fallback_fbo_);
	glGenRenderbuffers_(1, &fallback_rbo_);

	glBindRenderbuffer_(GL_RENDERBUFFER, fallback_rbo_);
	glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, width, height);

	glBindFramebuffer_(GL_FRAMEBUFFER, fallback_fbo_);
	glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fallback_rbo_);

	GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
	glBindFramebuffer_(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer_(GL_RENDERBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) return false;

	// Allocate CPU readback buffer (used by both sync and PBO paths)
	int bufSize = width * height * BYTES_PER_PIXEL;
	readback_buf_ = new unsigned char[bufSize];

	// Create double-buffered PBOs for async readback
	glGenBuffers_(PBO_COUNT, pbo_ids_);
	for (int i = 0; i < PBO_COUNT; i++) {
		glBindBuffer_(GL_PIXEL_PACK_BUFFER, pbo_ids_[i]);
		glBufferData_(GL_PIXEL_PACK_BUFFER, bufSize, NULL, GL_STREAM_READ);
	}
	glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
	pbo_active_ = pbo_ids_[0] != 0 && pbo_ids_[1] != 0;
	pbo_index_ = 0;
	pbo_pending_ = false;

	return true;
}

// ---------------------------------------------------------------------------
// GPU resource cleanup
// ---------------------------------------------------------------------------

static void releaseGpuResources(void) {
	// Fence cleanup
	if (blit_fence_ && glDeleteSync_) {
		glDeleteSync_(blit_fence_);
		blit_fence_ = NULL;
	}

	// D3D11 interop cleanup
	if (interop_object_ && wglDXUnregisterObjectNV_) {
		wglDXUnregisterObjectNV_(interop_device_, interop_object_);
		interop_object_ = NULL;
	}
	if (interop_device_ && wglDXCloseDeviceNV_) {
		wglDXCloseDeviceNV_(interop_device_);
		interop_device_ = NULL;
	}
	if (interop_fbo_) {
		glDeleteFramebuffers_(1, &interop_fbo_);
		interop_fbo_ = 0;
	}
	if (interop_tex_) {
		glDeleteTextures(1, &interop_tex_);
		interop_tex_ = 0;
	}
	for (int i = 0; i < POOL_SIZE; i++) {
		if (pool_textures_[i]) {
			pool_textures_[i]->Release();
			pool_textures_[i] = NULL;
		}
	}
	pool_index_ = 0;
	if (d3d_texture_) {
		d3d_texture_->Release();
		d3d_texture_ = NULL;
	}
	if (d3d_context_) {
		d3d_context_->Release();
		d3d_context_ = NULL;
	}
	if (d3d_device_) {
		d3d_device_->Release();
		d3d_device_ = NULL;
	}
	safeRelease(&dxgi_manager_);
	dxgi_reset_token_ = 0;

	// Fallback cleanup
	if (pbo_active_ && glDeleteBuffers_) {
		glDeleteBuffers_(PBO_COUNT, pbo_ids_);
		pbo_ids_[0] = 0;
		pbo_ids_[1] = 0;
		pbo_active_ = false;
		pbo_pending_ = false;
	}
	if (fallback_fbo_ && glDeleteFramebuffers_) {
		glDeleteFramebuffers_(1, &fallback_fbo_);
		fallback_fbo_ = 0;
	}
	if (fallback_rbo_ && glDeleteRenderbuffers_) {
		glDeleteRenderbuffers_(1, &fallback_rbo_);
		fallback_rbo_ = 0;
	}
	delete[] readback_buf_;
	readback_buf_ = NULL;

	gpu_mode_ = false;
	interop_available_ = false;
	gl_functions_resolved_ = false;
}

// ---------------------------------------------------------------------------
// Create an IMFMediaType for the H.264 output stream
// ---------------------------------------------------------------------------

static HRESULT createOutputType(int width, int height, int fps, int bitrate, int keyframeInterval, IMFMediaType** ppType) {
	IMFMediaType* pType = NULL;
	HRESULT hr = MFCreateMediaType(&pType);
	if (FAILED(hr)) return hr;

	hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	// MF_MT_AVG_BITRATE = VBR (average, not constant). MF SinkWriter uses VBR by default.
	hr = pType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, (UINT32)fps, 1);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	// High profile — matches AVVideoProfileLevelH264HighAutoLevel on macOS.
	// Better compression: CABAC, 8x8 transform, custom quant matrices.
	hr = pType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	// Keyframe interval in frames (seconds * fps)
	hr = pType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, (UINT32)(keyframeInterval * fps));
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	*ppType = pType;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Create an IMFMediaType for the BGRA input stream
// ---------------------------------------------------------------------------

static HRESULT createInputType(int width, int height, int fps, bool topDown, IMFMediaType** ppType) {
	IMFMediaType* pType = NULL;
	HRESULT hr = MFCreateMediaType(&pType);
	if (FAILED(hr)) return hr;

	hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	// MFVideoFormat_RGB32 is BGRA in memory on little-endian Windows
	hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, (UINT32)fps, 1);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	// Stride: negative = bottom-up (OpenGL/OpenFL convention), positive = top-down
	int stride = width * BYTES_PER_PIXEL;
	if (!topDown) stride = -stride;
	hr = pType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)stride);
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	*ppType = pType;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Create SinkWriter with optional D3D11 device manager
// ---------------------------------------------------------------------------

static HRESULT createSinkWriter(const char* outputPath, bool useD3D11, IMFSinkWriter** ppWriter) {
	// Convert path to wide string
	int wideLen = MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, NULL, 0);
	if (wideLen <= 0) return E_FAIL;

	wchar_t* widePath = new wchar_t[wideLen];
	MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, widePath, wideLen);

	IMFAttributes* pAttributes = NULL;
	HRESULT hr = MFCreateAttributes(&pAttributes, 2);
	if (SUCCEEDED(hr)) {
		pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		if (useD3D11 && dxgi_manager_) pAttributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_manager_);
	}

	hr = MFCreateSinkWriterFromURL(widePath, NULL, pAttributes, ppWriter);
	safeRelease(&pAttributes);
	delete[] widePath;

	return hr;
}

// ---------------------------------------------------------------------------
// Write pre-allocated sample with BGRA pixel data
// ---------------------------------------------------------------------------

static HRESULT writeSampleFromBGRA(const unsigned char* bgraPixels, LONGLONG timestamp, LONGLONG duration) {
	BYTE* pData = NULL;
	HRESULT hr = buffer_->Lock(&pData, NULL, NULL);
	if (FAILED(hr)) return hr;

	memcpy(pData, bgraPixels, buffer_size_);

	hr = buffer_->Unlock();
	if (FAILED(hr)) return hr;

	hr = buffer_->SetCurrentLength(buffer_size_);
	if (FAILED(hr)) return hr;

	hr = sample_->SetSampleTime(timestamp);
	if (FAILED(hr)) return hr;

	hr = sample_->SetSampleDuration(duration);
	if (FAILED(hr)) return hr;

	return writer_->WriteSample(stream_index_, sample_);
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

static void releaseResources(void) {
	safeRelease(&sample_);
	safeRelease(&buffer_);
	safeRelease(&writer_);
	width_ = 0;
	height_ = 0;
	fps_ = 0;
	frame_index_ = 0;
	stream_index_ = 0;
	buffer_size_ = 0;

	if (mf_started_) {
		MFShutdown();
		mf_started_ = FALSE;
	}
	if (com_initialized_) {
		CoUninitialize();
		com_initialized_ = FALSE;
	}
}

int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate, int keyframeInterval) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || keyframeInterval <= 0) {
		setError("Invalid encoder parameters");
		return -1;
	}

	// Initialize COM
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hr) || hr == S_FALSE) {
		com_initialized_ = TRUE;
	} else if (hr == RPC_E_CHANGED_MODE) {
		// COM already initialized with different threading model — that's OK
		com_initialized_ = FALSE;
	} else {
		setErrorHR("CoInitializeEx", hr);
		return -1;
	}

	// Start Media Foundation
	hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) {
		setErrorHR("MFStartup", hr);
		releaseResources();
		return -1;
	}
	mf_started_ = TRUE;

	// Delete existing file
	DeleteFileA(outputPath);

	// Create SinkWriter (no D3D11 for CPU path)
	hr = createSinkWriter(outputPath, false, &writer_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateSinkWriterFromURL", hr);
		releaseResources();
		return -1;
	}

	// Add H.264 output stream
	IMFMediaType* pOutputType = NULL;
	hr = createOutputType(width, height, fps, bitrate, keyframeInterval, &pOutputType);
	if (FAILED(hr)) {
		setErrorHR("Create output media type", hr);
		releaseResources();
		return -1;
	}

	hr = writer_->AddStream(pOutputType, &stream_index_);
	safeRelease(&pOutputType);
	if (FAILED(hr)) {
		setErrorHR("AddStream", hr);
		releaseResources();
		return -1;
	}

	// Set BGRA input type (bottom-up for CPU path)
	IMFMediaType* pInputType = NULL;
	hr = createInputType(width, height, fps, false, &pInputType);
	if (FAILED(hr)) {
		setErrorHR("Create input media type", hr);
		releaseResources();
		return -1;
	}

	hr = writer_->SetInputMediaType(stream_index_, pInputType, NULL);
	safeRelease(&pInputType);
	if (FAILED(hr)) {
		setErrorHR("SetInputMediaType", hr);
		releaseResources();
		return -1;
	}

	// Start writing
	hr = writer_->BeginWriting();
	if (FAILED(hr)) {
		setErrorHR("BeginWriting", hr);
		releaseResources();
		return -1;
	}

	// Pre-allocate reusable buffer and sample for addFrame
	buffer_size_ = width * height * BYTES_PER_PIXEL;
	hr = MFCreateMemoryBuffer(buffer_size_, &buffer_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateMemoryBuffer", hr);
		releaseResources();
		return -1;
	}

	hr = MFCreateSample(&sample_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateSample", hr);
		releaseResources();
		return -1;
	}

	hr = sample_->AddBuffer(buffer_);
	if (FAILED(hr)) {
		setErrorHR("AddBuffer", hr);
		releaseResources();
		return -1;
	}

	width_ = width;
	height_ = height;
	fps_ = fps;
	frame_index_ = 0;

	return 0;
}

/**
 * Add a BGRA frame via CPU path. Expects top-down pixel data (first byte = top-left).
 * Raw glReadPixels gives bottom-up data — flip rows before calling, or use the GPU path instead.
 */
int videoEncoderAddFrame(const unsigned char* bgraPixels, int dataLength) {
	clearError();

	if (!writer_) {
		setError("Encoder not initialized");
		return -1;
	}

	int expectedLength = width_ * height_ * BYTES_PER_PIXEL;
	if (dataLength != expectedLength) {
		setError("Data length mismatch: %d != %d", dataLength, expectedLength);
		return -1;
	}

	LONGLONG frameDuration = MF_TICKS_PER_SECOND / fps_;
	LONGLONG timestamp = (LONGLONG)frame_index_ * frameDuration;

	HRESULT hr = writeSampleFromBGRA(bgraPixels, timestamp, frameDuration);
	if (FAILED(hr)) {
		setErrorHR("writeSampleFromBGRA", hr);
		return -1;
	}

	frame_index_++;
	return 0;
}

int videoEncoderFinish(void) {
	clearError();

	if (!writer_) {
		setError("Encoder not initialized");
		return -1;
	}

	// Flush last pending PBO frame before finalizing
	if (gpu_mode_ && !interop_available_ && pbo_pending_) {
		LONGLONG frameDuration = MF_TICKS_PER_SECOND / fps_;
		LONGLONG timestamp = (LONGLONG)frame_index_ * frameDuration;

		int prev = 1 - pbo_index_;
		glBindBuffer_(GL_PIXEL_PACK_BUFFER, pbo_ids_[prev]);
		void* mapped = glMapBuffer_(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (mapped) {
			memcpy(readback_buf_, mapped, buffer_size_);
			glUnmapBuffer_(GL_PIXEL_PACK_BUFFER);
		}
		glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);

		HRESULT flushHr = writeSampleFromBGRA(readback_buf_, timestamp, frameDuration);
		if (FAILED(flushHr)) {
			setErrorHR("writeSampleFromBGRA (flush last PBO)", flushHr);
			return -1;
		}
		frame_index_++;
		pbo_pending_ = false;
	}

	HRESULT hr = writer_->Finalize();
	if (FAILED(hr)) {
		setErrorHR("Finalize", hr);
		return -1;
	}

	return 0;
}

void videoEncoderDispose(void) {
	if (gpu_mode_) releaseGpuResources();
	releaseResources();
	clearError();
}

const char* videoEncoderGetError(void) {
	return error_buf_[0] != '\0' ? error_buf_ : NULL;
}

// ---------------------------------------------------------------------------
// GPU path
// ---------------------------------------------------------------------------

int videoEncoderSupportsGpuInput(void) {
	return 1;
}

int videoEncoderInitGpu(const char* outputPath, int width, int height, int fps, int bitrate, int keyframeInterval) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || keyframeInterval <= 0) {
		setError("Invalid encoder parameters");
		return -1;
	}

	// Resolve GL extension functions (requires active WGL context)
	if (!resolveGlFunctions()) {
		setError("Failed to resolve GL functions — no WGL context?");
		return -1;
	}

	// Initialize COM
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hr) || hr == S_FALSE) {
		com_initialized_ = TRUE;
	} else if (hr == RPC_E_CHANGED_MODE) {
		com_initialized_ = FALSE;
	} else {
		setErrorHR("CoInitializeEx", hr);
		return -1;
	}

	// Start Media Foundation
	hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) {
		setErrorHR("MFStartup", hr);
		releaseResources();
		return -1;
	}
	mf_started_ = TRUE;

	// Delete existing file
	DeleteFileA(outputPath);

	// Probe D3D11 interop support (WGL_NV_DX_interop2).
	// Zero-copy on NVIDIA/some AMD; falls back to PBO readback on Intel/others.
	interop_available_ = checkInteropSupport();
	if (interop_available_) {
		if (!initD3D11Interop(width, height)) {
			releaseGpuResources();
			interop_available_ = false;
		}
	}

	// Create SinkWriter (with D3D11 manager if interop is available)
	hr = createSinkWriter(outputPath, interop_available_, &writer_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateSinkWriterFromURL", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Add H.264 output stream
	IMFMediaType* pOutputType = NULL;
	hr = createOutputType(width, height, fps, bitrate, keyframeInterval, &pOutputType);
	if (FAILED(hr)) {
		setErrorHR("Create output media type", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	hr = writer_->AddStream(pOutputType, &stream_index_);
	safeRelease(&pOutputType);
	if (FAILED(hr)) {
		setErrorHR("AddStream", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Set BGRA input type (bottom-up — MF negative stride handles Y-flip)
	IMFMediaType* pInputType = NULL;
	hr = createInputType(width, height, fps, false, &pInputType);
	if (FAILED(hr)) {
		setErrorHR("Create input media type", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	hr = writer_->SetInputMediaType(stream_index_, pInputType, NULL);
	safeRelease(&pInputType);
	if (FAILED(hr)) {
		setErrorHR("SetInputMediaType", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Start writing
	hr = writer_->BeginWriting();
	if (FAILED(hr)) {
		setErrorHR("BeginWriting", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Pre-allocate reusable buffer and sample (used by fallback path, harmless for interop)
	buffer_size_ = width * height * BYTES_PER_PIXEL;
	hr = MFCreateMemoryBuffer(buffer_size_, &buffer_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateMemoryBuffer", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	hr = MFCreateSample(&sample_);
	if (FAILED(hr)) {
		setErrorHR("MFCreateSample", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	hr = sample_->AddBuffer(buffer_);
	if (FAILED(hr)) {
		setErrorHR("AddBuffer", hr);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	gpu_mode_ = true;
	width_ = width;
	height_ = height;
	fps_ = fps;
	frame_index_ = 0;

	return 0;
}

unsigned int videoEncoderGetSurfaceId(void) {
	return 0;
}

int videoEncoderSetupGpuFbo(int width, int height) {
	clearError();
	if (!gpu_mode_) {
		setError("GPU encoder not initialized");
		return -1;
	}

	// Interop path: FBO already created in initD3D11Interop
	if (interop_available_) return 0;

	// Fallback path: create internal FBO for readback
	if (!initFallbackGpu(width, height)) {
		setError("Failed to create fallback FBO");
		return -1;
	}
	return 0;
}

void videoEncoderBlitGpuFrame(unsigned int srcFbo, int width, int height) {
	if (!gpu_mode_) return;

	if (interop_available_) {
		// D3D11 interop: lock, blit with Y-flip, unlock.
		// DXGI textures are always top-down — MF_MT_DEFAULT_STRIDE is ignored for GPU surfaces.
		// Must flip here because GL FBO origin is bottom-left, D3D11 is top-left.
		wglDXLockObjectsNV_(interop_device_, 1, &interop_object_);

		glBindFramebuffer_(GL_READ_FRAMEBUFFER, srcFbo);
		glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, interop_fbo_);
		glBlitFramebuffer_(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Fence to ensure blit completes before submitGpuFrame copies via D3D11
		if (blit_fence_) glDeleteSync_(blit_fence_);
		blit_fence_ = glFenceSync_(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

		wglDXUnlockObjectsNV_(interop_device_, 1, &interop_object_);
	} else {
		// Fallback: blit with Y-flip into internal FBO, then readback.
		// Flip needed: glReadPixels produces top-down after flip, matching MF's expectation.
		glBindFramebuffer_(GL_READ_FRAMEBUFFER, srcFbo);
		glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, fallback_fbo_);
		glBlitFramebuffer_(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		if (pbo_active_) {
			// Async PBO readback — kick off DMA transfer, don't block
			glBindFramebuffer_(GL_READ_FRAMEBUFFER, fallback_fbo_);
			glBindBuffer_(GL_PIXEL_PACK_BUFFER, pbo_ids_[pbo_index_]);
			glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
			glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
		} else {
			// Sync fallback
			glBindFramebuffer_(GL_READ_FRAMEBUFFER, fallback_fbo_);
			glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, readback_buf_);
		}
	}
}

int videoEncoderSubmitGpuFrame(void) {
	clearError();

	if (!writer_ || !gpu_mode_) {
		setError("GPU encoder not initialized");
		return -1;
	}

	LONGLONG frameDuration = MF_TICKS_PER_SECOND / fps_;
	LONGLONG timestamp = (LONGLONG)frame_index_ * frameDuration;

	HRESULT hr;

	if (interop_available_) {
		// Wait for GL blit to complete before D3D11 reads the interop texture
		if (blit_fence_) {
			glClientWaitSync_(blit_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
			glDeleteSync_(blit_fence_);
			blit_fence_ = NULL;
		}

		// Copy interop texture to a fresh pool texture (B-frame safety).
		// The encoder holds refs to previous textures during B-frame reordering —
		// without copy, the next blit overwrites data the encoder is still reading.
		ID3D11Texture2D* fresh = pool_textures_[pool_index_];
		d3d_context_->CopyResource(fresh, d3d_texture_);
		pool_index_ = (pool_index_ + 1) % POOL_SIZE;

		IMFMediaBuffer* dxgi_buffer = NULL;
		hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), fresh, 0, FALSE, &dxgi_buffer);
		if (FAILED(hr)) {
			setErrorHR("MFCreateDXGISurfaceBuffer", hr);
			return -1;
		}

		IMFSample* gpu_sample = NULL;
		hr = MFCreateSample(&gpu_sample);
		if (FAILED(hr)) {
			dxgi_buffer->Release();
			setErrorHR("MFCreateSample (GPU)", hr);
			return -1;
		}

		hr = gpu_sample->AddBuffer(dxgi_buffer);
		if (FAILED(hr)) {
			gpu_sample->Release();
			dxgi_buffer->Release();
			setErrorHR("AddBuffer (GPU)", hr);
			return -1;
		}

		hr = gpu_sample->SetSampleTime(timestamp);
		if (FAILED(hr)) {
			gpu_sample->Release();
			dxgi_buffer->Release();
			setErrorHR("SetSampleTime (GPU)", hr);
			return -1;
		}

		hr = gpu_sample->SetSampleDuration(frameDuration);
		if (FAILED(hr)) {
			gpu_sample->Release();
			dxgi_buffer->Release();
			setErrorHR("SetSampleDuration (GPU)", hr);
			return -1;
		}

		hr = writer_->WriteSample(stream_index_, gpu_sample);
		gpu_sample->Release();
		dxgi_buffer->Release();

		if (FAILED(hr)) {
			setErrorHR("WriteSample (GPU)", hr);
			return -1;
		}
	} else if (pbo_active_) {
		// PBO double-buffer: map PREVIOUS PBO (completed by now), submit its data.
		// Use previous frame's timestamp since the pixels are one frame behind.
		if (pbo_pending_) {
			LONGLONG prevTimestamp = (LONGLONG)(frame_index_ - 1) * frameDuration;
			int prev = 1 - pbo_index_;
			glBindBuffer_(GL_PIXEL_PACK_BUFFER, pbo_ids_[prev]);
			void* mapped = glMapBuffer_(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
			if (mapped) {
				memcpy(readback_buf_, mapped, buffer_size_);
				glUnmapBuffer_(GL_PIXEL_PACK_BUFFER);
			}
			glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);

			hr = writeSampleFromBGRA(readback_buf_, prevTimestamp, frameDuration);
			if (FAILED(hr)) {
				setErrorHR("writeSampleFromBGRA (PBO fallback)", hr);
				return -1;
			}
		}
		pbo_index_ = 1 - pbo_index_;
		pbo_pending_ = true;
	} else {
		// Sync fallback: readback_buf_ already filled in blitGpuFrame
		hr = writeSampleFromBGRA(readback_buf_, timestamp, frameDuration);
		if (FAILED(hr)) {
			setErrorHR("writeSampleFromBGRA (sync fallback)", hr);
			return -1;
		}
	}

	frame_index_++;
	return 0;
}

void videoEncoderDisposeGpuFbo(void) {
	releaseGpuResources();
}

}  // extern "C"

#endif	// _WIN32
