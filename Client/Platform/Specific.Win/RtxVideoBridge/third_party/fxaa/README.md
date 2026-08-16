# FXAA provenance

`shaders/Fxaa3_11_MicrosoftPort.hlsl` is a D3D11 pixel-shader adaptation of
Microsoft MiniEngine's compute-optimized **FXAA 3.11 PC Quality** implementation:

https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine/Core/Shaders

The source reference is `FXAAPass1CS.hlsli` and `FXAAPass2CS.hlsli` from the
Microsoft DirectX Graphics Samples repository. That repository is MIT licensed;
the license is reproduced in `LICENSE.txt`. Modification and redistribution are
permitted provided the copyright and permission notice are retained.

This port uses the preset-25 search distances, green-as-luma input, a subpixel
amount of 0.50, edge threshold 1/6, and minimum edge threshold 1/12. It is a
spatial, same-frame pass with no depth, motion vectors, or frame history.

The historical NVIDIA `Fxaa3_11.h` header was deliberately not copied because
the surviving header's notice is a warranty disclaimer rather than an explicit
redistribution license.
