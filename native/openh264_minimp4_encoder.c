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

/* -------------------------------------------------------------------------- */
/*  Error buffer                                                              */
/* -------------------------------------------------------------------------- */

#define ERROR_BUF_SIZE 512

static char g_errorBuf[ERROR_BUF_SIZE] = {0};

static void setError(const char *msg) {
    strncpy(g_errorBuf, msg, ERROR_BUF_SIZE - 1);
    g_errorBuf[ERROR_BUF_SIZE - 1] = '\0';
}

static void clearError(void) {
    g_errorBuf[0] = '\0';
}

/* -------------------------------------------------------------------------- */
/*  Encoder state                                                             */
/* -------------------------------------------------------------------------- */

static ISVCEncoder *g_encoder = NULL;
static FILE *g_outputFile = NULL;
static MP4E_mux_t *g_mux = NULL;
static mp4_h26x_writer_t g_mp4Writer;
static unsigned char *g_i420Buf = NULL;
static int g_width = 0;
static int g_height = 0;
static int g_fps = 0;
static int g_frameIndex = 0;

/* -------------------------------------------------------------------------- */
/*  minimp4 write callback                                                    */
/* -------------------------------------------------------------------------- */

static int mp4WriteCallback(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseek(f, (long)offset, SEEK_SET))
        return -1;
    return fwrite(buffer, 1, size, f) != size ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
/*  BGRA → I420 conversion (BT.601)                                          */
/* -------------------------------------------------------------------------- */

