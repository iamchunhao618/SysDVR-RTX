# Phase 3 — GPU-resident RTX VSR presentation

Date: 2026-08-16

Machine: NVIDIA GeForce RTX 4060 Laptop GPU

Driver: NVIDIA 610.62 (`32.0.16.1062`)
Source: 1280×720 YUV420P at 30 fps over the existing SysDVR path

## Decision

The GPU-direct path is clearly better and should become the preferred RTX
backend after a short period as an opt-in experimental setting. The compatible
CPU-readback backend remains the saved default in Phase 3, and vanilla IYUV
remains the final fallback.

Normal GPU-direct frames eliminate the staging copy, blocking `Map`, CPU output
row-copy, CPU RGBA output traffic, `SDL_UpdateTexture` of the enhanced frame,
and the second GPU upload. At 1440p the measured CPU-side enhancement duration
fell from 9.501 ms to 0.755 ms at Medium and from 12.041 ms to 0.709 ms at High.
RTX VSR still consumes real GPU time; it did not become “free.”

## 1–2. Checkpoint and branch

- Known-good Phase 2C-2 checkpoint:
  `0371d7ac1acfd1abfa33c94916055f1c5cc7cdbc`
- Commit message: `Add experimental RTX Video Super Resolution support`
- Phase 3 branch: `codex/rtx-vsr-gpu-direct`
- Generated executables, DLLs, PNGs, logs, benchmark results, and NVIDIA SDK
  binaries were not included in the checkpoint.

## 3. Phase 2C-2 live baseline

Method: 20 warm-up frames followed by 120 measured frames. Values below are
`average / median / p95 / maximum` in milliseconds.

| Metric | 1440p Medium | 1440p High |
|---|---:|---:|
| FFmpeg receive | 0.385 / 0.006 / 1.425 / 13.276 | 0.235 / 0.007 / 1.201 / 10.514 |
| YUV420P→RGBA | 0.280 / 0.269 / 0.395 / 0.549 | 0.294 / 0.276 / 0.396 / 0.834 |
| Bridge host call | 6.849 / 7.138 / 8.082 / 8.630 | 9.400 / 9.432 / 10.970 / 15.425 |
| Input upload, CPU submit | 0.172 / 0.165 / 0.231 / 0.329 | 0.188 / 0.171 / 0.280 / 0.519 |
| Input upload, GPU | 0.330 / 0.319 / 0.392 / 0.480 | 0.316 / 0.309 / 0.356 / 0.369 |
| VSR call, CPU submit | 0.171 / 0.159 / 0.247 / 0.338 | 0.236 / 0.160 / 0.311 / 6.772 |
| VSR, GPU | 3.094 / 3.485 / 4.229 / 4.868 | 5.656 / 5.791 / 6.746 / 7.250 |
| Output CopyResource, GPU | 1.323 / 1.302 / 1.441 / 1.923 | 1.317 / 1.220 / 1.750 / 1.954 |
| Total native GPU | 4.748 / 5.157 / 5.872 / 6.438 | 7.288 / 7.328 / 8.727 / 9.361 |
| Blocking Map | 4.876 / 5.253 / 6.065 / 6.532 | 7.388 / 7.418 / 8.781 / 9.389 |
| CPU output row-copy | 1.452 / 1.429 / 1.750 / 1.869 | 1.452 / 1.427 / 1.704 / 2.110 |
| Total readback CPU | 6.329 / 6.641 / 7.528 / 8.100 | 8.841 / 8.897 / 10.199 / 10.684 |
| SDL enhanced upload | 2.366 / 2.382 / 2.685 / 3.349 | 2.339 / 2.343 / 2.758 / 3.269 |
| Complete enhancement host path | 9.501 / 9.905 / 10.869 / 11.596 | 12.041 / 12.079 / 13.452 / 18.321 |
| SDL video submit | 0.003 / 0.003 / 0.004 / 0.016 | 0.004 / 0.004 / 0.005 / 0.007 |
| UI build | 0.064 / 0.053 / 0.118 / 0.324 | 0.056 / 0.052 / 0.104 / 0.140 |
| UI submit | 0.007 / 0.007 / 0.009 / 0.013 | 0.008 / 0.007 / 0.012 / 0.028 |
| SDL_RenderPresent | 1.407 / 1.389 / 1.757 / 2.283 | 1.573 / 1.518 / 2.058 / 2.755 |
| Decoded-frame CPU timestamp→Present return | 10.935 / 11.232 / 12.531 / 13.287 | 13.643 / 13.566 / 15.227 / 19.836 |

