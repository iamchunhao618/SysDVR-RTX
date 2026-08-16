# Experimental SysDVR RTX Video native bridge

This Windows x64 DLL is the experimental CPU-readback and GPU-direct RTX Video
bridge. It is separate from the normal SysDVR native dependencies and uses a
plain C ABI declared in `include/RtxVideoBridge.h`.

The implementation supports only:

- 1280x720 CPU RGBA8 input (`R8G8B8A8_UNORM` byte order)
- 1920x1080, 2560x1440, or 3840x2160 CPU RGBA8 output
- NVIDIA RTX Video SDK VSR qualities 0 through 4
- a synchronous CPU-readback compatibility path
- a same-adapter GPU-direct path that presents into SDL's D3D11 render target
- optional GPU-only FXAA or spatial SMAA 1x after VSR in GPU-direct mode

The requested output dimensions control the NGX output subrect, D3D11 output
texture, post-AA intermediates, readback staging texture, and required CPU
stride. The bridge does not create or scale through a fixed 1440p intermediate.
ABI v4 exposes D3D11 timestamp results for VSR, FXAA, each SMAA pass, and the
final video draw in addition to CPU submission/Map/row-copy timings.

Normal GPU-direct video frames do not use a staging readback, blocking read
Map, CPU output copy, or enhanced-frame `SDL_UpdateTexture`. Screenshot capture
is an explicit exception and reads back the final AA result. AA applies only to
the Switch video; SDL/ImGui UI is composed afterward.

## Post-AA provenance and formats

- **FXAA:** D3D11 pixel-shader adaptation of Microsoft MiniEngine's
  MIT-licensed FXAA 3.11 PC Quality implementation, preset-25 search distances,
  green-as-luma, subpixel 0.50, edge threshold 1/6, minimum threshold 1/12.
- **SMAA 1x:** canonical `iryoku/smaa` commit
  `71c806a838bdd7d517df19192a20f0c61b3ca29d`, MIT license, High preset,
  luma edge detection + blending weights + neighborhood blending.

FXAA uses one `R8G8B8A8_UNORM` output. SMAA uses `R8G8_UNORM` edges,
`R8G8B8A8_UNORM` weights, and `R8G8B8A8_UNORM` output, plus the upstream
`R8G8_UNORM` AreaTex and `R8_UNORM` SearchTex. The reference source and license
notices are under `third_party/`; no NVIDIA SDK shader source or binary is
vendored.

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
