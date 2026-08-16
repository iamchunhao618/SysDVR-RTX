# Building SysDVR RTX VSR

This repository contains the original SysDVR Switch sysmodule, its settings application, and the cross-platform client. The RTX VSR extension affects only the Windows x64 client.

No NVIDIA SDK file is vendored or downloaded by this repository. Obtain the official NVIDIA RTX Video SDK 1.1.0 separately and comply with its license.

## Windows RTX client prerequisites

- Windows 10 or Windows 11 x64
- .NET 9 SDK
- Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK, and CMake
- 7-Zip available as `7z.exe`
- Official NVIDIA RTX Video SDK 1.1.0
- A supported NVIDIA RTX GPU and compatible driver for runtime testing

## Clean Windows Release build

Set the SDK environment variable to a location outside the repository:

```powershell
$env:RTX_VIDEO_SDK_DIR = 'D:\SDKs\RTX_Video_SDK_1.1.0' # replace this
```

Populate the original SysDVR Windows native dependencies. The upstream script downloads its pinned FFmpeg, SDL, libusb, and cimgui dependencies and performs an initial publish:

```powershell
Push-Location .\Client\Platform
cmd.exe /c BuildWindows.bat
Pop-Location
```

Build the native bridge. It consumes NVIDIA headers and the import/static library from `RTX_VIDEO_SDK_DIR`, writes build products only to ignored directories, and does not copy NVIDIA SDK files:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $env:RTX_VIDEO_SDK_DIR -Configuration Release
```

Build and publish the managed client:

```powershell
dotnet restore .\Client\Client.csproj -r win-x64 -p:SysDvrTarget=windows
dotnet build .\Client\Client.csproj -c Release -r win-x64 `
  -p:SysDvrTarget=windows --no-restore
dotnet publish .\Client\Client.csproj -c Release -r win-x64 `
  --self-contained true -p:SysDvrTarget=windows --no-restore
```

The publish directory is `Client/bin/Release/net9.0/win-x64/publish`.

For local testing, set the feature directory instead of copying `nvngx_vsr.dll` into source control:

```powershell
$env:SYSDVR_RTX_VIDEO_FEATURE_DIR = `
  Join-Path $env:RTX_VIDEO_SDK_DIR 'bin\Windows\x64\dev'
```

The development feature module adds a visible `DO NOT DISTRIBUTE` watermark. Do not publish that DLL or output captured from it.

## Native smoke test

After building the bridge:

```powershell
$feature = Join-Path $env:RTX_VIDEO_SDK_DIR 'bin\Windows\x64\dev'
& .\Client\Platform\Specific.Win\RtxVideoBridge\build\Release\RtxVideoBridgeSmokeTest.exe `
  $feature 2
```

The optional second argument selects quality `0..4`; the optional third argument selects a DXGI adapter index.

## Standalone benchmark

The standalone D3D11 test lives in `experiments/RtxVideoBridgeTest`. Its [README](experiments/RtxVideoBridgeTest/README.md) documents the build and benchmark arguments. Generated images, CSV/JSON results, logs, and executables are ignored.

## Switch sysmodule and settings application

These components are unchanged from upstream SysDVR and require the devkitA64 toolchain from devkitPro. Build each Switch project with its `Makefile`. For example, the USB-only sysmodule configuration is:

```text
make -j DEFINES="-DUSB_ONLY"
```

See the [upstream SysDVR repository](https://github.com/exelix11/SysDVR) for platform-specific packaging and broader project documentation.

## Other client platforms

RTX VSR is Windows-only. The upstream client still contains Linux, macOS, and Android targets, but this fork's RTX code is excluded from those platforms. Their build scripts remain under `Client/Platform`.
