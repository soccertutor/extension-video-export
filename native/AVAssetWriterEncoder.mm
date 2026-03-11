/**
 * Video encoder: AVFoundation AVAssetWriter (H.264/MP4).
 * Used on macOS and iOS where AVFoundation is available.
 *
 * Single-instance design. All state is held in static globals.
 * GPU path uses async encoding: blit runs on the render thread, appendPixelBuffer
 * is dispatched to a serial queue so it overlaps the next frame's rendering.
 * Input: BGRA pixel data (matches OpenFL native BitmapData). Output: H.264/MP4 file.
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;			 // BGRA
static const double READY_WAIT_INTERVAL = 0.01;	 // seconds per run-loop drain
static const int READY_WAIT_MAX_RETRIES = 500;	 // 500 * 0.01 = 5s timeout
static const int ASYNC_READY_WAIT_MAX = 5000;	 // 5000 * 1ms = 5s timeout (async path)

static AVAssetWriter *writer_ = nil;
static AVAssetWriterInput *writer_input_ = nil;
static AVAssetWriterInputPixelBufferAdaptor *adaptor_ = nil;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;
static char error_buf_[ERROR_BUF_SIZE] = {0};
static const int BUFFER_COUNT = 2;
static IOSurfaceRef io_surfaces_[BUFFER_COUNT] = {nil, nil};
static CVPixelBufferRef gpu_pixel_buffers_[BUFFER_COUNT] = {NULL, NULL};
static GLuint io_surface_texs_[BUFFER_COUNT] = {0, 0};
static GLuint io_surface_fbos_[BUFFER_COUNT] = {0, 0};
static int current_buf_ = 0;

// Async encoding pipeline state
static dispatch_queue_t encode_queue_ = nil;
static dispatch_semaphore_t buffer_sema_[BUFFER_COUNT] = {NULL, NULL};
static GLsync blit_fence_ = NULL;
static volatile bool async_error_ = false;

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

/** Release all double-buffered IOSurface and CVPixelBuffer resources. */
static void releaseGpuBuffers(void) {
	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (gpu_pixel_buffers_[i]) {
			CVPixelBufferRelease(gpu_pixel_buffers_[i]);
			gpu_pixel_buffers_[i] = NULL;
		}
		if (io_surfaces_[i]) {
			CFRelease(io_surfaces_[i]);
			io_surfaces_[i] = nil;
		}
	}
	current_buf_ = 0;
}

/** Delete all IOSurface-backed GL textures and FBOs. */
static void releaseGpuFbos(void) {
	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (io_surface_fbos_[i]) {
			glDeleteFramebuffers(1, &io_surface_fbos_[i]);
			io_surface_fbos_[i] = 0;
		}
		if (io_surface_texs_[i]) {
			glDeleteTextures(1, &io_surface_texs_[i]);
			io_surface_texs_[i] = 0;
		}
	}
}

/**
 * Shared AVAssetWriter setup: remove existing file, create writer + input + adaptor,
 * start writing session. Sets width_/height_/fps_/frame_index_ on success.
 * On failure, sets error and nils writer_/writer_input_/adaptor_. Returns 0/-1.
 */
