# extension-video-export

Cross-platform H.264/MP4 video encoder for [OpenFL](https://www.openfl.org/) / hxcpp.

Encode `BitmapData` frames into an MP4 file using native platform APIs — no external processes, no FFmpeg dependency.

## Platform backends

| Platform | Backend | Notes |
|----------|---------|-------|
| macOS / iOS | AVFoundation (AVAssetWriter) | BGRA direct, GPU path (macOS) |
| Windows | Media Foundation (IMFSinkWriter) | BGRA direct |
| Android | NDK AMediaCodec + AMediaMuxer | BGRA to NV12 |
| Linux | OpenH264 + minimp4 | BGRA to I420 |

## Minimum platform versions

| Platform | Minimum version | Limiting API |
|----------|-----------------|--------------|
| macOS (x64) | 10.13 High Sierra | `AVVideoCodecTypeH264` |
| macOS (ARM64) | 11.7 Big Sur | First macOS on Apple Silicon |
| iOS | 11.0 | `AVVideoCodecTypeH264` |
| Windows | 7 | Media Foundation SinkWriter |
| Android | API 21 (5.0 Lollipop) | NDK AMediaCodec / AMediaMuxer |
| Linux | Any | Requires `libopenh264` at runtime |

## Installation

```bash
haxelib install extension-video-export
```

Then add the dependency to your `project.xml`:

```xml
<haxelib name="extension-video-export" />
```

## Usage

```haxe
import extension.videoexport.VideoEncoder;

// Initialize encoder: output path, width, height, fps, bitrate
VideoEncoder.init("output.mp4", 1280, 720, 30, 4000000);

// Feed BGRA frames (matches OpenFL BitmapData layout)
var bitmapData = getBitmapData();
var pixels = bitmapData.getPixels(bitmapData.rect);
VideoEncoder.addFrame(pixels.getData(), pixels.length);

// Finalize and release resources
VideoEncoder.finish();
VideoEncoder.dispose();
```

### API

| Method | Signature | Returns |
|--------|-----------|---------|
| `init` | `(path, width, height, fps, bitrate)` | `Bool` — true on success |
| `addFrame` | `(bgraPixels, dataLength)` | `Bool` — true on success |
| `finish` | `()` | `Bool` — true on success |
| `dispose` | `()` | `Void` |
| `getError` | `()` | `Null<String>` — last error message |

All input must be **BGRA** pixel data. Single-instance, not thread-safe — call everything from the same thread.

### GPU path (macOS only)

Zero-copy encoding via IOSurface — the GPU renders directly into a surface shared with the encoder, avoiding `glReadPixels`. Double-buffered: GPU writes to one surface while the encoder reads the other. Encoding runs asynchronously on a serial dispatch queue so it overlaps the next frame's rendering.

```haxe
if (VideoEncoder.supportsGpuInput()) {
    VideoEncoder.initGpu("output.mp4", 1280, 720, 30, 4000000);
    VideoEncoder.setupIoSurfaceFbo(1280, 720);

    // Per frame: blit from your FBO, then submit
    VideoEncoder.blitToIoSurface(myFboId, 1280, 720);
    VideoEncoder.submitGpuFrame();

    // Finalize
    VideoEncoder.finish();
    VideoEncoder.disposeIoSurfaceFbo();
    VideoEncoder.dispose();
}
```

| Method | Signature | Returns |
|--------|-----------|---------|
| `supportsGpuInput` | `()` | `Bool` — true if GPU path available |
| `initGpu` | `(path, width, height, fps, bitrate)` | `Bool` — true on success |
| `getSurfaceId` | `()` | `Int` — IOSurface ID (0 = none) |
| `submitGpuFrame` | `()` | `Bool` — true on success |
| `setupIoSurfaceFbo` | `(width, height)` | `Bool` — true on success |
| `blitToIoSurface` | `(srcFboId, width, height)` | `Void` |
| `disposeIoSurfaceFbo` | `()` | `Void` |

## Building from source

### Prerequisites

| Platform | Requirement |
|----------|-------------|
| macOS | Xcode (AVFoundation, IOSurface, OpenGL) |
| Windows | MSVC (Media Foundation) |
| Linux | `libopenh264-dev` |
| Android | NDK r26c+ |

### IDE support (optional)

For clangd-based IDEs (Zed, VS Code with clangd, Neovim LSP), create a symlink so the LSP can find hxcpp headers:

```bash
ln -sfn "$(haxelib path hxcpp | head -1)include" .hxcpp-include
```

### Build the NDLL

```bash
# Using lime (recommended for CI / cross-platform)
haxelib run lime rebuild . mac -release

# Using hxcpp directly (local development)
cd project && haxelib run hxcpp Build.xml && cd ..
```

### Run tests

```bash
haxe test.hxml
./test/bin/TestEncode
```

## License

MIT