static void bgraToI420(const unsigned char *bgra, int width, int height, unsigned char *i420) {
    const int yPlaneSize = width * height;
    const int uvWidth = width / 2;
    const int uvHeight = height / 2;

    unsigned char *yPlane = i420;
    unsigned char *uPlane = i420 + yPlaneSize;
    unsigned char *vPlane = uPlane + uvWidth * uvHeight;

    /* Compute Y for every pixel */
    for (int row = 0; row < height; row++) {
        const unsigned char *srcRow = bgra + row * width * 4;
        unsigned char *yRow = yPlane + row * width;
        for (int col = 0; col < width; col++) {
            const int idx = col * 4;
            const int b = srcRow[idx];
            const int g = srcRow[idx + 1];
            const int r = srcRow[idx + 2];
            yRow[col] = (unsigned char)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    /* Compute U and V with 2x2 subsampling */
    for (int row = 0; row < uvHeight; row++) {
        const int srcRow0 = row * 2;
        const int srcRow1 = srcRow0 + 1;
        const unsigned char *line0 = bgra + srcRow0 * width * 4;
        const unsigned char *line1 = bgra + srcRow1 * width * 4;
        unsigned char *uRow = uPlane + row * uvWidth;
        unsigned char *vRow = vPlane + row * uvWidth;

        for (int col = 0; col < uvWidth; col++) {
            const int col0 = col * 2;
            const int col1 = col0 + 1;

            /* Average 2x2 block */
            const int idx00 = col0 * 4;
            const int idx01 = col1 * 4;
            const int idx10 = col0 * 4;
            const int idx11 = col1 * 4;

            const int r = (line0[idx00 + 2] + line0[idx01 + 2] + line1[idx10 + 2] + line1[idx11 + 2] + 2) >> 2;
            const int g = (line0[idx00 + 1] + line0[idx01 + 1] + line1[idx10 + 1] + line1[idx11 + 1] + 2) >> 2;
            const int b = (line0[idx00]     + line0[idx01]     + line1[idx10]     + line1[idx11]     + 2) >> 2;

            uRow[col] = (unsigned char)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vRow[col] = (unsigned char)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate) {
    clearError();

    if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
        setError("Invalid encoder parameters");
        return -1;
    }

    if (width % 2 != 0 || height % 2 != 0) {
        setError("Width and height must be even for I420 conversion");
        return -1;
    }

    /* Open output file */
    g_outputFile = fopen(outputPath, "wb");
    if (!g_outputFile) {
        setError("Failed to open output file");
        return -1;
    }

    /* Create OpenH264 encoder */
    if (WelsCreateSVCEncoder(&g_encoder) != 0 || !g_encoder) {
        setError("Failed to create OpenH264 encoder");
        fclose(g_outputFile);
        g_outputFile = NULL;
        return -1;
    }

    /* Configure encoder */
    SEncParamBase param;
    memset(&param, 0, sizeof(SEncParamBase));
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = width;
    param.iPicHeight = height;
    param.iTargetBitrate = bitrate;
    param.fMaxFrameRate = (float)fps;

    if ((*g_encoder)->Initialize(g_encoder, &param) != 0) {
        setError("Failed to initialize OpenH264 encoder");
        WelsDestroySVCEncoder(g_encoder);
        g_encoder = NULL;
        fclose(g_outputFile);
        g_outputFile = NULL;
        return -1;
    }

    /* Initialize MP4 muxer */
    g_mux = MP4E_open(0, 0, g_outputFile, mp4WriteCallback);
    if (!g_mux) {
        setError("Failed to initialize MP4 muxer");
        (*g_encoder)->Uninitialize(g_encoder);
        WelsDestroySVCEncoder(g_encoder);
        g_encoder = NULL;
        fclose(g_outputFile);
        g_outputFile = NULL;
        return -1;
    }

    /* Initialize H.264 NAL writer (is_hevc = 0) */
    if (mp4_h26x_write_init(&g_mp4Writer, g_mux, width, height, 0) != MP4E_STATUS_OK) {
        setError("Failed to initialize MP4 H.264 writer");
        MP4E_close(g_mux);
        g_mux = NULL;
        (*g_encoder)->Uninitialize(g_encoder);
        WelsDestroySVCEncoder(g_encoder);
        g_encoder = NULL;
        fclose(g_outputFile);
        g_outputFile = NULL;
        return -1;
    }

    /* Allocate I420 conversion buffer */
    const int i420Size = width * height * 3 / 2;
    g_i420Buf = (unsigned char *)malloc(i420Size);
    if (!g_i420Buf) {
        setError("Failed to allocate I420 buffer");
        mp4_h26x_write_close(&g_mp4Writer);
        MP4E_close(g_mux);
        g_mux = NULL;
        (*g_encoder)->Uninitialize(g_encoder);
        WelsDestroySVCEncoder(g_encoder);
        g_encoder = NULL;
        fclose(g_outputFile);
        g_outputFile = NULL;
        return -1;
    }

    g_width = width;
    g_height = height;
    g_fps = fps;
    g_frameIndex = 0;

    return 0;
}

int videoEncoderAddFrame(const unsigned char *bgraPixels, int dataLength) {
    clearError();

    if (!g_encoder || !g_mux || !g_i420Buf) {
        setError("Encoder not initialized");
        return -1;
    }

    const int expectedLength = g_width * g_height * 4;
    if (dataLength != expectedLength) {
        setError("Frame data length mismatch");
        return -1;
    }

    /* BGRA → I420 */
    bgraToI420(bgraPixels, g_width, g_height, g_i420Buf);

    /* Set up source picture */
    SSourcePicture srcPic;
    memset(&srcPic, 0, sizeof(SSourcePicture));
    srcPic.iColorFormat = videoFormatI420;
    srcPic.iPicWidth = g_width;
    srcPic.iPicHeight = g_height;
    srcPic.iStride[0] = g_width;       /* Y stride */
    srcPic.iStride[1] = g_width / 2;   /* U stride */
    srcPic.iStride[2] = g_width / 2;   /* V stride */
    srcPic.pData[0] = g_i420Buf;
    srcPic.pData[1] = g_i420Buf + g_width * g_height;
    srcPic.pData[2] = g_i420Buf + g_width * g_height + (g_width / 2) * (g_height / 2);
    srcPic.uiTimeStamp = (long long)g_frameIndex * 1000 / g_fps;

    /* Encode */
    SFrameBSInfo bsInfo;
    memset(&bsInfo, 0, sizeof(SFrameBSInfo));

    int encResult = (*g_encoder)->EncodeFrame(g_encoder, &srcPic, &bsInfo);
    if (encResult != 0) {
        setError("EncodeFrame failed");
        return -1;
    }

    /* Write encoded NALs to MP4 */
    if (bsInfo.eFrameType != videoFrameTypeSkip) {
        /* Timestamp in 90kHz units for next frame */
        const unsigned nextTimestamp90k = (unsigned)((long long)(g_frameIndex + 1) * 90000 / g_fps);

        for (int layer = 0; layer < bsInfo.iLayerNum; layer++) {
            const SLayerBSInfo *layerInfo = &bsInfo.sLayerInfo[layer];
            unsigned char *nalData = layerInfo->pBsBuf;

            for (int nal = 0; nal < layerInfo->iNalCount; nal++) {
                if (mp4_h26x_write_nal(&g_mp4Writer, nalData, layerInfo->pNalLengthInByte[nal], nextTimestamp90k) != MP4E_STATUS_OK) {
                    setError("Failed to write NAL to MP4");
                    return -1;
                }
                nalData += layerInfo->pNalLengthInByte[nal];
            }
        }
    }

    g_frameIndex++;
    return 0;
}

int videoEncoderFinish(void) {
    clearError();

    if (!g_encoder || !g_mux) {
        setError("Encoder not initialized");
        return -1;
    }

    /* OpenH264 in CAMERA_VIDEO_REAL_TIME mode encodes synchronously —
       no buffered frames to flush. Close MP4 writer and muxer. */
    mp4_h26x_write_close(&g_mp4Writer);
    MP4E_close(g_mux);
    g_mux = NULL;

    /* Close output file */
    if (g_outputFile) {
        fclose(g_outputFile);
        g_outputFile = NULL;
    }

    return 0;
}

void videoEncoderDispose(void) {
    if (g_encoder) {
        (*g_encoder)->Uninitialize(g_encoder);
        WelsDestroySVCEncoder(g_encoder);
        g_encoder = NULL;
    }

    if (g_i420Buf) {
        free(g_i420Buf);
        g_i420Buf = NULL;
    }

    /* Close muxer before file — MP4E_close flushes via write callback */
    if (g_mux) {
        mp4_h26x_write_close(&g_mp4Writer);
        MP4E_close(g_mux);
        g_mux = NULL;
    }

    /* Close file after muxer is done writing */
    if (g_outputFile) {
        fclose(g_outputFile);
        g_outputFile = NULL;
    }

    g_width = 0;
    g_height = 0;
    g_fps = 0;
    g_frameIndex = 0;

    clearError();
}

const char *videoEncoderGetError(void) {
    return g_errorBuf[0] != '\0' ? g_errorBuf : NULL;
}
