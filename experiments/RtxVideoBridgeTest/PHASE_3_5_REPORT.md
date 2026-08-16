# Phase 3.5 — GPU-resident post-process AA

## 1. Branch

`codex/rtx-vsr-post-aa`

## 2. Starting Phase 3 commit

`22f789ba94a6dcf9eec27caecc7d59a35021c917`

The branch was created directly from that known-good commit. Phase 3 and the
public release branch were not rewritten.

## 3. Files added or modified

Added:

- `Client/Platform/Specific.Win/RtxVideoBridge/src/PostProcessAA.h`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/PostProcessAA.cpp`
- `Client/Platform/Specific.Win/RtxVideoBridge/shaders/Fxaa3_11_MicrosoftPort.hlsl`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/EmbeddedFxaaShader.h.in`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/EmbeddedSmaaShader.h.in`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/fxaa/LICENSE.txt`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/fxaa/README.md`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/LICENSE.txt`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/README.md`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/.gitattributes`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/SMAA.hlsl`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/Textures/AreaTex.h`
- `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/Textures/SearchTex.h`
- `experiments/RtxVideoBridgeTest/Run-Phase3_5-PostAA.ps1`
- this report

Modified:

- `Client/Core/Options.cs`
- `Client/Core/StringTable.cs`
- `Client/GUI/OptionsView.cs`
- `Client/Platform/Specific.Win/RtxVideo/RtxVideoEnhancer.cs`
- `Client/Platform/Specific.Win/RtxVideo/RtxVideoNative.cs`
- `Client/Platform/Specific.Win/RtxVideoBridge/CMakeLists.txt`
- `Client/Platform/Specific.Win/RtxVideoBridge/README.md`
- `Client/Platform/Specific.Win/RtxVideoBridge/include/RtxVideoBridge.h`
- `Client/Platform/Specific.Win/RtxVideoBridge/src/RtxVideoBridge.cpp`
- `Client/Targets/Player/Player.cs`

Generated build products, logs, screenshots, and NVIDIA SDK files remain
ignored and are not part of the commit.

## 4. Exact GPU pipeline before and after

Before:

```text
CPU YUV420P -> CPU RGBA8 -> UpdateSubresource -> NGX RTX VSR RGBA8
-> native fullscreen textured draw into the current SDL D3D11 target
-> ImGui/SDL UI -> Present
```

After:

```text
CPU YUV420P -> CPU RGBA8 -> UpdateSubresource -> NGX RTX VSR RGBA8
-> [Off | FXAA | SMAA 1x] at the actual VSR output resolution
-> native destination/rotation draw into the current SDL D3D11 target
-> ImGui/SDL UI -> Present
```

The insertion is inside `rvb_render_output_d3d11`, after
`rvb_process_rgba8_gpu` completes NGX evaluation and before the existing final
video draw. `ID3DDeviceContextState` swapping still isolates native state from
SDL. The AA targets are tied to VSR dimensions, not window dimensions, and are
reused until the VSR output resolution changes.

## 5. FXAA implementation, reference, and license

The implementation is a D3D11 pixel-shader adaptation of Microsoft
MiniEngine's compute-optimized **FXAA 3.11 PC Quality** implementation from the
MIT-licensed DirectX Graphics Samples repository:

https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine/Core/Shaders

It uses preset-25 search distances, gamma-encoded SDR RGBA8 sampled with
green-as-luma, subpixel amount `0.50`, edge threshold `1/6`, and minimum edge
threshold `1/12`. It has no depth, motion vectors, history, or queue.

The historical NVIDIA `Fxaa3_11.h` was not copied because the surviving header
contains a warranty disclaimer but no clear redistribution grant. The
Microsoft-derived port and retained MIT notice remove that ambiguity.

## 6. SMAA implementation, reference, and license

The implementation embeds the canonical source and lookup data from
`iryoku/smaa` commit `71c806a838bdd7d517df19192a20f0c61b3ca29d`:

https://github.com/iryoku/smaa

It uses spatial SMAA 1x with the standard High preset and exactly three passes:
luma edge detection, blending-weight calculation, and neighborhood blending.
`subsampleIndices` is zero. No temporal, reprojection, T2x, S2x, or 4x code path
is used.

The upstream implementation, AreaTex, and SearchTex carry the same MIT notice.
Source modification and redistribution are permitted when the notice is kept.
MIT is permissive and compatible with distribution inside this GPL-2.0
project. Upstream SHA-256 values are:

