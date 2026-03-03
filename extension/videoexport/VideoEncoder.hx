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

	public static inline function init(outputPath: String, width: Int, height: Int, fps: Int,
			bitrate: Int): Bool return _ve_init(outputPath, width, height, fps, bitrate) == 0;

	public static inline function addFrame(bgraPixels: BytesData, dataLength: Int): Bool return _ve_addFrame(bgraPixels, dataLength) == 0;

	public static inline function finish(): Bool return _ve_finish() == 0;

	public static inline function dispose(): Void _ve_dispose();

	public static inline function getError(): Null<String> return _ve_getError();

}