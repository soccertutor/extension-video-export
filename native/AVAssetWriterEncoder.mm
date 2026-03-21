/**
 * Video encoder: AVFoundation AVAssetWriter (H.264/MP4).
 * Used on macOS and iOS where AVFoundation is available.
 *
 * Single-instance design. All state is held in static globals.
 * GPU path uses async encoding: blit runs on the render thread, appendPixelBuffer
 * is dispatched to a serial queue so it overlaps the next frame's rendering.
 *
 * macOS GPU path: IOSurface + CGL (desktop OpenGL).
 * iOS GPU path: IOSurface-backed CVPixelBuffer + CVOpenGLESTextureCache (OpenGL ES 3.0).
 *
 * Input: BGRA pixel data (matches OpenFL native BitmapData). Output: H.264/MP4 file.
 */

#import <TargetConditionals.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <string.h>

#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTexture.h>
#import <CoreVideo/CVMetalTextureCache.h>
#if TARGET_OS_OSX
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#else
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#endif

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static const int ERROR_BUF_SIZE = 512;
static const int BYTES_PER_PIXEL = 4;					// BGRA
static const double READY_WAIT_INTERVAL = 0.01;			// seconds per run-loop drain
static const int READY_WAIT_MAX_RETRIES = 500;			// 500 * 0.01 = 5s timeout
static const int ASYNC_READY_WAIT_MAX = 5000;			// 5000 * 1ms = 5s timeout (async path)
static const useconds_t ASYNC_POLL_INTERVAL_US = 1000;	// 1ms poll interval for async path

static AVAssetWriter *writer_ = nil;
static AVAssetWriterInput *writer_input_ = nil;
static AVAssetWriterInputPixelBufferAdaptor *adaptor_ = nil;
static int width_ = 0;
static int height_ = 0;
static int fps_ = 0;
static int frame_index_ = 0;
static char error_buf_[ERROR_BUF_SIZE] = {0};
static const int BUFFER_COUNT = 2;
static CVPixelBufferRef gpu_pixel_buffers_[BUFFER_COUNT] = {NULL, NULL};
static GLuint io_surface_fbos_[BUFFER_COUNT] = {0, 0};
static int current_buf_ = 0;

// Async encoding pipeline state
static dispatch_queue_t encode_queue_ = nil;
static dispatch_semaphore_t buffer_sema_[BUFFER_COUNT] = {NULL, NULL};
static GLsync blit_fence_ = NULL;
static _Atomic bool async_error_ = false;

// Metal copy: GPU blit from IOSurface to fresh pooled CVPixelBuffer.
// Shared across macOS and iOS — prevents encoder B-frame buffer reuse issues.
static id<MTLDevice> mtl_device_ = nil;
static id<MTLCommandQueue> mtl_queue_ = nil;
static CVMetalTextureCacheRef mtl_tex_cache_ = NULL;
static CVPixelBufferRef frame_copy_ = NULL;

/** Create Metal device, command queue, and texture cache for GPU buffer copies. */
static void initMetalCopyResources(void) {
	mtl_device_ = MTLCreateSystemDefaultDevice();
	if (mtl_device_) {
		mtl_queue_ = [mtl_device_ newCommandQueue];
		CVReturn ret = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, mtl_device_, NULL, &mtl_tex_cache_);
		if (ret != kCVReturnSuccess) mtl_tex_cache_ = NULL;
	}
}

#if TARGET_OS_OSX
static IOSurfaceRef io_surfaces_[BUFFER_COUNT] = {nil, nil};
static GLuint io_surface_texs_[BUFFER_COUNT] = {0, 0};
#else
static CVOpenGLESTextureCacheRef tex_cache_ = NULL;
static CVOpenGLESTextureRef cv_textures_[BUFFER_COUNT] = {NULL, NULL};
static GLuint pbo_[BUFFER_COUNT] = {0, 0};	// fallback if Metal not available
#endif

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

