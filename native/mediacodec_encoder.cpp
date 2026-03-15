/**
 * Android NDK video encoder — hardware H.264 via AMediaCodec + AMediaMuxer.
 *
 * BGRA input → NV12 conversion in native code (not all devices accept BGRA).
 * Same C API as AVAssetWriterEncoder:
 * videoEncoderInit/AddFrame/Finish/Dispose/GetError. Requires API 21+ (our
 * minimum SDK).
 */

#ifdef __ANDROID__

#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VE_USE_NEON 1
#include <arm_neon.h>
#endif

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;		  // BGRA
static const int TIMEOUT_US = 10000;		  // 10ms dequeue timeout
static const int INPUT_TIMEOUT_US = 1000000;  // 1s input buffer timeout
static const int COLOR_FORMAT_NV12 = 21;	  // COLOR_FormatYUV420SemiPlanar
static const char* const MIME_H264 = "video/avc";
static const int COLOR_FORMAT_SURFACE = 0x7F000789;

// BT.601 color conversion coefficients
static const int COEFF_R_Y = 66;
static const int COEFF_G_Y = 129;
static const int COEFF_B_Y = 25;
static const int COEFF_R_U = -38;
static const int COEFF_G_U = -74;
static const int COEFF_B_U = 112;
static const int COEFF_R_V = 112;
static const int COEFF_G_V = -94;
static const int COEFF_B_V = -18;
static const int Y_OFFSET = 16;
static const int UV_OFFSET = 128;
static const int ROUNDING_BIAS = 128;
static const int UV_AVG_BIAS = 2;  // round-half-up for 2x2 averaging (>> 2)

static AMediaCodec* codec_ = NULL;
static AMediaMuxer* muxer_ = NULL;
static int muxer_track_index_ = -1;
static bool muxer_started_ = false;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;
static int muxer_fd_ = -1;
static char error_buf_[ERROR_BUF_SIZE] = {0};

static ANativeWindow* input_surface_ = NULL;
static EGLDisplay egl_display_ = EGL_NO_DISPLAY;
static EGLSurface egl_surface_ = EGL_NO_SURFACE;
static GLsync blit_fence_ = NULL;
static bool gpu_mode_ = false;

// eglPresentationTimeANDROID — sets frame timestamp for MediaCodec surface input.
// Resolved at runtime via eglGetProcAddress (EGL_ANDROID_presentation_time extension).
typedef EGLBoolean (*PFNEGLPRESENTATIONTIMEANDROIDPROC)(EGLDisplay, EGLSurface, EGLnsecsANDROID);
static PFNEGLPRESENTATIONTIMEANDROIDPROC eglPresentationTimeANDROID_ = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(error_buf_, ERROR_BUF_SIZE, fmt, args);
	va_end(args);
}

static void clearError(void) {
	error_buf_[0] = '\0';
}

// ---------------------------------------------------------------------------
// EGL context save/restore
// ---------------------------------------------------------------------------

struct EglState {
	EGLDisplay display;
	EGLSurface draw;
	EGLSurface read;
	EGLContext context;
};

static EglState saveEglState(void) {
	return {eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), eglGetCurrentSurface(EGL_READ), eglGetCurrentContext()};
}

static void restoreEglState(const EglState& s) {
	eglMakeCurrent(s.display, s.draw, s.read, s.context);
}

// ---------------------------------------------------------------------------
// GPU resource cleanup
// ---------------------------------------------------------------------------

static void releaseGpuResources(void) {
	// Context may already be destroyed during dispose — just null the pointer
	if (blit_fence_) blit_fence_ = NULL;
	if (egl_display_ != EGL_NO_DISPLAY) {
		if (egl_surface_ != EGL_NO_SURFACE) {
			eglDestroySurface(egl_display_, egl_surface_);
			egl_surface_ = EGL_NO_SURFACE;
		}
	}
	egl_display_ = EGL_NO_DISPLAY;
	if (input_surface_) {
		ANativeWindow_release(input_surface_);
		input_surface_ = NULL;
	}
	gpu_mode_ = false;
}

