# Managed RTX Video path

This directory contains the Windows-only managed integration for NVIDIA RTX Video Super Resolution. Enhancement is disabled by default and does not add a frame queue or worker thread.

## GPU-direct frame path

```text
FFmpeg AVFrame (1280x720 YUV420P)
  -> libswscale RGBA8 conversion
  -> RtxVideoBridge.dll upload and RTX VSR evaluation
  -> GPU-resident D3D11 output texture
  -> SDL D3D11 target and existing presentation/overlay path
```

The selected output dimensions control the VSR output texture directly. Supported sizes are 1920x1080, 2560x1440, and 3840x2160.

GPU-direct mode verifies that SDL and NGX use the same NVIDIA DXGI adapter LUID. It avoids the compatibility backend's staging texture, blocking `Map`, CPU row copy, and SDL streaming-texture upload. Screenshots request a deliberate one-frame readback.

## Compatibility frame path

```text
FFmpeg AVFrame (1280x720 YUV420P)
  -> libswscale RGBA8 conversion
  -> RtxVideoBridge.dll CPU input
  -> RTX VSR D3D11 evaluation and staging readback
  -> SDL_PIXELFORMAT_ABGR8888 streaming texture
  -> existing SDL presentation
```

On little-endian Windows, `SDL_PIXELFORMAT_ABGR8888` consumes R, G, B, A bytes, matching FFmpeg `AV_PIX_FMT_RGBA` and `DXGI_FORMAT_R8G8B8A8_UNORM`.

## Runtime discovery

`RtxVideoBridge.dll` is loaded from `<client>\runtimes\win-x64\native`, unless `SYSDVR_RTX_VIDEO_BRIDGE_PATH` overrides it for local testing.

The separately obtained official `nvngx_vsr.dll` feature directory is selected in this order:

1. `SYSDVR_RTX_VIDEO_FEATURE_DIR`
2. `RTX_VIDEO_SDK_DIR\bin\Windows\x64\dev`
3. `<client>\runtimes\win-x64\native`

NVIDIA SDK files are not stored by this project.

## Color assumptions

- Frame range metadata is honored; missing Switch metadata defaults to limited range.
- BT.709 is the normal HD and fallback matrix; explicit BT.601/FCC metadata is honored.
- RGBA output is full-range with opaque alpha.
- No HDR transfer conversion is performed; the path assumes SDR display-referred nonlinear RGB.

## Failure behavior

A fatal load, capability, adapter, initialization, conversion, processing, device, or SDL error is logged once. RTX VSR is then disabled for the process and the original IYUV texture resumes immediately.