The realistically removable readback plus SDL upload averaged 8.695 ms at
Medium and 11.180 ms at High, comfortably exceeding the Phase 3 stop threshold.

Logs:

- `results/phase3_baseline/baseline_1440p_medium.log`
- `results/phase3_baseline/baseline_1440p_high.log`

## 4. SDL D3D11 findings

- Bundled runtime: SDL 2.28.5 (`SDL-2.28.5-no-vcs`), file version 2.28.5.0.
- SHA-256:
  `15B3A9BF8069F862B99ADE425D0E906444B0C6DFFEF4E8CA47FED33BAE3856A0`.
- The C# binding snapshot contains the required declarations. The actual DLL
  exports both `SDL_RenderGetD3D11Device` and `SDL_RenderFlush`.
- Phase 2 used SDL's `direct3d` (D3D9) renderer. GPU direct explicitly requests
  the `direct3d11` renderer before `SDL_CreateRenderer`.
- `SDL_RenderGetD3D11Device` is public since SDL 2.0.16 and returns an AddRef'd
  `ID3D11Device`; the managed caller releases that returned reference.
- `SDL_RenderFlush` is public since SDL 2.0.10 and is the documented boundary
  for mixing low-level graphics calls with SDL rendering.
- Verified SDL 2.28.5 source behavior: DXGI adapter 0, D3D11 feature-level
  negotiation, BGRA8 (`DXGI_FORMAT_B8G8R8A8_UNORM`) swapchain, two buffers,
  maximum frame latency 1, and `ResizeBuffers` on window-size changes.
- SDL's clear command calls `ClearRenderTargetView` but does not rebind the RTV
  after a flip. A harmless offscreen `SDL_RenderDrawPoint` is queued before the
  public flush so this verified backend leaves its current window target bound.
  No private `SDL_Renderer` or `SDL_Texture` memory layout is accessed.
- The bridge obtains the current RTV with `OMGetRenderTargets`; it never owns or
  exposes the SDL swapchain.

## 5–7. Adapter identity and same-device NGX

The live log reported:

- SDL D3D11 adapter: `NVIDIA GeForce RTX 4060 Laptop GPU`
- Vendor/device: `0x10DE / 0x28E0`
- SDL adapter LUID: `00000000:0001D122`
- NGX probe adapter LUID: `00000000:0001D122`
- D3D feature level: `0xB100` (11.1)

The exact LUID match proves the test did not silently cross from an Intel GPU to
the NVIDIA GPU. A mismatch, non-NVIDIA vendor, software adapter, or sub-11.0
device rejects GPU direct and selects CPU readback.

Official SDK headers/samples confirm `NVSDK_NGX_D3D11_Init` accepts an
application-supplied `ID3D11Device`. VSR feature creation and evaluation worked
on SDL's device. The bridge AddRefs the supplied device, uses its immediate
context, calls `NVSDK_NGX_D3D11_Shutdown1(device)` before releasing its own COM
references, and never resets, destroys, or assumes ownership of the SDL device.

## 8–9. Architectures considered and selected

| Architecture | Feasibility and result |
|---|---|
| A — Phase 2 CPU readback | Known-good, lowest interop risk, but blocks and copies 14.7 MB per 1440p frame before uploading it again. Retained as fallback. |
| B — same-device raw copy | Same-device resources are feasible, but a raw copy cannot implement letterboxing, scaling, and rotation, and SDL exposes no public backbuffer accessor. Not selected. |
| C — native D3D11 video pass before SDL UI | Selected. Public SDL flush, verified current RTV, a small textured draw, complete D3D11.1 context-state swap/restore, then normal SDL/ImGui UI. Meaningful savings with limited code surface. |
| D — wrap an existing D3D11 texture as SDL_Texture | SDL 2.28.5 has no public API for this. Private-structure hacks were rejected. |

