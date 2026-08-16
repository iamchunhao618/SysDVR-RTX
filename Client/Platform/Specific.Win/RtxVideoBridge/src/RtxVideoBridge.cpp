#include "RtxVideoBridge.h"
#include "PostProcessAA.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_vsr.h>
#include <nvsdk_ngx_helpers_vsr.h>

using Microsoft::WRL::ComPtr;

namespace
{
thread_local std::string g_lastError;
using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

class BridgeError final : public std::runtime_error
{
public:
    BridgeError(RvbStatus status, std::string message)
        : std::runtime_error(std::move(message)), status_(status)
    {
    }

    RvbStatus Status() const noexcept { return status_; }

private:
    RvbStatus status_;
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
    ComPtr<IDXGIAdapter1> adapter;
};

std::string Hex32(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::string Utf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return "<UTF-16 conversion failed>";
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string NgxResultText(NVSDK_NGX_Result result)
{
    const char* name = "Unknown";
    switch (result)
    {
    case NVSDK_NGX_Result_Success: name = "Success"; break;
    case NVSDK_NGX_Result_Fail: name = "Fail"; break;
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
        name = "FeatureNotSupported"; break;
    case NVSDK_NGX_Result_FAIL_PlatformError: name = "PlatformError"; break;
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
        name = "FeatureAlreadyExists"; break;
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: name = "FeatureNotFound"; break;
    case NVSDK_NGX_Result_FAIL_InvalidParameter: name = "InvalidParameter"; break;
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:
        name = "ScratchBufferTooSmall"; break;
    case NVSDK_NGX_Result_FAIL_NotInitialized: name = "NotInitialized"; break;
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
        name = "UnsupportedInputFormat"; break;
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: name = "RWFlagMissing"; break;
    case NVSDK_NGX_Result_FAIL_MissingInput: name = "MissingInput"; break;
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
        name = "UnableToInitializeFeature"; break;
    case NVSDK_NGX_Result_FAIL_OutOfDate: name = "OutOfDate"; break;
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: name = "OutOfGPUMemory"; break;
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: name = "UnsupportedFormat"; break;
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
        name = "UnableToWriteToAppDataPath"; break;
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
        name = "UnsupportedParameter"; break;
    case NVSDK_NGX_Result_FAIL_Denied: name = "Denied"; break;
    case NVSDK_NGX_Result_FAIL_NotImplemented: name = "NotImplemented"; break;
    }

    std::ostringstream stream;
    stream << name << " (" << Hex32(static_cast<std::uint32_t>(result)) << ")";
    return stream.str();
}

void CheckHr(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result))
    {
        return;
    }

    const RvbStatus status =
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
            ? RVB_STATUS_DEVICE_LOST
            : RVB_STATUS_D3D_FAILED;
    throw BridgeError(
        status,
        std::string(operation) + " failed with HRESULT " +
            Hex32(static_cast<std::uint32_t>(result)));
}

template <typename T>
void WaitForQuery(
    ID3D11DeviceContext* context,
    ID3D11Query* query,
    T& value,
    const char* name)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        const HRESULT result = context->GetData(
            query, &value, static_cast<UINT>(sizeof(value)), 0);
        if (result == S_OK)
        {
            return;
        }
        if (FAILED(result))
        {
            CheckHr(result, name);
        }
        if (Clock::now() >= deadline)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                std::string("Timed out resolving ") + name);
        }
        std::this_thread::yield();
    }
}

void ValidateOutputSize(
    std::uint32_t outputWidth,
    std::uint32_t outputHeight)
{
    const bool supported =
        (outputWidth == RVB_OUTPUT_1080P_WIDTH &&
         outputHeight == RVB_OUTPUT_1080P_HEIGHT) ||
        (outputWidth == RVB_OUTPUT_1440P_WIDTH &&
         outputHeight == RVB_OUTPUT_1440P_HEIGHT) ||
        (outputWidth == RVB_OUTPUT_2160P_WIDTH &&
         outputHeight == RVB_OUTPUT_2160P_HEIGHT);
    if (!supported)
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "Output resolution must be 1920x1080, 2560x1440, or 3840x2160");
    }
}

std::filesystem::path ModulePath(HMODULE module)
{
    if (module == nullptr)
    {
        return {};
    }

    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

class MultithreadGuard
{
public:
    explicit MultithreadGuard(ID3D10Multithread* multithread)
        : multithread_(multithread)
    {
        if (multithread_ != nullptr)
        {
            multithread_->Enter();
        }
    }

    ~MultithreadGuard()
    {
        if (multithread_ != nullptr)
        {
            multithread_->Leave();
        }
    }

private:
    ID3D10Multithread* multithread_;
};

class ContextStateGuard final
{
public:
    ContextStateGuard(
        ID3D11DeviceContext1* context,
        ID3DDeviceContextState* bridgeState)
        : context_(context)
    {
        if (context_ == nullptr && bridgeState == nullptr)
        {
            return;
        }
        if (context_ == nullptr || bridgeState == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "D3D11.1 context-state isolation is unavailable");
        }
        context_->SwapDeviceContextState(bridgeState, &previousState_);
    }

    ~ContextStateGuard()
    {
        if (context_ != nullptr && previousState_ != nullptr)
        {
            context_->SwapDeviceContextState(previousState_.Get(), nullptr);
        }
    }

    ContextStateGuard(const ContextStateGuard&) = delete;
    ContextStateGuard& operator=(const ContextStateGuard&) = delete;

private:
    ID3D11DeviceContext1* context_ = nullptr;
    ComPtr<ID3DDeviceContextState> previousState_;
};

void QueryOptionalInt(
    NVSDK_NGX_Parameter* parameters,
    const char* name,
    int& value)
{
    const auto result = parameters->Get(name, &value);
    if (NVSDK_NGX_FAILED(result))
    {
        value = 0;
    }
}

void QueryOptionalUnsigned(
    NVSDK_NGX_Parameter* parameters,
    const char* name,
    unsigned int& value)
{
    const auto result = parameters->Get(name, &value);
    if (NVSDK_NGX_FAILED(result))
    {
        value = 0;
    }
}

std::vector<AdapterInfo> EnumerateAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::vector<AdapterInfo> adapters;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        CheckHr(result, "IDXGIFactory1::EnumAdapters1");

        DXGI_ADAPTER_DESC1 description{};
        CheckHr(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");

        AdapterInfo info;
        info.index = index;
        info.name = description.Description;
        info.vendorId = description.VendorId;
        info.deviceId = description.DeviceId;
        info.dedicatedVideoMemory = description.DedicatedVideoMemory;
        info.luid = description.AdapterLuid;
        info.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        info.adapter = std::move(adapter);
        adapters.push_back(std::move(info));
    }
    return adapters;
}

AdapterInfo SelectAdapter(int adapterIndex)
{
    auto adapters = EnumerateAdapters();

    if (adapterIndex >= 0)
    {
        const auto match = std::find_if(
            adapters.begin(), adapters.end(),
            [adapterIndex](const AdapterInfo& adapter) {
                return adapter.index == static_cast<std::uint32_t>(adapterIndex);
            });
        if (match == adapters.end())
        {
            throw BridgeError(
                RVB_STATUS_WRONG_ADAPTER,
                "Requested DXGI adapter index " + std::to_string(adapterIndex) +
                    " does not exist");
        }
        if (match->software || match->vendorId != RVB_NVIDIA_VENDOR_ID)
        {
            throw BridgeError(
                RVB_STATUS_WRONG_ADAPTER,
                "Requested DXGI adapter " + std::to_string(adapterIndex) +
                    " ('" + Utf8(match->name) + "', vendor " +
                    Hex32(match->vendorId) + ") is not an NVIDIA hardware adapter");
        }
        return *match;
    }

    const auto match = std::find_if(
        adapters.begin(), adapters.end(), [](const AdapterInfo& adapter) {
            return !adapter.software && adapter.vendorId == RVB_NVIDIA_VENDOR_ID;
        });
    if (match == adapters.end())
    {
        throw BridgeError(
            RVB_STATUS_WRONG_ADAPTER,
            "No NVIDIA hardware DXGI adapter was found");
    }
    return *match;
}

AdapterInfo AdapterFromDevice(ID3D11Device* device)
{
    if (device == nullptr)
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "The external ID3D11Device pointer must not be NULL");
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    CheckHr(
        device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)),
        "ID3D11Device::QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");
    ComPtr<IDXGIAdapter1> adapter1;
    CheckHr(
        adapter.As(&adapter1),
        "IDXGIAdapter::QueryInterface(IDXGIAdapter1)");

    DXGI_ADAPTER_DESC1 description{};
    CheckHr(adapter1->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");

    AdapterInfo info;
    info.name = description.Description;
    info.vendorId = description.VendorId;
    info.deviceId = description.DeviceId;
    info.dedicatedVideoMemory = description.DedicatedVideoMemory;
    info.luid = description.AdapterLuid;
    info.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info.adapter = std::move(adapter1);

    for (const AdapterInfo& candidate : EnumerateAdapters())
    {
        if (candidate.luid.HighPart == info.luid.HighPart &&
            candidate.luid.LowPart == info.luid.LowPart)
        {
            info.index = candidate.index;
            break;
        }
    }
    return info;
}

