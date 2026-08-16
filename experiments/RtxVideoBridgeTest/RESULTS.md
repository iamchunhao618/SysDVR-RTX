# Phase 2A results

Date: 2026-08-16

This experiment invoked NVIDIA RTX Video Super Resolution directly through
the official RTX Video SDK 1.1.0 D3D11/NGX API. No SysDVR client, decoder,
transport, SDL, or Switch-side source was modified.

## Environment

- GPU: NVIDIA GeForce RTX 4060 Laptop GPU (DXGI adapter 0, device `0x28E0`)
- Driver: 610.62
- Dedicated video memory reported by DXGI: 7956 MiB
- Compiler: MSVC 19.44.35228.0
- Windows SDK: 10.0.26100.0
- Loaded feature module: official SDK 1.1.0 `bin/Windows/x64/dev/nvngx_vsr.dll`
- `VSR.Available`: 1
- `VSR.NeedsUpdatedDriver`: 0
- VSR scratch buffer: 0 bytes

The development feature DLL adds NVIDIA's development watermark to output
images. It is not copied into this repository.

## Pipeline

```text
WIC RGBA8 CPU buffer (1280x720)
  -> ID3D11DeviceContext::UpdateSubresource
  -> ID3D11Texture2D (DXGI_FORMAT_R8G8B8A8_UNORM)
  -> NGX_D3D11_EVALUATE_VSR_EXT
  -> ID3D11Texture2D UAV (2560x1440 RGBA8)
  -> ID3D11DeviceContext::CopyResource
  -> D3D11 staging texture
  -> ID3D11DeviceContext::Map(D3D11_MAP_READ)
  -> CPU RGBA8 buffer
  -> WIC PNG encoder
```

## NVIDIA API calls

- `NVSDK_NGX_D3D11_Init`
- `NVSDK_NGX_D3D11_GetCapabilityParameters`
- `NVSDK_NGX_Parameter::Get(NVSDK_NGX_Parameter_VSR_Available, ...)`
- `NVSDK_NGX_D3D11_GetScratchBufferSize`
- `NGX_D3D11_CREATE_VSR_EXT`
- `NGX_D3D11_EVALUATE_VSR_EXT`
- `NVSDK_NGX_D3D11_ReleaseFeature`
- `NVSDK_NGX_D3D11_Shutdown1`
- `NVSDK_NGX_D3D11_DestroyParameters`

Quality values are the exact `NVSDK_NGX_VSR_QualityLevel` values from
`nvsdk_ngx_defs_vsr.h`: Bicubic=0, Low=1, Medium=2, High=3, Ultra=4.

## Timings

Each quality used 20 warm-up frames followed by 100 measured frames. D3D11
timestamp queries measure GPU work. CPU wall clocks measure submission, the
blocking staging-texture map, row copy, and complete `ProcessFrame` latency.

| Quality | GPU upload avg | GPU VSR avg | GPU copy avg | CPU Map wait avg | Process avg | Process median | Process p95 | Process max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Bicubic | 0.327 ms | 0.653 ms | 1.392 ms | 2.404 ms | 3.719 ms | 3.699 ms | 4.124 ms | 4.437 ms |
| Low | 0.326 ms | 1.534 ms | 1.367 ms | 3.236 ms | 4.705 ms | 4.690 ms | 5.122 ms | 5.213 ms |
| Medium | 0.329 ms | 1.982 ms | 1.381 ms | 3.703 ms | 5.248 ms | 5.256 ms | 5.816 ms | 6.005 ms |
| High | 0.328 ms | 4.488 ms | 1.392 ms | 6.223 ms | 7.861 ms | 7.839 ms | 8.291 ms | 8.938 ms |
| Ultra | 0.331 ms | 6.203 ms | 1.399 ms | 7.963 ms | 9.692 ms | 9.643 ms | 10.222 ms | 10.545 ms |

Full summary metrics are in `results/benchmark.csv` and
`results/benchmark.json`.

## Synchronization result

`NGX_D3D11_EVALUATE_VSR_EXT` only occupied the CPU for 0.080-0.134 ms on
average, so the D3D11 evaluation call submits work asynchronously. The later
`Map` is the synchronization barrier. For Ultra, `Map` blocked for 7.963 ms
on average while total queued GPU work was 7.933 ms. It therefore waits for
the preceding upload, VSR evaluation, and GPU copy to finish.

The final CPU row copy of the mapped 2560x1440 RGBA image cost 1.007-1.309 ms
on average. No buffering or extra frame queue was used, so the measurements
represent same-frame incremental latency rather than hidden queue latency.

## Color and alpha

Input and output textures use `DXGI_FORMAT_R8G8B8A8_UNORM`; WIC decodes to
RGBA byte order and converts to BGRA only at the final Windows PNG encoder
boundary. Visual inspection confirmed correct red/blue channel order. Every
output alpha byte was 255.

`UNORM` does not itself declare a transfer function. This test treats the
image as 8-bit SDR display-referred RGB, matching the NVIDIA D3D11 sample.
Future SysDVR integration must convert YUV420P using the frame's correct
BT.709 matrix and limited/full range into display-referred RGB. It should set
alpha to 255 rather than depend on VSR preserving meaningful transparency.

## Failure paths

| Condition | Result |
|---|---|
| Official `nvngx_vsr.dll` absent | Tested; exit 10 with the exact missing path |
| Intel adapter selected | Tested; exit 11 with adapter name and vendor ID |
| VSR unsupported or driver too old | Capability path implemented; not physically reproducible on this supported machine |
| Feature creation failure | NGX result and `VSR.FeatureInitResult` reporting implemented; successful feature creation on this machine |
| Device loss/reset | `GetDeviceRemovedReason` checked after processing and on Map/evaluation failures; destructive removal was not induced |

## Build and run

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$sdk = 'C:\Users\18109\AppData\Local\Temp\SysDVR-RtxVideoResearch-20260816\sdk'

cd C:\Users\18109\Downloads\SysDVR\experiments\RtxVideoBridgeTest
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 "-DRTX_VIDEO_SDK_DIR=$sdk"
& $cmake --build build --config Release --parallel

.\build\Release\RtxVideoBridgeTest.exe `
  --feature-dir "$sdk\bin\Windows\x64\dev" `
  --prepare-input '..\..\.github\images\Screenshot.jpg' `
  --input '.\results\input.png' `
  --output-dir '.\results' `
  --warmup 20 `
  --iterations 100 `
  --quality all
```

## Conclusion

The CPU-readback architecture is viable for a first live 720p30 SysDVR
prototype on this RTX 4060 Laptop GPU. It adds about 5.25 ms at Medium,
7.86 ms at High, or 9.69 ms at Ultra before the later SysDVR YUV conversion
and SDL upload costs are included. Medium or High is the safer initial live
setting; Ultra should remain optional until end-to-end measurement.

The next step, only after approval, is to turn this processor into a small
C ABI bridge DLL while preserving the same synchronous, queue-free behavior.
It should not begin zero-copy or SysDVR integration automatically.
