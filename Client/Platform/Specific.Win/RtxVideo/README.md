# Experimental Phase 2B/2C-2 managed RTX Video path

This directory contains the Windows-only managed side of the local Phase 2B
prototype. The feature is off by default and runs synchronously on the decoded
frame; it does not add a frame queue or worker thread.

## Frame path

```text
FFmpeg AVFrame (1280x720 YUV420P)
  -> libswscale RGBA8 conversion
  -> RtxVideoBridge.dll CPU input
  -> NVIDIA RTX VSR D3D11 evaluation and CPU readback
  -> SDL_PIXELFORMAT_ABGR8888 streaming texture (selected output size)
  -> existing SDL_RenderCopy / SDL_RenderCopyEx presentation
```

On little-endian Windows, `SDL_PIXELFORMAT_ABGR8888` consumes memory bytes in
R, G, B, A order, matching FFmpeg `AV_PIX_FMT_RGBA` and the bridge's
`DXGI_FORMAT_R8G8B8A8_UNORM` buffers.

## Color assumptions

- FFmpeg frame range metadata is honored (`JPEG` full or `MPEG` limited).
- When range metadata is absent, Switch video is treated as limited range.
- BT.709 is used for normal Switch HD video and as the fallback when matrix
  metadata is absent or ambiguous. Explicit BT.601/FCC metadata is honored.
- RGBA output is full range and libswscale writes opaque alpha.
- No HDR transfer conversion is performed. Phase 2B assumes SDR and preserves
  the decoded nonlinear transfer values for normal SDL display.

## Runtime discovery

`RtxVideoBridge.dll` is loaded explicitly from:

```text
<client>\runtimes\win-x64\native\RtxVideoBridge.dll
```

`SYSDVR_RTX_VIDEO_BRIDGE_PATH` can override that path for local testing.
The official `nvngx_vsr.dll` feature directory is selected in this order:

1. `SYSDVR_RTX_VIDEO_FEATURE_DIR`
2. `RTX_VIDEO_SDK_DIR\bin\Windows\x64\dev`
3. `<client>\runtimes\win-x64\native`

NVIDIA SDK files are not stored by this project.

## Failure and telemetry behavior

Any fatal load, capability, initialization, conversion, processing, device,
or SDL upload error is logged once. RTX VSR is then disabled for the process
session and the original IYUV texture is updated immediately.

Every 120 enhanced frames the client logs per-quality frame count, averages,
median, p95, and maximum for:

- YUV420P to RGBA conversion
- GPU VSR evaluation
- synchronous native bridge call
- staging readback, blocking Map, and CPU row copy
- SDL RGBA texture upload
- complete enhancement path

Supported direct NGX output sizes are 1920x1080, 2560x1440, and 3840x2160.
The default remains 2560x1440 Medium with enhancement disabled. Changing the
output size recreates only the experimental native output/staging resources,
CPU output buffer, and SDL RGBA texture; the decoder, audio path, vanilla IYUV
texture, overlays, window layout, and rotation path are unchanged.
