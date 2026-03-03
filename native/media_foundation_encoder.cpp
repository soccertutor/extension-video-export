// ---------------------------------------------------------------------------
// Windows Media Foundation video encoder — hardware H.264 via MFSinkWriter
// ---------------------------------------------------------------------------
// BGRA input (MFVideoFormat_RGB32) → MF handles color conversion internally.
// Same C API as AVAssetWriterEncoder: videoEncoderInit/AddFrame/Finish/Dispose/GetError.
// ---------------------------------------------------------------------------

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
static const int BYTES_PER_PIXEL = 4; // BGRA

static IMFSinkWriter *_writer = NULL;
static DWORD _streamIndex = 0;
static int _width = 0;
static int _height = 0;
static int _fps = 0;
static int _frameIndex = 0;
static BOOL _comInitialized = FALSE;
static BOOL _mfStarted = FALSE;
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

static void setErrorHR(const char *context, HRESULT hr) {
    snprintf(_errorBuf, ERROR_BUF_SIZE, "%s failed: HRESULT 0x%08lX", context, (unsigned long)hr);
}

static void clearError(void) {
    _errorBuf[0] = '\0';
}

template<class T>
static void safeRelease(T **ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

// ---------------------------------------------------------------------------
// Create an IMFMediaType for the H.264 output stream
// ---------------------------------------------------------------------------

static HRESULT createOutputType(int width, int height, int fps, int bitrate, IMFMediaType **ppType) {
    IMFMediaType *pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, (UINT32)fps, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    *ppType = pType;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Create an IMFMediaType for the BGRA input stream
// ---------------------------------------------------------------------------

static HRESULT createInputType(int width, int height, int fps, IMFMediaType **ppType) {
    IMFMediaType *pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) { pType->Release(); return hr; }

    // MFVideoFormat_RGB32 is BGRA in memory on little-endian Windows
    hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, (UINT32)fps, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    *ppType = pType;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Create an IMFSample from raw BGRA pixels
// ---------------------------------------------------------------------------

static HRESULT createSampleFromBGRA(const unsigned char *bgraPixels, int width, int height,
                                     LONGLONG timestamp, LONGLONG duration, IMFSample **ppSample) {
    int stride = width * BYTES_PER_PIXEL;
    int bufferSize = stride * height;

    IMFMediaBuffer *pBuffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE *pData = NULL;
    hr = pBuffer->Lock(&pData, NULL, NULL);
    if (FAILED(hr)) { pBuffer->Release(); return hr; }

    // Copy rows bottom-up — MF RGB32 expects bottom-up row order
    for (int y = 0; y < height; y++) {
        const unsigned char *srcRow = bgraPixels + y * stride;
        BYTE *dstRow = pData + (height - 1 - y) * stride;
        memcpy(dstRow, srcRow, stride);
    }

    hr = pBuffer->Unlock();
    if (FAILED(hr)) { pBuffer->Release(); return hr; }

    hr = pBuffer->SetCurrentLength(bufferSize);
    if (FAILED(hr)) { pBuffer->Release(); return hr; }

    IMFSample *pSample = NULL;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) { pBuffer->Release(); return hr; }

    hr = pSample->AddBuffer(pBuffer);
    pBuffer->Release();
    if (FAILED(hr)) { pSample->Release(); return hr; }

    hr = pSample->SetSampleTime(timestamp);
    if (FAILED(hr)) { pSample->Release(); return hr; }

    hr = pSample->SetSampleDuration(duration);
    if (FAILED(hr)) { pSample->Release(); return hr; }

    *ppSample = pSample;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

static void releaseResources(void) {
    safeRelease(&_writer);
    _width = 0;
    _height = 0;
    _fps = 0;
    _frameIndex = 0;
    _streamIndex = 0;

    if (_mfStarted) {
        MFShutdown();
        _mfStarted = FALSE;
    }
    if (_comInitialized) {
        CoUninitialize();
        _comInitialized = FALSE;
    }
}

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate) {
    clearError();

    if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
        setError("Invalid encoder parameters");
        return -1;
    }

    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        _comInitialized = TRUE;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // COM already initialized with different threading model — that's OK
        _comInitialized = FALSE;
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
    _mfStarted = TRUE;

    // Delete existing file
    DeleteFileA(outputPath);

    // Convert path to wide string for MFCreateSinkWriterFromURL
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, NULL, 0);
    if (wideLen <= 0) {
        setError("Failed to convert output path to wide string");
        releaseResources();
        return -1;
    }
    wchar_t *widePath = new wchar_t[wideLen];
    MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, widePath, wideLen);

    // Create SinkWriter with MPEG4 container
    IMFAttributes *pAttributes = NULL;
    hr = MFCreateAttributes(&pAttributes, 1);
    if (SUCCEEDED(hr)) {
        // Enable hardware transforms (MFT)
        hr = pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    hr = MFCreateSinkWriterFromURL(widePath, NULL, pAttributes, &_writer);
    delete[] widePath;
    safeRelease(&pAttributes);

    if (FAILED(hr)) {
        setErrorHR("MFCreateSinkWriterFromURL", hr);
        releaseResources();
        return -1;
    }

    // Add H.264 output stream
    IMFMediaType *pOutputType = NULL;
    hr = createOutputType(width, height, fps, bitrate, &pOutputType);
    if (FAILED(hr)) {
        setErrorHR("Create output media type", hr);
        releaseResources();
        return -1;
    }

    hr = _writer->AddStream(pOutputType, &_streamIndex);
    safeRelease(&pOutputType);
    if (FAILED(hr)) {
        setErrorHR("AddStream", hr);
        releaseResources();
        return -1;
    }

    // Set BGRA input type
    IMFMediaType *pInputType = NULL;
    hr = createInputType(width, height, fps, &pInputType);
    if (FAILED(hr)) {
        setErrorHR("Create input media type", hr);
        releaseResources();
        return -1;
    }

    hr = _writer->SetInputMediaType(_streamIndex, pInputType, NULL);
    safeRelease(&pInputType);
    if (FAILED(hr)) {
        setErrorHR("SetInputMediaType", hr);
        releaseResources();
        return -1;
    }

    // Start writing
    hr = _writer->BeginWriting();
    if (FAILED(hr)) {
        setErrorHR("BeginWriting", hr);
        releaseResources();
        return -1;
    }

    _width = width;
    _height = height;
    _fps = fps;
    _frameIndex = 0;

    return 0;
}

