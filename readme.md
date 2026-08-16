# SysDVR RTX VSR

**Experimental Windows fork of [SysDVR](https://github.com/exelix11/SysDVR) with real-time NVIDIA RTX Video Super Resolution.**

SysDVR RTX VSR enhances SysDVR's fixed 1280x720, 30 fps H.264 capture stream to 1920x1080, 2560x1440, or 3840x2160. The preferred GPU-direct D3D11 backend keeps the enhanced frame GPU-resident through presentation, avoiding the earlier GPU-to-CPU-to-GPU readback path.

This project is an independent community experiment. It is not affiliated with, endorsed by, or officially supported by NVIDIA or the upstream SysDVR project.

> [!IMPORTANT]
> RTX VSR is disabled by default. This source repository does not contain NVIDIA SDK headers, libraries, packages, feature DLLs, or `nvngx_vsr.dll`. Obtain the official NVIDIA RTX Video SDK separately and follow NVIDIA's license terms.

## Features

- NVIDIA RTX Video Super Resolution integration for the Windows client
- 1080p, 1440p, and 4K VSR output selected directly at native VSR evaluation time
- Low, Medium, High, and Ultra quality levels
- GPU-direct D3D11 presentation with no normal-path GPU readback
- CPU-readback compatibility backend
- Automatic fallback to vanilla SysDVR rendering
- USB and TCP Bridge compatibility
- Windowed, resizable, and fullscreen presentation with preserved aspect ratio
- Existing screenshots, overlays, and rotation path retained
- NVIDIA RTX adapter selection and SDL/NGX adapter-LUID verification

## Recommended settings

For everyday use:

- Output: **2560x1440**
- Quality: **Medium**
- Presentation backend: **GPU direct (D3D11)**

For a modest quality increase when GPU headroom is available, use **2560x1440 High**. Although 4K output works, current 720p-source testing found little meaningful visual advantage over direct 1440p output, especially when displayed on a 1440p monitor.

## Performance

Phase 3 measurements on an NVIDIA GeForce RTX 4060 Laptop GPU:

| Output and quality | CPU enhancement submit | Native GPU work |
| --- | ---: | ---: |
| 1440p Medium | ~0.755 ms | ~6.614 ms |
| 1440p High | ~0.709 ms | ~8.406 ms |
| 4K High | ~0.672 ms | ~9.185 ms |

CPU submission is asynchronous and is **not** the total VSR processing latency. The GPU work remains part of the same-frame latency even though the CPU no longer blocks on a staging-texture `Map`. See the [Phase 3 report](experiments/RtxVideoBridgeTest/PHASE_3_REPORT.md) for methodology, fallback comparisons, and limitations.

## Requirements

Runtime:

- Windows 10 or Windows 11 x64
- A supported NVIDIA RTX GPU and compatible NVIDIA driver
- A Nintendo Switch running [SysDVR](https://github.com/exelix11/SysDVR)
- USB or TCP Bridge connectivity
- A properly licensed RTX Video VSR feature module supplied through the official NVIDIA RTX Video SDK/runtime arrangement

Build:

- [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0)
- Visual Studio 2022 Build Tools with **Desktop development with C++**, the Windows SDK, and CMake
- 7-Zip for the upstream Windows dependency script
- Official NVIDIA RTX Video SDK 1.1.0, obtained separately

The Switch sysmodule is unchanged by the RTX work. Building it still requires devkitA64 as described in [building.md](building.md).

## Build the Windows client

The commands below are the release commands verified during Phase 3. Set `RTX_VIDEO_SDK_DIR` to your separately extracted official SDK directory; do not copy the SDK into this repository.

```powershell
git clone https://github.com/iamchunhao618/SysDVR-RTX.git
Set-Location .\SysDVR-RTX

$env:RTX_VIDEO_SDK_DIR = 'D:\SDKs\RTX_Video_SDK_1.1.0' # replace this

# Populate the upstream Windows runtime dependencies once.
Push-Location .\Client\Platform
cmd.exe /c BuildWindows.bat
Pop-Location

# Build the locally authored native RTX bridge. NVIDIA SDK files are not copied.
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $env:RTX_VIDEO_SDK_DIR -Configuration Release

# Build and publish the Windows client with the bridge in its runtime directory.
dotnet restore .\Client\Client.csproj -r win-x64 -p:SysDvrTarget=windows
dotnet build .\Client\Client.csproj -c Release -r win-x64 `
  -p:SysDvrTarget=windows --no-restore
dotnet publish .\Client\Client.csproj -c Release -r win-x64 `
  --self-contained true -p:SysDvrTarget=windows --no-restore
```

Output is written under `Client/bin/Release/net9.0/win-x64/publish`. All build output and NVIDIA feature binaries remain ignored.

For local development, point the client at the feature directory without copying it into the repository:

```powershell
$env:SYSDVR_RTX_VIDEO_FEATURE_DIR = `
  Join-Path $env:RTX_VIDEO_SDK_DIR 'bin\Windows\x64\dev'
```

NVIDIA's development feature module visibly watermarks output and marks it as not distributable. Use it only as permitted by NVIDIA's SDK terms; do not publish captures produced by it.

## Use

1. Start the built Windows client and connect to the Switch using SysDVR USB or TCP Bridge.
2. Open the video options and enable **Experimental NVIDIA RTX Video Super Resolution**.
3. Select 1080p, 1440p, or 4K output and a quality level.
4. Select the **GPU direct (D3D11)** backend.
5. Restart the client when prompted so SDL can select its D3D11 renderer.

If initialization fails, the client disables RTX VSR for the session and returns to the original SysDVR rendering path.

## Rendering fallback order

```text
GPU-direct RTX VSR
  -> CPU-readback RTX VSR compatibility backend
  -> vanilla SysDVR rendering
```

The compatibility backend is intentionally retained for troubleshooting and systems where GPU-direct interop cannot be established.

## Known limitations

- The source remains 1280x720 at 30 fps H.264. VSR can improve perceived clarity but cannot reconstruct true native 1440p or 4K source detail.
- Mild audio crackling occurred on the test system, but it also occurred in official/vanilla SysDVR and was not shown to be RTX-specific.
- Rotation, arbitrary drag-resize, and exiting fullscreen need broader community testing.
- Testing has primarily used an NVIDIA GeForce RTX 4060 Laptop GPU.
- GPU-direct mode requires SDL and NGX to use the same D3D11 adapter; hybrid-GPU configurations can expose compatibility problems.
- Development-SDK captures were excluded from this repository because they contain NVIDIA's explicit `DO NOT DISTRIBUTE` watermark. The image below is the already-public upstream vanilla SysDVR example, not an RTX VSR comparison.
- This is a beta-quality experimental fork. Keep vanilla fallback available and report reproducible failures with logs that do not contain private information.

<p align="center">
  <img alt="Upstream vanilla SysDVR capture" src="https://raw.githubusercontent.com/exelix11/SysDVR/master/.github/images/Screenshot.jpg" width="60%">
</p>

## Documentation

- [Windows and RTX build guide](building.md)
- [Architecture overview](docs/ARCHITECTURE.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [v0.1.0-beta release notes](docs/RELEASE_NOTES_v0.1.0-beta.md)
- [Phase 2C-2 benchmark report](experiments/RtxVideoBridgeTest/PHASE_2C2_REPORT.md)
- [Phase 3 GPU-direct report](experiments/RtxVideoBridgeTest/PHASE_3_REPORT.md)
- [Standalone benchmark](experiments/RtxVideoBridgeTest/README.md)

## Credits and license

SysDVR RTX VSR is a fork of [SysDVR](https://github.com/exelix11/SysDVR), created by [exelix11](https://github.com/exelix11). The upstream project, contributors, credits, and GPLv2 license are preserved. See [NOTICE.md](NOTICE.md) and [LICENSE](LICENSE).

RTX Video Super Resolution is implemented using NVIDIA RTX Video SDK / NGX technology obtained separately by the builder. NVIDIA, GeForce, and RTX are trademarks of NVIDIA Corporation. Their mention does not imply affiliation or endorsement.