- `SMAA.hlsl`: `4DB53D92EF0B45661F8450303589FFABCE93C352CAE4282A6F32DD2B80EFBEB9`
- `AreaTex.h`: `1933BF43CE86BA71F7ED5F0FA94B148610C634DC3395B9BC67A817D51EE73AF2`
- `SearchTex.h`: `1FAC315AB5F87B60083C9B8387AF17F056204D3D1BF21500F2ACC5B3E8C1A37A`

## 7. Intermediate texture formats

| Resource | Format | Resolution |
|---|---|---|
| NGX VSR input | `R8G8B8A8_UNORM` | 1280x720 |
| NGX VSR output | `R8G8B8A8_UNORM` | selected 1080p/1440p/2160p |
| FXAA output | `R8G8B8A8_UNORM` | selected VSR resolution |
| SMAA edges | `R8G8_UNORM` | selected VSR resolution |
| SMAA weights | `R8G8B8A8_UNORM` | selected VSR resolution |
| SMAA final | `R8G8B8A8_UNORM` | selected VSR resolution |
| SMAA AreaTex | `R8G8_UNORM` | 160x560 |
| SMAA SearchTex | `R8_UNORM` | 64x16 |

Input is gamma-encoded SDR RGBA8 in the existing bridge convention. FXAA keeps
the center pixel alpha; SMAA blends alpha consistently with the reference
neighborhood pass. Current FFmpeg conversion supplies opaque alpha (`255`).

## 8. AA settings added to the UI

`Post-process anti-aliasing` exposes `Off`, `FXAA`, and `SMAA 1x`. The default is
`Off`; AA is never silently enabled. The UI states that post-AA affects only
Switch video and requires GPU direct. If CPU readback is active, the stream
continues with AA Off and logs the reason.

## 9. 1440p Medium benchmark

Twenty warm-up frames preceded each interval. Values are D3D11 GPU timestamp
milliseconds in `average / median / p95 / maximum` order over 120 frames.

| AA | VSR GPU | FXAA | SMAA edge | SMAA weights | SMAA neighborhood | Total AA | Complete native GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| Off | 6.058 / 6.503 / 8.719 / 10.290 | 0 | 0 | 0 | 0 | 0.004 / 0.004 / 0.005 / 0.014 | 6.658 / 7.174 / 9.361 / 10.978 |
| FXAA | 5.950 / 6.562 / 8.368 / 8.687 | 0.416 / 0.439 / 0.537 / 1.319 | 0 | 0 | 0 | 0.416 / 0.439 / 0.537 / 1.319 | 6.946 / 7.802 / 9.616 / 10.000 |
| SMAA 1x | 5.705 / 6.057 / 7.683 / 9.905 | 0 | 0.207 / 0.224 / 0.262 / 0.721 | 0.356 / 0.384 / 0.447 / 0.911 | 0.224 / 0.235 / 0.276 / 0.802 | 0.787 / 0.843 / 0.984 / 1.919 | 7.065 / 7.548 / 9.269 / 12.493 |

The tiny nonzero Off total is timestamp/query command spacing, not an AA pass.

## 10. 1440p High benchmark

| AA | VSR GPU | FXAA | SMAA edge | SMAA weights | SMAA neighborhood | Total AA | Complete native GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| Off | 8.083 / 8.665 / 9.961 / 10.138 | 0 | 0 | 0 | 0 | 0.003 / 0.003 / 0.004 / 0.007 | 8.508 / 9.101 / 10.354 / 11.556 |
| FXAA | 8.934 / 9.559 / 11.119 / 11.474 | 0.333 / 0.353 / 0.396 / 0.790 | 0 | 0 | 0 | 0.333 / 0.353 / 0.396 / 0.790 | 9.680 / 10.271 / 12.123 / 12.467 |
| SMAA 1x | 7.644 / 7.611 / 10.628 / 11.430 | 0 | 0.120 / 0.123 / 0.143 / 0.706 | 0.241 / 0.223 / 0.357 / 1.009 | 0.124 / 0.128 / 0.150 / 0.387 | 0.485 / 0.474 / 0.875 / 1.291 | 8.539 / 8.496 / 11.659 / 12.828 |

VSR timing varies with content and clock state, so the AA-specific timestamps,
not differences between independent VSR averages, are the incremental cost.

## 11. Optional 4K High benchmark

