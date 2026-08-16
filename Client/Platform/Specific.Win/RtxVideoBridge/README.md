# SysDVR RTX Video native bridge

This Windows x64 DLL is the native D3D11 bridge between the managed SysDVR client, SDL, and NVIDIA RTX Video Super Resolution. It uses the versioned C ABI declared in `include/RtxVideoBridge.h`.

The bridge supports:

- 1280x720 RGBA8 input (`DXGI_FORMAT_R8G8B8A8_UNORM`)
- 1920x1080, 2560x1440, or 3840x2160 output created directly by VSR
- NVIDIA RTX Video SDK quality values 0 through 4
- GPU-direct same-device processing and presentation
- synchronous CPU-readback compatibility processing
- D3D11 timestamp telemetry and explicit screenshot readback

It deliberately selects an NVIDIA hardware DXGI adapter. GPU-direct initialization additionally verifies that the SDL D3D11 renderer and NGX use the same adapter LUID.

The official SDK is not vendored. Neither `nvngx_vsr.dll`, NVIDIA libraries, nor NVIDIA headers are copied into the repository by the build script.

## Build

```powershell
$sdk = $env:RTX_VIDEO_SDK_DIR
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
  -SdkDirectory $sdk -Configuration Release
```

This creates the DLL and smoke-test executable in the ignored `build` folder. It copies only the locally built `RtxVideoBridge.dll` to the ignored `Client/Platform/Resources/win-x64/native` directory so the managed client publish can include it.

## Native smoke test

```powershell
$feature = Join-Path $sdk 'bin\Windows\x64\dev'
& .\Client\Platform\Specific.Win\RtxVideoBridge\build\Release\RtxVideoBridgeSmokeTest.exe `
  $feature 2
```

The optional second argument is quality `0..4`; the optional third argument is a DXGI adapter index. Omitting the adapter selects the first NVIDIA hardware adapter.

The SDK development feature module watermarks output as `DO NOT DISTRIBUTE`. Do not redistribute it or publish captures produced by it.
