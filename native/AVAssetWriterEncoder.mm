/**
 * Video encoder: AVFoundation AVAssetWriter (H.264/MP4).
 * Used on macOS and iOS where AVFoundation is available.
 *
 * Single-instance, single-threaded design. All state is held in static globals.
 * Input: BGRA pixel data (matches OpenFL native BitmapData). Output: H.264/MP4 file.
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

// ---------------------------------------------------------------------------
// Static state — single-threaded access from main thread
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;  // BGRA

static AVAssetWriter *writer_ = nil;
static AVAssetWriterInput *writer_input_ = nil;
static AVAssetWriterInputPixelBufferAdaptor *adaptor_ = nil;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;
static char error_buf_[ERROR_BUF_SIZE] = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(NSString *message) {
	const char *utf8 = [message UTF8String];
	strlcpy(error_buf_, utf8, ERROR_BUF_SIZE);
}

static void clearError(void) {
	error_buf_[0] = '\0';
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate) {
	@autoreleasepool {
		clearError();

		if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
			setError(@"Invalid encoder parameters");
			return -1;
		}

		// Remove existing file
		NSString *path = [NSString stringWithUTF8String:outputPath];
		NSFileManager *fm = [NSFileManager defaultManager];
		if ([fm fileExistsAtPath:path]) [fm removeItemAtPath:path error:nil];

		NSURL *url = [NSURL fileURLWithPath:path];

		// Create asset writer
		NSError *error = nil;
		writer_ = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
		if (!writer_) {
			setError([NSString stringWithFormat:@"AVAssetWriter init failed: %@", error.localizedDescription]);
			return -1;
		}

		// H.264 output settings with requested bitrate
		NSDictionary *videoSettings = @{
			AVVideoCodecKey : AVVideoCodecH264,
			AVVideoWidthKey : @(width),
			AVVideoHeightKey : @(height),
			AVVideoCompressionPropertiesKey : @{
				AVVideoAverageBitRateKey : @(bitrate),
				AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
				AVVideoExpectedSourceFrameRateKey : @(fps)
			}
		};

		writer_input_ = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:videoSettings];
		writer_input_.expectsMediaDataInRealTime = NO;

		// Pixel buffer adaptor — BGRA matches OpenFL native BitmapData
		NSDictionary *bufferAttributes = @{
			(NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
			(NSString *)kCVPixelBufferWidthKey : @(width),
			(NSString *)kCVPixelBufferHeightKey : @(height)
		};

		adaptor_ = [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:writer_input_
															  sourcePixelBufferAttributes:bufferAttributes];

		if (![writer_ canAddInput:writer_input_]) {
			setError(@"Cannot add writer input to AVAssetWriter");
			writer_ = nil;
			writer_input_ = nil;
			adaptor_ = nil;
			return -1;
		}

		[writer_ addInput:writer_input_];

		if (![writer_ startWriting]) {
			setError([NSString stringWithFormat:@"startWriting failed: %@", writer_.error.localizedDescription]);
			writer_ = nil;
			writer_input_ = nil;
			adaptor_ = nil;
			return -1;
		}

		[writer_ startSessionAtSourceTime:kCMTimeZero];

		width_ = width;
		height_ = height;
		fps_ = fps;
		frame_index_ = 0;

		return 0;
	}
}

int videoEncoderAddFrame(const unsigned char *bgraPixels, int dataLength) {
	@autoreleasepool {
		clearError();

		if (!writer_ || !adaptor_) {
			setError(@"Encoder not initialized");
			return -1;
		}

		int expectedLength = width_ * height_ * BYTES_PER_PIXEL;
		if (dataLength != expectedLength) {
			setError([NSString stringWithFormat:@"Data length mismatch: %d != %d", dataLength, expectedLength]);
			return -1;
		}

		// Wait until the input is ready to accept more data (5s timeout)
		int waitRetries = 0;
		while (!writer_input_.isReadyForMoreMediaData) {
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
			width_,
			height_,
			kCVPixelFormatType_32BGRA,
			(void *)bgraPixels,
			width_ * BYTES_PER_PIXEL,
			NULL,  // no release callback — caller owns the data
			NULL,
			NULL,  // no pixel buffer attributes needed for temporary buffer
			&pixelBuffer
		);

		if (status != kCVReturnSuccess || !pixelBuffer) {
			setError([NSString stringWithFormat:@"CVPixelBufferCreateWithBytes failed: %d", (int)status]);
			return -1;
		}

		CMTime presentationTime = CMTimeMake(frame_index_, fps_);
		BOOL appended = [adaptor_ appendPixelBuffer:pixelBuffer withPresentationTime:presentationTime];
		CVPixelBufferRelease(pixelBuffer);

		if (!appended) {
			setError([NSString
				stringWithFormat:@"appendPixelBuffer failed at frame %d: %@", frame_index_, writer_.error.localizedDescription]);
			return -1;
		}

		frame_index_++;
		return 0;
	}
}

int videoEncoderFinish(void) {
	@autoreleasepool {
		clearError();

		if (!writer_) {
			setError(@"Encoder not initialized");
			return -1;
		}

		[writer_input_ markAsFinished];

		// Wait synchronously for finishWriting
		dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
		__block BOOL success = YES;

		[writer_ finishWritingWithCompletionHandler:^{
			if (writer_.status == AVAssetWriterStatusFailed) {
				setError([NSString stringWithFormat:@"finishWriting failed: %@", writer_.error.localizedDescription]);
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
		if (writer_ && writer_.status == AVAssetWriterStatusWriting) [writer_ cancelWriting];
		adaptor_ = nil;
		writer_input_ = nil;
		writer_ = nil;
		width_ = 0;
		height_ = 0;
		fps_ = 0;
		frame_index_ = 0;
		clearError();
	}
}

const char *videoEncoderGetError(void) {
	return error_buf_[0] != '\0' ? error_buf_ : NULL;
}

}  // extern "C"
