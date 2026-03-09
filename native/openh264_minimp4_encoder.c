/**
 * Video encoder: OpenH264 (H.264 software encoder) + minimp4 (MP4 muxer).
 * Used on Linux where no platform-native encoder API is available.
 *
 * Single-instance, single-threaded design. All state is held in static globals.
 * Input: BGRA pixel data. Output: H.264/MP4 file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
#define VE_USE_SSE2 1
#include <emmintrin.h>
#endif

#include "wels/codec_api.h"

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

// ---------------------------------------------------------------------------
// Error buffer
// ---------------------------------------------------------------------------

#define ERROR_BUF_SIZE 512

/* BT.601 color conversion coefficients */
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

static char error_buf_[ERROR_BUF_SIZE] = {0};

static void setError(const char* msg) {
	strncpy(error_buf_, msg, ERROR_BUF_SIZE - 1);
	error_buf_[ERROR_BUF_SIZE - 1] = '\0';
}

static void clearError(void) {
	error_buf_[0] = '\0';
}

// ---------------------------------------------------------------------------
// Encoder state
// ---------------------------------------------------------------------------

static ISVCEncoder* encoder_ = NULL;
static FILE* output_file_ = NULL;
static MP4E_mux_t* mux_ = NULL;
static mp4_h26x_writer_t mp4_writer_;
static unsigned char* i420_buf_ = NULL;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;

// ---------------------------------------------------------------------------
// minimp4 write callback
// ---------------------------------------------------------------------------

static int mp4WriteCallback(int64_t offset, const void* buffer, size_t size, void* token) {
	FILE* f = (FILE*)token;
	if (fseek(f, (long)offset, SEEK_SET)) return -1;
	return fwrite(buffer, 1, size, f) != size ? -1 : 0;
}

// ---------------------------------------------------------------------------
// BGRA → I420 conversion (BT.601)
// ---------------------------------------------------------------------------

#ifdef VE_USE_SSE2