| AA | VSR GPU | FXAA | SMAA edge | SMAA weights | SMAA neighborhood | Total AA | Complete native GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| Off | 8.518 / 8.400 / 13.032 / 15.534 | 0 | 0 | 0 | 0 | 0.003 / 0.003 / 0.005 / 0.008 | 8.993 / 8.788 / 14.202 / 16.225 |
| FXAA | 8.539 / 8.685 / 13.508 / 14.330 | 0.533 / 0.544 / 0.845 / 0.894 | 0 | 0 | 0 | 0.533 / 0.544 / 0.845 / 0.894 | 9.482 / 9.607 / 15.017 / 15.837 |
| SMAA 1x | 8.400 / 8.881 / 12.848 / 14.473 | 0 | 0.262 / 0.272 / 0.378 / 1.587 | 0.413 / 0.435 / 0.632 / 0.788 | 0.279 / 0.292 / 0.424 / 0.766 | 0.954 / 0.997 / 1.446 / 2.687 | 9.730 / 10.230 / 14.954 / 17.558 |

## 12. FXAA GPU cost

FXAA averaged `0.416 ms` at 1440p Medium, `0.333 ms` at 1440p High, and
`0.533 ms` at 4K High. The independent content runs explain the small 1440p
ordering reversal. All averages are well below the 1 ms target.

## 13. SMAA GPU cost per pass and total

At 1440p Medium the averages were edge `0.207 ms`, weights `0.356 ms`,
neighborhood `0.224 ms`, total `0.787 ms`. At 1440p High they were `0.120`,
`0.241`, `0.124`, total `0.485 ms`. At 4K High they were `0.262`, `0.413`,
`0.279`, total `0.954 ms`. No queue or history was added; this is same-frame
incremental GPU latency.

CPU video submission averages remained `0.027–0.036 ms` at 1440p and
`0.022–0.030 ms` at 4K. Normal-frame staging copy, readback, blocking Map, row
copy, and enhanced-frame SDL upload were all `0.000 ms` in every measured run.

## 14. Screenshots generated

Generated, ignored screenshots are under:

`experiments/RtxVideoBridgeTest/results/phase3_5_post_aa/captures/`

- `vsr_1440p_medium_off_gpu_direct.png`
- `vsr_1440p_medium_fxaa_gpu_direct.png`
- `vsr_1440p_medium_smaa1x_gpu_direct.png`
- `vsr_1440p_high_off_gpu_direct.png`
- `vsr_1440p_high_fxaa_gpu_direct.png`
- `vsr_1440p_high_smaa1x_gpu_direct.png`
- `vsr_2160p_high_off_gpu_direct.png`
- `vsr_2160p_high_fxaa_gpu_direct.png`
- `vsr_2160p_high_smaa1x_gpu_direct.png`

A prior vanilla reference remains at
`results/phase2c2_live/vanilla_720p.png`. The weather/lighting and some captures
changed between process launches, so these are useful implementation proofs
and near-scene comparisons, not a mathematically exact registered image set.

## 15. Dynamic shimmering observations

The interactive resize/maximize smoke observed continuing Skyrim camera and
scene movement with SMAA active, without a black frame, gross flicker, missing
video, or unstable presentation. The automation captures point-in-time window
images and cannot reliably rank subtle foliage shimmer, crawling edges, or
thin-line flicker. Therefore Phase 3.5 does **not** claim a definitive temporal
winner. A human-controlled slow-pan A/B is still required before changing the
default.

## 16. Detail-loss and blur observations

Static inspection found conservative FXAA slightly softer on stone, timber,
and fine ground detail. SMAA preserved more recovered VSR texture while more
selectively smoothing roof and railing diagonals. Neither showed obvious halos,
ringing, UI corruption inside the game, or disappearing large geometry. These
are modest differences; the screenshots do not justify forcing AA on.

## 17. Best-looking configuration

**Provisional:** 1440p Medium + SMAA 1x. It retains the more natural Medium VSR
look and costs under 1 ms average while preserving more fine detail than FXAA.
This must remain provisional until a direct slow-camera-pan A/B confirms that
it reduces shimmer rather than only static stair-steps.

## 18. Best performance/quality configuration

1440p Medium + FXAA is the lowest-cost AA option (`0.416 ms` average), but its
small softness tradeoff is visible. If detail preservation matters more than
roughly `0.37 ms` of additional average GPU time, use 1440p Medium + SMAA 1x.
Off remains the safest default. No tested combination needs to be hidden for
performance.

