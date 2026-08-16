// D3D11 pixel-shader port of the MIT-licensed Microsoft MiniEngine
// compute-optimized FXAA 3.11 PC Quality implementation. See
// third_party/fxaa/README.md and LICENSE.txt for exact provenance.

Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer FxaaConstants : register(b0)
{
    float2 RcpFrame;
    float SubpixelRemoval;
    float EdgeThreshold;
    float EdgeThresholdMin;
    float3 FxaaPadding;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VertexOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        output.texcoord.x * 2.0 - 1.0,
        1.0 - output.texcoord.y * 2.0,
        0.0,
        1.0);
    return output;
}

float Luma(float3 color)
{
    // FXAA_GREEN_AS_LUMA: the source is gamma-encoded SDR RGBA8.
    return color.g;
}

float4 FxaaPS(VertexOutput input) : SV_TARGET
{
    const float2 uv = input.texcoord;
    const float4 center = SourceTexture.SampleLevel(LinearClampSampler, uv, 0.0);
    const float lumaM = Luma(center.rgb);
    const float lumaN = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(0.0, -RcpFrame.y), 0.0).rgb);
    const float lumaS = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(0.0, RcpFrame.y), 0.0).rgb);
    const float lumaW = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(-RcpFrame.x, 0.0), 0.0).rgb);
    const float lumaE = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(RcpFrame.x, 0.0), 0.0).rgb);

    const float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = rangeMax - rangeMin;
    if (range < max(EdgeThresholdMin, rangeMax * EdgeThreshold))
        return center;

    const float lumaNW = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(-RcpFrame.x, -RcpFrame.y), 0.0).rgb);
    const float lumaNE = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(RcpFrame.x, -RcpFrame.y), 0.0).rgb);
    const float lumaSW = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(-RcpFrame.x, RcpFrame.y), 0.0).rgb);
    const float lumaSE = Luma(SourceTexture.SampleLevel(LinearClampSampler, uv + float2(RcpFrame.x, RcpFrame.y), 0.0).rgb);

    const float edgeHorizontal =
        abs(lumaNW + lumaSW - 2.0 * lumaW) +
        2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
        abs(lumaNE + lumaSE - 2.0 * lumaE);
    const float edgeVertical =
        abs(lumaSW + lumaSE - 2.0 * lumaS) +
        2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
        abs(lumaNW + lumaNE - 2.0 * lumaN);
    const bool horizontal = edgeHorizontal >= edgeVertical;

    const float lumaNegative = horizontal ? lumaN : lumaW;
    const float lumaPositive = horizontal ? lumaS : lumaE;
    const float gradientNegative = abs(lumaNegative - lumaM);
    const float gradientPositive = abs(lumaPositive - lumaM);
    const bool useNegative = gradientNegative >= gradientPositive;
    const float gradientScaled = 0.25 * max(gradientNegative, gradientPositive);
    const float stepLength = horizontal ? RcpFrame.y : RcpFrame.x;
    const float lumaLocalAverage = 0.5 * (lumaM + (useNegative ? lumaNegative : lumaPositive));

    float2 currentUv = uv;
    if (horizontal)
        currentUv.y += (useNegative ? -0.5 : 0.5) * stepLength;
    else
        currentUv.x += (useNegative ? -0.5 : 0.5) * stepLength;

    const float2 searchOffset = horizontal ? float2(RcpFrame.x, 0.0) : float2(0.0, RcpFrame.y);
    float2 uvNegative = currentUv - searchOffset;
    float2 uvPositive = currentUv + searchOffset;
    float lumaEndNegative = Luma(SourceTexture.SampleLevel(LinearClampSampler, uvNegative, 0.0).rgb) - lumaLocalAverage;
    float lumaEndPositive = Luma(SourceTexture.SampleLevel(LinearClampSampler, uvPositive, 0.0).rgb) - lumaLocalAverage;
    bool doneNegative = abs(lumaEndNegative) >= gradientScaled;
    bool donePositive = abs(lumaEndPositive) >= gradientScaled;

    // PC Quality preset 25 search distances from Microsoft MiniEngine.
    static const float distances[7] = {2.5, 4.5, 6.5, 8.5, 10.5, 14.5, 22.5};
    [unroll]
    for (int i = 0; i < 7; ++i)
    {
        if (!doneNegative)
        {
            uvNegative = currentUv - searchOffset * distances[i];
            lumaEndNegative = Luma(SourceTexture.SampleLevel(LinearClampSampler, uvNegative, 0.0).rgb) - lumaLocalAverage;
            doneNegative = abs(lumaEndNegative) >= gradientScaled;
        }
        if (!donePositive)
        {
            uvPositive = currentUv + searchOffset * distances[i];
            lumaEndPositive = Luma(SourceTexture.SampleLevel(LinearClampSampler, uvPositive, 0.0).rgb) - lumaLocalAverage;
            donePositive = abs(lumaEndPositive) >= gradientScaled;
        }
    }

    const float distanceNegative = horizontal ? uv.x - uvNegative.x : uv.y - uvNegative.y;
    const float distancePositive = horizontal ? uvPositive.x - uv.x : uvPositive.y - uv.y;
    const bool negativeCloser = distanceNegative < distancePositive;
    const float shortestDistance = min(distanceNegative, distancePositive);
    const float edgeThickness = max(distanceNegative + distancePositive, 1e-6);
    const bool correctVariation =
        ((negativeCloser ? lumaEndNegative : lumaEndPositive) < 0.0) !=
        (lumaM < lumaLocalAverage);
    float pixelOffset = correctVariation
        ? (0.5 - shortestDistance / edgeThickness)
        : 0.0;

    const float averageNeighborLuma =
        ((lumaN + lumaS + lumaW + lumaE) * 2.0 +
         lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subpixel = saturate(abs(averageNeighborLuma - lumaM) / max(range, 1e-6));
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * SubpixelRemoval * 0.5;
    pixelOffset = max(pixelOffset, subpixel);

    float2 finalUv = uv;
    if (horizontal)
        finalUv.y += pixelOffset * (useNegative ? -RcpFrame.y : RcpFrame.y);
    else
        finalUv.x += pixelOffset * (useNegative ? -RcpFrame.x : RcpFrame.x);
    return float4(
        SourceTexture.SampleLevel(LinearClampSampler, finalUv, 0.0).rgb,
        center.a);
}
