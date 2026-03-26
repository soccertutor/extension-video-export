/**
 * Standalone iOS Simulator encoder test — exercises CPU and GPU paths via AVAssetWriter.
 *
 * Build with CMake targeting iOS Simulator SDK, run via `xcrun simctl spawn`.
 * GPU tests are non-fatal: simulator may lack Metal/IOSurface support.
 */

#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#import <OpenGLES/EAGL.h>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Extern C encoder API
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const int WIDTH_ALIGNED = 64;
static const int HEIGHT_ALIGNED = 64;
static const int WIDTH_UNALIGNED = 62;
static const int HEIGHT_UNALIGNED = 62;
static const int FPS = 30;
static const int BITRATE = 500000;
static const int KEYFRAME_INTERVAL = 2;
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
// Shared CPU encode helper
// ---------------------------------------------------------------------------

static int testCpuEncode(const char* const path, int width, int height) {
	const int dataLen = width * height * BYTES_PER_PIXEL;
	unsigned char* pixels = (unsigned char*)calloc(dataLen, 1);
	ASSERT(pixels != nullptr, "alloc failed");

	fillTestPattern(pixels, dataLen);

	const int rc_init = videoEncoderInit(path, width, height, FPS, BITRATE, KEYFRAME_INTERVAL);
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
// Test 1: CPU path — aligned dimensions (64x64)
// ---------------------------------------------------------------------------

static int testCpuAligned() {
	return testCpuEncode("/tmp/test_ios_cpu_aligned.mp4", WIDTH_ALIGNED, HEIGHT_ALIGNED);
}

// ---------------------------------------------------------------------------
// Test 2: CPU path — non-aligned dimensions (62x62)
// ---------------------------------------------------------------------------

static int testCpuUnaligned() {
	return testCpuEncode("/tmp/test_ios_cpu_unaligned.mp4", WIDTH_UNALIGNED, HEIGHT_UNALIGNED);
}

// ---------------------------------------------------------------------------
// Test 3: supportsGpuInput check
// ---------------------------------------------------------------------------

static int testSupportsGpu() {
	const int supported = videoEncoderSupportsGpuInput();
	printf("  supportsGpuInput: %d\n", supported);
	// Metal may not be available on all simulators — just report, don't hard-fail
	return supported == 1 ? 0 : -1;
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
	const char* const path = "/tmp/test_ios_gpu.mp4";
	EAGLContext* ctx = nil;
	GLuint srcFbo = 0, srcTex = 0;
	bool encoder_init = false;
	bool fbo_init = false;

	// 1. Create standalone EAGL context (OpenGL ES 3.0)
	ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
	if (!ctx) {
		printf("  GPU test SKIPPED (EAGLContext creation failed)\n");
		return -1;
	}
	[EAGLContext setCurrentContext:ctx];

	// 2. Create source FBO with test content
	glGenFramebuffers(1, &srcFbo);
	glGenTextures(1, &srcTex);
	glBindTexture(GL_TEXTURE_2D, srcTex);

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

	glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("  GPU test SKIPPED (framebuffer incomplete)\n");
		goto cleanup;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// 3. GPU encode
	if (videoEncoderInitGpu(path, WIDTH_ALIGNED, HEIGHT_ALIGNED, FPS, BITRATE, KEYFRAME_INTERVAL) != 0) {
		printf("  GPU test SKIPPED (initGpu failed: %s)\n", videoEncoderGetError() ?: "unknown");
		goto cleanup;
	}
	encoder_init = true;

	if (videoEncoderSetupGpuFbo(WIDTH_ALIGNED, HEIGHT_ALIGNED) != 0) {
		printf("  GPU test SKIPPED (setupGpuFbo failed: %s)\n", videoEncoderGetError() ?: "unknown");
		goto cleanup;
	}
	fbo_init = true;

	for (int i = 0; i < FRAME_COUNT; i++) {
		videoEncoderBlitGpuFrame(srcFbo, WIDTH_ALIGNED, HEIGHT_ALIGNED);
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
	if (fbo_init) videoEncoderDisposeGpuFbo();
	if (encoder_init) videoEncoderDispose();
	if (srcFbo) glDeleteFramebuffers(1, &srcFbo);
	if (srcTex) glDeleteTextures(1, &srcTex);
	[EAGLContext setCurrentContext:nil];
	unlink(path);
	return result;
}

// ---------------------------------------------------------------------------
// Test 5: Error handling
// ---------------------------------------------------------------------------

static int testErrorHandling() {
	// Init with invalid params (zero dimensions)
	int rc = videoEncoderInit("/tmp/test_ios_err.mp4", 0, 0, 0, 0, 0);
	ASSERT(rc == -1, "init with invalid params should fail");
	ASSERT(videoEncoderGetError() != nullptr, "error should be set after invalid init");
	videoEncoderDispose();

	// AddFrame without init
	unsigned char dummy[BYTES_PER_PIXEL] = {0};
	rc = videoEncoderAddFrame(dummy, BYTES_PER_PIXEL);
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
	@autoreleasepool {
		printf("=== iOS Encoder Tests ===\n\n");

		// CPU tests (must pass)
		RUN_TEST(testCpuAligned);
		RUN_TEST(testCpuUnaligned);

		// GPU tests (non-fatal — simulator may lack support)
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
}