/** Release all GPU pixel buffers and platform-specific surface resources. */
static void releaseGpuBuffers(void) {
	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (gpu_pixel_buffers_[i]) {
			CVPixelBufferRelease(gpu_pixel_buffers_[i]);
			gpu_pixel_buffers_[i] = NULL;
		}
#if TARGET_OS_OSX
		if (io_surfaces_[i]) {
			CFRelease(io_surfaces_[i]);
			io_surfaces_[i] = nil;
		}
#endif
	}
	current_buf_ = 0;
}

/** Delete all GL textures/FBOs and release platform texture resources. */
static void releaseGpuFbos(void) {
	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (io_surface_fbos_[i]) {
			glDeleteFramebuffers(1, &io_surface_fbos_[i]);
			io_surface_fbos_[i] = 0;
		}
#if TARGET_OS_OSX
		if (io_surface_texs_[i]) {
			glDeleteTextures(1, &io_surface_texs_[i]);
			io_surface_texs_[i] = 0;
		}
#else
		if (cv_textures_[i]) {
			CFRelease(cv_textures_[i]);
			cv_textures_[i] = NULL;
		}
#endif
	}
#if !TARGET_OS_OSX
	if (tex_cache_) {
		CVOpenGLESTextureCacheFlush(tex_cache_, 0);
		CFRelease(tex_cache_);
		tex_cache_ = NULL;
	}
	for (int i = 0; i < BUFFER_COUNT; i++)
		if (pbo_[i]) {
			glDeleteBuffers(1, &pbo_[i]);
			pbo_[i] = 0;
		}
#endif
	// Metal cleanup (shared macOS/iOS)
	if (frame_copy_) {
		CVPixelBufferRelease(frame_copy_);
		frame_copy_ = NULL;
	}
	if (mtl_tex_cache_) {
		CFRelease(mtl_tex_cache_);
		mtl_tex_cache_ = NULL;
	}
	mtl_queue_ = nil;
	mtl_device_ = nil;
}

/**
 * Shared AVAssetWriter setup: remove existing file, create writer + input + adaptor,
 * start writing session. Sets width_/height_/fps_/frame_index_ on success.
 * On failure, sets error and nils writer_/writer_input_/adaptor_. Returns 0/-1.
 */
