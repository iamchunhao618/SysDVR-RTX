#pragma once

#include "WicImage.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_vsr.h>
#include <nvsdk_ngx_helpers_vsr.h>

enum class AppExitCode : int
{
    Success = 0,
    InvalidArguments = 2,
    InputError = 3,
    MissingFeatureDll = 10,
    WrongAdapter = 11,
    VsrUnsupported = 12,
    NgxInitializationFailure = 13,
    FeatureCreationFailure = 14,
    EvaluationFailure = 15,
    DeviceLost = 16,
    D3dFailure = 17,
};

class AppError final : public std::runtime_error
{
public:
    AppError(AppExitCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code)
    {
    }

    AppExitCode Code() const noexcept { return code_; }

private:
    AppExitCode code_;
};

struct AdapterInfo
{
    std::uint32_t index = 0;
    std::wstring name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemory = 0;
    LUID luid{};
    bool software = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
};

struct FrameTiming
{
    double uploadSubmitCpuMs = 0.0;
    double evaluateCallCpuMs = 0.0;
    double copySubmitCpuMs = 0.0;
    double mapWaitCpuMs = 0.0;
    double cpuRowCopyMs = 0.0;
    double readbackCpuMs = 0.0;
    double processCpuMs = 0.0;
    double queryResolveCpuMs = 0.0;
    double uploadGpuMs = 0.0;
    double evaluateGpuMs = 0.0;
    double readbackCopyGpuMs = 0.0;
    double totalGpuMs = 0.0;
    bool gpuTimingValid = false;
};

std::string Utf8(const std::wstring& value);
std::string NgxResultText(NVSDK_NGX_Result result);

class RtxVsrBenchmark final
{
public:
    static constexpr std::uint32_t InputWidth = 1280;
    static constexpr std::uint32_t InputHeight = 720;

    RtxVsrBenchmark() = default;
    ~RtxVsrBenchmark();

    RtxVsrBenchmark(const RtxVsrBenchmark&) = delete;
    RtxVsrBenchmark& operator=(const RtxVsrBenchmark&) = delete;

    static std::vector<AdapterInfo> EnumerateAdapters();

    void Initialize(
        const AdapterInfo& adapter,
        const std::filesystem::path& featureDirectory,
        const std::filesystem::path& applicationDataDirectory,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight);

    FrameTiming ProcessFrame(
        const ImageRgba& input,
        NVSDK_NGX_VSR_QualityLevel quality,
        ImageRgba& output);

    void Shutdown() noexcept;

    const AdapterInfo& SelectedAdapter() const { return selectedAdapter_; }
    const std::filesystem::path& LoadedFeatureModule() const
    {
        return loadedFeatureModule_;
    }
    int VsrAvailable() const { return vsrAvailable_; }
    int NeedsUpdatedDriver() const { return needsUpdatedDriver_; }
    unsigned int MinimumDriverMajor() const { return minimumDriverMajor_; }
    unsigned int MinimumDriverMinor() const { return minimumDriverMinor_; }
    std::size_t ScratchBufferBytes() const { return scratchBufferBytes_; }
    std::uint32_t OutputWidth() const { return outputWidth_; }
    std::uint32_t OutputHeight() const { return outputHeight_; }

private:
    void CreateDeviceAndTextures(const AdapterInfo& adapter);
    void InitializeNgx(
        const std::filesystem::path& featureDirectory,
        const std::filesystem::path& applicationDataDirectory);
    void CreateTimingQueries();
    void ThrowIfDeviceLost(const char* operation);

    AdapterInfo selectedAdapter_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture_;
    Microsoft::WRL::ComPtr<ID3D11Query> disjointQuery_;
    Microsoft::WRL::ComPtr<ID3D11Query> timestampStartQuery_;
    Microsoft::WRL::ComPtr<ID3D11Query> timestampUploadQuery_;
    Microsoft::WRL::ComPtr<ID3D11Query> timestampEvaluateQuery_;
    Microsoft::WRL::ComPtr<ID3D11Query> timestampCopyQuery_;

    NVSDK_NGX_Parameter* ngxParameters_ = nullptr;
    NVSDK_NGX_Handle* vsrFeature_ = nullptr;
    bool ngxInitialized_ = false;
    int vsrAvailable_ = 0;
    int needsUpdatedDriver_ = 0;
    unsigned int minimumDriverMajor_ = 0;
    unsigned int minimumDriverMinor_ = 0;
    std::size_t scratchBufferBytes_ = 0;
    std::filesystem::path loadedFeatureModule_;
    std::uint32_t outputWidth_ = 2560;
    std::uint32_t outputHeight_ = 1440;
};
