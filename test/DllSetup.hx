/** Adds ndll/ to the DLL search path before any NDLL is loaded. */
@:nullSafety(Strict) final class DllSetup {

	public static function __init__(): Void {
		cpp.Lib.pushDllSearchPath('ndll/' + cpp.Lib.getBinDirectory());
	}

}