static void bgraToI420(const unsigned char* bgra, int width, int height, unsigned char* i420) {
	const int yPlaneSize = width * height;
	const int uvWidth = width / 2;
	const int uvHeight = height / 2;

	unsigned char* yPlane = i420;
	unsigned char* uPlane = i420 + yPlaneSize;
	unsigned char* vPlane = uPlane + uvWidth * uvHeight;

	/* Channel extraction masks for packed BGRA dwords */
	const __m128i mask_b = _mm_set1_epi32(0x000000FF);
	const __m128i mask_g = _mm_set1_epi32(0x0000FF00);
	const __m128i mask_r = _mm_set1_epi32(0x00FF0000);

	/* BT.601 Y coefficients */
	const __m128i coeff_r_y = _mm_set1_epi16(COEFF_R_Y);
	const __m128i coeff_g_y = _mm_set1_epi16(COEFF_G_Y);
	const __m128i coeff_b_y = _mm_set1_epi16(COEFF_B_Y);
	const __m128i bias_y = _mm_set1_epi16(ROUNDING_BIAS);
	const __m128i offset_y = _mm_set1_epi16(Y_OFFSET);

	/* Y plane: 8 pixels per SSE2 iteration */
	int row, col;
	for (row = 0; row < height; row++) {
		const unsigned char* srcRow = bgra + row * width * 4;
		unsigned char* yRow = yPlane + row * width;
		col = 0;

		for (; col <= width - 8; col += 8) {
			/* Load 4+4 BGRA pixels */
			__m128i px0 = _mm_loadu_si128((const __m128i*)(srcRow + col * 4));
			__m128i px1 = _mm_loadu_si128((const __m128i*)(srcRow + (col + 4) * 4));

			/* Extract B, G, R channels to int16 */
			__m128i b_16 = _mm_packs_epi32(_mm_and_si128(px0, mask_b), _mm_and_si128(px1, mask_b));
			__m128i g_16 = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(px0, mask_g), 8), _mm_srli_epi32(_mm_and_si128(px1, mask_g), 8));
			__m128i r_16 = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(px0, mask_r), 16), _mm_srli_epi32(_mm_and_si128(px1, mask_r), 16));

			/* Y = ((66*R + 129*G + 25*B + 128) >> 8) + 16
			 * Note: 129*255=32895 > int16 max, but mullo gives correct low 16 bits.
			 * Sum max=56228 fits uint16. Use logical shift (srli) not arithmetic. */
			__m128i y_val = _mm_mullo_epi16(r_16, coeff_r_y);
			y_val = _mm_add_epi16(y_val, _mm_mullo_epi16(g_16, coeff_g_y));
			y_val = _mm_add_epi16(y_val, _mm_mullo_epi16(b_16, coeff_b_y));
			y_val = _mm_add_epi16(y_val, bias_y);
			y_val = _mm_srli_epi16(y_val, 8);
			y_val = _mm_add_epi16(y_val, offset_y);

			/* Pack to uint8 and store 8 Y bytes */
			_mm_storel_epi64((__m128i*)(yRow + col), _mm_packus_epi16(y_val, _mm_setzero_si128()));
		}

		for (; col < width; col++) {
			const int idx = col * 4;
			const int b = srcRow[idx];
			const int g = srcRow[idx + 1];
			const int r = srcRow[idx + 2];
			yRow[col] = (unsigned char)(((COEFF_R_Y * r + COEFF_G_Y * g + COEFF_B_Y * b + ROUNDING_BIAS) >> 8) + Y_OFFSET);
		}
	}

	/* UV plane: 8 UV pairs per SSE2 iteration */
	const __m128i coeff_r_u = _mm_set1_epi16(COEFF_R_U);
	const __m128i coeff_g_u = _mm_set1_epi16(COEFF_G_U);
	const __m128i coeff_b_u = _mm_set1_epi16(COEFF_B_U);
	const __m128i coeff_r_v = _mm_set1_epi16(COEFF_R_V);
	const __m128i coeff_g_v = _mm_set1_epi16(COEFF_G_V);
	const __m128i coeff_b_v = _mm_set1_epi16(COEFF_B_V);
	const __m128i bias_uv = _mm_set1_epi16(ROUNDING_BIAS);
	const __m128i offset_uv = _mm_set1_epi16(UV_OFFSET);
	const __m128i ones = _mm_set1_epi16(1);
	const __m128i two_32 = _mm_set1_epi32(UV_AVG_BIAS);

	for (row = 0; row < uvHeight; row++) {
		const unsigned char* line0 = bgra + (row * 2) * width * 4;
		const unsigned char* line1 = bgra + (row * 2 + 1) * width * 4;
		unsigned char* uRow = uPlane + row * uvWidth;
		unsigned char* vRow = vPlane + row * uvWidth;
		col = 0;

		/* 8 UV pairs = 16 source pixel columns = 4 loads per row */
		for (; col <= uvWidth - 8; col += 8) {
			const int srcOff = (col * 2) * 4;

			/* Load 16 BGRA pixels from each row (4 groups of 4) */
			__m128i r0a = _mm_loadu_si128((const __m128i*)(line0 + srcOff));
			__m128i r0b = _mm_loadu_si128((const __m128i*)(line0 + srcOff + 16));
			__m128i r0c = _mm_loadu_si128((const __m128i*)(line0 + srcOff + 32));
			__m128i r0d = _mm_loadu_si128((const __m128i*)(line0 + srcOff + 48));
			__m128i r1a = _mm_loadu_si128((const __m128i*)(line1 + srcOff));
			__m128i r1b = _mm_loadu_si128((const __m128i*)(line1 + srcOff + 16));
			__m128i r1c = _mm_loadu_si128((const __m128i*)(line1 + srcOff + 32));
			__m128i r1d = _mm_loadu_si128((const __m128i*)(line1 + srcOff + 48));

			/* Extract channels to int16, pack pairs of 4-pixel groups */
			__m128i b_r0_lo = _mm_packs_epi32(_mm_and_si128(r0a, mask_b), _mm_and_si128(r0b, mask_b));
			__m128i b_r0_hi = _mm_packs_epi32(_mm_and_si128(r0c, mask_b), _mm_and_si128(r0d, mask_b));
			__m128i g_r0_lo = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r0a, mask_g), 8), _mm_srli_epi32(_mm_and_si128(r0b, mask_g), 8));
			__m128i g_r0_hi = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r0c, mask_g), 8), _mm_srli_epi32(_mm_and_si128(r0d, mask_g), 8));
			__m128i r_r0_lo =
				_mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r0a, mask_r), 16), _mm_srli_epi32(_mm_and_si128(r0b, mask_r), 16));
			__m128i r_r0_hi =
				_mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r0c, mask_r), 16), _mm_srli_epi32(_mm_and_si128(r0d, mask_r), 16));

			__m128i b_r1_lo = _mm_packs_epi32(_mm_and_si128(r1a, mask_b), _mm_and_si128(r1b, mask_b));
			__m128i b_r1_hi = _mm_packs_epi32(_mm_and_si128(r1c, mask_b), _mm_and_si128(r1d, mask_b));
			__m128i g_r1_lo = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r1a, mask_g), 8), _mm_srli_epi32(_mm_and_si128(r1b, mask_g), 8));
			__m128i g_r1_hi = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r1c, mask_g), 8), _mm_srli_epi32(_mm_and_si128(r1d, mask_g), 8));
			__m128i r_r1_lo =
				_mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r1a, mask_r), 16), _mm_srli_epi32(_mm_and_si128(r1b, mask_r), 16));
			__m128i r_r1_hi =
				_mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(r1c, mask_r), 16), _mm_srli_epi32(_mm_and_si128(r1d, mask_r), 16));

			/* Horizontal pairwise add via madd trick: a[0]+a[1], a[2]+a[3], ... */
			__m128i b_sum_lo = _mm_add_epi32(_mm_madd_epi16(b_r0_lo, ones), _mm_madd_epi16(b_r1_lo, ones));
			__m128i b_sum_hi = _mm_add_epi32(_mm_madd_epi16(b_r0_hi, ones), _mm_madd_epi16(b_r1_hi, ones));
			__m128i g_sum_lo = _mm_add_epi32(_mm_madd_epi16(g_r0_lo, ones), _mm_madd_epi16(g_r1_lo, ones));
			__m128i g_sum_hi = _mm_add_epi32(_mm_madd_epi16(g_r0_hi, ones), _mm_madd_epi16(g_r1_hi, ones));
			__m128i r_sum_lo = _mm_add_epi32(_mm_madd_epi16(r_r0_lo, ones), _mm_madd_epi16(r_r1_lo, ones));
			__m128i r_sum_hi = _mm_add_epi32(_mm_madd_epi16(r_r0_hi, ones), _mm_madd_epi16(r_r1_hi, ones));

			/* Average: (sum + 2) >> 2, pack back to int16 */
			__m128i b_avg =
				_mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(b_sum_lo, two_32), 2), _mm_srai_epi32(_mm_add_epi32(b_sum_hi, two_32), 2));
			__m128i g_avg =
				_mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(g_sum_lo, two_32), 2), _mm_srai_epi32(_mm_add_epi32(g_sum_hi, two_32), 2));
			__m128i r_avg =
				_mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(r_sum_lo, two_32), 2), _mm_srai_epi32(_mm_add_epi32(r_sum_hi, two_32), 2));

			/* U = ((-38*R - 74*G + 112*B + 128) >> 8) + 128 */
			__m128i u_val = _mm_mullo_epi16(r_avg, coeff_r_u);
			u_val = _mm_add_epi16(u_val, _mm_mullo_epi16(g_avg, coeff_g_u));
			u_val = _mm_add_epi16(u_val, _mm_mullo_epi16(b_avg, coeff_b_u));
			u_val = _mm_add_epi16(u_val, bias_uv);
			u_val = _mm_srai_epi16(u_val, 8);
			u_val = _mm_add_epi16(u_val, offset_uv);

			/* V = ((112*R - 94*G - 18*B + 128) >> 8) + 128 */
			__m128i v_val = _mm_mullo_epi16(r_avg, coeff_r_v);
			v_val = _mm_add_epi16(v_val, _mm_mullo_epi16(g_avg, coeff_g_v));
			v_val = _mm_add_epi16(v_val, _mm_mullo_epi16(b_avg, coeff_b_v));
			v_val = _mm_add_epi16(v_val, bias_uv);
			v_val = _mm_srai_epi16(v_val, 8);
			v_val = _mm_add_epi16(v_val, offset_uv);

			/* Pack to uint8, store 8 U + 8 V bytes (planar I420) */
			_mm_storel_epi64((__m128i*)(uRow + col), _mm_packus_epi16(u_val, _mm_setzero_si128()));
			_mm_storel_epi64((__m128i*)(vRow + col), _mm_packus_epi16(v_val, _mm_setzero_si128()));
		}

		for (; col < uvWidth; col++) {
			const int off0 = (col * 2) * 4;
			const int off1 = (col * 2 + 1) * 4;
			const int r = (line0[off0 + 2] + line0[off1 + 2] + line1[off0 + 2] + line1[off1 + 2] + UV_AVG_BIAS) >> 2;
			const int g = (line0[off0 + 1] + line0[off1 + 1] + line1[off0 + 1] + line1[off1 + 1] + UV_AVG_BIAS) >> 2;
			const int b = (line0[off0] + line0[off1] + line1[off0] + line1[off1] + UV_AVG_BIAS) >> 2;
			uRow[col] = (unsigned char)(((COEFF_R_U * r + COEFF_G_U * g + COEFF_B_U * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
			vRow[col] = (unsigned char)(((COEFF_R_V * r + COEFF_G_V * g + COEFF_B_V * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
		}
	}
}

#else /* !VE_USE_SSE2 — scalar fallback */

static void bgraToI420(const unsigned char* bgra, int width, int height, unsigned char* i420) {
	const int yPlaneSize = width * height;
	const int uvWidth = width / 2;
	const int uvHeight = height / 2;

	unsigned char* yPlane = i420;
	unsigned char* uPlane = i420 + yPlaneSize;
	unsigned char* vPlane = uPlane + uvWidth * uvHeight;

	int row, col;
	for (row = 0; row < height; row++) {
		const unsigned char* srcRow = bgra + row * width * 4;
		unsigned char* yRow = yPlane + row * width;
		for (col = 0; col < width; col++) {
			const int idx = col * 4;
			const int b = srcRow[idx];
			const int g = srcRow[idx + 1];
			const int r = srcRow[idx + 2];
			yRow[col] = (unsigned char)(((COEFF_R_Y * r + COEFF_G_Y * g + COEFF_B_Y * b + ROUNDING_BIAS) >> 8) + Y_OFFSET);
		}
	}

	for (row = 0; row < uvHeight; row++) {
		const unsigned char* line0 = bgra + (row * 2) * width * 4;
		const unsigned char* line1 = bgra + (row * 2 + 1) * width * 4;
		unsigned char* uRow = uPlane + row * uvWidth;
		unsigned char* vRow = vPlane + row * uvWidth;

		for (col = 0; col < uvWidth; col++) {
			const int off0 = (col * 2) * 4;
			const int off1 = (col * 2 + 1) * 4;
			const int r = (line0[off0 + 2] + line0[off1 + 2] + line1[off0 + 2] + line1[off1 + 2] + UV_AVG_BIAS) >> 2;
			const int g = (line0[off0 + 1] + line0[off1 + 1] + line1[off0 + 1] + line1[off1 + 1] + UV_AVG_BIAS) >> 2;
			const int b = (line0[off0] + line0[off1] + line1[off0] + line1[off1] + UV_AVG_BIAS) >> 2;
			uRow[col] = (unsigned char)(((COEFF_R_U * r + COEFF_G_U * g + COEFF_B_U * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
			vRow[col] = (unsigned char)(((COEFF_R_V * r + COEFF_G_V * g + COEFF_B_V * b + ROUNDING_BIAS) >> 8) + UV_OFFSET);
		}
	}
}

#endif /* VE_USE_SSE2 */

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int videoEncoderInit(const char* outputPath, int width, int height, int fps, int bitrate) {
	clearError();

	if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
		setError("Invalid encoder parameters");
		return -1;
	}

	if (width % 2 != 0 || height % 2 != 0) {
		setError("Width and height must be even for I420 conversion");
		return -1;
	}

	// Open output file
	output_file_ = fopen(outputPath, "wb");
	if (!output_file_) {
		setError("Failed to open output file");
		return -1;
	}

	// Create OpenH264 encoder
	if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
		setError("Failed to create OpenH264 encoder");
		fclose(output_file_);
		output_file_ = NULL;
		return -1;
	}

	// Configure encoder
	SEncParamBase param;
	memset(&param, 0, sizeof(SEncParamBase));
	param.iUsageType = CAMERA_VIDEO_REAL_TIME;
	param.iPicWidth = width;
	param.iPicHeight = height;
	param.iTargetBitrate = bitrate;
	param.fMaxFrameRate = (float)fps;

	if ((*encoder_)->Initialize(encoder_, &param) != 0) {
		setError("Failed to initialize OpenH264 encoder");
		WelsDestroySVCEncoder(encoder_);
		encoder_ = NULL;
		fclose(output_file_);
		output_file_ = NULL;
		return -1;
	}

	// Initialize MP4 muxer
	mux_ = MP4E_open(0, 0, output_file_, mp4WriteCallback);
	if (!mux_) {
		setError("Failed to initialize MP4 muxer");
		(*encoder_)->Uninitialize(encoder_);
		WelsDestroySVCEncoder(encoder_);
		encoder_ = NULL;
		fclose(output_file_);
		output_file_ = NULL;
		return -1;
	}

	// Initialize H.264 NAL writer (is_hevc = 0)
	if (mp4_h26x_write_init(&mp4_writer_, mux_, width, height, 0) != MP4E_STATUS_OK) {
		setError("Failed to initialize MP4 H.264 writer");
		MP4E_close(mux_);
		mux_ = NULL;
		(*encoder_)->Uninitialize(encoder_);
		WelsDestroySVCEncoder(encoder_);
		encoder_ = NULL;
		fclose(output_file_);
		output_file_ = NULL;
		return -1;
	}

	// Allocate I420 conversion buffer
	const int i420Size = width * height * 3 / 2;
	i420_buf_ = (unsigned char*)malloc(i420Size);
	if (!i420_buf_) {
		setError("Failed to allocate I420 buffer");
		mp4_h26x_write_close(&mp4_writer_);
		MP4E_close(mux_);
		mux_ = NULL;
		(*encoder_)->Uninitialize(encoder_);
		WelsDestroySVCEncoder(encoder_);
		encoder_ = NULL;
		fclose(output_file_);
		output_file_ = NULL;
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

	if (!encoder_ || !mux_ || !i420_buf_) {
		setError("Encoder not initialized");
		return -1;
	}

	const int expectedLength = width_ * height_ * 4;
	if (dataLength != expectedLength) {
		setError("Frame data length mismatch");
		return -1;
	}

	// BGRA → I420
	bgraToI420(bgraPixels, width_, height_, i420_buf_);

	// Set up source picture
	SSourcePicture srcPic;
	memset(&srcPic, 0, sizeof(SSourcePicture));
	srcPic.iColorFormat = videoFormatI420;
	srcPic.iPicWidth = width_;
	srcPic.iPicHeight = height_;
	srcPic.iStride[0] = width_;		 // Y stride
	srcPic.iStride[1] = width_ / 2;	 // U stride
	srcPic.iStride[2] = width_ / 2;	 // V stride
	srcPic.pData[0] = i420_buf_;
	srcPic.pData[1] = i420_buf_ + width_ * height_;
	srcPic.pData[2] = i420_buf_ + width_ * height_ + (width_ / 2) * (height_ / 2);
	srcPic.uiTimeStamp = (long long)frame_index_ * 1000 / fps_;

	// Encode
	SFrameBSInfo bsInfo;
	memset(&bsInfo, 0, sizeof(SFrameBSInfo));

	int encResult = (*encoder_)->EncodeFrame(encoder_, &srcPic, &bsInfo);
	if (encResult != 0) {
		setError("EncodeFrame failed");
		return -1;
	}

	// Write encoded NALs to MP4
	if (bsInfo.eFrameType != videoFrameTypeSkip) {
		// Timestamp in 90kHz units for next frame
		const unsigned nextTimestamp90k = (unsigned)((long long)(frame_index_ + 1) * 90000 / fps_);

		for (int layer = 0; layer < bsInfo.iLayerNum; layer++) {
			const SLayerBSInfo* layerInfo = &bsInfo.sLayerInfo[layer];
			unsigned char* nalData = layerInfo->pBsBuf;

			for (int nal = 0; nal < layerInfo->iNalCount; nal++) {
				if (mp4_h26x_write_nal(&mp4_writer_, nalData, layerInfo->pNalLengthInByte[nal], nextTimestamp90k) != MP4E_STATUS_OK) {
					setError("Failed to write NAL to MP4");
					return -1;
				}
				nalData += layerInfo->pNalLengthInByte[nal];
			}
		}
	}

	frame_index_++;
	return 0;
}

int videoEncoderFinish(void) {
	clearError();

	if (!encoder_ || !mux_) {
		setError("Encoder not initialized");
		return -1;
	}

	// OpenH264 in CAMERA_VIDEO_REAL_TIME mode encodes synchronously —
	// no buffered frames to flush. Close MP4 writer and muxer.
	mp4_h26x_write_close(&mp4_writer_);
	MP4E_close(mux_);
	mux_ = NULL;

	// Close output file
	if (output_file_) {
		fclose(output_file_);
		output_file_ = NULL;
	}

	return 0;
}

void videoEncoderDispose(void) {
	if (encoder_) {
		(*encoder_)->Uninitialize(encoder_);
		WelsDestroySVCEncoder(encoder_);
		encoder_ = NULL;
	}

	if (i420_buf_) {
		free(i420_buf_);
		i420_buf_ = NULL;
	}

	// Close muxer before file — MP4E_close flushes via write callback
	if (mux_) {
		mp4_h26x_write_close(&mp4_writer_);
		MP4E_close(mux_);
		mux_ = NULL;
	}

	// Close file after muxer is done writing
	if (output_file_) {
		fclose(output_file_);
		output_file_ = NULL;
	}

	width_ = 0;
	height_ = 0;
	fps_ = 0;
	frame_index_ = 0;

	clearError();
}

const char* videoEncoderGetError(void) {
	return error_buf_[0] != '\0' ? error_buf_ : NULL;
}
