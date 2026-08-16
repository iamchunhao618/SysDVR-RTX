# Phase 2C-2: configurable RTX VSR output report

Date: 2026-08-16

## Outcome

Phase 2C-2 is complete on the NVIDIA GeForce RTX 4060 Laptop GPU. The
standalone test and live SysDVR path both requested 1920x1080, 2560x1440, and
3840x2160 directly from the official NVIDIA RTX Video SDK. The native output
texture, readback staging texture, CPU RGBA buffer, and SDL RGBA texture all
use the selected size. There is no fixed 1440p intermediate and no added frame
queue.

Recommended everyday setting: **1440p Medium**. Recommended higher-quality
setting on this 1440p display: **1440p High**. VSR remains disabled by default.

## Prerequisites and environment

- GPU: NVIDIA GeForce RTX 4060 Laptop GPU, PCI device `0x28E0`
- NVIDIA driver: 610.62
- NVIDIA-reported memory: 8188 MiB; DXGI dedicated memory: 7956 MiB
- Display used for the downsampling test: 2560x1440
- SDK: official NVIDIA RTX Video SDK 1.1.0
- Feature module: official SDK development `nvngx_vsr.dll`; its visible
  NVIDIA development watermark is expected
- VSR capability: available; no driver update requested
- Bridge ABI: version 2
- Source: fixed 1280x720 at 30 fps, YUV420P in the live test

No unofficial SDK binary was downloaded or added to Git.

## Files changed

Tracked client files modified:

- `Client/Core/Options.cs`
- `Client/Core/StringTable.cs`
- `Client/GUI/OptionsView.cs`
- `Client/Platform/Resources/resources/strings/english.json`
- `Client/Platform/Resources/resources/strings/zh-CN.json`
- `Client/Targets/Player/Player.cs`

Experimental Windows files created or extended:

- `Client/Platform/Specific.Win/RtxVideo/RtxVideoNative.cs`
- `Client/Platform/Specific.Win/RtxVideo/RtxVideoEnhancer.cs`
- `Client/Platform/Specific.Win/RtxVideo/README.md`
- `Client/Platform/Specific.Win/RtxVideoBridge/include/RtxVideoBridge.h`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/RtxVideoBridge.cpp`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/SmokeTest.cpp`
- `Client/Platform/Specific.Win/RtxVideoBridge/CMakeLists.txt`
- `Client/Platform/Specific.Win/RtxVideoBridge/Build-RtxVideoBridge.ps1`
- `Client/Platform/Specific.Win/RtxVideoBridge/README.md`
- `experiments/RtxVideoBridgeTest/src/RtxVsrBenchmark.cpp`
- `experiments/RtxVideoBridgeTest/src/RtxVsrBenchmark.h`
- `experiments/RtxVideoBridgeTest/src/main.cpp`
- `experiments/RtxVideoBridgeTest/Run-Phase2C2-LiveMatrix.ps1`
- `experiments/RtxVideoBridgeTest/README.md`
- `experiments/RtxVideoBridgeTest/PHASE_2C2_REPORT.md`

The existing standalone WIC image helper and CMake project remain isolated
under `experiments/RtxVideoBridgeTest`. Generated binaries, benchmark data,
and official SDK files remain ignored by Git.

## Implementation and UI

The settings UI exposes:

- RTX VSR output resolution: 1080p, 1440p, 2160p / 4K
- RTX VSR quality: Low, Medium, High, Ultra
- Initial state: disabled, 1440p, Medium

Changing resolution destroys and recreates only the VSR output/staging
resources, CPU buffer, and separate SDL RGBA streaming texture. The decoder,
audio path, vanilla IYUV texture, overlay rendering, rotation, and normal SDL
window presentation remain separate.

Window maximize/restore and a deliberately non-16:9 resize were exercised.
The video retained 16:9 with black bars and no crop. A 90-degree rotation was
also exercised: the video rotated with side bars while the overlay stayed in
its native UI orientation. The pre-existing fullscreen implementation was not
changed; its current direct confirmation was interrupted by user interaction,
so this report does not claim a new fullscreen regression test beyond the
unchanged code path.

## NVIDIA and D3D11 APIs used

Names were verified against the installed SDK 1.1.0 headers and samples:

