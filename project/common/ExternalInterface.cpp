#ifndef STATIC_LINK
#define IMPLEMENT_API
#endif

#if defined(HX_WINDOWS) || defined(HX_MACOS) || defined(HX_LINUX)
#define NEKO_COMPATIBLE
#endif

#include <hx/CFFIPrime.h>

extern "C" {
int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate, int keyframeInterval);
int videoEncoderAddFrame(const unsigned char* bgraPixels, int dataLength);
int videoEncoderFinish(void);
void videoEncoderDispose(void);
const char* videoEncoderGetError(void);
int videoEncoderSupportsGpuInput(void);
int videoEncoderInitGpu(const char* outputPath, int width, int height, int fps, int bitrate, int keyframeInterval);
unsigned int videoEncoderGetSurfaceId(void);
int videoEncoderSubmitGpuFrame(void);
int videoEncoderSetupGpuFbo(int width, int height);
void videoEncoderBlitGpuFrame(unsigned int srcFbo, int width, int height);
void videoEncoderDisposeGpuFbo(void);
}

int ve_init(const char* path, int w, int h, int fps, int br, int kfi) {
	return videoEncoderInit(path, w, h, fps, br, kfi);
}
DEFINE_PRIME6(ve_init);

int ve_addFrame(value pixels, int len) {
	buffer buf = val_to_buffer(pixels);
	const unsigned char* data = (const unsigned char*)buffer_data(buf);
	return videoEncoderAddFrame(data, len);
}
DEFINE_PRIME2(ve_addFrame);

int ve_finish() {
	return videoEncoderFinish();
}
DEFINE_PRIME0(ve_finish);

void ve_dispose() {
	videoEncoderDispose();
}
DEFINE_PRIME0v(ve_dispose);

value ve_getError() {
	const char* err = videoEncoderGetError();
	return err ? alloc_string(err) : alloc_null();
}
DEFINE_PRIME0(ve_getError);

int ve_supportsGpuInput() {
	return videoEncoderSupportsGpuInput();
}
DEFINE_PRIME0(ve_supportsGpuInput);

int ve_initGpu(const char* path, int w, int h, int fps, int br, int kfi) {
	return videoEncoderInitGpu(path, w, h, fps, br, kfi);
}
DEFINE_PRIME6(ve_initGpu);

// SurfaceID is uint32_t; CFFI Prime has no unsigned type, so we narrow to int.
// Bit pattern is preserved — callers should compare != 0, not > 0.
int ve_getSurfaceId() {
	return (int)videoEncoderGetSurfaceId();
}
DEFINE_PRIME0(ve_getSurfaceId);

int ve_submitGpuFrame() {
	return videoEncoderSubmitGpuFrame();
}
DEFINE_PRIME0(ve_submitGpuFrame);

int ve_setupGpuFbo(int w, int h) {
	return videoEncoderSetupGpuFbo(w, h);
}
DEFINE_PRIME2(ve_setupGpuFbo);

void ve_blitGpuFrame(int srcFbo, int w, int h) {
	videoEncoderBlitGpuFrame((unsigned int)srcFbo, w, h);
}
DEFINE_PRIME3v(ve_blitGpuFrame);

void ve_disposeGpuFbo() {
	videoEncoderDisposeGpuFbo();
}
DEFINE_PRIME0v(ve_disposeGpuFbo);

extern "C" void extension_video_export_main() {
	val_int(0);
}
DEFINE_ENTRY_POINT(extension_video_export_main);

extern "C" int extension_video_export_register_prims() {
	return 0;
}
