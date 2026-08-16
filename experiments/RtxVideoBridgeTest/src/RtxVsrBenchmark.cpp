#include "RtxVsrBenchmark.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr std::uint32_t NvidiaVendorId = 0x10DE;

double Milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::string Hex32(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

void CheckHr(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        const auto code =
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                ? AppExitCode::DeviceLost
                : AppExitCode::D3dFailure;
        throw AppError(
            code,
            std::string(operation) + " failed with HRESULT " +
                Hex32(static_cast<std::uint32_t>(hr)));
    }
}

std::filesystem::path ModulePath(HMODULE module)
{
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
        const HRESULT hr = context->GetData(query, &value, sizeof(value), 0);
        if (hr == S_OK)
        {
            return;
        }
        if (FAILED(hr))
        {
            CheckHr(hr, name);
        }
        if (Clock::now() >= deadline)
        {
            throw AppError(
                AppExitCode::D3dFailure,
                std::string("Timed out resolving ") + name);
        }
        std::this_thread::yield();
    }
}

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
} // namespace

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

RtxVsrBenchmark::~RtxVsrBenchmark()
{
    Shutdown();
}

std::vector<AdapterInfo> RtxVsrBenchmark::EnumerateAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
            "CreateDXGIFactory1");

    std::vector<AdapterInfo> result;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        CheckHr(hr, "IDXGIFactory1::EnumAdapters1");

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
        result.push_back(std::move(info));
    }
    return result;
}

void RtxVsrBenchmark::Initialize(
    const AdapterInfo& adapter,
    const std::filesystem::path& featureDirectory,
    const std::filesystem::path& applicationDataDirectory,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight)
{
    Shutdown();

    const bool supportedOutput =
        (outputWidth == 1920 && outputHeight == 1080) ||
        (outputWidth == 2560 && outputHeight == 1440) ||
        (outputWidth == 3840 && outputHeight == 2160);
    if (!supportedOutput)
    {
        throw AppError(
            AppExitCode::InvalidArguments,
            "Output resolution must be 1920x1080, 2560x1440, or 3840x2160");
    }
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;

    if (adapter.software || adapter.vendorId != NvidiaVendorId)
    {
        throw AppError(
            AppExitCode::WrongAdapter,
            "Adapter " + std::to_string(adapter.index) + " ('" +
                Utf8(adapter.name) + "', vendor " + Hex32(adapter.vendorId) +
                ") is not an NVIDIA hardware adapter");
    }

    const auto featureDll = featureDirectory / L"nvngx_vsr.dll";
    if (!std::filesystem::is_regular_file(featureDll))
    {
        throw AppError(
            AppExitCode::MissingFeatureDll,
            "Required RTX VSR feature DLL is missing: " +
                Utf8(featureDll.wstring()));
    }

    std::filesystem::create_directories(applicationDataDirectory);
    selectedAdapter_ = adapter;
    CreateDeviceAndTextures(adapter);
    InitializeNgx(featureDirectory, applicationDataDirectory);
    CreateTimingQueries();
}

void RtxVsrBenchmark::CreateDeviceAndTextures(const AdapterInfo& adapter)
{
    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL actualLevel{};
    CheckHr(D3D11CreateDevice(
                adapter.adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                0,
                requestedLevels,
                static_cast<UINT>(std::size(requestedLevels)),
                D3D11_SDK_VERSION,
                &device_,
                &actualLevel,
                &context_),
            "D3D11CreateDevice");
    if (actualLevel < D3D_FEATURE_LEVEL_11_0)
    {
        throw AppError(
            AppExitCode::D3dFailure,
            "The selected adapter does not support D3D feature level 11.0");
    }

    if (SUCCEEDED(context_.As(&multithread_)))
    {
        multithread_->SetMultithreadProtected(TRUE);
    }

    D3D11_TEXTURE2D_DESC inputDescription{};
    inputDescription.Width = InputWidth;
    inputDescription.Height = InputHeight;
    inputDescription.MipLevels = 1;
    inputDescription.ArraySize = 1;
    inputDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    inputDescription.SampleDesc.Count = 1;
    inputDescription.Usage = D3D11_USAGE_DEFAULT;
    inputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CheckHr(device_->CreateTexture2D(
                &inputDescription, nullptr, &inputTexture_),
            "ID3D11Device::CreateTexture2D(input RGBA8)");

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
    CheckHr(device_->CreateTexture2D(
                &outputDescription, nullptr, &outputTexture_),
            "ID3D11Device::CreateTexture2D(output RGBA8 UAV)");

    D3D11_TEXTURE2D_DESC stagingDescription = outputDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHr(device_->CreateTexture2D(
                &stagingDescription, nullptr, &stagingTexture_),
            "ID3D11Device::CreateTexture2D(readback staging)");
}