- `NVSDK_NGX_D3D11_Init`
- `NVSDK_NGX_D3D11_GetCapabilityParameters`
- `NVSDK_NGX_Parameter::Get` with
  `NVSDK_NGX_Parameter_VSR_Available`,
  `NVSDK_NGX_Parameter_VSR_NeedsUpdatedDriver`,
  `NVSDK_NGX_Parameter_VSR_MinDriverVersionMajor`,
  `NVSDK_NGX_Parameter_VSR_MinDriverVersionMinor`, and
  `NVSDK_NGX_Parameter_VSR_FeatureInitResult`
- `NVSDK_NGX_D3D11_GetScratchBufferSize`
- `NGX_D3D11_CREATE_VSR_EXT`
- `NGX_D3D11_EVALUATE_VSR_EXT`
- `NVSDK_NGX_D3D11_ReleaseFeature`
- `NVSDK_NGX_D3D11_DestroyParameters`
- `NVSDK_NGX_D3D11_Shutdown1`

The D3D11 path uses `UpdateSubresource`, an RGBA8 input texture, an exact-size
RGBA8 UAV output texture, `CopyResource` to an exact-size staging texture,
timestamp/disjoint queries, blocking `Map(D3D11_MAP_READ)`, and a CPU row copy.
Quality values use the installed `NVSDK_NGX_VSR_QualityLevel` definitions.

## Standalone benchmark methodology

Each resolution/quality used 20 warm-up frames followed by 100 measured
frames. The table cells are **average / median / p95 / maximum**, in
milliseconds. `ProcessFrame` is the complete synchronous native call. GPU
upload, VSR, and staging-copy values come from D3D11 timestamps; Map and row
copy are CPU wall-clock measurements.

### 720p to 1080p

| Quality | Metric | avg / median / p95 / max (ms) |
|---|---|---:|
| Medium | Upload GPU | 0.320 / 0.318 / 0.355 / 0.375 |
| Medium | VSR GPU | 1.923 / 1.868 / 2.249 / 2.404 |
| Medium | GPU to CPU copy | 0.771 / 0.761 / 0.843 / 0.977 |
| Medium | Blocking Map | 3.044 / 3.015 / 3.322 / 3.625 |
| Medium | CPU row copy | 0.499 / 0.482 / 0.627 / 0.688 |
| Medium | ProcessFrame | 3.868 / 3.802 / 4.280 / 4.623 |
| High | Upload GPU | 0.405 / 0.326 / 0.578 / 2.394 |
| High | VSR GPU | 4.348 / 4.263 / 4.661 / 4.983 |
| High | GPU to CPU copy | 0.794 / 0.766 / 0.920 / 1.100 |
| High | Blocking Map | 5.486 / 5.456 / 5.929 / 6.683 |
| High | CPU row copy | 1.085 / 1.113 / 2.010 / 2.546 |
| High | ProcessFrame | 7.405 / 7.465 / 9.586 / 10.167 |
| Ultra | Upload GPU | 0.447 / 0.349 / 0.686 / 1.110 |
| Ultra | VSR GPU | 5.995 / 5.873 / 6.310 / 6.493 |
| Ultra | GPU to CPU copy | 0.782 / 0.770 / 0.840 / 0.919 |
| Ultra | Blocking Map | 7.164 / 7.095 / 7.639 / 9.470 |
| Ultra | CPU row copy | 1.498 / 1.437 / 2.033 / 3.452 |
| Ultra | ProcessFrame | 9.948 / 9.773 / 11.480 / 14.142 |

### 720p to 1440p

