#!/bin/bash
# Build and run iOS encoder tests on the Simulator.
# Usage: ./test/ios/run.sh [ios-runtime]
# Example: ./test/ios/run.sh com.apple.CoreSimulator.SimRuntime.iOS-18-4
# Without arguments, uses the latest available iOS runtime.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Pick runtime: argument or latest available
if [ "${1:-}" != "" ]; then
	RUNTIME="$1"
else
	RUNTIME=$(xcrun simctl list runtimes iOS -j | python3 -c "
import sys, json, re
runtimes = json.load(sys.stdin)['runtimes']
ga = [r for r in runtimes if r.get('isAvailable')
      and (m := re.search(r'(\d+)', r.get('version',''))) and int(m.group(1)) < 20]
pick = ga[-1] if ga else runtimes[-1]
print(pick['identifier'])
")
fi
echo "Runtime: $RUNTIME"

# Build
cmake -B "$BUILD_DIR" \
	-DCMAKE_OSX_SYSROOT="$(xcrun --sdk iphonesimulator --show-sdk-path)" \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-S "$SCRIPT_DIR"
cmake --build "$BUILD_DIR"

# Create simulator, ensure cleanup on exit
UDID=$(xcrun simctl create TestEncoder "iPhone 16" "$RUNTIME")
cleanup() {
	xcrun simctl shutdown "$UDID" 2>/dev/null || true
	xcrun simctl delete "$UDID" 2>/dev/null || true
}
trap cleanup EXIT

xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" -b

# Run
xcrun simctl spawn "$UDID" "$BUILD_DIR/ios_encoder_test"
