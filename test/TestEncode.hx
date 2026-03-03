import DllSetup;

import extension.videoexport.VideoEncoder;

import haxe.io.Bytes;

/** Functional smoke test for the video encoder NDLL. */
@:nullSafety(Strict) final class TestEncode {

	private static final WIDTH: Int = 64;
	private static final HEIGHT: Int = 64;
	private static final FPS: Int = 30;
	private static final BITRATE: Int = 500000;
	private static final FRAME_COUNT: Int = 30;
	private static final MIN_OUTPUT_SIZE: Int = 100;
	private static final OUTPUT: String = 'test_output.mp4';

	public static function main(): Void {
		check(VideoEncoder.init(OUTPUT, WIDTH, HEIGHT, FPS, BITRATE), 'init');

		final frameSize: Int = WIDTH * HEIGHT * 4;
		final frame: Bytes = Bytes.alloc(frameSize);
		for (i in 0...FRAME_COUNT)
			check(VideoEncoder.addFrame(frame.getData(), frameSize), 'addFrame #$i');

		check(VideoEncoder.finish(), 'finish');
		VideoEncoder.dispose();

		final size: Int = sys.FileSystem.stat(OUTPUT).size;
		Sys.println('  OK   output: $size bytes');
		if (size < MIN_OUTPUT_SIZE)
			fail('output file too small: $size bytes');

		sys.FileSystem.deleteFile(OUTPUT);
		Sys.println('ALL TESTS PASSED');
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