| Quality | Metric | avg / median / p95 / max (ms) |
|---|---|---:|
| Medium | Upload GPU | 0.330 / 0.326 / 0.359 / 0.390 |
| Medium | VSR GPU | 1.861 / 1.829 / 2.061 / 2.237 |
| Medium | GPU to CPU copy | 1.241 / 1.223 / 1.359 / 1.493 |
| Medium | Blocking Map | 3.434 / 3.389 / 3.728 / 3.964 |
| Medium | CPU row copy | 1.030 / 0.974 / 1.364 / 1.481 |
| Medium | ProcessFrame | 4.867 / 4.776 / 5.453 / 5.633 |
| High | Upload GPU | 0.476 / 0.366 / 0.886 / 2.070 |
| High | VSR GPU | 4.313 / 4.235 / 4.632 / 4.865 |
| High | GPU to CPU copy | 1.258 / 1.231 / 1.440 / 1.528 |
| High | Blocking Map | 5.924 / 5.905 / 6.389 / 7.015 |
| High | CPU row copy | 3.166 / 3.006 / 4.636 / 5.811 |
| High | ProcessFrame | 10.503 / 10.254 / 12.905 / 16.040 |
| Ultra | Upload GPU | 0.468 / 0.338 / 0.892 / 1.294 |
| Ultra | VSR GPU | 5.948 / 5.840 / 6.228 / 6.262 |
| Ultra | GPU to CPU copy | 1.243 / 1.220 / 1.370 / 1.612 |
| Ultra | Blocking Map | 7.568 / 7.520 / 8.171 / 8.592 |
| Ultra | CPU row copy | 3.081 / 2.860 / 4.568 / 6.116 |
| Ultra | ProcessFrame | 12.011 / 11.831 / 13.781 / 15.558 |

### 720p to 2160p / 4K

| Quality | Metric | avg / median / p95 / max (ms) |
|---|---|---:|
| Medium | Upload GPU | 0.411 / 0.348 / 0.647 / 0.904 |
| Medium | VSR GPU | 2.369 / 2.342 / 2.552 / 3.003 |
| Medium | GPU to CPU copy | 2.996 / 2.928 / 3.257 / 3.682 |
| Medium | Blocking Map | 5.722 / 5.730 / 6.277 / 7.133 |
| Medium | CPU row copy | 7.383 / 7.165 / 8.445 / 15.447 |
| Medium | ProcessFrame | 14.423 / 14.170 / 15.941 / 22.897 |
| High | Upload GPU | 0.475 / 0.368 / 0.943 / 1.872 |
| High | VSR GPU | 4.847 / 4.747 / 5.145 / 5.386 |
| High | GPU to CPU copy | 3.030 / 2.936 / 3.570 / 4.152 |
| High | Blocking Map | 8.274 / 8.180 / 9.080 / 12.331 |
| High | CPU row copy | 7.674 / 7.346 / 9.829 / 14.210 |
| High | ProcessFrame | 17.341 / 17.020 / 19.262 / 24.604 |
| Ultra | Upload GPU | 0.421 / 0.337 / 0.669 / 1.187 |
| Ultra | VSR GPU | 6.461 / 6.346 / 6.778 / 7.083 |
| Ultra | GPU to CPU copy | 3.012 / 2.914 / 3.517 / 4.478 |
| Ultra | Blocking Map | 9.828 / 9.774 / 10.378 / 11.339 |
| Ultra | CPU row copy | 7.275 / 7.153 / 8.347 / 10.777 |
| Ultra | ProcessFrame | 18.504 / 18.317 / 20.100 / 21.374 |

Full machine-readable results, including CPU submission, query-resolution,
total readback, and total GPU timings, are in each resolution's
`benchmark.csv` and `benchmark.json`.

## Complete live SysDVR benchmark

Each row below is a 120-frame live interval. Cells are **average / median /
p95 / maximum**, in milliseconds. `Native process` includes upload, VSR,
readback, and timing resolution; `Readback` is copy submission plus blocking
Map plus CPU row copy. `Total` is the same-frame incremental enhancement path
from YUV conversion through SDL upload. No latency is hidden in a queue.

