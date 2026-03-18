#!/bin/bash
# Build and run Android encoder tests on an emulator.
# Usage: ./test/android/run.sh [api-level]
# Example: ./test/android/run.sh 30
# Default API level: 30. Requires ANDROID_HOME and an emulator system image.
#
# First-time setup (downloads ~1GB system image):
#   sdkmanager "system-images;android-30;google_apis;arm64-v8a"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
API=${1:-30}
AVD_NAME="TestEncoder_api${API}"

# Find NDK (prefer numeric version dirs like 28.0.12916984 over legacy android-ndk-r21e)
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
	NDK="$ANDROID_NDK_HOME"
elif [ -d "$ANDROID_HOME/ndk" ]; then
	NDK=$(ls -d "$ANDROID_HOME/ndk"/[0-9]*/ 2>/dev/null | sort -V | tail -1)
	if [ -z "${NDK:-}" ]; then
		NDK=$(ls -d "$ANDROID_HOME/ndk"/*/ 2>/dev/null | sort -V | tail -1)
	fi
else
	echo "Error: no NDK found. Set ANDROID_NDK_HOME or install via sdkmanager." >&2
	exit 1
fi
echo "NDK: $NDK"

# Detect host arch for emulator ABI
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
	ABI="arm64-v8a"
	SYS_IMG="system-images;android-${API};google_apis;arm64-v8a"
else
	ABI="x86_64"
	SYS_IMG="system-images;android-${API};google_apis;x86_64"
fi
echo "ABI: $ABI, API: $API"

# Build test binary for the emulator ABI
# CMAKE_POLICY_VERSION_MINIMUM silences deprecation warnings from NDK toolchain
cmake -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DANDROID_ABI="$ABI" \
	-DANDROID_PLATFORM="android-$API" \
	-S "$SCRIPT_DIR"
cmake --build "$BUILD_DIR"

# Check system image
if ! "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --list_installed 2>/dev/null | grep -q "android-${API}.*${ABI}"; then
	echo "System image not found. Installing $SYS_IMG ..."
	"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" "$SYS_IMG"
fi

# Create AVD if needed
if ! "$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager" list avd -c 2>/dev/null | grep -q "^${AVD_NAME}$"; then
	echo "Creating AVD: $AVD_NAME"
	echo "no" | "$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager" create avd \
		-n "$AVD_NAME" -k "$SYS_IMG" --device "pixel" --force
fi

# Boot emulator in background
echo "Starting emulator..."
"$ANDROID_HOME/emulator/emulator" -avd "$AVD_NAME" -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect &
EMU_PID=$!
cleanup() {
	kill "$EMU_PID" 2>/dev/null || true
	wait "$EMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for device to boot
"$ANDROID_HOME/platform-tools/adb" wait-for-device
echo "Waiting for boot to complete..."
while [ "$("$ANDROID_HOME/platform-tools/adb" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != "1" ]; do
	sleep 2
done
echo "Emulator booted."

# Push and run
"$ANDROID_HOME/platform-tools/adb" push "$BUILD_DIR/android_encoder_test" /data/local/tmp/
"$ANDROID_HOME/platform-tools/adb" shell chmod +x /data/local/tmp/android_encoder_test
"$ANDROID_HOME/platform-tools/adb" shell /data/local/tmp/android_encoder_test