int videoEncoderAddFrame(const unsigned char *bgraPixels, int dataLength) {
    clearError();

    if (!_writer) {
        setError("Encoder not initialized");
        return -1;
    }

    int expectedLength = _width * _height * BYTES_PER_PIXEL;
    if (dataLength != expectedLength) {
        setError("Data length mismatch: %d != %d", dataLength, expectedLength);
        return -1;
    }

    // 100-nanosecond units per second
    LONGLONG frameDuration = 10000000LL / _fps;
    LONGLONG timestamp = (LONGLONG)_frameIndex * frameDuration;

    IMFSample *pSample = NULL;
    HRESULT hr = createSampleFromBGRA(bgraPixels, _width, _height, timestamp, frameDuration, &pSample);
    if (FAILED(hr)) {
        setErrorHR("createSampleFromBGRA", hr);
        return -1;
    }

    hr = _writer->WriteSample(_streamIndex, pSample);
    safeRelease(&pSample);
    if (FAILED(hr)) {
        setErrorHR("WriteSample", hr);
        return -1;
    }

    _frameIndex++;
    return 0;
}

int videoEncoderFinish(void) {
    clearError();

    if (!_writer) {
        setError("Encoder not initialized");
        return -1;
    }

    HRESULT hr = _writer->Finalize();
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

const char *videoEncoderGetError(void) {
    return _errorBuf[0] != '\0' ? _errorBuf : NULL;
}

} // extern "C"

#endif // _WIN32