void RtxVsrBenchmark::InitializeNgx(
    const std::filesystem::path& featureDirectory,
    const std::filesystem::path& applicationDataDirectory)
{
    const std::wstring featurePath =
        std::filesystem::absolute(featureDirectory).wstring();
    const wchar_t* featurePaths[] = {featurePath.c_str()};
    NVSDK_NGX_FeatureCommonInfo commonInfo{};
    commonInfo.PathListInfo.Path = featurePaths;
    commonInfo.PathListInfo.Length = 1;

    MultithreadGuard guard(multithread_.Get());

    auto status = NVSDK_NGX_D3D11_Init(
        0,
        std::filesystem::absolute(applicationDataDirectory).c_str(),
        device_.Get(),
        &commonInfo);
    if (NVSDK_NGX_FAILED(status))
    {
        throw AppError(
            AppExitCode::NgxInitializationFailure,
            "NVSDK_NGX_D3D11_Init failed: " + NgxResultText(status));
    }
    ngxInitialized_ = true;

    status = NVSDK_NGX_D3D11_GetCapabilityParameters(&ngxParameters_);
    if (NVSDK_NGX_FAILED(status) || ngxParameters_ == nullptr)
    {
        throw AppError(
            AppExitCode::NgxInitializationFailure,
            "NVSDK_NGX_D3D11_GetCapabilityParameters failed: " +
                NgxResultText(status));
    }

    status = ngxParameters_->Get(
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

    if (NVSDK_NGX_FAILED(status) || vsrAvailable_ == 0)
    {
        std::ostringstream message;
        message << "RTX Video Super Resolution is unavailable on adapter '"
                << Utf8(selectedAdapter_.name) << "'";
        if (needsUpdatedDriver_ != 0)
        {
            message << "; NGX reports that a newer driver is required"
                    << " (minimum " << minimumDriverMajor_ << "."
                    << minimumDriverMinor_ << ")";
        }
        throw AppError(AppExitCode::VsrUnsupported, message.str());
    }

    status = NVSDK_NGX_D3D11_GetScratchBufferSize(
        NVSDK_NGX_Feature_VSR, ngxParameters_, &scratchBufferBytes_);
    if (NVSDK_NGX_FAILED(status))
    {
        throw AppError(
            AppExitCode::FeatureCreationFailure,
            "NVSDK_NGX_D3D11_GetScratchBufferSize(VSR) failed: " +
                NgxResultText(status));
    }
    if (scratchBufferBytes_ != 0)
    {
        throw AppError(
            AppExitCode::FeatureCreationFailure,
            "VSR unexpectedly requested a scratch buffer of " +
                std::to_string(scratchBufferBytes_) + " bytes");
    }

    NVSDK_NGX_Feature_Create_Params createParameters{};
    status = NGX_D3D11_CREATE_VSR_EXT(
        context_.Get(), &vsrFeature_, ngxParameters_, &createParameters);
    if (NVSDK_NGX_FAILED(status) || vsrFeature_ == nullptr)
    {
        int featureInitResult = 0;
        QueryOptionalInt(
            ngxParameters_, NVSDK_NGX_Parameter_VSR_FeatureInitResult,
            featureInitResult);
        throw AppError(
            AppExitCode::FeatureCreationFailure,
            "NGX_D3D11_CREATE_VSR_EXT failed: " + NgxResultText(status) +
                "; VSR.FeatureInitResult=" +
                Hex32(static_cast<std::uint32_t>(featureInitResult)));
    }

    const HMODULE featureModule = GetModuleHandleW(L"nvngx_vsr.dll");
    loadedFeatureModule_ = ModulePath(featureModule);
    if (loadedFeatureModule_.empty())
    {
        throw AppError(
            AppExitCode::FeatureCreationFailure,
            "VSR feature was created, but the loaded nvngx_vsr.dll path "
            "could not be verified");
    }
}

void RtxVsrBenchmark::CreateTimingQueries()
{
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    CheckHr(device_->CreateQuery(&description, &disjointQuery_),
            "ID3D11Device::CreateQuery(TIMESTAMP_DISJOINT)");

    description.Query = D3D11_QUERY_TIMESTAMP;
    CheckHr(device_->CreateQuery(&description, &timestampStartQuery_),
            "ID3D11Device::CreateQuery(timestamp start)");
    CheckHr(device_->CreateQuery(&description, &timestampUploadQuery_),
            "ID3D11Device::CreateQuery(timestamp upload)");
    CheckHr(device_->CreateQuery(&description, &timestampEvaluateQuery_),
            "ID3D11Device::CreateQuery(timestamp evaluate)");
    CheckHr(device_->CreateQuery(&description, &timestampCopyQuery_),
            "ID3D11Device::CreateQuery(timestamp copy)");
}

FrameTiming RtxVsrBenchmark::ProcessFrame(
    const ImageRgba& input,
    NVSDK_NGX_VSR_QualityLevel quality,
    ImageRgba& output)
{
    if (vsrFeature_ == nullptr || ngxParameters_ == nullptr)
    {
        throw AppError(
            AppExitCode::NgxInitializationFailure,
            "ProcessFrame was called before RTX VSR initialization");
    }
    if (input.width != InputWidth || input.height != InputHeight ||
        input.pixels.size() !=
            static_cast<std::size_t>(InputWidth) * InputHeight * 4u)
    {
        throw AppError(
            AppExitCode::InputError,
            "ProcessFrame requires exactly 1280x720 RGBA8 input");
    }
    if (quality < NVSDK_NGX_VSR_Quality_Bicubic ||
        quality > NVSDK_NGX_VSR_Quality_Ultra)
    {
        throw AppError(
            AppExitCode::InvalidArguments,
            "VSR quality must be in the verified SDK range 0..4");
    }

    output.width = outputWidth_;
    output.height = outputHeight_;
    output.pixels.resize(
        static_cast<std::size_t>(outputWidth_) * outputHeight_ * 4u);

    FrameTiming timing;
    const auto processStart = Clock::now();

    context_->Begin(disjointQuery_.Get());
    context_->End(timestampStartQuery_.Get());

    const auto uploadStart = Clock::now();
    context_->UpdateSubresource(
        inputTexture_.Get(), 0, nullptr,
        input.pixels.data(), InputWidth * 4u, 0);
    const auto uploadEnd = Clock::now();
    timing.uploadSubmitCpuMs = Milliseconds(uploadEnd - uploadStart);
    context_->End(timestampUploadQuery_.Get());

    NVSDK_NGX_D3D11_VSR_Eval_Params evaluateParameters{};
    evaluateParameters.pInput = inputTexture_.Get();
    evaluateParameters.pOutput = outputTexture_.Get();
    evaluateParameters.InputSubrectBase = {0, 0};
    evaluateParameters.InputSubrectSize = {InputWidth, InputHeight};
    evaluateParameters.OutputSubrectBase = {0, 0};
    evaluateParameters.OutputSubrectSize = {outputWidth_, outputHeight_};
    evaluateParameters.QualityLevel = quality;

    NVSDK_NGX_Result evaluateStatus{};
    const auto evaluateStart = Clock::now();
    {
        MultithreadGuard guard(multithread_.Get());
        evaluateStatus = NGX_D3D11_EVALUATE_VSR_EXT(
            context_.Get(), vsrFeature_, ngxParameters_, &evaluateParameters);
    }
    const auto evaluateEnd = Clock::now();
    timing.evaluateCallCpuMs = Milliseconds(evaluateEnd - evaluateStart);
    context_->End(timestampEvaluateQuery_.Get());

    if (NVSDK_NGX_FAILED(evaluateStatus))
    {
        ThrowIfDeviceLost("NGX_D3D11_EVALUATE_VSR_EXT");
        throw AppError(
            AppExitCode::EvaluationFailure,
            "NGX_D3D11_EVALUATE_VSR_EXT failed: " +
                NgxResultText(evaluateStatus));
    }

    const auto copySubmitStart = Clock::now();
    context_->CopyResource(stagingTexture_.Get(), outputTexture_.Get());
    const auto copySubmitEnd = Clock::now();
    timing.copySubmitCpuMs = Milliseconds(copySubmitEnd - copySubmitStart);
    context_->End(timestampCopyQuery_.Get());
    context_->End(disjointQuery_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mapStart = Clock::now();
    const HRESULT mapResult = context_->Map(
        stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    const auto mapEnd = Clock::now();
    timing.mapWaitCpuMs = Milliseconds(mapEnd - mapStart);
    if (FAILED(mapResult))
    {
        ThrowIfDeviceLost("ID3D11DeviceContext::Map(readback)");
        CheckHr(mapResult, "ID3D11DeviceContext::Map(readback)");
    }

    const auto rowCopyStart = Clock::now();
    const std::size_t destinationStride = outputWidth_ * 4u;
    for (std::uint32_t row = 0; row < outputHeight_; ++row)
    {
        std::memcpy(
            output.pixels.data() + row * destinationStride,
            static_cast<const std::uint8_t*>(mapped.pData) +
                row * mapped.RowPitch,
            destinationStride);
    }
    const auto rowCopyEnd = Clock::now();
    context_->Unmap(stagingTexture_.Get(), 0);

    timing.cpuRowCopyMs = Milliseconds(rowCopyEnd - rowCopyStart);
    timing.readbackCpuMs =
        timing.copySubmitCpuMs + timing.mapWaitCpuMs + timing.cpuRowCopyMs;
    const auto queryStart = Clock::now();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 timestampStart = 0;
    UINT64 timestampUpload = 0;
    UINT64 timestampEvaluate = 0;
    UINT64 timestampCopy = 0;
    WaitForQuery(context_.Get(), disjointQuery_.Get(), disjoint,
                 "TIMESTAMP_DISJOINT query");
    WaitForQuery(context_.Get(), timestampStartQuery_.Get(), timestampStart,
                 "start timestamp query");
    WaitForQuery(context_.Get(), timestampUploadQuery_.Get(), timestampUpload,
                 "upload timestamp query");
    WaitForQuery(context_.Get(), timestampEvaluateQuery_.Get(), timestampEvaluate,
                 "evaluate timestamp query");
    WaitForQuery(context_.Get(), timestampCopyQuery_.Get(), timestampCopy,
                 "copy timestamp query");
    timing.queryResolveCpuMs = Milliseconds(Clock::now() - queryStart);

    if (!disjoint.Disjoint && disjoint.Frequency != 0)
    {
        const double toMilliseconds =
            1000.0 / static_cast<double>(disjoint.Frequency);
        timing.uploadGpuMs =
            static_cast<double>(timestampUpload - timestampStart) *
            toMilliseconds;
        timing.evaluateGpuMs =
            static_cast<double>(timestampEvaluate - timestampUpload) *
            toMilliseconds;
        timing.readbackCopyGpuMs =
            static_cast<double>(timestampCopy - timestampEvaluate) *
            toMilliseconds;
        timing.totalGpuMs =
            static_cast<double>(timestampCopy - timestampStart) *
            toMilliseconds;
        timing.gpuTimingValid = true;
    }

    timing.processCpuMs = Milliseconds(Clock::now() - processStart);

    ThrowIfDeviceLost("ProcessFrame");
    return timing;
}

void RtxVsrBenchmark::ThrowIfDeviceLost(const char* operation)
{
    if (device_ == nullptr)
    {
        return;
    }
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (FAILED(reason))
    {
        throw AppError(
            AppExitCode::DeviceLost,
            std::string(operation) + ": D3D11 device was removed/reset; reason " +
                Hex32(static_cast<std::uint32_t>(reason)));
    }
}

void RtxVsrBenchmark::Shutdown() noexcept
{
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

    timestampCopyQuery_.Reset();
    timestampEvaluateQuery_.Reset();
    timestampUploadQuery_.Reset();
    timestampStartQuery_.Reset();
    disjointQuery_.Reset();
    stagingTexture_.Reset();
    outputTexture_.Reset();
    inputTexture_.Reset();
    multithread_.Reset();
    context_.Reset();
    device_.Reset();
    loadedFeatureModule_.clear();
    vsrAvailable_ = 0;
    needsUpdatedDriver_ = 0;
    minimumDriverMajor_ = 0;
    minimumDriverMinor_ = 0;
    scratchBufferBytes_ = 0;
}
