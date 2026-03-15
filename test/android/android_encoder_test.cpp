/**
 * Standalone Android encoder test — exercises CPU and GPU paths via NDK MediaCodec.
 *
 * Build with CMake + NDK toolchain, push to device/emulator via adb, run via adb shell.
 * GPU tests are non-fatal: emulators without full MediaCodec surface support will skip them.
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Extern C encoder API
// ---------------------------------------------------------------------------

extern "C" {
int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate);
int videoEncoderAddFrame(const unsigned char* bgraPixels, int dataLength);
int videoEncoderFinish(void);
void videoEncoderDispose(void);
const char* videoEncoderGetError(void);
int videoEncoderSupportsGpuInput(void);
int videoEncoderInitGpu(const char* outputPath, int width, int height, int fps, int bitrate);
unsigned int videoEncoderGetSurfaceId(void);
int videoEncoderSubmitGpuFrame(void);
int videoEncoderSetupIoSurfaceFbo(int width, int height);
void videoEncoderBlitToIoSurface(unsigned int srcFbo, int width, int height);
void videoEncoderDisposeIoSurfaceFbo(void);
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const int WIDTH_ALIGNED = 64;
static const int HEIGHT_ALIGNED = 64;
static const int WIDTH_UNALIGNED = 62;
static const int HEIGHT_UNALIGNED = 62;
static const int FPS = 30;
static const int BITRATE = 500000;
static const int FRAME_COUNT = 30;
static const int MIN_FILE_SIZE = 100;
static const int BYTES_PER_PIXEL = 4;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static long getFileSize(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 ? st.st_size : -1;
}

static int tests_run_ = 0;
static int tests_passed_ = 0;
static int gpu_tests_run_ = 0;
static int gpu_tests_passed_ = 0;

#define ASSERT(cond, msg)                                  \
	do {                                                   \
		if (!(cond)) {                                     \
			printf("  FAIL: %s\n", (msg));                 \
			const char* err = videoEncoderGetError();      \
			if (err) printf("  encoder error: %s\n", err); \
			return -1;                                     \
		}                                                  \
	} while (0)

#define RUN_TEST(fn)                \
	do {                            \
		tests_run_++;               \
		printf("[TEST] %s\n", #fn); \
		if ((fn)() == 0) {          \
			tests_passed_++;        \
			printf("  PASS\n");     \
		} else {                    \
			printf("  FAILED\n");   \
		}                           \
	} while (0)

#define RUN_GPU_TEST(fn)                              \
	do {                                              \
		gpu_tests_run_++;                             \
		printf("[GPU TEST] %s\n", #fn);               \
		if ((fn)() == 0) {                            \
			gpu_tests_passed_++;                      \
			printf("  PASS\n");                       \
		} else {                                      \
			printf("  SKIPPED/FAILED (non-fatal)\n"); \
		}                                             \
	} while (0)

// Fill a BGRA buffer with a solid color test pattern
static void fillTestPattern(unsigned char* pixels, int dataLen) {
	for (int i = 0; i < dataLen; i += BYTES_PER_PIXEL) {
		pixels[i] = 0;		  // B
		pixels[i + 1] = 128;  // G
		pixels[i + 2] = 255;  // R
		pixels[i + 3] = 255;  // A
	}
}

// ---------------------------------------------------------------------------
// Test 1: CPU path — aligned dimensions (64x64)
// ---------------------------------------------------------------------------

static int testCpuAligned() {
	const char* path = "/data/local/tmp/test_cpu_aligned.mp4";
	const int dataLen = WIDTH_ALIGNED * HEIGHT_ALIGNED * BYTES_PER_PIXEL;
	unsigned char* pixels = (unsigned char*)calloc(dataLen, 1);
	ASSERT(pixels != nullptr, "alloc failed");

	fillTestPattern(pixels, dataLen);

	const int rc_init = videoEncoderInit(path, WIDTH_ALIGNED, HEIGHT_ALIGNED, FPS, BITRATE);
	ASSERT(rc_init == 0, "init failed");

	for (int i = 0; i < FRAME_COUNT; i++) {
		const int rc_frame = videoEncoderAddFrame(pixels, dataLen);
		ASSERT(rc_frame == 0, "addFrame failed");
	}

	const int rc_finish = videoEncoderFinish();
	ASSERT(rc_finish == 0, "finish failed");

	const long size = getFileSize(path);
	printf("  output: %ld bytes\n", size);
	ASSERT(size >= MIN_FILE_SIZE, "output file too small");

	videoEncoderDispose();
	free(pixels);
	unlink(path);
	return 0;
}

// ---------------------------------------------------------------------------
// Test 2: CPU path — non-aligned dimensions (62x62)
// ---------------------------------------------------------------------------

static int testCpuUnaligned() {
	const char* path = "/data/local/tmp/test_cpu_unaligned.mp4";
	const int dataLen = WIDTH_UNALIGNED * HEIGHT_UNALIGNED * BYTES_PER_PIXEL;
	unsigned char* pixels = (unsigned char*)calloc(dataLen, 1);
	ASSERT(pixels != nullptr, "alloc failed");

	fillTestPattern(pixels, dataLen);

	const int rc_init = videoEncoderInit(path, WIDTH_UNALIGNED, HEIGHT_UNALIGNED, FPS, BITRATE);
	ASSERT(rc_init == 0, "init failed");

	for (int i = 0; i < FRAME_COUNT; i++) {
		const int rc_frame = videoEncoderAddFrame(pixels, dataLen);
		ASSERT(rc_frame == 0, "addFrame failed");
	}

	const int rc_finish = videoEncoderFinish();
	ASSERT(rc_finish == 0, "finish failed");

	const long size = getFileSize(path);
	printf("  output: %ld bytes\n", size);
	ASSERT(size >= MIN_FILE_SIZE, "output file too small");

	videoEncoderDispose();
	free(pixels);
	unlink(path);
	return 0;
}

// ---------------------------------------------------------------------------
// Test 3: supportsGpuInput check
// ---------------------------------------------------------------------------

static int testSupportsGpu() {
	const int supported = videoEncoderSupportsGpuInput();
	printf("  supportsGpuInput: %d\n", supported);
	ASSERT(supported == 1, "supportsGpuInput should return 1");
	return 0;
}

// ---------------------------------------------------------------------------
// Test 4: GPU path — basic encode (64x64)
// ---------------------------------------------------------------------------

static int testGpuEncode() {
	if (videoEncoderSupportsGpuInput() != 1) {
		printf("  GPU test SKIPPED (supportsGpuInput returned 0)\n");
		return -1;
	}

	int result = -1;
	const char* path = "/data/local/tmp/test_gpu.mp4";
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	GLuint fbo = 0, tex = 0;
	bool encoder_init = false;
	bool fbo_init = false;

	// 1. Create standalone EGL context
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY) {
		printf("  GPU test SKIPPED (eglGetDisplay failed)\n");
		goto cleanup;
	}

	if (!eglInitialize(display, nullptr, nullptr)) {
		printf("  GPU test SKIPPED (eglInitialize failed)\n");
		display = EGL_NO_DISPLAY;
		goto cleanup;
	}

	{
		const EGLint configAttribs[] = {
			EGL_SURFACE_TYPE,
			EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
			EGL_RED_SIZE,
			8,
			EGL_GREEN_SIZE,
			8,
			EGL_BLUE_SIZE,
			8,
			EGL_ALPHA_SIZE,
			8,
			EGL_RENDERABLE_TYPE,
			EGL_OPENGL_ES3_BIT,
			EGL_NONE
		};
		EGLConfig config;
		EGLint numConfigs;
		eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);
		if (numConfigs == 0) {
			printf("  GPU test SKIPPED (no suitable EGL config)\n");
			goto cleanup;
		}

		const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
		if (ctx == EGL_NO_CONTEXT) {
			printf("  GPU test SKIPPED (eglCreateContext failed)\n");
			goto cleanup;
		}

		const EGLint pbufAttribs[] = {EGL_WIDTH, WIDTH_ALIGNED, EGL_HEIGHT, HEIGHT_ALIGNED, EGL_NONE};
		surface = eglCreatePbufferSurface(display, config, pbufAttribs);
		if (surface == EGL_NO_SURFACE) {
			printf("  GPU test SKIPPED (eglCreatePbufferSurface failed)\n");
			goto cleanup;
		}
	}

	eglMakeCurrent(display, surface, surface, ctx);

	// 2. Create source FBO with test content
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);

	{
		const int texSize = WIDTH_ALIGNED * HEIGHT_ALIGNED * BYTES_PER_PIXEL;
		unsigned char* texData = (unsigned char*)calloc(texSize, 1);
		for (int i = 0; i < texSize; i += BYTES_PER_PIXEL) {
			texData[i] = 255;
			texData[i + 1] = 128;
			texData[i + 2] = 0;
			texData[i + 3] = 255;
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH_ALIGNED, HEIGHT_ALIGNED, 0, GL_RGBA, GL_UNSIGNED_BYTE, texData);
		free(texData);
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("  GPU test SKIPPED (framebuffer incomplete)\n");
		goto cleanup;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// 3. GPU encode
	if (videoEncoderInitGpu(path, WIDTH_ALIGNED, HEIGHT_ALIGNED, FPS, BITRATE) != 0) {
		printf("  GPU test SKIPPED (MediaCodec surface input not available on this emulator)\n");
		goto cleanup;
	}
	encoder_init = true;

	if (videoEncoderSetupIoSurfaceFbo(WIDTH_ALIGNED, HEIGHT_ALIGNED) != 0) {
		printf("  GPU test SKIPPED (setupIoSurfaceFbo failed)\n");
		goto cleanup;
	}
	fbo_init = true;

	for (int i = 0; i < FRAME_COUNT; i++) {
		videoEncoderBlitToIoSurface(fbo, WIDTH_ALIGNED, HEIGHT_ALIGNED);
		if (videoEncoderSubmitGpuFrame() != 0) {
			printf("  GPU test FAILED at frame %d: submitGpuFrame error\n", i);
			goto cleanup;
		}
	}

	if (videoEncoderFinish() != 0) {
		printf("  FAIL: GPU finish failed\n");
		goto cleanup;
	}

	{
		const long size = getFileSize(path);
		printf("  GPU output: %ld bytes\n", size);
		if (size < MIN_FILE_SIZE) {
			printf("  FAIL: GPU output file too small\n");
			goto cleanup;
		}
	}

	result = 0;

cleanup:
	if (fbo_init) videoEncoderDisposeIoSurfaceFbo();
	if (encoder_init) videoEncoderDispose();
	if (fbo) glDeleteFramebuffers(1, &fbo);
	if (tex) glDeleteTextures(1, &tex);
	if (surface != EGL_NO_SURFACE) {
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(display, surface);
	}
	if (ctx != EGL_NO_CONTEXT) eglDestroyContext(display, ctx);
	if (display != EGL_NO_DISPLAY) eglTerminate(display);
	unlink(path);
	return result;
}

// ---------------------------------------------------------------------------
// Test 5: Error handling
// ---------------------------------------------------------------------------

static int testErrorHandling() {
	// Init with invalid params (zero dimensions)
	int rc = videoEncoderInit("/data/local/tmp/err.mp4", 0, 0, 0, 0);
	ASSERT(rc == -1, "init with invalid params should fail");
	ASSERT(videoEncoderGetError() != nullptr, "error should be set after invalid init");
	videoEncoderDispose();

	// AddFrame without init
	unsigned char dummy[4] = {0};
	rc = videoEncoderAddFrame(dummy, 4);
	ASSERT(rc == -1, "addFrame without init should fail");
	ASSERT(videoEncoderGetError() != nullptr, "error should be set after addFrame without init");

	// Finish without init
	rc = videoEncoderFinish();
	ASSERT(rc == -1, "finish without init should fail");
	ASSERT(videoEncoderGetError() != nullptr, "error should be set after finish without init");

	return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
	printf("=== Android Encoder Tests ===\n\n");

	// CPU tests (must pass)
	RUN_TEST(testCpuAligned);
	RUN_TEST(testCpuUnaligned);

	// GPU tests (non-fatal — emulators may lack support)
	RUN_GPU_TEST(testSupportsGpu);
	RUN_GPU_TEST(testGpuEncode);

	// Error handling (must pass)
	RUN_TEST(testErrorHandling);

	printf("\n=== Results ===\n");
	printf("CPU/core tests: %d/%d passed\n", tests_passed_, tests_run_);
	printf("GPU tests:      %d/%d passed (non-fatal)\n", gpu_tests_passed_, gpu_tests_run_);

	if (tests_passed_ < tests_run_) {
		printf("\nFAILED: %d core test(s) failed\n", tests_run_ - tests_passed_);
		return 1;
	}

	printf("\nALL CORE TESTS PASSED\n");
	return 0;
}
