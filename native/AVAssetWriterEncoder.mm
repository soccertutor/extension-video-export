#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

// ---------------------------------------------------------------------------
// Static state — single-threaded access from main thread
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;  // BGRA

static AVAssetWriter *_writer = nil;
static AVAssetWriterInput *_writerInput = nil;
static AVAssetWriterInputPixelBufferAdaptor *_adaptor = nil;
static int _width = 0;
static int _height = 0;
static int _fps = 0;
static int _frameIndex = 0;
static char _errorBuf[ERROR_BUF_SIZE] = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(NSString *message) {
    const char *utf8 = [message UTF8String];
    strlcpy(_errorBuf, utf8, ERROR_BUF_SIZE);
}

static void clearError(void) {
    _errorBuf[0] = '\0';
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate) {
    @autoreleasepool {
        clearError();

        // Remove existing file
        NSString *path = [NSString stringWithUTF8String:outputPath];
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:path]) [fm removeItemAtPath:path error:nil];

        NSURL *url = [NSURL fileURLWithPath:path];

        // Create asset writer
        NSError *error = nil;
        _writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
        if (!_writer) {
            setError([NSString stringWithFormat:@"AVAssetWriter init failed: %@", error.localizedDescription]);
            return -1;
        }

        // H.264 output settings with requested bitrate
        NSDictionary *videoSettings = @{
            AVVideoCodecKey: AVVideoCodecTypeH264,
            AVVideoWidthKey: @(width),
            AVVideoHeightKey: @(height),
            AVVideoCompressionPropertiesKey: @{
                AVVideoAverageBitRateKey: @(bitrate),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                AVVideoExpectedSourceFrameRateKey: @(fps)
            }
        };

        _writerInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                      outputSettings:videoSettings];
        _writerInput.expectsMediaDataInRealTime = NO;

        // Pixel buffer adaptor — BGRA matches OpenFL native BitmapData
        NSDictionary *bufferAttributes = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (NSString *)kCVPixelBufferWidthKey: @(width),
            (NSString *)kCVPixelBufferHeightKey: @(height)
        };

        _adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:_writerInput
            sourcePixelBufferAttributes:bufferAttributes];

        if (![_writer canAddInput:_writerInput]) {
            setError(@"Cannot add writer input to AVAssetWriter");
            _writer = nil;
            _writerInput = nil;
            _adaptor = nil;
            return -1;
        }

        [_writer addInput:_writerInput];

        if (![_writer startWriting]) {
            setError([NSString stringWithFormat:@"startWriting failed: %@", _writer.error.localizedDescription]);
            _writer = nil;
            _writerInput = nil;
            _adaptor = nil;
            return -1;
        }

        [_writer startSessionAtSourceTime:kCMTimeZero];

        _width = width;
        _height = height;
        _fps = fps;
        _frameIndex = 0;

        return 0;
    }
}

int videoEncoderAddFrame(const unsigned char *bgraPixels, int dataLength) {
    @autoreleasepool {
        clearError();

        if (!_writer || !_adaptor) {
            setError(@"Encoder not initialized");
            return -1;
        }

        int expectedLength = _width * _height * BYTES_PER_PIXEL;
        if (dataLength != expectedLength) {
            setError([NSString stringWithFormat:@"Data length mismatch: %d != %d", dataLength, expectedLength]);
            return -1;
        }

        // Wait until the input is ready to accept more data (5s timeout)
        int waitRetries = 0;
        while (!_writerInput.isReadyForMoreMediaData) {
            [NSThread sleepForTimeInterval:0.005];
            if (++waitRetries > 1000) {
                setError(@"Timed out waiting for writer input to become ready");
                return -1;
            }
        }

        // Create CVPixelBuffer wrapping the caller's BGRA data — no copy
        CVPixelBufferRef pixelBuffer = NULL;
        CVReturn status = CVPixelBufferCreateWithBytes(
            kCFAllocatorDefault,
            _width,
            _height,
            kCVPixelFormatType_32BGRA,
            (void *)bgraPixels,
            _width * BYTES_PER_PIXEL,
            NULL,  // no release callback — caller owns the data
            NULL,
            NULL,  // no pixel buffer attributes needed for temporary buffer
            &pixelBuffer
        );

        if (status != kCVReturnSuccess || !pixelBuffer) {
            setError([NSString stringWithFormat:@"CVPixelBufferCreateWithBytes failed: %d", (int)status]);
            return -1;
        }

        CMTime presentationTime = CMTimeMake(_frameIndex, _fps);
        BOOL appended = [_adaptor appendPixelBuffer:pixelBuffer withPresentationTime:presentationTime];
        CVPixelBufferRelease(pixelBuffer);

        if (!appended) {
            setError([NSString stringWithFormat:@"appendPixelBuffer failed at frame %d: %@",
                _frameIndex, _writer.error.localizedDescription]);
            return -1;
        }

        _frameIndex++;
        return 0;
    }
}

int videoEncoderFinish(void) {
    @autoreleasepool {
        clearError();

        if (!_writer) {
            setError(@"Encoder not initialized");
            return -1;
        }

        [_writerInput markAsFinished];

        // Wait synchronously for finishWriting
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        __block BOOL success = YES;

        [_writer finishWritingWithCompletionHandler:^{
            if (_writer.status == AVAssetWriterStatusFailed) {
                setError([NSString stringWithFormat:@"finishWriting failed: %@",
                    _writer.error.localizedDescription]);
                success = NO;
            }
            dispatch_semaphore_signal(semaphore);
        }];

        dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

        return success ? 0 : -1;
    }
}

void videoEncoderDispose(void) {
    @autoreleasepool {
        _adaptor = nil;
        _writerInput = nil;
        _writer = nil;
        _width = 0;
        _height = 0;
        _fps = 0;
        _frameIndex = 0;
        clearError();
    }
}

const char *videoEncoderGetError(void) {
    return _errorBuf[0] != '\0' ? _errorBuf : NULL;
}

} // extern "C"