RvbD3D11DeviceInfo MakeDeviceInfo(ID3D11Device* device)
{
    const AdapterInfo adapter = AdapterFromDevice(device);
    RvbD3D11DeviceInfo info{};
    info.struct_size = sizeof(RvbD3D11DeviceInfo);
    info.adapter_index = adapter.index;
    info.adapter_vendor_id = adapter.vendorId;
    info.adapter_device_id = adapter.deviceId;
    info.adapter_dedicated_video_memory = adapter.dedicatedVideoMemory;
    info.adapter_luid_high = adapter.luid.HighPart;
    info.adapter_luid_low = adapter.luid.LowPart;
    info.feature_level = static_cast<std::uint32_t>(device->GetFeatureLevel());
    const std::string name = Utf8(adapter.name);
    const std::size_t bytes = std::min(
        name.size(),
        static_cast<std::size_t>(RVB_ADAPTER_NAME_UTF8_SIZE - 1u));
    std::memcpy(info.adapter_name_utf8, name.data(), bytes);
    info.adapter_name_utf8[bytes] = '\0';
    return info;
}

void ValidateDirectories(
    const wchar_t* featureDirectory,
    const wchar_t* applicationDataDirectory)
{
    if (featureDirectory == nullptr || featureDirectory[0] == L'\0')
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "feature_directory must point to the official SDK dev/rel directory");
    }
    if (applicationDataDirectory == nullptr || applicationDataDirectory[0] == L'\0')
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "application_data_directory must not be empty");
    }

    const auto featureDll =
        std::filesystem::path(featureDirectory) / L"nvngx_vsr.dll";
    if (!std::filesystem::is_regular_file(featureDll))
    {
        throw BridgeError(
            RVB_STATUS_MISSING_FEATURE_DLL,
            "Required official RTX VSR feature DLL is missing: " +
                Utf8(featureDll.wstring()));
    }

    std::error_code error;
    std::filesystem::create_directories(applicationDataDirectory, error);
    if (error)
    {
        throw BridgeError(
            RVB_STATUS_NGX_INITIALIZATION_FAILED,
            "Could not create NGX application data directory: " + error.message());
    }
}

