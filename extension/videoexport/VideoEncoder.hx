package extension.videoexport;

import cpp.Callable;
import cpp.ConstCharStar;
import cpp.Object;
import cpp.Prime;

import haxe.io.BytesData;

/**
 * Cross-platform video encoder backed by a native NDLL.
 * The NDLL contains the correct platform implementation:
 * AVAssetWriter (macOS/iOS), Media Foundation (Windows),
 * AMediaCodec (Android), OpenH264+minimp4 (Linux).
 *
 * Single-instance only — native backends use static globals.
 * Not thread-safe — all calls must happen on the same thread.
 */
@:nullSafety(Strict) final class VideoEncoder {

	private static final _ve_init: Callable<ConstCharStar -> Int -> Int -> Int -> Int -> Int> = Prime.load('extension_video_export',
		've_init', 'ciiiii', false);

	private static final _ve_addFrame: Callable<Object -> Int -> Int> = Prime.load('extension_video_export', 've_addFrame', 'oii', false);

	private static final _ve_finish: Callable<Void -> Int> = Prime.load('extension_video_export', 've_finish', 'i', false);

	private static final _ve_dispose: Callable<Void -> cpp.Void> = Prime.load('extension_video_export', 've_dispose', 'v', false);

	private static final _ve_getError: Callable<Void -> Object> = Prime.load('extension_video_export', 've_getError', 'o', false);

	private static final _ve_supportsGpuInput: Callable<Void -> Int> = Prime.load('extension_video_export', 've_supportsGpuInput', 'i',
		false);

	private static final _ve_initGpu: Callable<ConstCharStar -> Int -> Int -> Int -> Int -> Int> = Prime.load('extension_video_export',
		've_initGpu', 'ciiiii', false);

	private static final _ve_getSurfaceId: Callable<Void -> Int> = Prime.load('extension_video_export', 've_getSurfaceId', 'i', false);

	private static final _ve_submitGpuFrame: Callable<Void -> Int> = Prime.load('extension_video_export', 've_submitGpuFrame', 'i', false);

	private static final _ve_setupIoSurfaceFbo: Callable<Int -> Int -> Int> = Prime.load('extension_video_export', 've_setupIoSurfaceFbo',
		'iii', false);

	private static final _ve_blitToIoSurface: Callable<Int -> Int -> Int -> cpp.Void> = Prime.load('extension_video_export',
		've_blitToIoSurface', 'iiiv', false);

	private static final _ve_disposeIoSurfaceFbo: Callable<Void -> cpp.Void> = Prime.load('extension_video_export',
		've_disposeIoSurfaceFbo', 'v', false);

	public static inline function init(outputPath: String, width: Int, height: Int, fps: Int,
			bitrate: Int): Bool return _ve_init(outputPath, width, height, fps, bitrate) == 0;

	public static inline function addFrame(bgraPixels: BytesData, dataLength: Int): Bool return _ve_addFrame(bgraPixels, dataLength) == 0;

	public static inline function finish(): Bool return _ve_finish() == 0;

	public static inline function dispose(): Void _ve_dispose();

	public static inline function getError(): Null<String> return _ve_getError();

	/** Whether the platform supports zero-copy GPU texture input. */
	public static inline function supportsGpuInput(): Bool return _ve_supportsGpuInput() != 0;

	/** Initialize encoder in GPU mode. Returns IOSurface ID via getSurfaceId(). */
	public static inline function initGpu(outputPath: String, width: Int, height: Int, fps: Int,
			bitrate: Int): Bool return _ve_initGpu(outputPath, width, height, fps, bitrate) == 0;

	/** Get IOSurface ID for binding as GL texture. 0 means no surface. Use != 0 to check validity (not > 0). */
	public static inline function getSurfaceId(): Int return _ve_getSurfaceId();

	/** Submit the current GPU frame (no pixel data — reads from shared surface). */
	public static inline function submitGpuFrame(): Bool return _ve_submitGpuFrame() == 0;

	/** Set up double-buffered IOSurface-backed FBOs for GPU-direct blit. Returns true on success. */
	public static inline function setupIoSurfaceFbo(width: Int, height: Int): Bool return _ve_setupIoSurfaceFbo(width, height) == 0;

	/** Blit from source FBO to IOSurface FBO (GPU-side copy). */
	public static inline function blitToIoSurface(srcFboId: Int, width: Int,
			height: Int): Void _ve_blitToIoSurface(srcFboId, width, height);

	/** Dispose IOSurface FBO GL resources. */
	public static inline function disposeIoSurfaceFbo(): Void _ve_disposeIoSurfaceFbo();

}