// ---------------------------------------------------------------------------
// BGRA → NV12 conversion
// ---------------------------------------------------------------------------
// NV12 layout: Y plane (w*h bytes), then interleaved UV plane (w*h/2 bytes).
// BT.601 coefficients for SD/HD content.

#ifdef VE_USE_NEON

static void bgraToNV12(const unsigned char* bgra, unsigned char* nv12, int width, int height) {
	const int frameSize = width * height;
	const int stride = width * BYTES_PER_PIXEL;
	unsigned char* yPlane = nv12;
	unsigned char* uvPlane = nv12 + frameSize;

	// NEON coefficient vectors for Y: ((66*R + 129*G + 25*B + 128) >> 8) + 16
	const uint8x8_t coeff_r_y = vdup_n_u8(COEFF_R_Y);
	const uint8x8_t coeff_g_y = vdup_n_u8(COEFF_G_Y);
	const uint8x8_t coeff_b_y = vdup_n_u8(COEFF_B_Y);
	const uint16x8_t bias_y = vdupq_n_u16(ROUNDING_BIAS);
	const uint8x8_t offset_y = vdup_n_u8(Y_OFFSET);

	// Y plane: 16 pixels per NEON iteration
	for (int y = 0; y < height; y++) {
		const unsigned char* row = bgra + y * stride;
		unsigned char* yRow = yPlane + y * width;
		int x = 0;

		for (; x <= width - 16; x += 16) {
			// Deinterleave BGRA: val[0]=B, val[1]=G, val[2]=R, val[3]=A
			uint8x16x4_t px = vld4q_u8(row + x * BYTES_PER_PIXEL);
			uint8x8_t b_lo = vget_low_u8(px.val[0]);
			uint8x8_t b_hi = vget_high_u8(px.val[0]);
			uint8x8_t g_lo = vget_low_u8(px.val[1]);
			uint8x8_t g_hi = vget_high_u8(px.val[1]);
			uint8x8_t r_lo = vget_low_u8(px.val[2]);
			uint8x8_t r_hi = vget_high_u8(px.val[2]);

			// Low 8: 66*R + 129*G + 25*B + 128
			uint16x8_t y_lo = vmull_u8(r_lo, coeff_r_y);
			y_lo = vmlal_u8(y_lo, g_lo, coeff_g_y);
			y_lo = vmlal_u8(y_lo, b_lo, coeff_b_y);
			y_lo = vaddq_u16(y_lo, bias_y);

			// High 8: same
			uint16x8_t y_hi = vmull_u8(r_hi, coeff_r_y);
			y_hi = vmlal_u8(y_hi, g_hi, coeff_g_y);
			y_hi = vmlal_u8(y_hi, b_hi, coeff_b_y);
			y_hi = vaddq_u16(y_hi, bias_y);

			// >>8, narrow to u8, +16
			uint8x8_t res_lo = vadd_u8(vshrn_n_u16(y_lo, 8), offset_y);
			uint8x8_t res_hi = vadd_u8(vshrn_n_u16(y_hi, 8), offset_y);

			vst1q_u8(yRow + x, vcombine_u8(res_lo, res_hi));
		}

		// Scalar tail
		for (; x < width; x++) {
			const int idx = x * BYTES_PER_PIXEL;
			const int b = row[idx];
			const int g = row[idx + 1];
			const int r = row[idx + 2];
			yRow[x] = (unsigned char)(((COEFF_R_Y * r + COEFF_G_Y * g + COEFF_B_Y * b + ROUNDING_BIAS) >> 8) + Y_OFFSET);
		}
	}

	// UV plane: 8 UV pairs per NEON iteration
	const int uvWidth = width / 2;
	const int uvHeight = height / 2;
	const int16x8_t coeff_r_u = vdupq_n_s16(COEFF_R_U);
	const int16x8_t coeff_g_u = vdupq_n_s16(COEFF_G_U);
	const int16x8_t coeff_b_u = vdupq_n_s16(COEFF_B_U);
	const int16x8_t coeff_r_v = vdupq_n_s16(COEFF_R_V);
	const int16x8_t coeff_g_v = vdupq_n_s16(COEFF_G_V);
	const int16x8_t coeff_b_v = vdupq_n_s16(COEFF_B_V);
	const int16x8_t bias_uv = vdupq_n_s16(ROUNDING_BIAS);
	const int16x8_t offset_uv = vdupq_n_s16(UV_OFFSET);

	for (int y = 0; y < uvHeight; y++) {
		const unsigned char* line0 = bgra + (y * 2) * stride;
		const unsigned char* line1 = bgra + (y * 2 + 1) * stride;
		unsigned char* uvRow = uvPlane + y * width;
		int x = 0;

		for (; x <= uvWidth - 8; x += 8) {
			const int srcOff = (x * 2) * BYTES_PER_PIXEL;

			// Load 16 BGRA pixels from each row, deinterleaved
			uint8x16x4_t r0 = vld4q_u8(line0 + srcOff);
			uint8x16x4_t r1 = vld4q_u8(line1 + srcOff);

			// Horizontal pairwise add (adjacent pixels) -> 8x uint16
			uint16x8_t b_sum = vaddq_u16(vpaddlq_u8(r0.val[0]), vpaddlq_u8(r1.val[0]));
			uint16x8_t g_sum = vaddq_u16(vpaddlq_u8(r0.val[1]), vpaddlq_u8(r1.val[1]));
			uint16x8_t r_sum = vaddq_u16(vpaddlq_u8(r0.val[2]), vpaddlq_u8(r1.val[2]));

			// Average: (sum + 2) >> 2
			uint16x8_t b_avg = vshrq_n_u16(vaddq_u16(b_sum, vdupq_n_u16(UV_AVG_BIAS)), 2);
			uint16x8_t g_avg = vshrq_n_u16(vaddq_u16(g_sum, vdupq_n_u16(UV_AVG_BIAS)), 2);
			uint16x8_t r_avg = vshrq_n_u16(vaddq_u16(r_sum, vdupq_n_u16(UV_AVG_BIAS)), 2);

			// Cast to signed for UV math
			int16x8_t r_s = vreinterpretq_s16_u16(r_avg);
			int16x8_t g_s = vreinterpretq_s16_u16(g_avg);
			int16x8_t b_s = vreinterpretq_s16_u16(b_avg);

			// U = ((-38*R - 74*G + 112*B + 128) >> 8) + 128
			int16x8_t u_val = vmulq_s16(r_s, coeff_r_u);
			u_val = vmlaq_s16(u_val, g_s, coeff_g_u);
			u_val = vmlaq_s16(u_val, b_s, coeff_b_u);
			u_val = vshrq_n_s16(vaddq_s16(u_val, bias_uv), 8);
			uint8x8_t u_u8 = vqmovun_s16(vaddq_s16(u_val, offset_uv));

			// V = ((112*R - 94*G - 18*B + 128) >> 8) + 128
			int16x8_t v_val = vmulq_s16(r_s, coeff_r_v);
			v_val = vmlaq_s16(v_val, g_s, coeff_g_v);
			v_val = vmlaq_s16(v_val, b_s, coeff_b_v);
			v_val = vshrq_n_s16(vaddq_s16(v_val, bias_uv), 8);
			uint8x8_t v_u8 = vqmovun_s16(vaddq_s16(v_val, offset_uv));

			// Interleave U/V for NV12: U0 V0 U1 V1 ...
			uint8x8x2_t uv;
			uv.val[0] = u_u8;
			uv.val[1] = v_u8;
			vst2_u8(uvRow + x * 2, uv);
		}

		// Scalar tail
		for (; x < uvWidth; x++) {
			const int col0 = (x * 2) * BYTES_PER_PIXEL;
			const int col1 = (x * 2 + 1) * BYTES_PER_PIXEL;
			const int r = (line0[col0 + 2] + line0[col1 + 2] + line1[col0 + 2] + line1[col1 + 2] + UV_AVG_BIAS) >> 2;
			const int g = (line0[col0 + 1] + line0[col1 + 1] + line1[col0 + 1] + line1[col1 + 1] + UV_AVG_BIAS) >> 2;
			const int b = (line0[col0] + line0[col1] + line1[col0] + line1[col1] + UV_AVG_BIAS) >> 2;
			const int uvIdx = y * width + x * 2;
			uvPlane[uvIdx] = (unsigned char)(((COEFF_R_U * r + COEFF_G_U * g + COEFF_B_U * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
			uvPlane[uvIdx + 1] = (unsigned char)(((COEFF_R_V * r + COEFF_G_V * g + COEFF_B_V * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
		}
	}
}

#else  // !VE_USE_NEON — scalar fallback

static void bgraToNV12(const unsigned char* bgra, unsigned char* nv12, int width, int height) {
	const int frameSize = width * height;
	const int stride = width * BYTES_PER_PIXEL;
	unsigned char* yPlane = nv12;
	unsigned char* uvPlane = nv12 + frameSize;

	for (int y = 0; y < height; y++) {
		const unsigned char* row = bgra + y * stride;
		unsigned char* yRow = yPlane + y * width;
		for (int x = 0; x < width; x++) {
			const int idx = x * BYTES_PER_PIXEL;
			const int b = row[idx];
			const int g = row[idx + 1];
			const int r = row[idx + 2];
			yRow[x] = (unsigned char)(((COEFF_R_Y * r + COEFF_G_Y * g + COEFF_B_Y * b + ROUNDING_BIAS) >> 8) + Y_OFFSET);
		}
	}

	const int uvWidth = width / 2;
	const int uvHeight = height / 2;
	for (int y = 0; y < uvHeight; y++) {
		const unsigned char* line0 = bgra + (y * 2) * stride;
		const unsigned char* line1 = bgra + (y * 2 + 1) * stride;
		for (int x = 0; x < uvWidth; x++) {
			const int col0 = (x * 2) * BYTES_PER_PIXEL;
			const int col1 = (x * 2 + 1) * BYTES_PER_PIXEL;
			const int r = (line0[col0 + 2] + line0[col1 + 2] + line1[col0 + 2] + line1[col1 + 2] + UV_AVG_BIAS) >> 2;
			const int g = (line0[col0 + 1] + line0[col1 + 1] + line1[col0 + 1] + line1[col1 + 1] + UV_AVG_BIAS) >> 2;
			const int b = (line0[col0] + line0[col1] + line1[col0] + line1[col1] + UV_AVG_BIAS) >> 2;
			const int uvIdx = y * width + x * 2;
			uvPlane[uvIdx] = (unsigned char)(((COEFF_R_U * r + COEFF_G_U * g + COEFF_B_U * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
			uvPlane[uvIdx + 1] = (unsigned char)(((COEFF_R_V * r + COEFF_G_V * g + COEFF_B_V * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
		}
	}
}

#endif	// VE_USE_NEON

// ---------------------------------------------------------------------------
// Drain encoder output — process all available output buffers
// ---------------------------------------------------------------------------

static const int MAX_DRAIN_RETRIES = 100;  // Safety limit for EOS drain

static int drainEncoder(bool endOfStream) {
	if (!codec_) return -1;

	int retries = 0;
	while (1) {
		AMediaCodecBufferInfo info;
		ssize_t outputIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, TIMEOUT_US);

		if (outputIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
			if (!endOfStream) break;
			if (++retries > MAX_DRAIN_RETRIES) {
				setError("EOS drain timeout after %d retries", MAX_DRAIN_RETRIES);
				return -1;
			}
			continue;
		}
		retries = 0;

		if (outputIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			if (muxer_started_) {
				setError("Output format changed after muxer started");
				return -1;
			}
			AMediaFormat* newFormat = AMediaCodec_getOutputFormat(codec_);
			muxer_track_index_ = AMediaMuxer_addTrack(muxer_, newFormat);
			AMediaFormat_delete(newFormat);
			if (muxer_track_index_ < 0) {
				setError("AMediaMuxer_addTrack failed");
				return -1;
			}
			media_status_t st = AMediaMuxer_start(muxer_);
			if (st != AMEDIA_OK) {
				setError("AMediaMuxer_start failed: %d", (int)st);
				return -1;
			}
			muxer_started_ = true;
			continue;
		}

		if (outputIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
			continue;
		}

		if (outputIdx < 0) {
			setError("dequeueOutputBuffer error: %zd", outputIdx);
			return -1;
		}

		// Valid output buffer
		if (info.size > 0 && muxer_started_) {
			size_t outSize = 0;
			uint8_t* outBuf = AMediaCodec_getOutputBuffer(codec_, (size_t)outputIdx, &outSize);
			if (outBuf) {
				AMediaCodecBufferInfo muxInfo;
				muxInfo.offset = 0;	 // Already offset by info.offset in the pointer
				muxInfo.size = info.size;
				muxInfo.presentationTimeUs = info.presentationTimeUs;
				muxInfo.flags = info.flags;
				const media_status_t st = AMediaMuxer_writeSampleData(muxer_, muxer_track_index_, outBuf + info.offset, &muxInfo);
				if (st != AMEDIA_OK) {
					AMediaCodec_releaseOutputBuffer(codec_, (size_t)outputIdx, false);
					setError("AMediaMuxer_writeSampleData failed: %d", (int)st);
					return -1;
				}
			}
		}

		AMediaCodec_releaseOutputBuffer(codec_, (size_t)outputIdx, false);

		if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

static void releaseResources(void);

extern "C" {

int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
		setError("Invalid encoder parameters");
		return -1;
	}

	// Open output file descriptor for muxer
	muxer_fd_ = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (muxer_fd_ < 0) {
		setError("Cannot open output file: %s", outputPath);
		return -1;
	}

	// Create muxer
	muxer_ = AMediaMuxer_new(muxer_fd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
	if (!muxer_) {
		setError("AMediaMuxer_new failed");
		releaseResources();
		return -1;
	}

	// Create H.264 encoder
	codec_ = AMediaCodec_createEncoderByType(MIME_H264);
	if (!codec_) {
		setError("AMediaCodec_createEncoderByType failed for video/avc");
		releaseResources();
		return -1;
	}

	// Configure encoder
	AMediaFormat* format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, MIME_H264);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
	AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, (float)fps);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FORMAT_NV12);

	media_status_t status = AMediaCodec_configure(codec_, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	AMediaFormat_delete(format);
	if (status != AMEDIA_OK) {
		setError("AMediaCodec_configure failed: %d", (int)status);
		releaseResources();
		return -1;
	}

	status = AMediaCodec_start(codec_);
	if (status != AMEDIA_OK) {
		setError("AMediaCodec_start failed: %d", (int)status);
		releaseResources();
		return -1;
	}

	width_ = width;
	height_ = height;
	fps_ = fps;
	frame_index_ = 0;
	muxer_track_index_ = -1;
	muxer_started_ = false;

	return 0;
}

int videoEncoderAddFrame(const unsigned char* bgraPixels, int dataLength) {
	clearError();

	if (!codec_ || !muxer_) {
		setError("Encoder not initialized");
		return -1;
	}

	int expectedLength = width_ * height_ * BYTES_PER_PIXEL;
	if (dataLength != expectedLength) {
		setError("Data length mismatch: %d != %d", dataLength, expectedLength);
		return -1;
	}

	// Dequeue input buffer
	ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(codec_, INPUT_TIMEOUT_US);
	if (inputIdx < 0) {
		setError("dequeueInputBuffer timeout");
		return -1;
	}

	size_t inputSize = 0;
	uint8_t* inputBuf = AMediaCodec_getInputBuffer(codec_, (size_t)inputIdx, &inputSize);
	if (!inputBuf) {
		setError("getInputBuffer returned NULL");
		AMediaCodec_queueInputBuffer(codec_, (size_t)inputIdx, 0, 0, 0, 0);
		return -1;
	}

	// NV12 size = width * height * 3 / 2
	int nv12Size = width_ * height_ * 3 / 2;
	if ((int)inputSize < nv12Size) {
		setError("Input buffer too small: %zu < %d", inputSize, nv12Size);
		AMediaCodec_queueInputBuffer(codec_, (size_t)inputIdx, 0, 0, 0, 0);
		return -1;
	}

	// Convert BGRA → NV12
	bgraToNV12(bgraPixels, inputBuf, width_, height_);

	int64_t presentationTimeUs = (int64_t)frame_index_ * 1000000LL / fps_;
	media_status_t status = AMediaCodec_queueInputBuffer(codec_, (size_t)inputIdx, 0, nv12Size, presentationTimeUs, 0);
	if (status != AMEDIA_OK) {
		setError("queueInputBuffer failed: %d", (int)status);
		return -1;
	}

	frame_index_++;

	// Drain available output
	if (drainEncoder(false) != 0) return -1;

	return 0;
}

int videoEncoderFinish(void) {
	clearError();

	if (!codec_ || !muxer_) {
		setError("Encoder not initialized");
		return -1;
	}

	// Signal end of stream
	if (gpu_mode_) {
		const media_status_t eos_status = AMediaCodec_signalEndOfInputStream(codec_);
		if (eos_status != AMEDIA_OK) {
			setError("AMediaCodec_signalEndOfInputStream failed: %d", (int)eos_status);
			return -1;
		}
	} else {
		ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(codec_, INPUT_TIMEOUT_US);
		if (inputIdx >= 0) AMediaCodec_queueInputBuffer(codec_, (size_t)inputIdx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
	}

	// Drain all remaining output
	if (drainEncoder(true) != 0) return -1;

	// Stop muxer
	if (muxer_started_) {
		AMediaMuxer_stop(muxer_);
		muxer_started_ = false;
	}

	return 0;
}

static void releaseResources(void) {
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
		codec_ = NULL;
	}
	if (muxer_) {
		AMediaMuxer_delete(muxer_);
		muxer_ = NULL;
	}
	if (muxer_fd_ >= 0) {
		close(muxer_fd_);
		muxer_fd_ = -1;
	}

	width_ = 0;
	height_ = 0;
	fps_ = 0;
	frame_index_ = 0;
	muxer_track_index_ = -1;
	muxer_started_ = false;
	gpu_mode_ = false;
}

void videoEncoderDispose(void) {
	if (gpu_mode_) releaseGpuResources();
	releaseResources();
	clearError();
}

const char* videoEncoderGetError(void) {
	return error_buf_[0] != '\0' ? error_buf_ : NULL;
}

int videoEncoderSupportsGpuInput(void) {
	return 1;
}

int videoEncoderInitGpu(const char* outputPath, int width, int height, int fps, int bitrate) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
		setError("Invalid encoder parameters");
		return -1;
	}

	// Open output file descriptor for muxer
	muxer_fd_ = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (muxer_fd_ < 0) {
		setError("Cannot open output file: %s", outputPath);
		return -1;
	}

	// Create muxer
	muxer_ = AMediaMuxer_new(muxer_fd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
	if (!muxer_) {
		setError("AMediaMuxer_new failed");
		releaseResources();
		return -1;
	}

	// Create H.264 encoder
	codec_ = AMediaCodec_createEncoderByType(MIME_H264);
	if (!codec_) {
		setError("AMediaCodec_createEncoderByType failed for video/avc");
		releaseResources();
		return -1;
	}

	// Configure encoder with surface input (no color conversion needed)
	AMediaFormat* format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, MIME_H264);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
	AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, (float)fps);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FORMAT_SURFACE);

	media_status_t status = AMediaCodec_configure(codec_, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	AMediaFormat_delete(format);
	if (status != AMEDIA_OK) {
		setError("AMediaCodec_configure failed: %d", (int)status);
		releaseResources();
		return -1;
	}

	// Get input surface from codec (before start)
	status = AMediaCodec_createInputSurface(codec_, &input_surface_);
	if (status != AMEDIA_OK || !input_surface_) {
		setError("AMediaCodec_createInputSurface failed: %d", (int)status);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	status = AMediaCodec_start(codec_);
	if (status != AMEDIA_OK) {
		setError("AMediaCodec_start failed: %d", (int)status);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Create EGL window surface over codec's input ANativeWindow.
	// We use the caller's EGL context (OpenFL) for blit/swap — no shared context needed.
	// This avoids FBO sharing issues (FBOs are per-context, not shared).
	egl_display_ = eglGetCurrentDisplay();
	const EGLContext caller_ctx = eglGetCurrentContext();

	if (egl_display_ == EGL_NO_DISPLAY || caller_ctx == EGL_NO_CONTEXT) {
		setError("No current EGL display/context — call from GL thread");
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Get config from caller's context — ensures surface compatibility
	EGLint config_id = 0;
	eglQueryContext(egl_display_, caller_ctx, EGL_CONFIG_ID, &config_id);

	const EGLint config_attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
	EGLConfig config = NULL;
	EGLint num_configs = 0;
	eglChooseConfig(egl_display_, config_attribs, &config, 1, &num_configs);
	if (num_configs == 0) {
		setError("eglChooseConfig failed for config_id %d", config_id);
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Create window surface over the codec's input ANativeWindow
	egl_surface_ = eglCreateWindowSurface(egl_display_, config, input_surface_, NULL);
	if (egl_surface_ == EGL_NO_SURFACE) {
		setError("eglCreateWindowSurface failed: 0x%x", eglGetError());
		releaseGpuResources();
		releaseResources();
		return -1;
	}

	// Resolve eglPresentationTimeANDROID for frame timestamps
	eglPresentationTimeANDROID_ = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");

	gpu_mode_ = true;
	width_ = width;
	height_ = height;
	fps_ = fps;
	frame_index_ = 0;
	muxer_track_index_ = -1;
	muxer_started_ = false;

	return 0;
}

unsigned int videoEncoderGetSurfaceId(void) {
	return 0;
}

int videoEncoderSubmitGpuFrame(void) {
	clearError();

	if (!codec_ || !gpu_mode_) {
		setError("GPU encoder not initialized");
		return -1;
	}

	// Use caller's context with encoder surface — fence was created in this context
	const EglState saved = saveEglState();
	eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, saved.context);

	// Wait for blit to complete
	if (blit_fence_) {
		glClientWaitSync(blit_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
		glDeleteSync(blit_fence_);
		blit_fence_ = NULL;
	}

	// Set presentation timestamp (nanoseconds) before swap
	if (eglPresentationTimeANDROID_) {
		const EGLnsecsANDROID timestamp_ns = (EGLnsecsANDROID)frame_index_ * 1000000000LL / fps_;
		eglPresentationTimeANDROID_(egl_display_, egl_surface_, timestamp_ns);
	}

	// Submit frame to encoder via eglSwapBuffers
	eglSwapBuffers(egl_display_, egl_surface_);

	restoreEglState(saved);

	frame_index_++;

	if (drainEncoder(false) != 0) return -1;

	return 0;
}

int videoEncoderSetupIoSurfaceFbo(int width, int height) {
	clearError();
	if (!gpu_mode_) {
		setError("GPU encoder not initialized");
		return -1;
	}
	// No FBO needed — blit goes to default framebuffer of encoder surface
	(void)width;
	(void)height;
	return 0;
}

void videoEncoderBlitToIoSurface(unsigned int srcFbo, int width, int height) {
	// Use caller's context with encoder surface — srcFbo is valid in caller's context
	const EglState saved = saveEglState();
	eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, saved.context);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFlush();

	// Fence to ensure blit completes before submitGpuFrame swaps
	if (blit_fence_) glDeleteSync(blit_fence_);
	blit_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

	restoreEglState(saved);
}

void videoEncoderDisposeIoSurfaceFbo(void) {
	releaseGpuResources();
}

}  // extern "C"

#endif	// __ANDROID__
