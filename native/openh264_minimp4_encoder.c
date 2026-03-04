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

#include "wels/codec_api.h"

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

// ---------------------------------------------------------------------------
// Error buffer
// ---------------------------------------------------------------------------

#define ERROR_BUF_SIZE 512

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

static void bgraToI420(const unsigned char* bgra, int width, int height, unsigned char* i420) {
	const int yPlaneSize = width * height;
	const int uvWidth = width / 2;
	const int uvHeight = height / 2;

	unsigned char* yPlane = i420;
	unsigned char* uPlane = i420 + yPlaneSize;
	unsigned char* vPlane = uPlane + uvWidth * uvHeight;

	// Compute Y for every pixel
	for (int row = 0; row < height; row++) {
		const unsigned char* srcRow = bgra + row * width * 4;
		unsigned char* yRow = yPlane + row * width;
		for (int col = 0; col < width; col++) {
			const int idx = col * 4;
			const int b = srcRow[idx];
			const int g = srcRow[idx + 1];
			const int r = srcRow[idx + 2];
			yRow[col] = (unsigned char)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
	}

	// Compute U and V with 2x2 subsampling
	for (int row = 0; row < uvHeight; row++) {
		const int srcRow0 = row * 2;
		const int srcRow1 = srcRow0 + 1;
		const unsigned char* line0 = bgra + srcRow0 * width * 4;
		const unsigned char* line1 = bgra + srcRow1 * width * 4;
		unsigned char* uRow = uPlane + row * uvWidth;
		unsigned char* vRow = vPlane + row * uvWidth;

		for (int col = 0; col < uvWidth; col++) {
			const int col0 = col * 2;
			const int col1 = col0 + 1;

			// Average 2x2 block
			const int off0 = col0 * 4;
			const int off1 = col1 * 4;

			const int r = (line0[off0 + 2] + line0[off1 + 2] + line1[off0 + 2] + line1[off1 + 2] + 2) >> 2;
			const int g = (line0[off0 + 1] + line0[off1 + 1] + line1[off0 + 1] + line1[off1 + 1] + 2) >> 2;
			const int b = (line0[off0] + line0[off1] + line1[off0] + line1[off1] + 2) >> 2;

			uRow[col] = (unsigned char)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			vRow[col] = (unsigned char)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
		}
	}
}

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