static int initAssetWriter(const char *outputPath, int width, int height, int fps, int bitrate) {
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

		return initAssetWriter(outputPath, width, height, fps, bitrate);
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

		// Wait until the input is ready — drain the run loop so AVAssetWriter's
		// internal completion handlers fire (flips isReadyForMoreMediaData).
		// Much faster than a fixed-interval sleep; returns immediately when a
		// source fires.  5-second total timeout preserved.
		int waitRetries = 0;
		while (!writer_input_.isReadyForMoreMediaData) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, READY_WAIT_INTERVAL, true);
			if (++waitRetries > READY_WAIT_MAX_RETRIES) {
				setError(@"Timed out waiting for writer input to become ready");
				return -1;
			}
		}

		// Get a pooled CVPixelBuffer — reuses allocations across frames
		CVPixelBufferPoolRef pool = adaptor_.pixelBufferPool;
		if (!pool) {
			setError(@"Pixel buffer pool not available");
			return -1;
		}

		CVPixelBufferRef pixelBuffer = NULL;
		CVReturn status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer);
		if (status != kCVReturnSuccess || !pixelBuffer) {
			setError([NSString stringWithFormat:@"CVPixelBufferPoolCreatePixelBuffer failed: %d", (int)status]);
			return -1;
		}

		CVPixelBufferLockBaseAddress(pixelBuffer, 0);
		void *const baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer);
		const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
		const int srcStride = width_ * BYTES_PER_PIXEL;

		if ((int)bytesPerRow == srcStride) {
			memcpy(baseAddress, bgraPixels, expectedLength);
		} else {
			// Pool buffer has padding — copy row by row
			for (int y = 0; y < height_; y++) {
				memcpy((unsigned char *)baseAddress + y * bytesPerRow, bgraPixels + y * srcStride, srcStride);
			}
		}
		CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

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

		// Drain pending async encodes before finishing
		if (encode_queue_)
			dispatch_sync(encode_queue_, ^{});

		if (async_error_) {
			setError(@"Async encode failed during export");
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
		// Drain pending async encodes before tearing down
		if (encode_queue_) {
			dispatch_sync(encode_queue_, ^{});
			encode_queue_ = nil;
		}
		for (int i = 0; i < BUFFER_COUNT; i++) {
			if (buffer_sema_[i]) {
				buffer_sema_[i] = nil;
			}
		}
		if (blit_fence_) {
			glDeleteSync(blit_fence_);
			blit_fence_ = NULL;
		}
		async_error_ = false;

		if (writer_ && writer_.status == AVAssetWriterStatusWriting) [writer_ cancelWriting];
		adaptor_ = nil;
		writer_input_ = nil;
		writer_ = nil;
		releaseGpuBuffers();
		releaseGpuFbos();
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

int videoEncoderSupportsGpuInput(void) {
	return 1;
}

int videoEncoderInitGpu(const char *outputPath, int width, int height, int fps, int bitrate) {
	@autoreleasepool {
		clearError();

		if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
			setError(@"Invalid encoder parameters");
			return -1;
		}

		// Create double-buffered IOSurfaces for zero-copy GPU texture sharing.
		// Two buffers alternate: GPU renders to one while encoder reads the other.
		NSDictionary *surfaceProps = @{
			(NSString *)kIOSurfaceWidth : @(width),
			(NSString *)kIOSurfaceHeight : @(height),
			(NSString *)kIOSurfaceBytesPerElement : @(BYTES_PER_PIXEL),
			(NSString *)kIOSurfaceBytesPerRow : @(width * BYTES_PER_PIXEL),
			(NSString *)kIOSurfacePixelFormat : @(kCVPixelFormatType_32BGRA)
		};
		for (int i = 0; i < BUFFER_COUNT; i++) {
			io_surfaces_[i] = IOSurfaceCreate((__bridge CFDictionaryRef)surfaceProps);
			if (!io_surfaces_[i]) {
				setError(@"IOSurfaceCreate failed");
				for (int j = 0; j < i; j++) {
					CVPixelBufferRelease(gpu_pixel_buffers_[j]);
					gpu_pixel_buffers_[j] = NULL;
					CFRelease(io_surfaces_[j]);
					io_surfaces_[j] = nil;
				}
				return -1;
			}

			CVReturn cvRet = CVPixelBufferCreateWithIOSurface(
				kCFAllocatorDefault,
				io_surfaces_[i],
				(__bridge CFDictionaryRef)
					@{(NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}},
				&gpu_pixel_buffers_[i]
			);
			if (cvRet != kCVReturnSuccess || !gpu_pixel_buffers_[i]) {
				setError([NSString stringWithFormat:@"CVPixelBufferCreateWithIOSurface failed: %d", (int)cvRet]);
				CFRelease(io_surfaces_[i]);
				io_surfaces_[i] = nil;
				for (int j = 0; j < i; j++) {
					CVPixelBufferRelease(gpu_pixel_buffers_[j]);
					gpu_pixel_buffers_[j] = NULL;
					CFRelease(io_surfaces_[j]);
					io_surfaces_[j] = nil;
				}
				return -1;
			}
		}
		current_buf_ = 0;

		if (initAssetWriter(outputPath, width, height, fps, bitrate) != 0) {
			releaseGpuBuffers();
			return -1;
		}

		// Async encoding pipeline
		encode_queue_ = dispatch_queue_create("com.tm.videoexport.encode", DISPATCH_QUEUE_SERIAL);
		for (int i = 0; i < BUFFER_COUNT; i++) buffer_sema_[i] = dispatch_semaphore_create(1);
		async_error_ = false;

		return 0;
	}
}