The selected ordering is:

1. SDL clear/background
2. `SDL_RenderFlush`
3. native VSR output shader draw
4. restore SDL's complete D3D11 context state
5. existing ImGui SDL renderer
6. existing `SDL_RenderPresent`

`ID3D11Device1::CreateDeviceContextState` and
`ID3D11DeviceContext1::SwapDeviceContextState` isolate NGX/native pipeline
state. No SDL renderer internals are modified or invalidated.

## 10. Files added or modified

Phase 3 implementation/instrumentation:

- `Client/App/ClientApp.cs`
- `Client/Core/Options.cs`
- `Client/Core/StringTable.cs`
- `Client/GUI/Components/SDLContext.cs`
- `Client/GUI/Interfaces.cs`
- `Client/GUI/OptionsView.cs`
- `Client/GUI/PlayerView.cs`
- `Client/Platform/Specific.Win/RtxVideo/RtxVideoEnhancer.cs`
- `Client/Platform/Specific.Win/RtxVideo/RtxVideoNative.cs`
- `Client/Platform/Specific.Win/RtxVideoBridge/CMakeLists.txt`
- `Client/Platform/Specific.Win/RtxVideoBridge/include/RtxVideoBridge.h`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/RtxVideoBridge.cpp`
- `Client/Targets/Player/Player.cs`
- `Client/Targets/SDLCapture.cs`
- `experiments/RtxVideoBridgeTest/Run-Phase3-Baseline.ps1`
- `experiments/RtxVideoBridgeTest/Run-Phase3-GpuDirect.ps1`
- `experiments/RtxVideoBridgeTest/PHASE_3_REPORT.md`

## 11. Native bridge ABI changes

The C ABI is version 3. New exports are:

- `rvb_create_with_d3d11_device`
- `rvb_get_d3d11_device_info`
- `rvb_process_rgba8_gpu`
- `rvb_render_output_d3d11`
- `rvb_readback_output_rgba8`

`rvb_process_rgba8_gpu` uploads the CPU RGBA input and evaluates VSR but leaves
the output in an SRV/UAV-capable `ID3D11Texture2D`. The native render export
consumes it without exposing a COM pointer to C#. `rvb_readback_output_rgba8` is
reserved for an explicit screenshot/debug capture and is not called on normal
frames. All entry points retain status/error translation; C++ exceptions never
cross the C ABI.

## 12–16. Elimination and before/after timing

Normal GPU-direct frames measured all of the following at 0.000 ms:

- output staging `CopyResource`
- blocking CPU `Map`
- CPU output row-copy
- SDL enhanced-frame upload
- CPU blocking wait inside the bridge

GPU-direct results use 20 warm-up + 120 measured frames. Values are
`average / median / p95 / maximum` in milliseconds.

| Metric | 1440p Medium GPU direct | 1440p High GPU direct |
|---|---:|---:|
| YUV420P→RGBA | 0.276 / 0.261 / 0.374 / 0.482 | 0.264 / 0.256 / 0.350 / 0.392 |
| Bridge CPU submit | 0.450 / 0.418 / 0.677 / 0.902 | 0.417 / 0.406 / 0.517 / 0.634 |
| Input upload, GPU | 0.586 / 0.604 / 0.653 / 0.905 | 0.472 / 0.348 / 0.416 / 13.128 |
| VSR, GPU | 5.986 / 6.159 / 8.335 / 9.991 | 7.908 / 8.480 / 10.340 / 15.795 |
| Native output draw, CPU submit | 0.004 / 0.004 / 0.004 / 0.016 | 0.004 / 0.004 / 0.005 / 0.029 |
| Native output draw, GPU | 0.042 / 0.042 / 0.054 / 0.108 | 0.025 / 0.027 / 0.032 / 0.057 |
| Total native GPU | 6.614 / 6.815 / 9.064 / 10.656 | 8.406 / 8.840 / 10.791 / 23.306 |
| Complete enhancement CPU submit path | 0.755 / 0.728 / 1.038 / 1.357 | 0.709 / 0.686 / 0.904 / 1.001 |
| SDL/native video submit | 0.025 / 0.020 / 0.035 / 0.270 | 0.023 / 0.020 / 0.031 / 0.270 |
| UI build | 0.054 / 0.045 / 0.111 / 0.131 | 0.053 / 0.047 / 0.094 / 0.255 |
| UI submit | 0.009 / 0.009 / 0.015 / 0.033 | 0.008 / 0.009 / 0.012 / 0.021 |
| SDL_RenderPresent CPU call | 0.263 / 0.281 / 0.455 / 0.715 | 0.098 / 0.070 / 0.190 / 1.677 |
| Frame interval | 32.727 / 27.879 / 55.326 / 56.584 | 33.316 / 27.698 / 55.518 / 56.269 |

| Comparison | Medium | High |
|---|---:|---:|
| CPU enhancement average, Phase 2 | 9.501 ms | 12.041 ms |
| CPU enhancement average, Phase 3 | 0.755 ms | 0.709 ms |
| CPU-thread reduction | **8.746 ms (92.1%)** | **11.332 ms (94.1%)** |
| Blocking Map before / after | 4.876 / **0.000 ms** | 7.388 / **0.000 ms** |
| Readback + SDL upload before / after | 8.695 / **0.000 ms** | 11.180 / **0.000 ms** |

The D3D11 timestamp results normally become readable during the following
source frame (median latency 1 query frame) using `D3D11_ASYNC_GETDATA_DONOTFLUSH`.
The CPU does not wait for those queries. The approximately 0.7 ms host path is
only submission time; the actual Medium/High GPU work averaged 6.614/8.406 ms.

Content, shared-device contention, and laptop power state make the direct GPU
numbers vary more than the self-owned-device baseline. A conservative estimate
of same-frame incremental processing is conversion plus total native GPU:
about 6.9 ms at Medium and 8.7 ms at High on the primary run. This remains below
the corresponding 9.5/12.0 ms blocking Phase 2 enhancement path, without adding
a frame queue.

Logs:

- `results/phase3_gpu_direct/gpu_direct_1440p_medium.log`
- `results/phase3_gpu_direct/gpu_direct_1440p_high.log`

## 17. End-to-end latency

The CPU timestamp from decoded-frame availability through `Present` return fell
to roughly 1 ms, but this is not presented as display latency because GPU work
continues asynchronously. GPU timestamps show the enhanced draw completing
roughly 6.9/8.7 ms after conversion begins in the primary Medium/High runs.

A camera test is prepared but was not fabricated from CPU timings:

1. Lock a 240 fps camera so the Switch-visible event/controller LED and PC
   display are in the same shot.
2. Use the same scene and input for vanilla, 1440p Medium CPU, 1440p Medium
   direct, and 1440p High direct.
3. Record at least 20 trials per mode.
4. Count frames from the physical/visible trigger to the first changed PC scan;
   divide by 240 and report median/p95.
5. Restart the client between CPU and direct because SDL's render driver is
   selected at process startup.

## 18–20. Resolution/quality results

- **1440p Medium:** successful; 0.755 ms average CPU submission and 6.614 ms
  average native GPU work. Best everyday balance.
- **1440p High:** successful; 0.709 ms average CPU submission and 8.406 ms
  average native GPU work. Recommended maximum quality on the 1440p display.
- **4K High:** successful; 0.672 ms average CPU submission, 8.786 ms VSR GPU,
  0.048 ms direct draw GPU, and 9.185 ms total native GPU average. Maximum was
  22.429 ms, still below the 33.3 ms source interval in this run.
- Compatibility runs also succeeded at 1080p Low and 4K Ultra. 4K Ultra averaged
  9.644 ms total native GPU with a 20.956 ms maximum.
- Medium, High, Low, and Ultra all used the official NGX quality values already
  verified in Phase 2; no extra postprocessing or scaling-to-4K stage was added.

Compatibility logs:

- `results/phase3_gpu_direct_compatibility/gpu_direct_1080p_low.log`
- `results/phase3_gpu_direct_compatibility/gpu_direct_2160p_ultra.log`

## 21–23. Window/UI/audio validation

- Windowed/maximized 1440p Medium was visually inspected. Aspect, orientation,
  full-frame coverage, color/levels, watermark, and game UI were correct.
- ImGui overlay was visibly rendered above the enhanced video with the expected
  dark panel; it was not overwritten by the native pass.
- Fullscreen-start Medium and High each completed 120 measured frames without
  fallback, stale-target errors, or device-loss errors. Logs are under
  `results/phase3_gpu_direct_fullscreen/`.
- SDL resize source behavior and the implementation were checked: SDL releases
  its RTV, calls `ResizeBuffers`, recreates the view, and the bridge reacquires
  and validates the current RTV size on every native draw. VSR output textures
  are independent of window size.
- An attempted automated rotation/arbitrary-drag-resize/fullscreen-exit sequence
  was interrupted when live user input moved focus to another application. The
  automation was stopped rather than stealing input. Those three interaction
  checks remain an explicitly documented manual release gate; no failure was
  observed, but they are not claimed as completed.
- Audio code, callback size, synchronization policy, and packet flow were not
  changed. Live logs retained the established 1024-sample callback and the same
  video-drop-to-audio resynchronization messages. No new RTX-specific audio
  error appeared. Subjective crackle was not reclassified beyond the user's
  accepted Phase 2C-1 baseline.

## 24. Hybrid-GPU result

Hybrid-GPU verification passed. SDL and NGX reported the same NVIDIA adapter
and exact LUID. The direct path refuses Intel/software/mismatched devices and
does not attempt a cross-adapter shared resource or copy.

## 25. Fallback behavior

Three paths remain available:

1. vanilla IYUV SDL rendering (RTX VSR off or unavailable)
2. Phase 2C-2 CPU-readback RTX VSR
3. experimental GPU-direct RTX VSR

Fallback order is GPU direct → CPU-readback VSR → vanilla IYUV. A live forced
presentation failure (before the target-bind correction) exercised the first
transition successfully and continued the stream. Initialization, adapter,
feature, evaluation, direct-render, and device errors all produce explicit log
text. No application-managed frame queue, extra decoded-frame queue, or
artificial frame delay was added.

Default settings remain:

- RTX VSR: off
- output: 1440p
- quality: Medium
- presentation backend: CPU readback (compatible)

Selecting GPU direct requests SDL `direct3d11` at the next application startup.

## 26. Build, verification, and git state

Exact build commands used:

```powershell
Set-Location <path-to-SysDVR-RTX>
$sdk = $env:RTX_VIDEO_SDK_DIR

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $sdk -Configuration Release