| Mode | Metric | avg / median / p95 / max (ms) |
|---|---|---:|
| 1080p Medium | YUV to RGBA | 0.298 / 0.264 / 0.375 / 2.162 |
| 1080p Medium | VSR GPU | 3.519 / 2.375 / 5.642 / 6.085 |
| 1080p Medium | Native process | 6.377 / 5.088 / 9.455 / 15.668 |
| 1080p Medium | Readback | 5.801 / 4.468 / 8.650 / 9.174 |
| 1080p Medium | SDL upload | 1.296 / 1.246 / 1.550 / 2.754 |
| 1080p Medium | Total | 8.015 / 7.005 / 10.999 / 22.920 |
| 1080p High | YUV to RGBA | 0.266 / 0.243 / 0.379 / 0.884 |
| 1080p High | VSR GPU | 5.921 / 5.191 / 7.688 / 8.179 |
| 1080p High | Native process | 8.339 / 7.610 / 10.269 / 18.816 |
| 1080p High | Readback | 7.840 / 7.099 / 9.808 / 11.380 |
| 1080p High | SDL upload | 1.263 / 1.213 / 1.501 / 2.313 |
| 1080p High | Total | 9.908 / 9.234 / 11.758 / 24.127 |
| 1440p Medium | YUV to RGBA | 0.289 / 0.274 / 0.416 / 0.947 |
| 1440p Medium | VSR GPU | 3.381 / 4.007 / 4.925 / 5.342 |
| 1440p Medium | Native process | 7.192 / 7.496 / 10.218 / 14.865 |
| 1440p Medium | Readback | 6.682 / 6.966 / 9.694 / 9.986 |
| 1440p Medium | SDL upload | 2.353 / 2.353 / 2.826 / 3.312 |
| 1440p Medium | Total | 9.872 / 9.766 / 12.979 / 21.198 |
| 1440p High | YUV to RGBA | 0.288 / 0.269 / 0.407 / 0.811 |
| 1440p High | VSR GPU | 6.026 / 6.669 / 7.690 / 8.120 |
| 1440p High | Native process | 9.566 / 10.317 / 11.272 / 19.257 |
| 1440p High | Readback | 9.051 / 9.907 / 10.787 / 11.616 |
| 1440p High | SDL upload | 2.324 / 2.323 / 2.822 / 3.748 |
| 1440p High | Total | 12.216 / 12.595 / 14.367 / 25.421 |
| 1440p Ultra | YUV to RGBA | 0.292 / 0.271 / 0.443 / 0.906 |
| 1440p Ultra | VSR GPU | 7.176 / 6.945 / 8.630 / 9.376 |
| 1440p Ultra | Native process | 10.746 / 10.637 / 12.044 / 24.219 |
| 1440p Ultra | Readback | 10.198 / 10.001 / 11.613 / 14.001 |
| 1440p Ultra | SDL upload | 2.345 / 2.292 / 2.924 / 6.472 |
| 1440p Ultra | Total | 13.422 / 13.276 / 14.808 / 30.858 |
| 4K Medium | YUV to RGBA | 0.271 / 0.259 / 0.361 / 0.875 |
| 4K Medium | VSR GPU | 4.169 / 5.106 / 5.965 / 6.864 |
| 4K Medium | Native process | 10.802 / 11.475 / 12.867 / 21.523 |
| 4K Medium | Readback | 10.266 / 11.017 / 12.397 / 13.735 |
| 4K Medium | SDL upload | 5.081 / 4.961 / 5.870 / 8.120 |
| 4K Medium | Total | 16.194 / 16.547 / 18.912 / 32.568 |
| 4K High | YUV to RGBA | 0.269 / 0.259 / 0.348 / 0.893 |
| 4K High | VSR GPU | 6.202 / 6.252 / 10.000 / 10.506 |
| 4K High | Native process | 12.779 / 12.376 / 16.701 / 24.134 |
| 4K High | Readback | 12.305 / 12.012 / 16.315 / 17.282 |
| 4K High | SDL upload | 4.939 / 4.792 / 5.834 / 9.765 |
| 4K High | Total | 18.022 / 17.428 / 22.058 / 36.862 |
| 4K Ultra | YUV to RGBA | 0.263 / 0.248 / 0.338 / 1.051 |
| 4K Ultra | VSR GPU | 7.099 / 6.681 / 9.754 / 13.590 |
| 4K Ultra | Native process | 13.663 / 13.058 / 17.139 / 29.515 |
| 4K Ultra | Readback | 13.171 / 12.666 / 16.694 / 20.706 |
| 4K Ultra | SDL upload | 4.791 / 4.655 / 5.508 / 8.642 |
| 4K Ultra | Total | 18.751 / 18.099 / 22.869 / 41.505 |

## Synchronization and frame-budget assessment

