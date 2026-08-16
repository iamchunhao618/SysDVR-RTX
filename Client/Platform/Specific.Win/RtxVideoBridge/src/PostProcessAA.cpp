#include "PostProcessAA.h"

#include "EmbeddedFxaaShader.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4828)
#endif
#include "EmbeddedSmaaShader.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "AreaTex.h"
#include "SearchTex.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace rvb
{
namespace
{
void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        char detail[128]{};
        std::snprintf(
            detail, sizeof(detail), "%s failed (HRESULT 0x%08X)",
            operation, static_cast<unsigned int>(result));
        throw std::runtime_error(detail);
    }
}

ComPtr<ID3DBlob> Compile(
    const char* source,
    const char* entry,
    const char* target)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const UINT flags =
        D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        "RtxVideoBridge post-AA",
        nullptr,
        nullptr,
        entry,
        target,
        flags,
        0,
        &bytecode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile(" + std::string(entry) + ") failed";
        if (errors != nullptr && errors->GetBufferSize() != 0)
        {
            message += ": ";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return bytecode;
}

struct Surface
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11ShaderResourceView> shaderResource;
};
}

struct PostProcessAA::Impl
{
    explicit Impl(ID3D11Device* suppliedDevice, ID3D11DeviceContext* suppliedContext)
        : device(suppliedDevice), context(suppliedContext)
    {
        if (device == nullptr || context == nullptr)
            throw std::invalid_argument("PostProcessAA requires a D3D11 device and context");
        CreateCommonResources();
    }

    void CreateCommonResources()
    {
        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        Check(
            device->CreateSamplerState(&samplerDescription, &linearClamp),
            "CreateSamplerState(post-AA linear clamp)");

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        Check(
            device->CreateRasterizerState(
                &rasterizerDescription, &rasterizerState),
            "CreateRasterizerState(post-AA)");

        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = FALSE;
        depthDescription.StencilEnable = FALSE;
        Check(
            device->CreateDepthStencilState(
                &depthDescription, &depthStencilState),
            "CreateDepthStencilState(post-AA)");

        D3D11_BLEND_DESC blendDescription{};
        blendDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(
            device->CreateBlendState(&blendDescription, &blendState),
            "CreateBlendState(post-AA)");

        D3D11_BUFFER_DESC constantsDescription{};
        constantsDescription.ByteWidth = 32;
        constantsDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantsDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(
            device->CreateBuffer(
                &constantsDescription, nullptr, &constants),
            "CreateBuffer(post-AA constants)");
    }

    Surface CreateSurface(DXGI_FORMAT format) const
    {
        Surface surface;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(
            device->CreateTexture2D(
                &description, nullptr, &surface.texture),
            "CreateTexture2D(post-AA surface)");
        Check(
            device->CreateRenderTargetView(
                surface.texture.Get(), nullptr, &surface.renderTarget),
            "CreateRenderTargetView(post-AA surface)");
        Check(
            device->CreateShaderResourceView(
                surface.texture.Get(), nullptr, &surface.shaderResource),
            "CreateShaderResourceView(post-AA surface)");
        return surface;
    }

    void EnsureSize(std::uint32_t requestedWidth, std::uint32_t requestedHeight)
    {
        if (width == requestedWidth && height == requestedHeight)
            return;
        ResetSize();
        width = requestedWidth;
        height = requestedHeight;
    }

    void EnsureFxaa()
    {
        if (fxaaPixelShader != nullptr)
            return;
        const ComPtr<ID3DBlob> vertex =
            Compile(kFxaaShaderSource, "FullscreenVS", "vs_5_0");
        const ComPtr<ID3DBlob> pixel =
            Compile(kFxaaShaderSource, "FxaaPS", "ps_5_0");
        Check(
            device->CreateVertexShader(
                vertex->GetBufferPointer(), vertex->GetBufferSize(),
                nullptr, &fxaaVertexShader),
            "CreateVertexShader(FXAA)");
        Check(
            device->CreatePixelShader(
                pixel->GetBufferPointer(), pixel->GetBufferSize(),
                nullptr, &fxaaPixelShader),
            "CreatePixelShader(FXAA)");
    }

