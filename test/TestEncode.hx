import DllSetup;

import extension.videoexport.VideoEncoder;

import haxe.io.Bytes;

/** Functional smoke test for the video encoder NDLL. */
@:nullSafety(Strict) final class TestEncode {

	private static final ALIGNED_SIZE: Int = 64;		// divisible by 16 — fully SIMD
	private static final NON_ALIGNED_SIZE: Int = 62;  // not divisible by 8 or 16 — scalar tails
	private static final FPS: Int = 30;
	private static final BITRATE: Int = 500000;
	private static final FRAME_COUNT: Int = 30;
	private static final MIN_OUTPUT_SIZE: Int = 100;
	private static final BYTES_PER_PIXEL: Int = 4;	   // BGRA
	private static final OUTPUT: String = 'test_output.mp4';

	public static function main(): Void {
		encodeAndVerify(ALIGNED_SIZE, ALIGNED_SIZE, 'aligned');
		encodeAndVerify(NON_ALIGNED_SIZE, NON_ALIGNED_SIZE, 'non-aligned');

		Sys.println('ALL TESTS PASSED');
	}

	private static function encodeAndVerify(width: Int, height: Int, label: String): Void {
		Sys.println('--- $label (${width}x$height) ---');
		check(VideoEncoder.init(OUTPUT, width, height, FPS, BITRATE), '$label init');

		final frameSize: Int = width * height * BYTES_PER_PIXEL;
		final frame: Bytes = Bytes.alloc(frameSize);
		for (i in 0...FRAME_COUNT)
			check(VideoEncoder.addFrame(frame.getData(), frameSize), '$label addFrame #$i');

		check(VideoEncoder.finish(), '$label finish');
		VideoEncoder.dispose();

		final size: Int = sys.FileSystem.stat(OUTPUT).size;
		Sys.println('  OK   output: $size bytes');
		if (size < MIN_OUTPUT_SIZE)
			fail('$label output file too small: $size bytes');

		sys.FileSystem.deleteFile(OUTPUT);
	}

	private static function check(ok: Bool, name: String): Void {
		if (!ok)
			fail('$name failed: ${VideoEncoder.getError()}');
		Sys.println('  PASS $name');
	}

	private static function fail(msg: String): Void {
		Sys.println('FAIL: $msg');
		Sys.exit(1);
	}

}