# Standalone RTX Video VSR D3D11 benchmark

This directory is an isolated Phase 2A/2C-2 experiment. It does not reference
the SysDVR client project.

The executable accepts a 1280x720 RGBA8 image, uploads it to an
`ID3D11Texture2D`, evaluates NVIDIA RTX Video Super Resolution into a
selected 1920x1080, 2560x1440, or 3840x2160 RGBA8 UAV texture, copies the
result to an equally sized staging texture, maps it, and writes the CPU result
as PNG. NGX directly produces the selected size; the tool does not upscale a
fixed intermediate output.

The NVIDIA RTX Video SDK is not vendored. Point CMake and the executable at an
officially extracted SDK directory.

## Build

From a Visual Studio 2022 x64 developer command prompt:

```powershell
$env:RTX_VIDEO_SDK_DIR = 'C:\path\to\RTX_Video_SDK_1.1.0'
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DRTX_VIDEO_SDK_DIR="$env:RTX_VIDEO_SDK_DIR"
cmake --build build --config Release
```

## Prepare an input and benchmark

The feature directory must contain the official `nvngx_vsr.dll`. Use `dev`
for the watermarked development feature and `rel` for a licensed release run.

```powershell
.\build\Release\RtxVideoBridgeTest.exe `
  --feature-dir "$env:RTX_VIDEO_SDK_DIR\bin\Windows\x64\dev" `
  --prepare-input ..\..\.github\images\Screenshot.jpg `
  --input .\results\input.png `
  --output-dir .\results `
  --warmup 20 `
  --iterations 100 `
  --resolution 1440p
```

Run the Phase 2C-2 resolution matrix by repeating the command with
`--resolution 1080p`, `--resolution 1440p`, and `--resolution 2160p`. The
default quality list is Medium, High, and Ultra; `--quality all` additionally
includes the SDK Bicubic and Low modes.

The per-frame instrumentation separates CPU upload submission, GPU upload,
NGX VSR GPU work, GPU staging copy, blocking `Map`, CPU output-row copy, total
readback, query resolution, and the complete synchronous `ProcessFrame` call.

Generated outputs and build artifacts are ignored by Git. Timing data is
written to `benchmark.csv` and `benchmark.json`.
