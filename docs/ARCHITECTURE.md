# RTX VSR architecture

## Scope

The RTX work changes only the Windows SysDVR client. The Switch sysmodule, transport protocol, H.264 decoder, audio path, overlays, and input frame rate remain unchanged.

The decoded source is 1280x720 YUV420P at 30 fps. VSR output can be 1920x1080, 2560x1440, or 3840x2160. Output resolution is passed directly to the NVIDIA feature; the implementation does not create a 1440p result and scale it to 4K.

## Preferred GPU-direct path

```text
FFmpeg AVFrame (YUV420P)
  -> libswscale RGBA8 conversion
  -> D3D11 upload texture
  -> NVIDIA RTX Video Super Resolution
  -> GPU-resident D3D11 output texture
  -> SDL D3D11 render target
  -> window/fullscreen presentation
```

SDL is asked to create its `direct3d11` renderer at application startup. The managed client obtains the renderer's D3D11 device/context and passes them to the native bridge. The bridge verifies that SDL and NGX use the same NVIDIA adapter LUID before enabling GPU-direct interop.

The native output texture stays GPU-resident. The normal path does not create a staging texture, call blocking `Map`, copy output rows to the CPU, or upload an SDL streaming texture. Screenshots use an explicit, exceptional readback path.

## Compatibility path

```text
FFmpeg AVFrame (YUV420P)
  -> libswscale RGBA8 conversion
  -> D3D11 upload texture
  -> NVIDIA RTX Video Super Resolution
  -> staging texture + blocking Map
  -> CPU RGBA8 buffer
  -> SDL RGBA streaming texture upload
  -> presentation
```

This backend is slower and adds a GPU-to-CPU-to-GPU round trip, but remains useful when GPU-direct interop cannot be established.

## Vanilla fallback

Fatal bridge load, adapter, feature creation, device, conversion, processing, or SDL errors disable RTX VSR for the current process. The original IYUV SysDVR texture is updated immediately, leaving the original video and audio paths available.

## Synchronization and latency

GPU-direct CPU submission is asynchronous. A sub-millisecond submission measurement means only that the CPU was not blocked; it does not mean VSR finished in that time. D3D11 timestamp queries measured approximately 6.614 ms of native GPU work for 1440p Medium, 8.406 ms for 1440p High, and 9.185 ms for 4K High on the tested RTX 4060 Laptop GPU.

No extra frame queue or worker-thread buffering is introduced. The VSR work therefore contributes to same-frame presentation latency without deliberately adding another queued frame.

## Color and alpha

- Switch HD video is converted through libswscale using frame range metadata and BT.709 by default.
- Explicit BT.601/FCC metadata is honored.
- The bridge uses `DXGI_FORMAT_R8G8B8A8_UNORM` and assumes SDR, display-referred nonlinear RGB values.
- RGBA output is full-range and alpha is opaque.
- No HDR transfer conversion is performed.

## Native boundary

The locally authored `RtxVideoBridge.dll` exposes a versioned C ABI to managed code. NVIDIA headers and libraries are external build inputs. The public repository contains only the bridge source and its own ABI header.

Detailed API names and benchmark methodology are recorded in the [Phase 2C-2 report](../experiments/RtxVideoBridgeTest/PHASE_2C2_REPORT.md) and [Phase 3 report](../experiments/RtxVideoBridgeTest/PHASE_3_REPORT.md).