static int initAssetWriter(const char *outputPath, int width, int height, int fps, int bitrate, int keyframeInterval) {
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
	NSString *codecType;
	if (@available(macOS 10.13, iOS 11.0, *))
		codecType = AVVideoCodecTypeH264;
	else
		codecType = @"avc1";

	// AVAssetWriter always uses VBR with AverageBitRateKey
	NSDictionary *videoSettings = @{
		AVVideoCodecKey : codecType,
		AVVideoWidthKey : @(width),
		AVVideoHeightKey : @(height),
		AVVideoCompressionPropertiesKey : @{
			AVVideoAverageBitRateKey : @(bitrate),
			AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
			AVVideoExpectedSourceFrameRateKey : @(fps),
			AVVideoMaxKeyFrameIntervalDurationKey : @(keyframeInterval)
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

int videoEncoderInit(const char *outputPath, int width, int height, int fps, int bitrate, int keyframeInterval) {
	@autoreleasepool {
		clearError();

		if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || keyframeInterval <= 0) {
			setError(@"Invalid encoder parameters");
			return -1;
		}

		return initAssetWriter(outputPath, width, height, fps, bitrate, keyframeInterval);
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
		// CFRunLoopRunInMode returns immediately if no sources are registered
		// (e.g. standalone CLI tests), so usleep provides the actual delay.
		int waitRetries = 0;
		while (!writer_input_.isReadyForMoreMediaData) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, READY_WAIT_INTERVAL, true);
			usleep(ASYNC_POLL_INTERVAL_US);
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
			dispatch_sync(
				encode_queue_,
				^{
				}
			);

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
			dispatch_sync(
				encode_queue_,
				^{
				}
			);
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
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	return device != nil ? 1 : 0;
}

int videoEncoderInitGpu(const char *outputPath, int width, int height, int fps, int bitrate, int keyframeInterval) {
	@autoreleasepool {
		clearError();

		if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || keyframeInterval <= 0) {
			setError(@"Invalid encoder parameters");
			return -1;
		}

#if TARGET_OS_OSX
		// macOS: create IOSurfaces directly, then wrap in CVPixelBuffers.
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
#else
		// iOS: create IOSurface-backed CVPixelBuffers via CoreVideo API.
		// CVOpenGLESTextureCache will bind them to GL textures in setupGpuFbo.
		NSDictionary *pbAttrs = @{
			(NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
			(NSString *)kCVPixelBufferWidthKey : @(width),
			(NSString *)kCVPixelBufferHeightKey : @(height),
			(NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
			(NSString *)kCVPixelBufferOpenGLESCompatibilityKey : @YES
		};
		for (int i = 0; i < BUFFER_COUNT; i++) {
			CVReturn cvRet = CVPixelBufferCreate(
				kCFAllocatorDefault,
				width,
				height,
				kCVPixelFormatType_32BGRA,
				(__bridge CFDictionaryRef)pbAttrs,
				&gpu_pixel_buffers_[i]
			);
			if (cvRet != kCVReturnSuccess || !gpu_pixel_buffers_[i]) {
				setError([NSString stringWithFormat:@"CVPixelBufferCreate failed: %d", (int)cvRet]);
				for (int j = 0; j < i; j++) {
					CVPixelBufferRelease(gpu_pixel_buffers_[j]);
					gpu_pixel_buffers_[j] = NULL;
				}
				return -1;
			}
		}
#endif
		current_buf_ = 0;

		if (initAssetWriter(outputPath, width, height, fps, bitrate, keyframeInterval) != 0) {
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
#if TARGET_OS_OSX
	return io_surfaces_[0] ? (unsigned int)IOSurfaceGetID(io_surfaces_[0]) : 0;
#else
	// iOS: texture binding handled by CVOpenGLESTextureCache, surface ID not needed externally
	return 0;
#endif
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

		// Wait for GL fence if no Metal copy was done
		if (!frame_copy_ && blit_fence_) {
			glClientWaitSync(blit_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
			glDeleteSync(blit_fence_);
			blit_fence_ = NULL;
		}

		// If Metal path copied to fresh buffer, use it; otherwise use IOSurface buffer
		const int bufIdx = current_buf_;
		CVPixelBufferRef const pb = frame_copy_ ? frame_copy_ : gpu_pixel_buffers_[bufIdx];
		CVPixelBufferRef const frameCopy = frame_copy_;
		frame_copy_ = NULL;	 // transfer ownership to async block
		const CMTime pt = CMTimeMake(frame_index_, fps_);

		dispatch_async(encode_queue_, ^{
			@autoreleasepool {
				int waitRetries = 0;
				while (!writer_input_.isReadyForMoreMediaData) {
					usleep(ASYNC_POLL_INTERVAL_US);
					if (++waitRetries > ASYNC_READY_WAIT_MAX) {
						setError(@"Async timed out waiting for writer input");
						async_error_ = true;
						dispatch_semaphore_signal(buffer_sema_[bufIdx]);
						return;
					}
				}

				BOOL ok = [adaptor_ appendPixelBuffer:pb withPresentationTime:pt];
				if (!ok) {
					setError([NSString stringWithFormat:@"Async appendPixelBuffer failed: %@", writer_.error.localizedDescription]);
					async_error_ = true;
				}
				// Release fresh buffer copy (encoder retains its own reference)
				if (frameCopy) CVPixelBufferRelease(frameCopy);

				dispatch_semaphore_signal(buffer_sema_[bufIdx]);
			}
		});

		current_buf_ = 1 - current_buf_;
		frame_index_++;
		return 0;
	}
}

int videoEncoderSetupGpuFbo(int width, int height) {
#if TARGET_OS_OSX
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

		GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
			releaseGpuFbos();
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return -1;
		}
	}

	initMetalCopyResources();
#else
	EAGLContext *ctx = [EAGLContext currentContext];
	if (!ctx) return -1;

	// Create texture cache for binding CVPixelBuffers to GL ES textures
	CVReturn cacheRet = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, NULL, ctx, NULL, &tex_cache_);
	if (cacheRet != kCVReturnSuccess || !tex_cache_) {
		setError([NSString stringWithFormat:@"CVOpenGLESTextureCacheCreate failed: %d", (int)cacheRet]);
		return -1;
	}

	for (int i = 0; i < BUFFER_COUNT; i++) {
		if (!gpu_pixel_buffers_[i]) {
			releaseGpuFbos();
			return -1;
		}

		CVReturn texRet = CVOpenGLESTextureCacheCreateTextureFromImage(
			kCFAllocatorDefault,
			tex_cache_,
			gpu_pixel_buffers_[i],
			NULL,
			GL_TEXTURE_2D,
			GL_RGBA,
			(GLsizei)width,
			(GLsizei)height,
			GL_BGRA_EXT,
			GL_UNSIGNED_BYTE,
			0,
			&cv_textures_[i]
		);
		if (texRet != kCVReturnSuccess || !cv_textures_[i]) {
			setError([NSString stringWithFormat:@"CVOpenGLESTextureCacheCreateTextureFromImage failed: %d", (int)texRet]);
			releaseGpuFbos();
			return -1;
		}

		GLuint texName = CVOpenGLESTextureGetName(cv_textures_[i]);
		glBindTexture(GL_TEXTURE_2D, texName);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glGenFramebuffers(1, &io_surface_fbos_[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, io_surface_fbos_[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texName, 0);

		GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
			setError(@"FBO incomplete after CVOpenGLESTexture attachment");
			releaseGpuFbos();
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return -1;
		}
	}

	// Metal can reliably sync IOSurface operations (unlike GL on Metal compat layer).
	initMetalCopyResources();

	// PBO fallback if Metal sync not available
	if (!mtl_tex_cache_) {
		for (int i = 0; i < BUFFER_COUNT; i++) {
			glGenBuffers(1, &pbo_[i]);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
			glBufferData(GL_PIXEL_PACK_BUFFER, width * height * BYTES_PER_PIXEL, NULL, GL_STREAM_READ);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
#endif

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return 0;
}

void videoEncoderBlitGpuFrame(unsigned int srcFbo, int width, int height) {
	if (!buffer_sema_[current_buf_]) {
		setError(@"Blit called before GPU encoder initialized");
		return;
	}

	// Wait for previous async encode of this buffer to complete
	dispatch_semaphore_wait(buffer_sema_[current_buf_], DISPATCH_TIME_FOREVER);

	// GL blit rendered frame to IOSurface FBO
	glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, io_surface_fbos_[current_buf_]);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if (mtl_tex_cache_ && mtl_queue_ && adaptor_) {
		// glFinish ensures GL has fully written the IOSurface before Metal reads it.
		glFinish();
		// Metal copy to fresh CVPixelBuffer from pool.
		// The H.264 encoder holds references to CVPixelBuffers across B-frames
		// (has_b_frames=2). With only 2 IOSurface buffers, the next GL blit
		// overwrites data the encoder is still reading, causing frame jerks.
		// Fix: copy to a fresh pooled buffer via Metal blit (GPU-to-GPU).
		// Metal's waitUntilCompleted provides the sync barrier that GL lacks.

		// Get fresh CVPixelBuffer from encoder pool
		CVPixelBufferRef freshPb = NULL;
		CVReturn poolRet = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, adaptor_.pixelBufferPool, &freshPb);
		if (poolRet != kCVReturnSuccess || !freshPb) {
			setError(@"Failed to get fresh CVPixelBuffer from pool");
			dispatch_semaphore_signal(buffer_sema_[current_buf_]);
			return;
		}

		// Metal blit from IOSurface to fresh buffer
		CVMetalTextureRef srcMtlTex = NULL, dstMtlTex = NULL;
		CVReturn r1 = CVMetalTextureCacheCreateTextureFromImage(
			kCFAllocatorDefault,
			mtl_tex_cache_,
			gpu_pixel_buffers_[current_buf_],
			NULL,
			MTLPixelFormatBGRA8Unorm,
			width,
			height,
			0,
			&srcMtlTex
		);
		CVReturn r2 = CVMetalTextureCacheCreateTextureFromImage(
			kCFAllocatorDefault,
			mtl_tex_cache_,
			freshPb,
			NULL,
			MTLPixelFormatBGRA8Unorm,
			width,
			height,
			0,
			&dstMtlTex
		);

		bool blit_ok = false;
		if (r1 == kCVReturnSuccess && r2 == kCVReturnSuccess && srcMtlTex && dstMtlTex) {
			id<MTLTexture> srcTex = CVMetalTextureGetTexture(srcMtlTex);
			id<MTLTexture> dstTex = CVMetalTextureGetTexture(dstMtlTex);
			id<MTLCommandBuffer> cmdBuf = [mtl_queue_ commandBuffer];
			id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
			[blit copyFromTexture:srcTex
					  sourceSlice:0
					  sourceLevel:0
					 sourceOrigin:MTLOriginMake(0, 0, 0)
					   sourceSize:MTLSizeMake(width, height, 1)
						toTexture:dstTex
				 destinationSlice:0
				 destinationLevel:0
				destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blit endEncoding];
			[cmdBuf commit];
			[cmdBuf waitUntilCompleted];
			blit_ok = true;
		}

		if (srcMtlTex) CFRelease(srcMtlTex);
		if (dstMtlTex) CFRelease(dstMtlTex);

		if (blit_ok) {
			// Store fresh buffer for submitGpuFrame
			if (frame_copy_) CVPixelBufferRelease(frame_copy_);
			frame_copy_ = freshPb;
		} else {
			// Metal texture creation failed — release unused buffer, fall through to IOSurface path
			CVPixelBufferRelease(freshPb);
		}
	}
#if !TARGET_OS_OSX
	else {
		// iOS fallback: PBO readback if Metal not available (slower but correct)
		glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
		size_t rowBytes = width * BYTES_PER_PIXEL;
		size_t totalBytes = rowBytes * height;
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[current_buf_]);
		glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0);
		GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
		glDeleteSync(fence);
		void *pboData = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, totalBytes, GL_MAP_READ_BIT);
		if (pboData) {
			CVPixelBufferLockBaseAddress(gpu_pixel_buffers_[current_buf_], 0);
			void *dst = CVPixelBufferGetBaseAddress(gpu_pixel_buffers_[current_buf_]);
			size_t dstStride = CVPixelBufferGetBytesPerRow(gpu_pixel_buffers_[current_buf_]);
			if (dstStride == rowBytes)
				memcpy(dst, pboData, totalBytes);
			else
				for (int row = 0; row < height; row++)
					memcpy((uint8_t *)dst + row * dstStride, (const uint8_t *)pboData + row * rowBytes, rowBytes);
			CVPixelBufferUnlockBaseAddress(gpu_pixel_buffers_[current_buf_], 0);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
#endif

	// Fence to track blit completion (waited on in submitGpuFrame before dispatch)
	if (blit_fence_) glDeleteSync(blit_fence_);
	blit_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void videoEncoderDisposeGpuFbo(void) {
	releaseGpuFbos();
}

}  // extern "C"