ComPtr<ID3DBlob> CompileShader(
    const char* source,
    const char* entryPoint,
    const char* target)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        "RtxVideoBridgeDirect.hlsl",
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader,
        &errors);
    if (FAILED(result))
    {
        std::string detail;
        if (errors != nullptr)
        {
            detail.assign(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw BridgeError(
            RVB_STATUS_D3D_FAILED,
            std::string("D3DCompile(") + entryPoint + ") failed with HRESULT " +
                Hex32(static_cast<std::uint32_t>(result)) +
                (detail.empty() ? std::string{} : ": " + detail));
    }
    return shader;
}

class Processor final
{
public:
    Processor() = default;
    ~Processor() { Shutdown(); }

    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    void Initialize(
        int adapterIndex,
        const std::filesystem::path& featureDirectory,
        const std::filesystem::path& applicationDataDirectory,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight)
    {
        Shutdown();
        ValidateOutputSize(outputWidth, outputHeight);
        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        selectedAdapter_ = SelectAdapter(adapterIndex);
        CreateDeviceAndTextures();
        InitializeNgx(featureDirectory, applicationDataDirectory);
    }

    void InitializeExternal(
        ID3D11Device* externalDevice,
        const std::filesystem::path& featureDirectory,
        const std::filesystem::path& applicationDataDirectory,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight)
    {
        Shutdown();
        ValidateOutputSize(outputWidth, outputHeight);
        selectedAdapter_ = AdapterFromDevice(externalDevice);
        if (selectedAdapter_.software ||
            selectedAdapter_.vendorId != RVB_NVIDIA_VENDOR_ID)
        {
            throw BridgeError(
                RVB_STATUS_WRONG_ADAPTER,
                "SDL D3D11 adapter '" + Utf8(selectedAdapter_.name) +
                    "' (vendor " + Hex32(selectedAdapter_.vendorId) +
                    ") is not the NVIDIA RTX adapter required by VSR");
        }
        if (externalDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "The SDL D3D11 device does not support feature level 11.0");
        }

        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        externalDevice_ = true;
        device_ = externalDevice;
        externalDevice->GetImmediateContext(&context_);
        ConfigureDeviceResources();
        InitializeNgx(featureDirectory, applicationDataDirectory);
    }

    RvbFrameTiming Process(
        const void* input,
        std::uint32_t inputStride,
        void* output,
        std::uint32_t outputStride,
        std::uint32_t quality)
    {
        if (externalDevice_)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_process_rgba8 is unavailable for a GPU-direct handle");
        }
        if (vsrFeature_ == nullptr || ngxParameters_ == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_NGX_INITIALIZATION_FAILED,
                "rvb_process_rgba8 was called before successful initialization");
        }
        if (input == nullptr || output == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_process_rgba8 input and output buffers must not be NULL");
        }
        if (inputStride < RVB_INPUT_WIDTH * 4u ||
            outputStride < outputWidth_ * 4u)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "RGBA8 strides are smaller than the fixed frame row size");
        }
        ValidateQuality(quality);

        RvbFrameTiming timing{};
        timing.struct_size = static_cast<std::uint32_t>(sizeof(RvbFrameTiming));
        const auto processStart = Clock::now();

        context_->Begin(disjointQuery_.Get());
        context_->End(timestampStartQuery_.Get());

        const auto uploadStart = Clock::now();
        context_->UpdateSubresource(
            inputTexture_.Get(), 0, nullptr, input, inputStride, 0);
        timing.upload_submit_cpu_ms =
            Milliseconds(Clock::now() - uploadStart);
        context_->End(timestampUploadQuery_.Get());

        NVSDK_NGX_D3D11_VSR_Eval_Params evaluateParameters{};
        evaluateParameters.pInput = inputTexture_.Get();
        evaluateParameters.pOutput = outputTexture_.Get();
        evaluateParameters.InputSubrectBase = {0, 0};
        evaluateParameters.InputSubrectSize = {RVB_INPUT_WIDTH, RVB_INPUT_HEIGHT};
        evaluateParameters.OutputSubrectBase = {0, 0};
        evaluateParameters.OutputSubrectSize = {outputWidth_, outputHeight_};
        evaluateParameters.QualityLevel =
            static_cast<NVSDK_NGX_VSR_QualityLevel>(quality);

        NVSDK_NGX_Result evaluateResult{};
        const auto evaluateStart = Clock::now();
        {
            MultithreadGuard guard(multithread_.Get());
            evaluateResult = NGX_D3D11_EVALUATE_VSR_EXT(
                context_.Get(), vsrFeature_, ngxParameters_, &evaluateParameters);
        }
        timing.evaluate_call_cpu_ms =
            Milliseconds(Clock::now() - evaluateStart);
        context_->End(timestampEvaluateQuery_.Get());
        if (NVSDK_NGX_FAILED(evaluateResult))
        {
            ThrowIfDeviceLost("NGX_D3D11_EVALUATE_VSR_EXT");
            throw BridgeError(
                RVB_STATUS_PROCESSING_FAILED,
                "NGX_D3D11_EVALUATE_VSR_EXT failed: " +
                    NgxResultText(evaluateResult));
        }

        const auto copyStart = Clock::now();
        context_->CopyResource(stagingTexture_.Get(), outputTexture_.Get());
        timing.copy_submit_cpu_ms = Milliseconds(Clock::now() - copyStart);
        context_->End(timestampCopyQuery_.Get());
        context_->End(disjointQuery_.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto mapStart = Clock::now();
        const HRESULT mapResult = context_->Map(
            stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        timing.map_wait_cpu_ms = Milliseconds(Clock::now() - mapStart);
        if (FAILED(mapResult))
        {
            ThrowIfDeviceLost("ID3D11DeviceContext::Map(readback)");
            CheckHr(mapResult, "ID3D11DeviceContext::Map(readback)");
        }

        const auto rowCopyStart = Clock::now();
        const std::size_t rowBytes = outputWidth_ * 4u;
        for (std::uint32_t row = 0; row < outputHeight_; ++row)
        {
            std::memcpy(
                static_cast<std::uint8_t*>(output) +
                    static_cast<std::size_t>(row) * outputStride,
                static_cast<const std::uint8_t*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch,
                rowBytes);
        }
        context_->Unmap(stagingTexture_.Get(), 0);
        timing.row_copy_cpu_ms =
            Milliseconds(Clock::now() - rowCopyStart);
        timing.readback_cpu_ms =
            timing.copy_submit_cpu_ms + timing.map_wait_cpu_ms +
            timing.row_copy_cpu_ms;
        const auto queryStart = Clock::now();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 timestampStart = 0;
        UINT64 timestampUpload = 0;
        UINT64 timestampEvaluate = 0;
        UINT64 timestampCopy = 0;
        WaitForQuery(
            context_.Get(), disjointQuery_.Get(), disjoint,
            "TIMESTAMP_DISJOINT query");
        WaitForQuery(
            context_.Get(), timestampStartQuery_.Get(), timestampStart,
            "start timestamp query");
        WaitForQuery(
            context_.Get(), timestampUploadQuery_.Get(), timestampUpload,
            "upload timestamp query");
        WaitForQuery(
            context_.Get(), timestampEvaluateQuery_.Get(), timestampEvaluate,
            "evaluate timestamp query");
        WaitForQuery(
            context_.Get(), timestampCopyQuery_.Get(), timestampCopy,
            "copy timestamp query");
        timing.query_resolve_cpu_ms = Milliseconds(Clock::now() - queryStart);

        if (!disjoint.Disjoint && disjoint.Frequency != 0)
        {
            const double toMilliseconds =
                1000.0 / static_cast<double>(disjoint.Frequency);
            timing.upload_gpu_ms =
                static_cast<double>(timestampUpload - timestampStart) *
                toMilliseconds;
            timing.evaluate_gpu_ms =
                static_cast<double>(timestampEvaluate - timestampUpload) *
                toMilliseconds;
            timing.readback_copy_gpu_ms =
                static_cast<double>(timestampCopy - timestampEvaluate) *
                toMilliseconds;
            timing.total_gpu_ms =
                static_cast<double>(timestampCopy - timestampStart) *
                toMilliseconds;
            timing.gpu_timing_valid = 1u;
        }
        timing.process_cpu_ms = Milliseconds(Clock::now() - processStart);
        ThrowIfDeviceLost("rvb_process_rgba8");
        return timing;
    }

    RvbFrameTiming ProcessGpu(
        const void* input,
        std::uint32_t inputStride,
        std::uint32_t quality)
    {
        if (!externalDevice_)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_process_rgba8_gpu requires an external-device handle");
        }
        if (directFramePending_)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "A GPU-direct VSR frame was processed without being rendered");
        }
        if (input == nullptr || inputStride < RVB_INPUT_WIDTH * 4u)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "GPU-direct input must be a 1280x720 RGBA8 buffer with a valid stride");
        }
        ValidateQuality(quality);

        const auto processStart = Clock::now();
        RvbFrameTiming timing{};
        timing.struct_size = static_cast<std::uint32_t>(sizeof(RvbFrameTiming));
        timing.direct_mode = 1u;
        timing.cpu_blocking_wait_ms = 0.0;

        const auto queryStart = Clock::now();
        ResolveDirectQueries(timing, true);
        timing.query_resolve_cpu_ms = Milliseconds(Clock::now() - queryStart);

        activeDirectQuery_ = AcquireDirectQuery();
        if (activeDirectQuery_ >= 0)
        {
            DirectQuerySet& query = directQueries_[activeDirectQuery_];
            context_->Begin(query.disjoint.Get());
            context_->End(query.start.Get());
        }

        const auto uploadStart = Clock::now();
        context_->UpdateSubresource(
            inputTexture_.Get(), 0, nullptr, input, inputStride, 0);
        timing.upload_submit_cpu_ms = Milliseconds(Clock::now() - uploadStart);
        if (activeDirectQuery_ >= 0)
        {
            context_->End(directQueries_[activeDirectQuery_].upload.Get());
        }

        NVSDK_NGX_D3D11_VSR_Eval_Params evaluateParameters{};
        evaluateParameters.pInput = inputTexture_.Get();
        evaluateParameters.pOutput = outputTexture_.Get();
        evaluateParameters.InputSubrectBase = {0, 0};
        evaluateParameters.InputSubrectSize = {RVB_INPUT_WIDTH, RVB_INPUT_HEIGHT};
        evaluateParameters.OutputSubrectBase = {0, 0};
        evaluateParameters.OutputSubrectSize = {outputWidth_, outputHeight_};
        evaluateParameters.QualityLevel =
            static_cast<NVSDK_NGX_VSR_QualityLevel>(quality);

        NVSDK_NGX_Result evaluateResult{};
        const auto evaluateStart = Clock::now();
        {
            MultithreadGuard multithreadGuard(multithread_.Get());
            ContextStateGuard stateGuard(context1_.Get(), bridgeContextState_.Get());
            evaluateResult = NGX_D3D11_EVALUATE_VSR_EXT(
                context_.Get(), vsrFeature_, ngxParameters_, &evaluateParameters);
        }
        timing.evaluate_call_cpu_ms = Milliseconds(Clock::now() - evaluateStart);
        if (activeDirectQuery_ >= 0)
        {
            context_->End(directQueries_[activeDirectQuery_].evaluate.Get());
        }
        if (NVSDK_NGX_FAILED(evaluateResult))
        {
            CancelActiveDirectQuery();
            ThrowIfDeviceLost("NGX_D3D11_EVALUATE_VSR_EXT(gpu-direct)");
            throw BridgeError(
                RVB_STATUS_PROCESSING_FAILED,
                "NGX_D3D11_EVALUATE_VSR_EXT(gpu-direct) failed: " +
                    NgxResultText(evaluateResult));
        }

        timing.process_cpu_ms = Milliseconds(Clock::now() - processStart);
        ApplyLastDirectGpuTiming(timing);
        directPendingTiming_ = timing;
        directFramePending_ = true;
        ThrowIfDeviceLost("rvb_process_rgba8_gpu");
        return timing;
    }

    RvbFrameTiming RenderDirect(const RvbDirectRenderOptions& options)
    {
        if (!externalDevice_)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_render_output_d3d11 requires an external-device handle");
        }
        if (options.target_width == 0 || options.target_height == 0 ||
            options.destination_width <= 0.0f ||
            options.destination_height <= 0.0f ||
            options.rotation_quarter_turns > 3u ||
            options.post_aa_mode > RVB_POST_AA_SMAA_1X)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "GPU-direct render dimensions or rotation are invalid");
        }

        ComPtr<ID3D11RenderTargetView> renderTarget;
        context_->OMGetRenderTargets(1, &renderTarget, nullptr);
        if (renderTarget == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "SDL D3D11 renderer has no bound render target after SDL_RenderFlush");
        }

        ComPtr<ID3D11Resource> targetResource;
        renderTarget->GetResource(&targetResource);
        ComPtr<ID3D11Texture2D> targetTexture;
        CheckHr(
            targetResource.As(&targetTexture),
            "SDL render target QueryInterface(ID3D11Texture2D)");
        D3D11_TEXTURE2D_DESC targetDescription{};
        targetTexture->GetDesc(&targetDescription);
        if (targetDescription.Width != options.target_width ||
            targetDescription.Height != options.target_height)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "SDL render-target size changed during GPU-direct presentation");
        }

        directPendingTiming_.post_aa_requested = options.post_aa_mode;
        directPendingTiming_.post_aa_applied = RVB_POST_AA_OFF;
        directPendingTiming_.post_aa_failed = 0u;
        rvb::PostAaOutput aaOutput{
            outputTexture_.Get(), outputShaderResource_.Get()};
        const auto drawStart = Clock::now();
        {
            MultithreadGuard multithreadGuard(multithread_.Get());
            ContextStateGuard stateGuard(context1_.Get(), bridgeContextState_.Get());
            const auto requestedMode =
                static_cast<rvb::PostAaMode>(options.post_aa_mode);
            rvb::PostAaMode appliedMode = requestedMode;
            const std::uint32_t modeBit = 1u << options.post_aa_mode;
            if ((disabledPostAaModes_ & modeBit) != 0u)
            {
                appliedMode = rvb::PostAaMode::Off;
                directPendingTiming_.post_aa_failed = 1u;
            }

            const rvb::PostAaQueries queries = ActivePostAaQueries();
            try
            {
                if (appliedMode == rvb::PostAaMode::Off)
                {
                    if (queries.edge != nullptr) context_->End(queries.edge);
                    if (queries.blend != nullptr) context_->End(queries.blend);
                    if (queries.neighborhood != nullptr)
                        context_->End(queries.neighborhood);
                }
                else
                {
                    if (postProcessAa_ == nullptr)
                    {
                        postProcessAa_ = std::make_unique<rvb::PostProcessAA>(
                            device_.Get(), context_.Get());
                    }
                    aaOutput = postProcessAa_->Apply(
                        appliedMode,
                        outputTexture_.Get(),
                        outputShaderResource_.Get(),
                        outputWidth_, outputHeight_,
                        {
                            options.fxaa_subpixel,
                            options.fxaa_edge_threshold,
                            options.fxaa_edge_threshold_min,
                        },
                        queries);
                }
                directPendingTiming_.post_aa_applied =
                    static_cast<std::uint32_t>(appliedMode);
            }
            catch (const std::exception& error)
            {
                ThrowIfDeviceLost("post-process AA");
                disabledPostAaModes_ |= modeBit;
                directPendingTiming_.post_aa_failed = 1u;
                directPendingTiming_.post_aa_applied = RVB_POST_AA_OFF;
                postAaLastError_ = error.what();
                const std::string message =
                    "[RTX VSR] Post-AA disabled; continuing GPU-direct VSR without AA: " +
                    postAaLastError_ + "\n";
                std::fputs(message.c_str(), stderr);
                OutputDebugStringA(message.c_str());
                if (queries.edge != nullptr) context_->End(queries.edge);
                if (queries.blend != nullptr) context_->End(queries.blend);
                if (queries.neighborhood != nullptr)
                    context_->End(queries.neighborhood);
            }

            DrawOutput(
                renderTarget.Get(), aaOutput.shaderResource, options);
        }
        screenshotSource_ = aaOutput.texture;
        directPendingTiming_.direct_draw_submit_cpu_ms =
            Milliseconds(Clock::now() - drawStart);

        if (activeDirectQuery_ >= 0)
        {
            DirectQuerySet& query = directQueries_[activeDirectQuery_];
            context_->End(query.draw.Get());
            context_->End(query.disjoint.Get());
            query.pending = true;
            query.sequence = ++directSequence_;
            query.postAaRequested = directPendingTiming_.post_aa_requested;
            query.postAaApplied = directPendingTiming_.post_aa_applied;
            query.postAaFailed = directPendingTiming_.post_aa_failed;
            activeDirectQuery_ = -1;
        }

        const auto queryStart = Clock::now();
        ResolveDirectQueries(directPendingTiming_, false);
        directPendingTiming_.query_resolve_cpu_ms +=
            Milliseconds(Clock::now() - queryStart);
        ApplyLastDirectGpuTiming(directPendingTiming_);
        directFramePending_ = false;
        ThrowIfDeviceLost("rvb_render_output_d3d11");
        return directPendingTiming_;
    }

    void ReadbackOutput(void* output, std::uint32_t outputStride)
    {
        if (output == nullptr || outputStride < outputWidth_ * 4u)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "Screenshot output buffer or stride is invalid");
        }
        EnsureScreenshotStagingTexture();

        MultithreadGuard guard(multithread_.Get());
        ContextStateGuard stateGuard(
            externalDevice_ ? context1_.Get() : nullptr,
            externalDevice_ ? bridgeContextState_.Get() : nullptr);
        ID3D11Texture2D* screenshotSource =
            screenshotSource_ != nullptr
                ? screenshotSource_.Get()
                : outputTexture_.Get();
        context_->CopyResource(stagingTexture_.Get(), screenshotSource);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(
            context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "ID3D11DeviceContext::Map(gpu-direct screenshot)");
        const std::size_t rowBytes = outputWidth_ * 4u;
        for (std::uint32_t row = 0; row < outputHeight_; ++row)
        {
            std::memcpy(
                static_cast<std::uint8_t*>(output) +
                    static_cast<std::size_t>(row) * outputStride,
                static_cast<const std::uint8_t*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch,
                rowBytes);
        }
        context_->Unmap(stagingTexture_.Get(), 0);
    }

    void Reconfigure(
        std::uint32_t outputWidth,
        std::uint32_t outputHeight)
    {
        ValidateOutputSize(outputWidth, outputHeight);
        if (outputWidth == outputWidth_ && outputHeight == outputHeight_)
        {
            return;
        }

        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        CreateOutputTextures();
        if (postProcessAa_ != nullptr)
            postProcessAa_->ResetSize();
    }

    const AdapterInfo& Adapter() const { return selectedAdapter_; }
    int NeedsUpdatedDriver() const { return needsUpdatedDriver_; }
    unsigned int MinimumDriverMajor() const { return minimumDriverMajor_; }
    unsigned int MinimumDriverMinor() const { return minimumDriverMinor_; }
    std::uint32_t OutputWidth() const { return outputWidth_; }
    std::uint32_t OutputHeight() const { return outputHeight_; }
    bool IsExternalDevice() const { return externalDevice_; }

    static void ValidateQuality(std::uint32_t quality)
    {
        if (quality > static_cast<std::uint32_t>(NVSDK_NGX_VSR_Quality_Ultra))
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "VSR quality must be in the SDK-verified range 0..4");
        }
    }