unsigned int videoEncoderGetSurfaceId(void) {
	return io_surfaces_[0] ? (unsigned int)IOSurfaceGetID(io_surfaces_[0]) : 0;
}

int videoEncoderSubmitGpuFrame(void) {
	@autoreleasepool {
		clearError();

		if (!writer_ || !adaptor_ || !gpu_pixel_buffers_[current_buf_]) {
			setError(@"GPU encoder not initialized");
			return -1;
		}
		if (async_error_) {
			setError(@"Previous async encode failed");
			return -1;
		}

		// Wait for GPU blit to complete (fence from blitToIoSurface)
		if (blit_fence_) {
			glClientWaitSync(blit_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
			glDeleteSync(blit_fence_);
			blit_fence_ = NULL;
		}

		// Capture state for async block
		int bufIdx = current_buf_;
		CVPixelBufferRef pb = gpu_pixel_buffers_[bufIdx];
		CMTime pt = CMTimeMake(frame_index_, fps_);

		dispatch_async(encode_queue_, ^{
			@autoreleasepool {
				// Wait for encoder readiness (1ms poll, 5s timeout)
				int waitRetries = 0;
				while (!writer_input_.isReadyForMoreMediaData) {
					usleep(1000);
					if (++waitRetries > ASYNC_READY_WAIT_MAX) {
						async_error_ = true;
						dispatch_semaphore_signal(buffer_sema_[bufIdx]);
						return;
					}
				}

				BOOL ok = [adaptor_ appendPixelBuffer:pb withPresentationTime:pt];
				if (!ok) async_error_ = true;

				// Release buffer for reuse by the next blit
				dispatch_semaphore_signal(buffer_sema_[bufIdx]);
			}
		});

		current_buf_ = 1 - current_buf_;
		frame_index_++;
		return 0;
	}
}

int videoEncoderSetupIoSurfaceFbo(int width, int height) {
	CGLContextObj cgl_ctx = CGLGetCurrentContext();
	if (!cgl_ctx) return -1;

	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (!io_surfaces_[i]) {
			releaseGpuFbos();
			return -1;
		}

		glGenTextures(1, &io_surface_texs_[i]);
		glBindTexture(GL_TEXTURE_RECTANGLE, io_surface_texs_[i]);
		CGLError err = CGLTexImageIOSurface2D(
			cgl_ctx,
			GL_TEXTURE_RECTANGLE,
			GL_RGBA,
			(GLsizei)width,
			(GLsizei)height,
			GL_BGRA,
			GL_UNSIGNED_INT_8_8_8_8_REV,
			io_surfaces_[i],
			0
		);
		if (err != kCGLNoError) {
			glDeleteTextures(1, &io_surface_texs_[i]);
			io_surface_texs_[i] = 0;
			releaseGpuFbos();
			return -1;
		}

		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glGenFramebuffers(1, &io_surface_fbos_[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, io_surface_fbos_[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, io_surface_texs_[i], 0);

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			releaseGpuFbos();
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return -1;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return 0;
}

void videoEncoderBlitToIoSurface(unsigned int srcFbo, int width, int height) {
	if (!buffer_sema_[current_buf_]) return;

	// Wait for previous async encode of this buffer to complete
	dispatch_semaphore_wait(buffer_sema_[current_buf_], DISPATCH_TIME_FOREVER);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, io_surface_fbos_[current_buf_]);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFlush();	// Submit to GPU without blocking — fence tracks completion

	// Fence to track blit completion (waited on in submitGpuFrame before dispatch)
	if (blit_fence_) glDeleteSync(blit_fence_);
	blit_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void videoEncoderDisposeIoSurfaceFbo(void) {
	releaseGpuFbos();
}

}  // extern "C"