The NGX evaluation call submits asynchronous GPU work. The later staging
texture `Map` is the synchronization point and blocks the same caller until
the upload, VSR evaluation, and copy have completed. Average live Map stalls
were 4.979/7.021 ms at 1080p Medium/High, 5.270/7.604/8.742 ms at 1440p
Medium/High/Ultra, and 7.181/9.202/10.111 ms at 4K Medium/High/Ultra.

The 4K CPU output is about 31.6 MiB per frame. Its average CPU row copy alone
was about 3.1 ms in the live client, while SDL upload was about 4.8-5.1 ms.
This explains why 4K cost grows substantially even though VSR GPU time grows
by much less.

At a 33.3 ms source interval:

- Comfortable typical and p95 margin: 1080p Medium/High and 1440p
  Medium/High/Ultra.
- Near the budget on an observed worst frame: 1440p Ultra (30.858 ms) and 4K
  Medium (32.568 ms).
- Occasional budget misses: 4K High (36.862 ms maximum) and 4K Ultra
  (41.505 ms maximum).

These are same-frame added latencies, not total glass-to-glass latency. Being
below 33.3 ms only avoids a local source-frame budget miss; it does not make
the extra 8-19 ms average latency disappear.

## Visual and temporal observations

All nine requested comparison images were captured from the same Skyrim area
and closely matched camera position after recapture. Moving smoke changes
naturally between frames. The official development DLL watermark is present.

- Medium looked the most natural during the qualitative motion pass.
- High produced somewhat stronger fine stone, wood, foliage, distant-edge,
  weapon-edge, and UI definition without an obvious large artifact in the
  inspected feed.
- Ultra looked more processed and artificially sharp. It has the greatest
  risk of ringing, crawling foliage/texture detail, and temporal instability,
  without a clearly superior gameplay result.
- No gross crop, broken frame, persistent ghost trail, or large halo was
  observed. A subjective live pass and still captures cannot establish that
  small-scale shimmer or crawling is absent, so this is not a frame-by-frame
  temporal-artifact certification.

The evidence does not support assuming Ultra is best. Medium was the most
natural choice; High is the reasonable maximum-quality choice.

## 4K to 1440p downsampling

The connected monitor is 2560x1440. The live 4K modes therefore explicitly
exercised 720p -> SDK VSR 3840x2160 -> SDL downscale to the 1440p display.
Direct 720p -> SDK VSR 2560x1440 was captured from the same scene.

No meaningful visual advantage was apparent from the 4K intermediate on this
1440p display. Compared with direct 1440p, average total enhancement latency
increased by:

| Quality | 1440p avg | 4K then downsample avg | Added average | Added p95 |
|---|---:|---:|---:|---:|
| Medium | 9.872 ms | 16.194 ms | 6.322 ms | 5.933 ms |
| High | 12.216 ms | 18.022 ms | 5.806 ms | 7.691 ms |
| Ultra | 13.422 ms | 18.751 ms | 5.329 ms | 8.061 ms |

The extra readback, row copy, and SDL upload cost is not justified on this
display. Direct 1440p is preferred.

## Captures and raw results

Live captures:

- `results/phase2c2_live/vanilla_720p.png` (1280x720)
- `results/phase2c2_live/vsr_1080p_medium.png` (1920x1080)
- `results/phase2c2_live/vsr_1080p_high.png` (1920x1080)
- `results/phase2c2_live/vsr_1440p_medium.png` (2560x1440)
- `results/phase2c2_live/vsr_1440p_high.png` (2560x1440)
- `results/phase2c2_live/vsr_1440p_ultra.png` (2560x1440)
- `results/phase2c2_live/vsr_2160p_medium.png` (3840x2160)
- `results/phase2c2_live/vsr_2160p_high.png` (3840x2160)
- `results/phase2c2_live/vsr_2160p_ultra.png` (3840x2160)

Standalone results:

- `results/phase2c2_1080p/benchmark.csv` and `benchmark.json`
- `results/phase2c2_1440p/benchmark.csv` and `benchmark.json`
- `results/phase2c2_2160p/benchmark.csv` and `benchmark.json`
- Each directory also contains Bicubic, Low, Medium, High, and Ultra PNGs.

Live logs are in `results/phase2c2_live/live_*.log`. A transient libusb
read/write error occurred during one rapidly restarted 1440p Medium launch;
the connection recovered and the required 120-frame measurement completed.
It was not an RTX VSR feature failure.

