// ---------------------------------------------------------------------------
// Android NDK video encoder — hardware H.264 via AMediaCodec + AMediaMuxer
// ---------------------------------------------------------------------------
// BGRA input → NV12 conversion in native code (not all devices accept BGRA).
// Same C API as AVAssetWriterEncoder: videoEncoderInit/AddFrame/Finish/Dispose/GetError.
// Requires API 21+ (our minimum SDK).
// ---------------------------------------------------------------------------

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4; // BGRA
static const int TIMEOUT_US = 10000;           // 10ms dequeue timeout
static const int INPUT_TIMEOUT_US = 1000000;   // 1s input buffer timeout
static const int COLOR_FORMAT_NV12 = 21;       // COLOR_FormatYUV420SemiPlanar
static const char *MIME_H264 = "video/avc";

static AMediaCodec *_codec = NULL;
static AMediaMuxer *_muxer = NULL;
static int _muxerTrackIndex = -1;
static bool _muxerStarted = false;
static int _width = 0;
static int _height = 0;
static int _fps = 0;
static int _frameIndex = 0;
static int _muxerFd = -1;
static char _errorBuf[ERROR_BUF_SIZE] = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(_errorBuf, ERROR_BUF_SIZE, fmt, args);
    va_end(args);
}

static void clearError(void) {
    _errorBuf[0] = '\0';
}

// ---------------------------------------------------------------------------
// BGRA → NV12 conversion
// ---------------------------------------------------------------------------
// NV12 layout: Y plane (w*h bytes), then interleaved UV plane (w*h/2 bytes).
// BT.601 coefficients for SD/HD content.

