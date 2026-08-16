# Experimental SysDVR RTX Video native bridge

This Windows x64 DLL is the Phase 2B/2C-2 CPU-readback bridge. It is separate from
the normal SysDVR native dependencies and uses a plain C ABI declared in
`include/RtxVideoBridge.h`.

The implementation supports only:

- 1280x720 CPU RGBA8 input (`R8G8B8A8_UNORM` byte order)
- 1920x1080, 2560x1440, or 3840x2160 CPU RGBA8 output
- NVIDIA RTX Video SDK VSR qualities 0 through 4
- synchronous upload, VSR evaluation, staging readback, and row copy

The requested output dimensions control the NGX output subrect, D3D11 output
texture, readback staging texture, and required CPU stride. The bridge does
not create or scale through a fixed 1440p intermediate. ABI v2 exposes the
last frame's D3D11 timestamp results and CPU submission/Map/row-copy timings.

It deliberately selects an NVIDIA hardware DXGI adapter. The official SDK is
not vendored, and neither `nvngx_vsr.dll` nor NVIDIA libraries are copied into
the repository by the build script.

## Build

```powershell
$sdk = 'C:\path\to\RTX_Video_SDK_1.1.0'
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\Client\Platform\Specific.Win\RtxVideoBridge\Build-RtxVideoBridge.ps1 `
    -SdkDirectory $sdk -Configuration Release
```

This builds the DLL and smoke-test executable in the ignored `build` folder.
It copies only the locally-built `RtxVideoBridge.dll` to SysDVR's ignored
`Client/Platform/Resources/win-x64/native` folder so the client build can copy
it to its runtime directory.

## Native smoke test

Use the SDK's official development feature DLL directory (watermarked output)
or a properly licensed release directory:

```powershell
$feature = "$sdk\bin\Windows\x64\dev"
& .\Client\Platform\Specific.Win\RtxVideoBridge\build\Release\RtxVideoBridgeSmokeTest.exe `
    $feature 2
```

The optional second argument is quality `0..4`; the optional third argument is
a DXGI adapter index. Omitting the adapter selects the first NVIDIA hardware
adapter.

At runtime the managed client resolves `nvngx_vsr.dll` from:

1. `SYSDVR_RTX_VIDEO_FEATURE_DIR`, or
2. `RTX_VIDEO_SDK_DIR\bin\Windows\x64\dev`, or
3. the client's `runtimes\win-x64\native` directory.
