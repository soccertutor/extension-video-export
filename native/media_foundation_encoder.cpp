/**
 * Windows Media Foundation video encoder — hardware H.264 via MFSinkWriter.
 *
 * BGRA input (MFVideoFormat_RGB32) → MF handles color conversion internally.
 * Same C API as AVAssetWriterEncoder: videoEncoderInit/AddFrame/Finish/Dispose/GetError.
 */

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;  // BGRA

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
// Create an IMFMediaType for the H.264 output stream
// ---------------------------------------------------------------------------

static HRESULT createOutputType(int width, int height, int fps, int bitrate, IMFMediaType** ppType) {
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

	*ppType = pType;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Create an IMFMediaType for the BGRA input stream
// ---------------------------------------------------------------------------

static HRESULT createInputType(int width, int height, int fps, IMFMediaType** ppType) {
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

	// Negative stride = top-down input, matching OpenFL BGRA layout.
	// Eliminates per-frame row-reversal copy.
	hr = pType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(-(int)(width * BYTES_PER_PIXEL)));
	if (FAILED(hr)) {
		pType->Release();
		return hr;
	}

	*ppType = pType;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Write pre-allocated sample with BGRA pixel data
// ---------------------------------------------------------------------------

static HRESULT writeSampleFromBGRA(const unsigned char* bgraPixels, LONGLONG timestamp, LONGLONG duration) {
	BYTE* pData = NULL;
	HRESULT hr = buffer_->Lock(&pData, NULL, NULL);
	if (FAILED(hr)) return hr;

	// Top-down copy — negative stride on input type handles orientation
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

int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
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

	// Convert path to wide string for MFCreateSinkWriterFromURL
	int wideLen = MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, NULL, 0);
	if (wideLen <= 0) {
		setError("Failed to convert output path to wide string");
		releaseResources();
		return -1;
	}
	wchar_t* widePath = new wchar_t[wideLen];
	MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, widePath, wideLen);

	// Create SinkWriter with MPEG4 container
	IMFAttributes* pAttributes = NULL;
	hr = MFCreateAttributes(&pAttributes, 1);
	if (SUCCEEDED(hr)) {
		// Enable hardware transforms (MFT)
		hr = pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	}

	hr = MFCreateSinkWriterFromURL(widePath, NULL, pAttributes, &writer_);
	delete[] widePath;
	safeRelease(&pAttributes);

	if (FAILED(hr)) {
		setErrorHR("MFCreateSinkWriterFromURL", hr);
		releaseResources();
		return -1;
	}

	// Add H.264 output stream
	IMFMediaType* pOutputType = NULL;
	hr = createOutputType(width, height, fps, bitrate, &pOutputType);
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

	// Set BGRA input type
	IMFMediaType* pInputType = NULL;
	hr = createInputType(width, height, fps, &pInputType);
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

	// 100-nanosecond units per second
	LONGLONG frameDuration = 10000000LL / fps_;
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

	HRESULT hr = writer_->Finalize();
	if (FAILED(hr)) {
		setErrorHR("Finalize", hr);
		return -1;
	}

	return 0;
}

void videoEncoderDispose(void) {
	releaseResources();
	clearError();
}

const char* videoEncoderGetError(void) {
	return error_buf_[0] != '\0' ? error_buf_ : NULL;
}

}  // extern "C"

#endif	// _WIN32
