# SysDVR RTX

[![Discord](https://img.shields.io/discord/643436008452521984.svg?logo=discord&logoColor=white&label=Discord&color=7289DA)](https://discord.gg/rqU5Tf8)
[![Latest release](https://img.shields.io/github/v/release/iamchunhao618/SysDVR-RTX)](https://github.com/iamchunhao618/SysDVR-RTX/releases)
[![License](https://img.shields.io/github/license/iamchunhao618/SysDVR-RTX)](LICENSE)

SysDVR RTX is an experimental fork of [SysDVR](https://github.com/exelix11/SysDVR). It keeps the original Nintendo Switch game-video streaming workflow and adds an **optional Windows-only NVIDIA RTX Video Super Resolution (VSR) path** for local research and testing.

The Switch stream itself remains fixed at 720p, 30 fps H.264. RTX VSR enhances the decoded video locally on the PC; it does not change the Switch, transport protocol, or capture quality.

> [!WARNING]
> The RTX Video feature is experimental, disabled by default, and has been validated on an NVIDIA GeForce RTX 4060 Laptop GPU with the official NVIDIA RTX Video SDK 1.1.0. It is not a portable replacement for the normal SysDVR client. The standard client path remains available and is used whenever RTX VSR is off or unavailable.

## Features

- Stream Nintendo Switch game video to Windows, macOS, Linux, or Android over USB or Wi-Fi.
- Fixed source quality: **720p at 30 fps**, H.264 video and 16-bit PCM, 48 kHz stereo audio.
- Optional Windows x64 RTX VSR enhancement of the 720p source to **1080p, 1440p, or 4K**.
- RTX VSR quality choices: Low, Medium, High, and Ultra.
- Experimental same-adapter D3D11 GPU-direct presentation path. It avoids enhanced-frame staging readback and CPU/SDL texture upload during normal playback.
- Optional GPU-only post-processing after VSR: Off, FXAA, or SMAA 1x. UI overlays are drawn afterward and are not filtered.
- Normal resize, fullscreen, aspect-ratio, black-bar, and rotation behavior are retained.

## Current project status

Phase 3.5 is complete on branch `codex/rtx-vsr-post-aa`.

| Area | Current state |
| --- | --- |
| Standard SysDVR streaming | Preserved; cross-platform and unchanged by default |
| RTX VSR | Windows x64 experiment; disabled by default |
| Output resolution | Native VSR output at 1080p, 1440p, or 4K |
| Presentation | CPU-readback compatibility path and experimental GPU-direct path |
| Post-AA | Off (default), FXAA, or SMAA 1x in GPU-direct mode |
| Recommended test setting | 1440p Medium + SMAA 1x, after enabling RTX VSR |
| Out of scope | Hardware decoding, zero-copy decode, extra frame queues, and Switch-side changes |

The default configuration remains conservative: RTX VSR is disabled, output is set to 1440p Medium, CPU-readback is selected for compatibility, and post-AA is Off. Enable the feature from the Windows client settings only after completing the prerequisites below.

Detailed architecture, diagnostics, benchmark methodology, and Phase 3.5 results are in [the Phase 3.5 report](experiments/RtxVideoBridgeTest/PHASE_3_5_REPORT.md). The native bridge has its own [setup and runtime reference](Client/Platform/Specific.Win/RtxVideoBridge/README.md).

## RTX VSR prerequisites (Windows only)

You need all of the following for the experimental path:

1. Windows x64 and a compatible NVIDIA RTX GPU selected by the SDL D3D11 renderer.
2. A driver that reports VSR support through NVIDIA NGX.
3. An official NVIDIA RTX Video SDK installation. SDK files and `nvngx_vsr.dll` are **not** stored in this repository or downloaded by its build scripts.
4. A local build of `RtxVideoBridge.dll`.

For local SDK development builds, set either `SYSDVR_RTX_VIDEO_FEATURE_DIR` to the official feature-DLL directory or `RTX_VIDEO_SDK_DIR` to the SDK root. NVIDIA's development feature DLL displays its expected development watermark; do not redistribute it.

Build the native bridge from the repository root after obtaining the SDK yourself:

```powershell
$sdk = 'C:\path\to\RTX_Video_SDK_1.1.0'
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $sdk -Configuration Release
```

The script builds the bridge locally and places only that locally-built DLL in the ignored Windows native-resource directory. Then build or publish the Windows client as described in [building.md](building.md).

## Usage

Install and use the Switch sysmodule as documented by the upstream [SysDVR wiki](https://github.com/exelix11/SysDVR/wiki). For ordinary streaming, run the standard client and connect over USB or Wi-Fi as usual.

For the RTX experiment, use the Windows client, ensure the official feature DLL can be found, then open Settings and enable **NVIDIA RTX Video Super Resolution**. Select the output resolution, quality, presentation backend, and optional post-process anti-aliasing. If capability probing, initialization, or a processing step fails, the client logs the reason once and continues with the original IYUV renderer for the rest of the session.

## Limitations

- The Nintendo Switch source remains 720p/30; VSR cannot create new source detail or remove streaming artifacts perfectly.
- RTX VSR is SDR-only in this implementation. The path accepts decoded 1280×720 YUV420P and converts it to full-range RGBA8 before VSR.
- GPU-direct operation requires SDL and VSR to use the same NVIDIA adapter. Otherwise the client falls back to the compatibility path or the vanilla renderer.
- FXAA and SMAA are spatial post-processes, not temporal AA. A slow-pan visual comparison is still recommended before choosing SMAA over FXAA for a particular game.
- The project has no bundled NVIDIA SDK or feature DLL, and no official RTX-enabled release package is claimed by this repository.
- SysDVR only captures game output. System UI, the Home Menu, and homebrew running as an applet are not captured. Games must support video recording, unless using the upstream [dvr-patches workaround](https://github.com/exelix11/dvr-patches).
- USB streaming is unavailable while docked. Stream quality depends heavily on USB cable quality or Wi-Fi signal.

This project does not replace a capture card.

## Building

See [building.md](building.md) for the original SysDVR components and platform build instructions. The Windows RTX-specific bridge instructions are in [Client/Platform/Specific.Win/RtxVideoBridge/README.md](Client/Platform/Specific.Win/RtxVideoBridge/README.md).

## Credits and licensing

- Original SysDVR project and its contributors: [exelix11/SysDVR](https://github.com/exelix11/SysDVR).
- [libnx](https://github.com/switchbrew/libnx), [mtp-server-nx](https://github.com/retronx-team/mtp-server-nx), [RTSPSharp](https://github.com/ngraziano/SharpRTSP), [CimguiSDL2Cross](https://github.com/exelix11/CimguiSDL2Cross), Bonta, and Xerpi.
- FXAA adaptation derived from the MIT-licensed [Microsoft DirectX Graphics Samples](https://github.com/microsoft/DirectX-Graphics-Samples); notice in `Client/Platform/Specific.Win/RtxVideoBridge/third_party/fxaa/`.
- SMAA 1x shader and lookup textures from the MIT-licensed [iryoku/smaa](https://github.com/iryoku/smaa), pinned and documented in `Client/Platform/Specific.Win/RtxVideoBridge/third_party/smaa/`.
- NVIDIA RTX Video SDK remains NVIDIA-provided software and is neither included nor redistributed by this repository.

See [LICENSE](LICENSE) and the included third-party notices for license details.