    void EnsureSmaa()
    {
        if (smaaNeighborhoodPixelShader != nullptr)
            return;
        const auto createVertex = [&](const char* entry, ComPtr<ID3D11VertexShader>& shader) {
            const ComPtr<ID3DBlob> code = Compile(kSmaaShaderSource, entry, "vs_5_0");
            Check(
                device->CreateVertexShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
                entry);
        };
        const auto createPixel = [&](const char* entry, ComPtr<ID3D11PixelShader>& shader) {
            const ComPtr<ID3DBlob> code = Compile(kSmaaShaderSource, entry, "ps_5_0");
            Check(
                device->CreatePixelShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
                entry);
        };
        createVertex("SmaaEdgeVS", smaaEdgeVertexShader);
        createPixel("SmaaEdgePS", smaaEdgePixelShader);
        createVertex("SmaaWeightVS", smaaWeightVertexShader);
        createPixel("SmaaWeightPS", smaaWeightPixelShader);
        createVertex("SmaaNeighborhoodVS", smaaNeighborhoodVertexShader);
        createPixel("SmaaNeighborhoodPS", smaaNeighborhoodPixelShader);

        D3D11_TEXTURE2D_DESC areaDescription{};
        areaDescription.Width = AREATEX_WIDTH;
        areaDescription.Height = AREATEX_HEIGHT;
        areaDescription.MipLevels = 1;
        areaDescription.ArraySize = 1;
        areaDescription.Format = DXGI_FORMAT_R8G8_UNORM;
        areaDescription.SampleDesc.Count = 1;
        areaDescription.Usage = D3D11_USAGE_IMMUTABLE;
        areaDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA areaData{};
        areaData.pSysMem = areaTexBytes;
        areaData.SysMemPitch = AREATEX_PITCH;
        Check(
            device->CreateTexture2D(
                &areaDescription, &areaData, &smaaAreaTexture),
            "CreateTexture2D(SMAA AreaTex)");
        Check(
            device->CreateShaderResourceView(
                smaaAreaTexture.Get(), nullptr, &smaaAreaShaderResource),
            "CreateShaderResourceView(SMAA AreaTex)");

        D3D11_TEXTURE2D_DESC searchDescription{};
        searchDescription.Width = SEARCHTEX_WIDTH;
        searchDescription.Height = SEARCHTEX_HEIGHT;
        searchDescription.MipLevels = 1;
        searchDescription.ArraySize = 1;
        searchDescription.Format = DXGI_FORMAT_R8_UNORM;
        searchDescription.SampleDesc.Count = 1;
        searchDescription.Usage = D3D11_USAGE_IMMUTABLE;
        searchDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA searchData{};
        searchData.pSysMem = searchTexBytes;
        searchData.SysMemPitch = SEARCHTEX_PITCH;
        Check(
            device->CreateTexture2D(
                &searchDescription, &searchData, &smaaSearchTexture),
            "CreateTexture2D(SMAA SearchTex)");
        Check(
            device->CreateShaderResourceView(
                smaaSearchTexture.Get(), nullptr, &smaaSearchShaderResource),
            "CreateShaderResourceView(SMAA SearchTex)");
    }