## Recommendations

1. Everyday: **1440p Medium**. It gives the best balance of natural image,
   average same-frame cost (9.872 ms), and budget margin on this display.
2. Maximum quality on this 1440p display: **1440p High**. Ultra is slower and
   looks more processed without a decisive improvement.
3. Native 4K-display experiment: **4K High** may be exposed as experimental,
   but its observed 36.862 ms spike means it is not budget-safe on every frame.
4. Do not hide a mode solely on the 120-frame averages. Mark 4K High/Ultra as
   potentially too slow instead; both had individual budget misses. 4K Ultra
   is the first candidate to hide if a strict safe-mode policy is required.
5. Do not use 4K as an intermediate on a 1440p monitor.

## Audio stability

Phase 2C-1 is considered stable relative to the accepted baseline, not
crackle-free. The user observed the same mild severity 1-2 crackle developing
later in the official release/clean client and the RTX prototype, with no
conspicuous VSR-specific escalation. Phase 2C-2 changes no audio code, queue,
batching, or synchronization policy. The matrix logs initialized the normal
1024-sample SDL audio callback and showed the existing video-drop-to-audio
resynchronization behavior. No new RTX-specific audio regression was found.

## Exact build and rerun instructions

```powershell
cd C:\Users\18109\Downloads\SysDVR
$sdk = 'C:\Users\18109\AppData\Local\Temp\SysDVR-RtxVideoResearch-20260816\sdk'

# Native bridge and smoke test (this machine blocks unsigned .ps1 files by default)
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
    -SdkDirectory $sdk -Configuration Release
& .\Client\Platform\Specific.Win\RtxVideoBridge\build\Release\RtxVideoBridgeSmokeTest.exe `
    "$sdk\bin\Windows\x64\dev" 2

# Standalone test
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S .\experiments\RtxVideoBridgeTest `
    -B .\experiments\RtxVideoBridgeTest\build `
    -G 'Visual Studio 17 2022' -A x64 "-DRTX_VIDEO_SDK_DIR=$sdk"
& $cmake --build .\experiments\RtxVideoBridgeTest\build `
    --config Release --parallel

$benchmark = '.\experiments\RtxVideoBridgeTest\build\Release\RtxVideoBridgeTest.exe'
foreach ($resolution in '1080p','1440p','2160p') {
    & $benchmark `
        --feature-dir "$sdk\bin\Windows\x64\dev" `
        --input '.\experiments\RtxVideoBridgeTest\results\input.png' `
        --output-dir ".\experiments\RtxVideoBridgeTest\results\phase2c2_$resolution" `
        --warmup 20 --iterations 100 --quality all `
        --resolution $resolution
}

# Windows client
dotnet restore .\Client\Client.csproj -r win-x64 `
    -p:SysDvrTarget=windows
dotnet publish .\Client\Client.csproj -c Release -r win-x64 `
    --self-contained -p:SysDvrTarget=windows --no-restore

# With the Switch already streaming and the official SDK dev DLL available
& .\experiments\RtxVideoBridgeTest\Run-Phase2C2-LiveMatrix.ps1
```

Published client location:

`Client/bin/Release/net9.0/win-x64/publish/SysDVR-Client.exe`

The live-matrix helper always restores the persisted client setting to VSR
disabled, 1440p, Medium in its `finally` block.

## Final status and next step

- Native bridge smoke test: passed, ABI 2
- Standalone Medium/High/Ultra at 1080p/1440p/4K: all passed
- Live primary matrix: all eight VSR combinations passed 120 measured frames
- Captures: all nine requested files generated at their native selected sizes
- Client build and self-contained Windows publish: passed
- Persisted published setting: VSR disabled, 1440p, Medium
- Git: modified and untracked experimental files remain unstaged; no commit
  was made

The CPU-readback architecture remains viable for the current 720p30 live
prototype at 1080p or 1440p. At 4K it is usable for experimentation but the
large synchronous readback, CPU copy, and SDL upload add too much latency and
produce occasional budget misses at High/Ultra.

Recommended next step, only after explicit approval: decide whether Phase 2C
should be retained as an experimental 1440p Medium/High option. Do not begin
zero-copy or hardware decoding automatically.