## 19. Screenshot behavior

The explicit screenshot path now copies from the last successfully presented
AA output texture. FXAA and SMAA filenames and visibly different output prove
that readback occurs after AA. This remains the only GPU-direct staging readback
and Map path; normal frames do not call it.

## 20. Resize, fullscreen, and rotation result

- 1440p Medium + SMAA completed a separate 120-frame fullscreen-start run.
- Interactive restore, resize to about 1400x850, and maximize all continued
  presenting. The 16:9 video remained centered with correct pillarboxing and no
  crop.
- AA surfaces stayed at 2560x1440 while the SDL destination changed.
- Rotation is still applied only by the unchanged final destination vertex
  transform after AA. It was code-path verified but was not manually exercised
  with a rotated Switch source in this run.

## 21. Audio regression result

No audio code was changed. All completed runs initialized SDL audio at the same
requested/obtained 1024-sample callback size and showed no new process or audio
errors. The existing video-to-audio resync packet-drop messages were still
present. Automated testing cannot listen for the previously reported mild
crackle, so this report records no log-level regression but does not claim a
new audible confirmation.

## 22. AA failure and fallback behavior

Shader/resources are initialized lazily on first use. A non-device-loss FXAA or
SMAA exception disables only that AA mode for the session, closes its timestamp
markers, logs a clear native and managed message, and draws the original VSR
SRV. GPU-direct VSR remains active. A selected AA mode in CPU-readback mode is
ignored with a one-time log. Existing GPU-direct -> CPU VSR -> vanilla fallback
behavior is unchanged. Device loss still propagates through the existing
device-loss path instead of being mislabeled as an AA-only failure.

All live AA error logs were empty; no fallback was triggered unintentionally.

## 23. Licensing audit result

- FXAA-derived code: Microsoft DirectX Graphics Samples, MIT; modification and
  redistribution allowed with notice retained.
- SMAA shader and lookup assets: canonical `iryoku/smaa`, MIT; modification and
  redistribution allowed with notice retained, including source distribution.
- Both MIT components may be distributed as part of GPL-2.0 code when their
  notices remain available.
- No NVIDIA RTX Video SDK source, shader code, library, DLL, or proprietary
  lookup asset was committed.
- The build still requires the user-supplied official RTX Video SDK and copies
  only the locally built bridge into ignored build/publish output.

## 24. Verification and final git status

Environment:

- GPU: NVIDIA GeForce RTX 4060 Laptop GPU (`0x28E0`)
- driver: `610.62`
- bridge and SDL LUID: `00000000:0001D122`
- feature level: `11.1`
- bridge ABI: `4`

Verification performed:

- native MSVC Release build
- managed .NET Release build
- trimmed self-contained Windows publish
- ABI v4/native VSR smoke test
- live 1440p Medium Off/FXAA/SMAA, 20 warm-up + 120 measured frames
- live 1440p High Off/FXAA/SMAA, 20 warm-up + 120 measured frames
- live 4K High Off/FXAA/SMAA, 20 warm-up + 120 measured frames
- separate fullscreen SMAA interval
- interactive restore/resize/maximize smoke
- final-AA explicit screenshot readback
- empty AA stderr/fallback scan
- normal-frame zero readback/Map/row-copy/SDL-upload telemetry
- `git diff --check`

Exact build commands:

```powershell
$sdk = 'C:\Users\18109\AppData\Local\Temp\SysDVR-RtxVideoResearch-20260816\sdk'
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $sdk -Configuration Release
dotnet build .\Client\Client.csproj -c Release --no-restore
dotnet publish .\Client\Client.csproj -c Release -r win-x64 `
  --self-contained true -p:SysDvrTarget=windows --no-restore
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\experiments\RtxVideoBridgeTest\Run-Phase3_5-PostAA.ps1 `
  -SdkDirectory $sdk -Include4KHigh
```

The report is committed with the implementation. Generated SDK/build/result
data remains ignored. See the final handoff for the exact commit and clean
status.

## 25. Final recommendation

**D — offer both FXAA and SMAA 1x, keep Off as default.** FXAA is a useful very
low-cost softer option. SMAA is a useful detail-preserving option and the
provisional best overall combination is 1440p Medium + SMAA 1x. Do not change
the default or call SMAA the temporal winner until the user performs a slow-pan
dynamic A/B. Do not begin Phase 4 hardware decoding without explicit approval.