    void EnsureFxaaSurface()
    {
        if (fxaaOutput.texture == nullptr)
            fxaaOutput = CreateSurface(DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    void EnsureSmaaSurfaces()
    {
        if (smaaEdges.texture != nullptr)
            return;
        smaaEdges = CreateSurface(DXGI_FORMAT_R8G8_UNORM);
        smaaWeights = CreateSurface(DXGI_FORMAT_R8G8B8A8_UNORM);
        smaaOutput = CreateSurface(DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    void UpdateConstants(const std::array<float, 8>& values)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(
            context->Map(
                constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Map(post-AA constants)");
        std::memcpy(mapped.pData, values.data(), sizeof(values));
        context->Unmap(constants.Get(), 0);
    }

    void PreparePass(ID3D11RenderTargetView* target, const float clear[4])
    {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizerState.Get());
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetDepthStencilState(depthStencilState.Get(), 0);
        context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFFu);
        context->ClearRenderTargetView(target, clear);
        context->IASetInputLayout(nullptr);
        context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->GSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* constantBuffer = constants.Get();
        context->VSSetConstantBuffers(0, 1, &constantBuffer);
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11SamplerState* sampler = linearClamp.Get();
        context->PSSetSamplers(0, 1, &sampler);
    }

    void UnbindShaderResources(std::uint32_t count)
    {
        ID3D11ShaderResourceView* empty[3]{};
        context->PSSetShaderResources(0, count, empty);
    }

    PostAaOutput ApplyFxaa(
        ID3D11ShaderResourceView* input,
        const PostAaParameters& parameters,
        const PostAaQueries& queries)
    {
        EnsureFxaa();
        EnsureFxaaSurface();
        UpdateConstants({
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            std::clamp(parameters.fxaaSubpixel, 0.0f, 1.0f),
            std::clamp(parameters.fxaaEdgeThreshold, 0.063f, 0.333f),
            std::clamp(parameters.fxaaEdgeThresholdMin, 0.0312f, 0.0833f),
            0.0f, 0.0f, 0.0f,
        });
        static constexpr float clear[4]{};
        PreparePass(fxaaOutput.renderTarget.Get(), clear);
        context->VSSetShader(fxaaVertexShader.Get(), nullptr, 0);
        context->PSSetShader(fxaaPixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &input);
        context->Draw(3, 0);
        UnbindShaderResources(1);
        if (queries.edge != nullptr)
            context->End(queries.edge);
        if (queries.blend != nullptr)
            context->End(queries.blend);
        if (queries.neighborhood != nullptr)
            context->End(queries.neighborhood);
        return {fxaaOutput.texture.Get(), fxaaOutput.shaderResource.Get()};
    }

    PostAaOutput ApplySmaa(
        ID3D11ShaderResourceView* input,
        const PostAaQueries& queries)
    {
        EnsureSmaa();
        EnsureSmaaSurfaces();
        UpdateConstants({
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f, 0.0f, 0.0f, 0.0f,
        });
        static constexpr float clear[4]{};

        PreparePass(smaaEdges.renderTarget.Get(), clear);
        context->VSSetShader(smaaEdgeVertexShader.Get(), nullptr, 0);
        context->PSSetShader(smaaEdgePixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &input);
        context->Draw(3, 0);
        UnbindShaderResources(1);
        if (queries.edge != nullptr)
            context->End(queries.edge);

        PreparePass(smaaWeights.renderTarget.Get(), clear);
        context->VSSetShader(smaaWeightVertexShader.Get(), nullptr, 0);
        context->PSSetShader(smaaWeightPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* weightInputs[] = {
            smaaEdges.shaderResource.Get(),
            smaaAreaShaderResource.Get(),
            smaaSearchShaderResource.Get(),
        };
        context->PSSetShaderResources(0, 3, weightInputs);
        context->Draw(3, 0);
        UnbindShaderResources(3);
        if (queries.blend != nullptr)
            context->End(queries.blend);

        PreparePass(smaaOutput.renderTarget.Get(), clear);
        context->VSSetShader(smaaNeighborhoodVertexShader.Get(), nullptr, 0);
        context->PSSetShader(smaaNeighborhoodPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* neighborhoodInputs[] = {
            input,
            smaaWeights.shaderResource.Get(),
        };
        context->PSSetShaderResources(0, 2, neighborhoodInputs);
        context->Draw(3, 0);
        UnbindShaderResources(2);
        if (queries.neighborhood != nullptr)
            context->End(queries.neighborhood);
        return {smaaOutput.texture.Get(), smaaOutput.shaderResource.Get()};
    }

    PostAaOutput Apply(
        PostAaMode mode,
        ID3D11Texture2D* inputTexture,
        ID3D11ShaderResourceView* input,
        std::uint32_t requestedWidth,
        std::uint32_t requestedHeight,
        const PostAaParameters& parameters,
        const PostAaQueries& queries)
    {
        if (inputTexture == nullptr || input == nullptr ||
            requestedWidth == 0 || requestedHeight == 0)
            throw std::invalid_argument("Post-AA input is invalid");
        EnsureSize(requestedWidth, requestedHeight);
        switch (mode)
        {
        case PostAaMode::Off:
            if (queries.edge != nullptr) context->End(queries.edge);
            if (queries.blend != nullptr) context->End(queries.blend);
            if (queries.neighborhood != nullptr) context->End(queries.neighborhood);
            return {inputTexture, input};
        case PostAaMode::Fxaa:
            return ApplyFxaa(input, parameters, queries);
        case PostAaMode::Smaa1x:
            return ApplySmaa(input, queries);
        default:
            throw std::invalid_argument("Unknown post-AA mode");
        }
    }

    void ResetSize()
    {
        fxaaOutput = {};
        smaaEdges = {};
        smaaWeights = {};
        smaaOutput = {};
        width = 0;
        height = 0;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11Buffer> constants;

    ComPtr<ID3D11VertexShader> fxaaVertexShader;
    ComPtr<ID3D11PixelShader> fxaaPixelShader;
    Surface fxaaOutput;

    ComPtr<ID3D11VertexShader> smaaEdgeVertexShader;
    ComPtr<ID3D11PixelShader> smaaEdgePixelShader;
    ComPtr<ID3D11VertexShader> smaaWeightVertexShader;
    ComPtr<ID3D11PixelShader> smaaWeightPixelShader;
    ComPtr<ID3D11VertexShader> smaaNeighborhoodVertexShader;
    ComPtr<ID3D11PixelShader> smaaNeighborhoodPixelShader;
    ComPtr<ID3D11Texture2D> smaaAreaTexture;
    ComPtr<ID3D11ShaderResourceView> smaaAreaShaderResource;
    ComPtr<ID3D11Texture2D> smaaSearchTexture;
    ComPtr<ID3D11ShaderResourceView> smaaSearchShaderResource;
    Surface smaaEdges;
    Surface smaaWeights;
    Surface smaaOutput;
};

PostProcessAA::PostProcessAA(
    ID3D11Device* device,
    ID3D11DeviceContext* context)
    : impl_(std::make_unique<Impl>(device, context))
{
}

PostProcessAA::~PostProcessAA() = default;
PostProcessAA::PostProcessAA(PostProcessAA&&) noexcept = default;
PostProcessAA& PostProcessAA::operator=(PostProcessAA&&) noexcept = default;

PostAaOutput PostProcessAA::Apply(
    PostAaMode mode,
    ID3D11Texture2D* inputTexture,
    ID3D11ShaderResourceView* input,
    std::uint32_t width,
    std::uint32_t height,
    const PostAaParameters& parameters,
    const PostAaQueries& queries)
{
    return impl_->Apply(
        mode, inputTexture, input, width, height, parameters, queries);
}

void PostProcessAA::ResetSize()
{
    impl_->ResetSize();
}
}