static void bgraToNV12(const unsigned char *bgra, unsigned char *nv12, int width, int height) {
    const int frameSize = width * height;
    const int stride = width * BYTES_PER_PIXEL;
    unsigned char *yPlane = nv12;
    unsigned char *uvPlane = nv12 + frameSize;

    /* Compute Y for every pixel */
    for (int y = 0; y < height; y++) {
        const unsigned char *row = bgra + y * stride;
        unsigned char *yRow = yPlane + y * width;
        for (int x = 0; x < width; x++) {
            const int idx = x * BYTES_PER_PIXEL;
            const int b = row[idx];
            const int g = row[idx + 1];
            const int r = row[idx + 2];
            yRow[x] = (unsigned char)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    /* Compute UV with 2x2 block averaging */
    const int uvWidth = width / 2;
    const int uvHeight = height / 2;
    for (int y = 0; y < uvHeight; y++) {
        const unsigned char *line0 = bgra + (y * 2) * stride;
        const unsigned char *line1 = bgra + (y * 2 + 1) * stride;
        for (int x = 0; x < uvWidth; x++) {
            const int col0 = (x * 2) * BYTES_PER_PIXEL;
            const int col1 = (x * 2 + 1) * BYTES_PER_PIXEL;

            const int r = (line0[col0 + 2] + line0[col1 + 2] + line1[col0 + 2] + line1[col1 + 2] + 2) >> 2;
            const int g = (line0[col0 + 1] + line0[col1 + 1] + line1[col0 + 1] + line1[col1 + 1] + 2) >> 2;
            const int b = (line0[col0]     + line0[col1]     + line1[col0]     + line1[col1]     + 2) >> 2;

            const int uvIdx = y * width + x * 2;
            uvPlane[uvIdx]     = (unsigned char)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uvPlane[uvIdx + 1] = (unsigned char)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

// ---------------------------------------------------------------------------
// Drain encoder output — process all available output buffers
// ---------------------------------------------------------------------------

static const int MAX_DRAIN_RETRIES = 100; // Safety limit for EOS drain

static int drainEncoder(bool endOfStream) {
    if (!_codec) return -1;

    int retries = 0;
    while (1) {
        AMediaCodecBufferInfo info;
        ssize_t outputIdx = AMediaCodec_dequeueOutputBuffer(_codec, &info, TIMEOUT_US);

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
            if (_muxerStarted) {
                setError("Output format changed after muxer started");
                return -1;
            }
            AMediaFormat *newFormat = AMediaCodec_getOutputFormat(_codec);
            _muxerTrackIndex = AMediaMuxer_addTrack(_muxer, newFormat);
            AMediaFormat_delete(newFormat);
            if (_muxerTrackIndex < 0) {
                setError("AMediaMuxer_addTrack failed");
                return -1;
            }
            media_status_t st = AMediaMuxer_start(_muxer);
            if (st != AMEDIA_OK) {
                setError("AMediaMuxer_start failed: %d", (int)st);
                return -1;
            }
            _muxerStarted = true;
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
        if (info.size > 0 && _muxerStarted) {
            size_t outSize = 0;
            uint8_t *outBuf = AMediaCodec_getOutputBuffer(_codec, (size_t)outputIdx, &outSize);
            if (outBuf) {
                AMediaCodecBufferInfo muxInfo;
                muxInfo.offset = 0; // Already offset by info.offset in the pointer
                muxInfo.size = info.size;
                muxInfo.presentationTimeUs = info.presentationTimeUs;
                muxInfo.flags = info.flags;
                const media_status_t st = AMediaMuxer_writeSampleData(_muxer, _muxerTrackIndex, outBuf + info.offset, &muxInfo);
                if (st != AMEDIA_OK) {
                    AMediaCodec_releaseOutputBuffer(_codec, (size_t)outputIdx, false);
                    setError("AMediaMuxer_writeSampleData failed: %d", (int)st);
                    return -1;
                }
            }
        }

        AMediaCodec_releaseOutputBuffer(_codec, (size_t)outputIdx, false);

        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

static void releaseResources(void);

extern "C" {

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate) {
    clearError();

    if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
        setError("Invalid encoder parameters");
        return -1;
    }

    // Open output file descriptor for muxer
    _muxerFd = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (_muxerFd < 0) {
        setError("Cannot open output file: %s", outputPath);
        return -1;
    }

    // Create muxer
    _muxer = AMediaMuxer_new(_muxerFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!_muxer) {
        setError("AMediaMuxer_new failed");
        releaseResources();
        return -1;
    }

    // Create H.264 encoder
    _codec = AMediaCodec_createEncoderByType(MIME_H264);
    if (!_codec) {
        setError("AMediaCodec_createEncoderByType failed for video/avc");
        releaseResources();
        return -1;
    }

    // Configure encoder
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, MIME_H264);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, (float)fps);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FORMAT_NV12);

    media_status_t status = AMediaCodec_configure(_codec, format, NULL, NULL,
                                                   AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) {
        setError("AMediaCodec_configure failed: %d", (int)status);
        releaseResources();
        return -1;
    }

    status = AMediaCodec_start(_codec);
    if (status != AMEDIA_OK) {
        setError("AMediaCodec_start failed: %d", (int)status);
        releaseResources();
        return -1;
    }

    _width = width;
    _height = height;
    _fps = fps;
    _frameIndex = 0;
    _muxerTrackIndex = -1;
    _muxerStarted = false;

    return 0;
}

int videoEncoderAddFrame(const unsigned char *bgraPixels, int dataLength) {
    clearError();

    if (!_codec || !_muxer) {
        setError("Encoder not initialized");
        return -1;
    }

    int expectedLength = _width * _height * BYTES_PER_PIXEL;
    if (dataLength != expectedLength) {
        setError("Data length mismatch: %d != %d", dataLength, expectedLength);
        return -1;
    }

    // Dequeue input buffer
    ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(_codec, INPUT_TIMEOUT_US);
    if (inputIdx < 0) {
        setError("dequeueInputBuffer timeout");
        return -1;
    }

    size_t inputSize = 0;
    uint8_t *inputBuf = AMediaCodec_getInputBuffer(_codec, (size_t)inputIdx, &inputSize);
    if (!inputBuf) {
        setError("getInputBuffer returned NULL");
        AMediaCodec_queueInputBuffer(_codec, (size_t)inputIdx, 0, 0, 0, 0);
        return -1;
    }

    // NV12 size = width * height * 3 / 2
    int nv12Size = _width * _height * 3 / 2;
    if ((int)inputSize < nv12Size) {
        setError("Input buffer too small: %zu < %d", inputSize, nv12Size);
        AMediaCodec_queueInputBuffer(_codec, (size_t)inputIdx, 0, 0, 0, 0);
        return -1;
    }

    // Convert BGRA → NV12
    bgraToNV12(bgraPixels, inputBuf, _width, _height);

    int64_t presentationTimeUs = (int64_t)_frameIndex * 1000000LL / _fps;
    media_status_t status = AMediaCodec_queueInputBuffer(_codec, (size_t)inputIdx, 0, nv12Size,
                                                          presentationTimeUs, 0);
    if (status != AMEDIA_OK) {
        setError("queueInputBuffer failed: %d", (int)status);
        return -1;
    }

    _frameIndex++;

    // Drain available output
    if (drainEncoder(false) != 0) return -1;

    return 0;
}

int videoEncoderFinish(void) {
    clearError();

    if (!_codec || !_muxer) {
        setError("Encoder not initialized");
        return -1;
    }

    // Signal end of stream
    ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(_codec, INPUT_TIMEOUT_US);
    if (inputIdx >= 0) {
        AMediaCodec_queueInputBuffer(_codec, (size_t)inputIdx, 0, 0, 0,
                                      AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    }

    // Drain all remaining output
    if (drainEncoder(true) != 0) return -1;

    // Stop muxer
    if (_muxerStarted) {
        AMediaMuxer_stop(_muxer);
        _muxerStarted = false;
    }

    return 0;
}

static void releaseResources(void) {
    if (_codec) {
        AMediaCodec_stop(_codec);
        AMediaCodec_delete(_codec);
        _codec = NULL;
    }
    if (_muxer) {
        AMediaMuxer_delete(_muxer);
        _muxer = NULL;
    }
    if (_muxerFd >= 0) {
        close(_muxerFd);
        _muxerFd = -1;
    }

    _width = 0;
    _height = 0;
    _fps = 0;
    _frameIndex = 0;
    _muxerTrackIndex = -1;
    _muxerStarted = false;
}

void videoEncoderDispose(void) {
    releaseResources();
    clearError();
}

const char *videoEncoderGetError(void) {
    return _errorBuf[0] != '\0' ? _errorBuf : NULL;
}

} // extern "C"

#endif // __ANDROID__
