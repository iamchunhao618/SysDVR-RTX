#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <d3d11.h>

namespace rvb
{
enum class PostAaMode : std::uint32_t
{
    Off = 0,
    Fxaa = 1,
    Smaa1x = 2,
};

struct PostAaParameters
{
    float fxaaSubpixel = 0.50f;
    float fxaaEdgeThreshold = 1.0f / 6.0f;
    float fxaaEdgeThresholdMin = 1.0f / 12.0f;
};

struct PostAaQueries
{
    ID3D11Query* edge = nullptr;
    ID3D11Query* blend = nullptr;
    ID3D11Query* neighborhood = nullptr;
};

struct PostAaOutput
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* shaderResource = nullptr;
};

class PostProcessAA
{
public:
    PostProcessAA(ID3D11Device* device, ID3D11DeviceContext* context);
    ~PostProcessAA();
    PostProcessAA(PostProcessAA&&) noexcept;
    PostProcessAA& operator=(PostProcessAA&&) noexcept;
    PostProcessAA(const PostProcessAA&) = delete;
    PostProcessAA& operator=(const PostProcessAA&) = delete;

    PostAaOutput Apply(
        PostAaMode mode,
        ID3D11Texture2D* inputTexture,
        ID3D11ShaderResourceView* input,
        std::uint32_t width,
        std::uint32_t height,
        const PostAaParameters& parameters,
        const PostAaQueries& queries);

    void ResetSize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
