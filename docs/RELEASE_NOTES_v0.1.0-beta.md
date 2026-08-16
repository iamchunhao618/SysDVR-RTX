# SysDVR RTX VSR v0.1.0-beta

This is the first public beta of an experimental Windows SysDVR fork that adds NVIDIA RTX Video Super Resolution and a low-latency GPU-direct D3D11 presentation backend.

## Highlights

- RTX Video Super Resolution for SysDVR's 1280x720, 30 fps H.264 stream
- Direct 1920x1080, 2560x1440, and 3840x2160 VSR output
- Low, Medium, High, and Ultra quality options
- GPU-direct D3D11 presentation that keeps enhanced output GPU-resident
- CPU-readback compatibility backend and automatic vanilla SysDVR fallback
- USB and TCP Bridge support
- Adapter-LUID verification for SDL/NGX same-GPU operation
- Existing windowed/fullscreen, overlay, screenshot, and rotation paths retained

Recommended starting settings are **1440p Medium with GPU direct**. **1440p High** is the current maximum-quality recommendation. Testing found little meaningful benefit from 4K output given the 720p source.

## Measured GPU work

On an NVIDIA GeForce RTX 4060 Laptop GPU:

| Setting | CPU submit | Native GPU work |
| --- | ---: | ---: |
| 1440p Medium | ~0.755 ms | ~6.614 ms |
| 1440p High | ~0.709 ms | ~8.406 ms |
| 4K High | ~0.672 ms | ~9.185 ms |

CPU submit time is asynchronous and is not total processing latency.

## Source-only release

This release intentionally has **no prebuilt binaries**. NVIDIA SDK redistribution terms were not assumed, so the release does not include NVIDIA SDK headers, libraries, packages, development feature DLLs, `nvngx_vsr.dll`, or locally built executables/DLLs. Builders must obtain the official NVIDIA RTX Video SDK separately and follow NVIDIA's terms.

Development-module VSR screenshots are also excluded because the captured output contains NVIDIA's explicit `DO NOT DISTRIBUTE` watermark.

## Known limitations

- The source remains 1280x720 at 30 fps; VSR cannot recreate native high-resolution detail.
- Mild audio crackling observed on the test system also occurred with official/vanilla SysDVR and was not shown to be RTX-specific.
- Rotation, arbitrary drag-resize, and fullscreen exit need broader community testing.
- Testing has primarily used an RTX 4060 Laptop GPU.
- Hybrid-GPU systems require SDL and NGX to select the same NVIDIA D3D11 adapter.

See the repository README, architecture overview, troubleshooting guide, and Phase 3 report for build instructions and technical detail.
