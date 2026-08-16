# Troubleshooting RTX VSR

## RTX VSR is unavailable

Confirm all of the following:

- The Windows client is running on a supported NVIDIA RTX GPU.
- The NVIDIA driver is compatible with the official RTX Video SDK release you obtained.
- `RtxVideoBridge.dll` exists under the published client's `runtimes/win-x64/native` directory.
- `SYSDVR_RTX_VIDEO_FEATURE_DIR` points to the directory containing the official `nvngx_vsr.dll`, or `RTX_VIDEO_SDK_DIR` points to the SDK root.
- The feature DLL architecture is Windows x64.

The client logs the selected DXGI adapter, vendor/device ID, LUID, driver, and feature-loading failure. Do not attach the proprietary feature DLL to bug reports.

## GPU-direct mode does not start

GPU-direct mode requires SDL's D3D11 renderer. Select the GPU-direct backend, close the client, and start it again. The client verifies that SDL and NGX selected the same NVIDIA adapter LUID.

On hybrid-GPU laptops, use Windows Graphics settings or the NVIDIA control panel to assign the client to the NVIDIA high-performance GPU. If the adapter identities still differ, use the CPU-readback compatibility backend or vanilla rendering and include the non-sensitive adapter log lines in the issue.

## The application falls back to vanilla video

This is intentional failure containment. A fatal RTX initialization or frame-processing error disables enhancement for the process session and resumes the original IYUV texture. Review the first RTX error in the log; later repeated errors are suppressed.

## Development watermark appears

NVIDIA's SDK development feature module adds a visible `DO NOT DISTRIBUTE` watermark. It is suitable only for development as permitted by NVIDIA's terms. Do not redistribute the module or publish output captured from it. This repository does not provide a release feature DLL.

## Audio crackling

Mild crackling was observed on the primary test system in both this fork and official/vanilla SysDVR. Current evidence does not identify RTX VSR as the cause. Compare with VSR disabled and, when possible, with an official SysDVR client using the same USB/network path before filing an RTX-specific issue.

## 4K does not look better than 1440p

The source contains only 1280x720 detail. In current testing, 4K VSR followed by downscaling to a 1440p display produced little meaningful advantage while increasing GPU work and bandwidth. Use 1440p Medium as the starting point and compare motion, foliage stability, ringing, and fine UI edges rather than relying only on a still image.

## Useful issue information

Include:

- GPU model and driver version
- Windows version
- output resolution, quality, and backend
- USB or TCP Bridge transport
- whether vanilla and CPU-readback modes work
- relevant client log lines with usernames, directories, IP addresses, and tokens removed

Never attach NVIDIA SDK binaries, `nvngx_vsr.dll`, private SDK packages, or development output marked `DO NOT DISTRIBUTE`.