private:
    void CreateDeviceAndTextures()
    {
        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL actualLevel{};
        CheckHr(
            D3D11CreateDevice(
                selectedAdapter_.adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                0,
                requestedLevels,
                static_cast<UINT>(std::size(requestedLevels)),
                D3D11_SDK_VERSION,
                &device_,
                &actualLevel,
                &context_),
            "D3D11CreateDevice(NVIDIA adapter)");
        if (actualLevel < D3D_FEATURE_LEVEL_11_0)
        {
            throw BridgeError(
                RVB_STATUS_D3D_FAILED,
                "The selected NVIDIA adapter does not support D3D feature level 11.0");
        }

        ConfigureDeviceResources();
    }

    void ConfigureDeviceResources()
    {
        if (SUCCEEDED(context_.As(&multithread_)))
        {
            multithread_->SetMultithreadProtected(TRUE);
        }

        D3D11_TEXTURE2D_DESC inputDescription{};
        inputDescription.Width = RVB_INPUT_WIDTH;
        inputDescription.Height = RVB_INPUT_HEIGHT;
        inputDescription.MipLevels = 1;
        inputDescription.ArraySize = 1;
        inputDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        inputDescription.SampleDesc.Count = 1;
        inputDescription.Usage = D3D11_USAGE_DEFAULT;
        inputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CheckHr(
            device_->CreateTexture2D(
                &inputDescription, nullptr, &inputTexture_),
            "ID3D11Device::CreateTexture2D(input RGBA8)");

        CreateOutputTextures();
        if (externalDevice_)
        {
            CreateDirectResources();
        }
        else
        {
            CreateTimingQueries();
        }
    }

    void CreateOutputTextures()
    {
        stagingTexture_.Reset();
        outputShaderResource_.Reset();
        outputTexture_.Reset();

        D3D11_TEXTURE2D_DESC outputDescription{};
        outputDescription.Width = outputWidth_;
        outputDescription.Height = outputHeight_;
        outputDescription.MipLevels = 1;
        outputDescription.ArraySize = 1;
        outputDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        outputDescription.SampleDesc.Count = 1;
        outputDescription.Usage = D3D11_USAGE_DEFAULT;
        outputDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET |
            D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_UNORDERED_ACCESS;
        CheckHr(
            device_->CreateTexture2D(
                &outputDescription, nullptr, &outputTexture_),
            "ID3D11Device::CreateTexture2D(output RGBA8 UAV)");

        if (!externalDevice_)
        {
            D3D11_TEXTURE2D_DESC stagingDescription = outputDescription;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            CheckHr(
                device_->CreateTexture2D(
                    &stagingDescription, nullptr, &stagingTexture_),
                "ID3D11Device::CreateTexture2D(readback staging)");
        }

        if (externalDevice_)
        {
            CheckHr(
                device_->CreateShaderResourceView(
                    outputTexture_.Get(), nullptr, &outputShaderResource_),
                "ID3D11Device::CreateShaderResourceView(VSR output)");
            screenshotSource_ = outputTexture_;
        }
    }

    void CreateTimingQueries()
    {
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        CheckHr(
            device_->CreateQuery(&description, &disjointQuery_),
            "ID3D11Device::CreateQuery(TIMESTAMP_DISJOINT)");

        description.Query = D3D11_QUERY_TIMESTAMP;
        CheckHr(
            device_->CreateQuery(&description, &timestampStartQuery_),
            "ID3D11Device::CreateQuery(timestamp start)");
        CheckHr(
            device_->CreateQuery(&description, &timestampUploadQuery_),
            "ID3D11Device::CreateQuery(timestamp upload)");
        CheckHr(
            device_->CreateQuery(&description, &timestampEvaluateQuery_),
            "ID3D11Device::CreateQuery(timestamp evaluate)");
        CheckHr(
            device_->CreateQuery(&description, &timestampCopyQuery_),
            "ID3D11Device::CreateQuery(timestamp copy)");
    }

    struct DirectVertex
    {
        float x;
        float y;
        float u;
        float v;
    };

    struct DirectQuerySet
    {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> start;
        ComPtr<ID3D11Query> upload;
        ComPtr<ID3D11Query> evaluate;
        ComPtr<ID3D11Query> aaEdge;
        ComPtr<ID3D11Query> aaBlend;
        ComPtr<ID3D11Query> aaNeighborhood;
        ComPtr<ID3D11Query> draw;
        bool pending = false;
        std::uint64_t sequence = 0;
        std::uint32_t postAaRequested = RVB_POST_AA_OFF;
        std::uint32_t postAaApplied = RVB_POST_AA_OFF;
        std::uint32_t postAaFailed = 0;
    };

    void CreateDirectResources()
    {
        CheckHr(
            device_.As(&device1_),
            "ID3D11Device::QueryInterface(ID3D11Device1)");
        CheckHr(
            context_.As(&context1_),
            "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1)");

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        CheckHr(
            device1_->CreateDeviceContextState(
                0,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                __uuidof(ID3D11Device),
                &selectedFeatureLevel,
                &bridgeContextState_),
            "ID3D11Device1::CreateDeviceContextState");

        static constexpr char shaderSource[] = R"(
Texture2D SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);