dotnet build .\Client\Client.csproj -c Release -r win-x64 --no-restore
dotnet publish .\Client\Client.csproj -c Release -r win-x64 `
  --self-contained true -p:SysDvrTarget=windows

& .\experiments\RtxVideoBridgeTest\Run-Phase3-GpuDirect.ps1 `
  -SdkDirectory $sdk -Include4KHigh
```

Verification completed:

- native MSVC Release build
- native ABI v3 smoke test and VSR processing
- managed Release build
- trimmed Windows publish
- live USB 1440p Medium/High benchmark
- live USB 4K High benchmark
- live USB 1080p Low and 4K Ultra compatibility benchmark
- live USB fullscreen-start Medium/High benchmark
- explicit 2560×1440 GPU-output screenshot readback
- `git diff --check`

The publish options were restored to VSR off, 1440p, Medium, CPU readback.
Generated results and NVIDIA SDK binaries remain ignored/untracked build data.
The final tracked worktree is clean after the Phase 3 commit.

## 27. Final recommendation

**GPU-direct path is clearly better and should become the preferred RTX
backend.** Keep it visibly experimental until the three manual interaction
checks above and a camera latency A/B are completed. Keep CPU readback as an
automatic compatibility fallback. Do not begin hardware decoding or Phase 4
without explicit approval.
