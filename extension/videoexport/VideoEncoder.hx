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

	private static inline final DEFAULT_KEYFRAME_INTERVAL:Int = 2;

	private static final _ve_init: Callable<ConstCharStar -> Int -> Int -> Int -> Int -> Int -> Int> = Prime.load('extension_video_export',
		've_init', 'ciiiiii', false);

	private static final _ve_addFrame: Callable<Object -> Int -> Int> = Prime.load('extension_video_export', 've_addFrame', 'oii', false);

	private static final _ve_finish: Callable<Void -> Int> = Prime.load('extension_video_export', 've_finish', 'i', false);

	private static final _ve_dispose: Callable<Void -> cpp.Void> = Prime.load('extension_video_export', 've_dispose', 'v', false);

	private static final _ve_getError: Callable<Void -> Object> = Prime.load('extension_video_export', 've_getError', 'o', false);

	private static final _ve_supportsGpuInput: Callable<Void -> Int> = Prime.load('extension_video_export', 've_supportsGpuInput', 'i',
		false);

	private static final _ve_initGpu: Callable<ConstCharStar -> Int -> Int -> Int -> Int -> Int -> Int> = Prime.load('extension_video_export',
		've_initGpu', 'ciiiiii', false);

	private static final _ve_getSurfaceId: Callable<Void -> Int> = Prime.load('extension_video_export', 've_getSurfaceId', 'i', false);

	private static final _ve_submitGpuFrame: Callable<Void -> Int> = Prime.load('extension_video_export', 've_submitGpuFrame', 'i', false);

	private static final _ve_setupGpuFbo: Callable<Int -> Int -> Int> = Prime.load('extension_video_export', 've_setupGpuFbo', 'iii', false);

	private static final _ve_blitGpuFrame: Callable<Int -> Int -> Int -> cpp.Void> = Prime.load('extension_video_export', 've_blitGpuFrame',
		'iiiv', false);

	private static final _ve_disposeGpuFbo: Callable<Void -> cpp.Void> = Prime.load('extension_video_export', 've_disposeGpuFbo', 'v',
		false);

	public static inline function init(outputPath: String, width: Int, height: Int, fps: Int,
			bitrate: Int, keyframeInterval: Int = DEFAULT_KEYFRAME_INTERVAL): Bool return _ve_init(outputPath, width, height, fps, bitrate, keyframeInterval) == 0;

	/**
	 * Add a BGRA frame via CPU path. Expects top-down pixel data (first byte = top-left).
	 *
	 * WARNING: raw glReadPixels returns bottom-up data (OpenGL convention). Passing it
	 * directly produces upside-down video on Windows, Android, and Linux.
	 * Either flip rows before calling, or use the GPU path (blitGpuFrame) instead.
	 */
	public static inline function addFrame(bgraPixels: BytesData, dataLength: Int): Bool return _ve_addFrame(bgraPixels, dataLength) == 0;

	public static inline function finish(): Bool return _ve_finish() == 0;

	public static inline function dispose(): Void _ve_dispose();

	public static inline function getError(): Null<String> return _ve_getError();

	/** Whether the platform supports zero-copy GPU texture input. */
	public static inline function supportsGpuInput(): Bool return _ve_supportsGpuInput() != 0;

	/** Initialize encoder in GPU mode. */
	public static inline function initGpu(outputPath: String, width: Int, height: Int, fps: Int,
			bitrate: Int, keyframeInterval: Int = DEFAULT_KEYFRAME_INTERVAL): Bool return _ve_initGpu(outputPath, width, height, fps, bitrate, keyframeInterval) == 0;

	/** Get platform surface ID for binding as GL texture. 0 means no surface. Use != 0 to check validity (not > 0). */
	public static inline function getSurfaceId(): Int return _ve_getSurfaceId();

	/** Submit the current GPU frame (no pixel data — reads from shared surface). */
	public static inline function submitGpuFrame(): Bool return _ve_submitGpuFrame() == 0;

	/** Set up GPU FBO for blit path. Returns true on success. */
	public static inline function setupGpuFbo(width: Int, height: Int): Bool return _ve_setupGpuFbo(width, height) == 0;

	/** Blit from source FBO to encoder surface (GPU-side copy, handles Y-flip internally). */
	public static inline function blitGpuFrame(srcFboId: Int, width: Int, height: Int): Void _ve_blitGpuFrame(srcFboId, width, height);

	/** Dispose GPU FBO resources. */
	public static inline function disposeGpuFbo(): Void _ve_disposeGpuFbo();

}