struct VertexInput
{
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

PixelInput VertexMain(VertexInput input)
{
    PixelInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.texcoord = input.texcoord;
    return output;
}

float4 PixelMain(PixelInput input) : SV_TARGET
{
    return SourceTexture.Sample(SourceSampler, input.texcoord);
}
)";

        const ComPtr<ID3DBlob> vertexBytecode =
            CompileShader(shaderSource, "VertexMain", "vs_5_0");
        const ComPtr<ID3DBlob> pixelBytecode =
            CompileShader(shaderSource, "PixelMain", "ps_5_0");
        CheckHr(
            device_->CreateVertexShader(
                vertexBytecode->GetBufferPointer(),
                vertexBytecode->GetBufferSize(),
                nullptr,
                &directVertexShader_),
            "ID3D11Device::CreateVertexShader(gpu-direct)");
        CheckHr(
            device_->CreatePixelShader(
                pixelBytecode->GetBufferPointer(),
                pixelBytecode->GetBufferSize(),
                nullptr,
                &directPixelShader_),
            "ID3D11Device::CreatePixelShader(gpu-direct)");

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        CheckHr(
            device_->CreateInputLayout(
                elements,
                static_cast<UINT>(std::size(elements)),
                vertexBytecode->GetBufferPointer(),
                vertexBytecode->GetBufferSize(),
                &directInputLayout_),
            "ID3D11Device::CreateInputLayout(gpu-direct)");

        D3D11_BUFFER_DESC vertexBufferDescription{};
        vertexBufferDescription.ByteWidth =
            static_cast<UINT>(sizeof(DirectVertex) * 6u);
        vertexBufferDescription.Usage = D3D11_USAGE_DYNAMIC;
        vertexBufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexBufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CheckHr(
            device_->CreateBuffer(
                &vertexBufferDescription, nullptr, &directVertexBuffer_),
            "ID3D11Device::CreateBuffer(gpu-direct vertices)");

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDescription.MinLOD = 0.0f;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        CheckHr(
            device_->CreateSamplerState(
                &samplerDescription, &directSampler_),
            "ID3D11Device::CreateSamplerState(gpu-direct)");

        for (DirectQuerySet& query : directQueries_)
        {
            D3D11_QUERY_DESC queryDescription{};
            queryDescription.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.disjoint),
                "ID3D11Device::CreateQuery(gpu-direct disjoint)");
            queryDescription.Query = D3D11_QUERY_TIMESTAMP;
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.start),
                "ID3D11Device::CreateQuery(gpu-direct start)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.upload),
                "ID3D11Device::CreateQuery(gpu-direct upload)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.evaluate),
                "ID3D11Device::CreateQuery(gpu-direct evaluate)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.aaEdge),
                "ID3D11Device::CreateQuery(gpu-direct AA edge)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.aaBlend),
                "ID3D11Device::CreateQuery(gpu-direct AA blend)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.aaNeighborhood),
                "ID3D11Device::CreateQuery(gpu-direct AA neighborhood)");
            CheckHr(
                device_->CreateQuery(&queryDescription, &query.draw),
                "ID3D11Device::CreateQuery(gpu-direct draw)");
        }
    }

    void DrawOutput(
        ID3D11RenderTargetView* renderTarget,
        ID3D11ShaderResourceView* source,
        const RvbDirectRenderOptions& options)
    {
        const float left = options.destination_x;
        const float top = options.destination_y;
        const float right = left + options.destination_width;
        const float bottom = top + options.destination_height;
        const float centerX = (left + right) * 0.5f;
        const float centerY = (top + bottom) * 0.5f;

        auto rotate = [&](float x, float y) {
            float dx = x - centerX;
            float dy = y - centerY;
            for (std::uint32_t turn = 0;
                 turn < options.rotation_quarter_turns;
                 ++turn)
            {
                const float rotatedX = -dy;
                dy = dx;
                dx = rotatedX;
            }
            return std::pair<float, float>{centerX + dx, centerY + dy};
        };
        auto vertex = [&](float x, float y, float u, float v) {
            const auto [rotatedX, rotatedY] = rotate(x, y);
            return DirectVertex{
                (2.0f * rotatedX /
                     static_cast<float>(options.target_width)) - 1.0f,
                1.0f - (2.0f * rotatedY /
                     static_cast<float>(options.target_height)),
                u,
                v};
        };

        const DirectVertex vertices[] = {
            vertex(left, top, 0.0f, 0.0f),
            vertex(right, top, 1.0f, 0.0f),
            vertex(left, bottom, 0.0f, 1.0f),
            vertex(left, bottom, 0.0f, 1.0f),
            vertex(right, top, 1.0f, 0.0f),
            vertex(right, bottom, 1.0f, 1.0f),
        };

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(
            context_->Map(
                directVertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                0, &mapped),
            "ID3D11DeviceContext::Map(gpu-direct vertices)");
        std::memcpy(mapped.pData, vertices, sizeof(vertices));
        context_->Unmap(directVertexBuffer_.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(options.target_width);
        viewport.Height = static_cast<float>(options.target_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11UnorderedAccessView* nullUnorderedAccessViews[
            D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        context_->CSSetUnorderedAccessViews(
            0,
            D3D11_PS_CS_UAV_REGISTER_COUNT,
            nullUnorderedAccessViews,
            nullptr);
        context_->OMSetRenderTargets(1, &renderTarget, nullptr);

        const UINT stride = sizeof(DirectVertex);
        const UINT offset = 0;
        ID3D11Buffer* vertexBuffer = directVertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context_->IASetInputLayout(directInputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(directVertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(directPixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* shaderResource = source;
        context_->PSSetShaderResources(0, 1, &shaderResource);
        ID3D11SamplerState* sampler = directSampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        context_->Draw(6, 0);

        ID3D11ShaderResourceView* nullResource = nullptr;
        context_->PSSetShaderResources(0, 1, &nullResource);
    }

    int AcquireDirectQuery()
    {
        for (std::size_t offset = 0; offset < directQueries_.size(); ++offset)
        {
            const std::size_t index =
                (directQueryCursor_ + offset) % directQueries_.size();
            if (!directQueries_[index].pending)
            {
                directQueryCursor_ = (index + 1u) % directQueries_.size();
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void CancelActiveDirectQuery() noexcept
    {
        if (activeDirectQuery_ < 0)
        {
            return;
        }
        DirectQuerySet& query = directQueries_[activeDirectQuery_];
        context_->End(query.aaEdge.Get());
        context_->End(query.aaBlend.Get());
        context_->End(query.aaNeighborhood.Get());
        context_->End(query.draw.Get());
        context_->End(query.disjoint.Get());
        query.pending = false;
        activeDirectQuery_ = -1;
    }

    rvb::PostAaQueries ActivePostAaQueries() const noexcept
    {
        if (activeDirectQuery_ < 0)
            return {};
        const DirectQuerySet& query = directQueries_[activeDirectQuery_];
        return {
            query.aaEdge.Get(),
            query.aaBlend.Get(),
            query.aaNeighborhood.Get(),
        };
    }

    void ResolveDirectQueries(
        RvbFrameTiming& timing,
        bool startingNewFrame)
    {
        for (DirectQuerySet& query : directQueries_)
        {
            if (!query.pending)
            {
                continue;
            }

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const HRESULT disjointResult = context_->GetData(
                query.disjoint.Get(), &disjoint, sizeof(disjoint),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (disjointResult == S_FALSE)
            {
                continue;
            }
            CheckHr(disjointResult, "gpu-direct disjoint query");

            UINT64 timestampStart = 0;
            UINT64 timestampUpload = 0;
            UINT64 timestampEvaluate = 0;
            UINT64 timestampAaEdge = 0;
            UINT64 timestampAaBlend = 0;
            UINT64 timestampAaNeighborhood = 0;
            UINT64 timestampDraw = 0;
            const HRESULT timestampResults[] = {
                context_->GetData(
                    query.start.Get(), &timestampStart, sizeof(timestampStart),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.upload.Get(), &timestampUpload, sizeof(timestampUpload),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.evaluate.Get(), &timestampEvaluate,
                    sizeof(timestampEvaluate), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.aaEdge.Get(), &timestampAaEdge,
                    sizeof(timestampAaEdge), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.aaBlend.Get(), &timestampAaBlend,
                    sizeof(timestampAaBlend), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.aaNeighborhood.Get(), &timestampAaNeighborhood,
                    sizeof(timestampAaNeighborhood), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                context_->GetData(
                    query.draw.Get(), &timestampDraw, sizeof(timestampDraw),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH),
            };
            bool ready = true;
            for (const HRESULT result : timestampResults)
            {
                if (result == S_FALSE)
                {
                    ready = false;
                    break;
                }
                CheckHr(result, "gpu-direct timestamp query");
            }
            if (!ready)
            {
                continue;
            }

            if (!disjoint.Disjoint && disjoint.Frequency != 0)
            {
                const double scale =
                    1000.0 / static_cast<double>(disjoint.Frequency);
                lastDirectGpuTiming_ = {};
                lastDirectGpuTiming_.struct_size = sizeof(RvbFrameTiming);
                lastDirectGpuTiming_.gpu_timing_valid = 1u;
                lastDirectGpuTiming_.direct_mode = 1u;
                lastDirectGpuTiming_.upload_gpu_ms =
                    static_cast<double>(timestampUpload - timestampStart) * scale;
                lastDirectGpuTiming_.evaluate_gpu_ms =
                    static_cast<double>(timestampEvaluate - timestampUpload) * scale;
                lastDirectGpuTiming_.post_aa_requested = query.postAaRequested;
                lastDirectGpuTiming_.post_aa_applied = query.postAaApplied;
                lastDirectGpuTiming_.post_aa_failed = query.postAaFailed;
                if (query.postAaApplied == RVB_POST_AA_FXAA)
                {
                    lastDirectGpuTiming_.fxaa_gpu_ms =
                        static_cast<double>(timestampAaEdge - timestampEvaluate) * scale;
                }
                else if (query.postAaApplied == RVB_POST_AA_SMAA_1X)
                {
                    lastDirectGpuTiming_.smaa_edge_gpu_ms =
                        static_cast<double>(timestampAaEdge - timestampEvaluate) * scale;
                    lastDirectGpuTiming_.smaa_blend_gpu_ms =
                        static_cast<double>(timestampAaBlend - timestampAaEdge) * scale;
                    lastDirectGpuTiming_.smaa_neighborhood_gpu_ms =
                        static_cast<double>(timestampAaNeighborhood - timestampAaBlend) * scale;
                }
                lastDirectGpuTiming_.post_aa_gpu_ms =
                    static_cast<double>(timestampAaNeighborhood - timestampEvaluate) * scale;
                lastDirectGpuTiming_.direct_draw_gpu_ms =
                    static_cast<double>(timestampDraw - timestampAaNeighborhood) * scale;
                lastDirectGpuTiming_.total_gpu_ms =
                    static_cast<double>(timestampDraw - timestampStart) * scale;
                const std::uint64_t observationSequence =
                    directSequence_ + (startingNewFrame ? 1u : 0u);
                lastDirectGpuTiming_.gpu_timing_latency_frames =
                    observationSequence >= query.sequence
                        ? static_cast<std::uint32_t>(
                              observationSequence - query.sequence)
                        : 0u;
                lastDirectGpuTimingValid_ = true;
            }
            query.pending = false;
        }
        ApplyLastDirectGpuTiming(timing);
    }

    void ApplyLastDirectGpuTiming(RvbFrameTiming& timing) const
    {
        if (!lastDirectGpuTimingValid_)
        {
            return;
        }
        timing.gpu_timing_valid = 1u;
        timing.upload_gpu_ms = lastDirectGpuTiming_.upload_gpu_ms;
        timing.evaluate_gpu_ms = lastDirectGpuTiming_.evaluate_gpu_ms;
        timing.direct_draw_gpu_ms = lastDirectGpuTiming_.direct_draw_gpu_ms;
        timing.fxaa_gpu_ms = lastDirectGpuTiming_.fxaa_gpu_ms;
        timing.smaa_edge_gpu_ms = lastDirectGpuTiming_.smaa_edge_gpu_ms;
        timing.smaa_blend_gpu_ms = lastDirectGpuTiming_.smaa_blend_gpu_ms;
        timing.smaa_neighborhood_gpu_ms =
            lastDirectGpuTiming_.smaa_neighborhood_gpu_ms;
        timing.post_aa_gpu_ms = lastDirectGpuTiming_.post_aa_gpu_ms;
        timing.post_aa_requested = lastDirectGpuTiming_.post_aa_requested;
        timing.post_aa_applied = lastDirectGpuTiming_.post_aa_applied;
        timing.post_aa_failed = lastDirectGpuTiming_.post_aa_failed;
        timing.total_gpu_ms = lastDirectGpuTiming_.total_gpu_ms;
        timing.gpu_timing_latency_frames =
            lastDirectGpuTiming_.gpu_timing_latency_frames;
    }

    void EnsureScreenshotStagingTexture()
    {
        if (stagingTexture_ != nullptr)
        {
            return;
        }
        D3D11_TEXTURE2D_DESC description{};
        outputTexture_->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        CheckHr(
            device_->CreateTexture2D(
                &description, nullptr, &stagingTexture_),
            "ID3D11Device::CreateTexture2D(gpu-direct screenshot staging)");
    }

    void InitializeNgx(
        const std::filesystem::path& featureDirectory,
        const std::filesystem::path& applicationDataDirectory)
    {
        const std::filesystem::path absoluteFeatureDirectory =
            std::filesystem::absolute(featureDirectory);
        const std::filesystem::path absoluteApplicationDataDirectory =
            std::filesystem::absolute(applicationDataDirectory);
        const std::wstring featurePath = absoluteFeatureDirectory.wstring();
        const wchar_t* featurePaths[] = {featurePath.c_str()};

        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        commonInfo.PathListInfo.Path = featurePaths;
        commonInfo.PathListInfo.Length = 1;

        MultithreadGuard guard(multithread_.Get());
        ContextStateGuard stateGuard(
            externalDevice_ ? context1_.Get() : nullptr,
            externalDevice_ ? bridgeContextState_.Get() : nullptr);

        auto result = NVSDK_NGX_D3D11_Init(
            0,
            absoluteApplicationDataDirectory.c_str(),
            device_.Get(),
            &commonInfo);
        if (NVSDK_NGX_FAILED(result))
        {
            throw BridgeError(
                RVB_STATUS_NGX_INITIALIZATION_FAILED,
                "NVSDK_NGX_D3D11_Init failed: " + NgxResultText(result));
        }
        ngxInitialized_ = true;

        result = NVSDK_NGX_D3D11_GetCapabilityParameters(&ngxParameters_);
        if (NVSDK_NGX_FAILED(result) || ngxParameters_ == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_NGX_INITIALIZATION_FAILED,
                "NVSDK_NGX_D3D11_GetCapabilityParameters failed: " +
                    NgxResultText(result));
        }

        result = ngxParameters_->Get(
            NVSDK_NGX_Parameter_VSR_Available, &vsrAvailable_);
        QueryOptionalInt(
            ngxParameters_, NVSDK_NGX_Parameter_VSR_NeedsUpdatedDriver,
            needsUpdatedDriver_);
        QueryOptionalUnsigned(
            ngxParameters_, NVSDK_NGX_Parameter_VSR_MinDriverVersionMajor,
            minimumDriverMajor_);
        QueryOptionalUnsigned(
            ngxParameters_, NVSDK_NGX_Parameter_VSR_MinDriverVersionMinor,
            minimumDriverMinor_);

        if (NVSDK_NGX_FAILED(result) || vsrAvailable_ == 0)
        {
            std::ostringstream message;
            message << "RTX Video Super Resolution is unavailable on adapter '"
                    << Utf8(selectedAdapter_.name) << "'";
            if (needsUpdatedDriver_ != 0)
            {
                message << "; NGX requires driver " << minimumDriverMajor_
                        << "." << minimumDriverMinor_ << " or newer";
            }
            throw BridgeError(RVB_STATUS_VSR_UNSUPPORTED, message.str());
        }

        std::size_t scratchBufferBytes = 0;
        result = NVSDK_NGX_D3D11_GetScratchBufferSize(
            NVSDK_NGX_Feature_VSR, ngxParameters_, &scratchBufferBytes);
        if (NVSDK_NGX_FAILED(result))
        {
            throw BridgeError(
                RVB_STATUS_FEATURE_CREATION_FAILED,
                "NVSDK_NGX_D3D11_GetScratchBufferSize(VSR) failed: " +
                    NgxResultText(result));
        }
        if (scratchBufferBytes != 0)
        {
            throw BridgeError(
                RVB_STATUS_FEATURE_CREATION_FAILED,
                "VSR unexpectedly requested a scratch buffer of " +
                    std::to_string(scratchBufferBytes) + " bytes");
        }

        NVSDK_NGX_Feature_Create_Params createParameters{};
        result = NGX_D3D11_CREATE_VSR_EXT(
            context_.Get(), &vsrFeature_, ngxParameters_, &createParameters);
        if (NVSDK_NGX_FAILED(result) || vsrFeature_ == nullptr)
        {
            int featureInitializationResult = 0;
            QueryOptionalInt(
                ngxParameters_, NVSDK_NGX_Parameter_VSR_FeatureInitResult,
                featureInitializationResult);
            throw BridgeError(
                RVB_STATUS_FEATURE_CREATION_FAILED,
                "NGX_D3D11_CREATE_VSR_EXT failed: " + NgxResultText(result) +
                    "; VSR.FeatureInitResult=" +
                    Hex32(static_cast<std::uint32_t>(
                        featureInitializationResult)));
        }

        const auto loadedFeatureModule =
            ModulePath(GetModuleHandleW(L"nvngx_vsr.dll"));
        if (loadedFeatureModule.empty())
        {
            throw BridgeError(
                RVB_STATUS_FEATURE_CREATION_FAILED,
                "VSR was created, but the loaded nvngx_vsr.dll path could not be verified");
        }

        const auto expectedFeatureModule =
            absoluteFeatureDirectory / L"nvngx_vsr.dll";
        std::error_code equivalentError;
        const bool sameModule = std::filesystem::equivalent(
            expectedFeatureModule, loadedFeatureModule, equivalentError);
        if (equivalentError || !sameModule)
        {
            throw BridgeError(
                RVB_STATUS_FEATURE_CREATION_FAILED,
                "NGX loaded nvngx_vsr.dll from an unexpected path: " +
                    Utf8(loadedFeatureModule.wstring()) +
                    "; expected " + Utf8(expectedFeatureModule.wstring()));
        }
    }

    void ThrowIfDeviceLost(const char* operation)
    {
        if (device_ == nullptr)
        {
            return;
        }

        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (FAILED(reason))
        {
            throw BridgeError(
                RVB_STATUS_DEVICE_LOST,
                std::string(operation) +
                    ": D3D11 device was removed/reset; reason " +
                    Hex32(static_cast<std::uint32_t>(reason)));
        }
    }

    void Shutdown() noexcept
    {
        ComPtr<ID3DDeviceContextState> previousContextState;
        if (externalDevice_ && context1_ != nullptr &&
            bridgeContextState_ != nullptr)
        {
            context1_->SwapDeviceContextState(
                bridgeContextState_.Get(), &previousContextState);
        }
        if (vsrFeature_ != nullptr)
        {
            NVSDK_NGX_D3D11_ReleaseFeature(vsrFeature_);
            vsrFeature_ = nullptr;
        }
        if (ngxInitialized_ && device_ != nullptr)
        {
            NVSDK_NGX_D3D11_Shutdown1(device_.Get());
        }
        if (ngxParameters_ != nullptr)
        {
            NVSDK_NGX_D3D11_DestroyParameters(ngxParameters_);
            ngxParameters_ = nullptr;
        }
        ngxInitialized_ = false;

        if (previousContextState != nullptr)
        {
            context1_->SwapDeviceContextState(
                previousContextState.Get(), nullptr);
        }

        timestampCopyQuery_.Reset();
        timestampEvaluateQuery_.Reset();
        timestampUploadQuery_.Reset();
        timestampStartQuery_.Reset();
        disjointQuery_.Reset();
        for (DirectQuerySet& query : directQueries_)
        {
            query = {};
        }
        activeDirectQuery_ = -1;
        directQueryCursor_ = 0;
        directSequence_ = 0;
        directFramePending_ = false;
        directPendingTiming_ = {};
        lastDirectGpuTiming_ = {};
        lastDirectGpuTimingValid_ = false;
        screenshotSource_.Reset();
        postProcessAa_.reset();
        disabledPostAaModes_ = 0;
        postAaLastError_.clear();
        directSampler_.Reset();
        directVertexBuffer_.Reset();
        directInputLayout_.Reset();
        directPixelShader_.Reset();
        directVertexShader_.Reset();
        bridgeContextState_.Reset();
        context1_.Reset();
        device1_.Reset();
        stagingTexture_.Reset();
        outputShaderResource_.Reset();
        outputTexture_.Reset();
        inputTexture_.Reset();
        multithread_.Reset();
        context_.Reset();
        device_.Reset();
        selectedAdapter_ = {};
        vsrAvailable_ = 0;
        needsUpdatedDriver_ = 0;
        minimumDriverMajor_ = 0;
        minimumDriverMinor_ = 0;
        externalDevice_ = false;
        outputWidth_ = RVB_DEFAULT_OUTPUT_WIDTH;
        outputHeight_ = RVB_DEFAULT_OUTPUT_HEIGHT;
    }

    AdapterInfo selectedAdapter_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device1> device1_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3DDeviceContextState> bridgeContextState_;
    ComPtr<ID3D10Multithread> multithread_;
    ComPtr<ID3D11Texture2D> inputTexture_;
    ComPtr<ID3D11Texture2D> outputTexture_;
    ComPtr<ID3D11ShaderResourceView> outputShaderResource_;
    ComPtr<ID3D11Texture2D> screenshotSource_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    ComPtr<ID3D11VertexShader> directVertexShader_;
    ComPtr<ID3D11PixelShader> directPixelShader_;
    ComPtr<ID3D11InputLayout> directInputLayout_;
    ComPtr<ID3D11Buffer> directVertexBuffer_;
    ComPtr<ID3D11SamplerState> directSampler_;
    std::unique_ptr<rvb::PostProcessAA> postProcessAa_;
    std::uint32_t disabledPostAaModes_ = 0;
    std::string postAaLastError_;
    std::array<DirectQuerySet, 4> directQueries_{};
    int activeDirectQuery_ = -1;
    std::size_t directQueryCursor_ = 0;
    std::uint64_t directSequence_ = 0;
    bool directFramePending_ = false;
    bool lastDirectGpuTimingValid_ = false;
    RvbFrameTiming directPendingTiming_{};
    RvbFrameTiming lastDirectGpuTiming_{};
    ComPtr<ID3D11Query> disjointQuery_;
    ComPtr<ID3D11Query> timestampStartQuery_;
    ComPtr<ID3D11Query> timestampUploadQuery_;
    ComPtr<ID3D11Query> timestampEvaluateQuery_;
    ComPtr<ID3D11Query> timestampCopyQuery_;
    NVSDK_NGX_Parameter* ngxParameters_ = nullptr;
    NVSDK_NGX_Handle* vsrFeature_ = nullptr;
    bool ngxInitialized_ = false;
    bool externalDevice_ = false;
    int vsrAvailable_ = 0;
    int needsUpdatedDriver_ = 0;
    unsigned int minimumDriverMajor_ = 0;
    unsigned int minimumDriverMinor_ = 0;
    std::uint32_t outputWidth_ = RVB_DEFAULT_OUTPUT_WIDTH;
    std::uint32_t outputHeight_ = RVB_DEFAULT_OUTPUT_HEIGHT;
};

RvbCapabilities MakeCapabilities(const Processor& processor)
{
    const AdapterInfo& adapter = processor.Adapter();
    RvbCapabilities capabilities{};
    capabilities.struct_size = sizeof(RvbCapabilities);
    capabilities.api_version = RVB_API_VERSION;
    capabilities.input_width = RVB_INPUT_WIDTH;
    capabilities.input_height = RVB_INPUT_HEIGHT;
    capabilities.output_width = processor.OutputWidth();
    capabilities.output_height = processor.OutputHeight();
    capabilities.quality_mask = 0x1Fu;
    capabilities.adapter_index = adapter.index;
    capabilities.adapter_vendor_id = adapter.vendorId;
    capabilities.adapter_device_id = adapter.deviceId;
    capabilities.adapter_dedicated_video_memory = adapter.dedicatedVideoMemory;
    capabilities.adapter_luid_high = adapter.luid.HighPart;
    capabilities.adapter_luid_low = adapter.luid.LowPart;
    capabilities.needs_updated_driver =
        processor.NeedsUpdatedDriver() != 0 ? 1u : 0u;
    capabilities.minimum_driver_major = processor.MinimumDriverMajor();
    capabilities.minimum_driver_minor = processor.MinimumDriverMinor();
    return capabilities;
}

RvbStatus TranslateCurrentException(std::string& message) noexcept
{
    try
    {
        throw;
    }
    catch (const BridgeError& error)
    {
        message = error.what();
        return error.Status();
    }
    catch (const std::bad_alloc&)
    {
        message = "RtxVideoBridge ran out of memory";
        return RVB_STATUS_INTERNAL_ERROR;
    }
    catch (const std::exception& error)
    {
        message = std::string("RtxVideoBridge internal error: ") + error.what();
        return RVB_STATUS_INTERNAL_ERROR;
    }
    catch (...)
    {
        message = "RtxVideoBridge encountered an unknown internal error";
        return RVB_STATUS_INTERNAL_ERROR;
    }
}

void ValidateProbeAbi(
    const RvbProbeOptions* options,
    const RvbCapabilities* capabilities)
{
    if (options == nullptr || capabilities == nullptr)
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "rvb_probe options and capabilities must not be NULL");
    }
    if (options->struct_size != sizeof(RvbProbeOptions) ||
        capabilities->struct_size != sizeof(RvbCapabilities))
    {
        throw BridgeError(
            RVB_STATUS_ABI_MISMATCH,
            "rvb_probe structure size mismatch; rebuild the managed wrapper and native bridge together");
    }
}

void ValidateCreateOptions(const RvbCreateOptions* options)
{
    if (options == nullptr)
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "rvb_create options must not be NULL");
    }
    if (options->struct_size != sizeof(RvbCreateOptions))
    {
        throw BridgeError(
            RVB_STATUS_ABI_MISMATCH,
            "rvb_create structure size mismatch; rebuild the managed wrapper and native bridge together");
    }
    if (options->input_width != RVB_INPUT_WIDTH ||
        options->input_height != RVB_INPUT_HEIGHT)
    {
        throw BridgeError(
            RVB_STATUS_INVALID_ARGUMENT,
            "RTX VSR input must be 1280x720 RGBA8");
    }
    ValidateOutputSize(options->output_width, options->output_height);
    Processor::ValidateQuality(options->quality);
}
} // namespace

struct RvbHandle
{
    Processor processor;
    std::mutex mutex;
    std::string lastError;
    std::uint32_t quality = RVB_QUALITY_MEDIUM;
    RvbFrameTiming lastTiming{
        static_cast<std::uint32_t>(sizeof(RvbFrameTiming))};
};

extern "C" uint32_t RVB_CALL rvb_get_api_version(void)
{
    return RVB_API_VERSION;
}

extern "C" RvbStatus RVB_CALL rvb_probe(
    const RvbProbeOptions* options,
    RvbCapabilities* capabilities)
{
    try
    {
        ValidateProbeAbi(options, capabilities);
        ValidateDirectories(
            options->feature_directory, options->application_data_directory);

        Processor processor;
        processor.Initialize(
            options->adapter_index,
            options->feature_directory,
            options->application_data_directory,
            RVB_DEFAULT_OUTPUT_WIDTH,
            RVB_DEFAULT_OUTPUT_HEIGHT);
        *capabilities = MakeCapabilities(processor);
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_create(
    const RvbCreateOptions* options,
    RvbHandle** handle)
{
    if (handle == nullptr)
    {
        g_lastError = "rvb_create handle output pointer must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }
    *handle = nullptr;

    try
    {
        ValidateCreateOptions(options);
        ValidateDirectories(
            options->feature_directory, options->application_data_directory);

        auto result = std::make_unique<RvbHandle>();
        result->quality = options->quality;
        result->processor.Initialize(
            options->adapter_index,
            options->feature_directory,
            options->application_data_directory,
            options->output_width,
            options->output_height);
        *handle = result.release();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_create_with_d3d11_device(
    const RvbCreateOptions* options,
    void* externalDevice,
    RvbHandle** handle)
{
    if (handle == nullptr)
    {
        g_lastError =
            "rvb_create_with_d3d11_device handle output pointer must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }
    *handle = nullptr;

    try
    {
        ValidateCreateOptions(options);
        ValidateDirectories(
            options->feature_directory, options->application_data_directory);
        if (externalDevice == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_create_with_d3d11_device external device must not be NULL");
        }

        auto result = std::make_unique<RvbHandle>();
        result->quality = options->quality;
        result->processor.InitializeExternal(
            static_cast<ID3D11Device*>(externalDevice),
            options->feature_directory,
            options->application_data_directory,
            options->output_width,
            options->output_height);
        if (options->adapter_index >= 0 &&
            result->processor.Adapter().index !=
                static_cast<std::uint32_t>(options->adapter_index))
        {
            throw BridgeError(
                RVB_STATUS_WRONG_ADAPTER,
                "The SDL D3D11 device adapter index does not match the requested adapter");
        }
        *handle = result.release();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_get_d3d11_device_info(
    void* device,
    RvbD3D11DeviceInfo* info)
{
    try
    {
        if (device == nullptr || info == nullptr)
        {
            throw BridgeError(
                RVB_STATUS_INVALID_ARGUMENT,
                "rvb_get_d3d11_device_info device and info must not be NULL");
        }
        if (info->struct_size != sizeof(RvbD3D11DeviceInfo))
        {
            throw BridgeError(
                RVB_STATUS_ABI_MISMATCH,
                "rvb_get_d3d11_device_info structure size mismatch");
        }
        *info = MakeDeviceInfo(static_cast<ID3D11Device*>(device));
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_process_rgba8(
    RvbHandle* handle,
    const void* input,
    uint32_t inputStride,
    void* output,
    uint32_t outputStride)
{
    if (handle == nullptr)
    {
        g_lastError = "rvb_process_rgba8 handle must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->mutex);
    try
    {
        handle->lastTiming = handle->processor.Process(
            input, inputStride, output, outputStride, handle->quality);
        handle->lastError.clear();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        handle->lastError = message;
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_process_rgba8_gpu(
    RvbHandle* handle,
    const void* input,
    uint32_t inputStride)
{
    if (handle == nullptr)
    {
        g_lastError = "rvb_process_rgba8_gpu handle must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->mutex);
    try
    {
        handle->lastTiming = handle->processor.ProcessGpu(
            input, inputStride, handle->quality);
        handle->lastError.clear();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        handle->lastError = message;
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_render_output_d3d11(
    RvbHandle* handle,
    const RvbDirectRenderOptions* options)
{
    if (handle == nullptr || options == nullptr)
    {
        g_lastError =
            "rvb_render_output_d3d11 handle and options must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }
    if (options->struct_size != sizeof(RvbDirectRenderOptions))
    {
        g_lastError =
            "rvb_render_output_d3d11 structure size mismatch";
        return RVB_STATUS_ABI_MISMATCH;
    }

    std::lock_guard lock(handle->mutex);
    try
    {
        handle->lastTiming = handle->processor.RenderDirect(*options);
        handle->lastError.clear();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        handle->lastError = message;
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_readback_output_rgba8(
    RvbHandle* handle,
    void* output,
    uint32_t outputStride)
{
    if (handle == nullptr)
    {
        g_lastError = "rvb_readback_output_rgba8 handle must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->mutex);
    try
    {
        handle->processor.ReadbackOutput(output, outputStride);
        handle->lastError.clear();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        handle->lastError = message;
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_reconfigure(
    RvbHandle* handle,
    uint32_t outputWidth,
    uint32_t outputHeight,
    uint32_t quality)
{
    if (handle == nullptr)
    {
        g_lastError = "rvb_reconfigure handle must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->mutex);
    try
    {
        Processor::ValidateQuality(quality);
        handle->processor.Reconfigure(outputWidth, outputHeight);
        handle->quality = quality;
        handle->lastError.clear();
        g_lastError.clear();
        return RVB_STATUS_OK;
    }
    catch (...)
    {
        std::string message;
        const RvbStatus status = TranslateCurrentException(message);
        handle->lastError = message;
        g_lastError = std::move(message);
        return status;
    }
}

extern "C" RvbStatus RVB_CALL rvb_get_last_frame_timing(
    RvbHandle* handle,
    RvbFrameTiming* timing)
{
    if (handle == nullptr || timing == nullptr)
    {
        g_lastError =
            "rvb_get_last_frame_timing handle and timing must not be NULL";
        return RVB_STATUS_INVALID_ARGUMENT;
    }
    if (timing->struct_size != sizeof(RvbFrameTiming))
    {
        g_lastError =
            "rvb_get_last_frame_timing structure size mismatch";
        return RVB_STATUS_ABI_MISMATCH;
    }

    std::lock_guard lock(handle->mutex);
    *timing = handle->lastTiming;
    return RVB_STATUS_OK;
}

extern "C" RvbStatus RVB_CALL rvb_get_last_error(
    RvbHandle* handle,
    char* utf8Buffer,
    size_t* bufferSize)
{
    if (bufferSize == nullptr)
    {
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    std::unique_lock<std::mutex> lock;
    const std::string* message = &g_lastError;
    if (handle != nullptr)
    {
        lock = std::unique_lock<std::mutex>(handle->mutex);
        message = &handle->lastError;
    }

    const std::size_t required = message->size() + 1u;
    if (utf8Buffer == nullptr)
    {
        *bufferSize = required;
        return RVB_STATUS_OK;
    }
    if (*bufferSize < required)
    {
        *bufferSize = required;
        return RVB_STATUS_BUFFER_TOO_SMALL;
    }

    std::memcpy(utf8Buffer, message->c_str(), required);
    *bufferSize = required;
    return RVB_STATUS_OK;
}

extern "C" void RVB_CALL rvb_destroy(RvbHandle* handle)
{
    try
    {
        delete handle;
    }
    catch (...)
    {
        // Never allow an exception to cross the plain C ABI.
    }
}
