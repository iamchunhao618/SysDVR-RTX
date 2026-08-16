#include "RtxVideoBridge.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
std::string LastError(RvbHandle* handle)
{
    std::size_t size = 0;
    if (rvb_get_last_error(handle, nullptr, &size) != RVB_STATUS_OK || size == 0)
    {
        return "<no bridge error text>";
    }

    std::string message(size, '\0');
    if (rvb_get_last_error(handle, message.data(), &size) != RVB_STATUS_OK)
    {
        return "<could not read bridge error text>";
    }
    if (!message.empty() && message.back() == '\0')
    {
        message.pop_back();
    }
    return message;
}

std::filesystem::path DefaultApplicationDataDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(path.size()), path.data());
    if (length == 0 || length >= path.size())
    {
        return std::filesystem::current_path() / L"ngx-cache";
    }
    path.resize(length);
    return std::filesystem::path(path) / L"SysDVR-RtxVideoBridge-Smoke";
}

void FillInput(std::vector<std::uint8_t>& input)
{
    for (std::uint32_t y = 0; y < RVB_INPUT_HEIGHT; ++y)
    {
        for (std::uint32_t x = 0; x < RVB_INPUT_WIDTH; ++x)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * RVB_INPUT_WIDTH + x) * 4u;
            input[offset + 0] = static_cast<std::uint8_t>(x * 255u / (RVB_INPUT_WIDTH - 1u));
            input[offset + 1] = static_cast<std::uint8_t>(y * 255u / (RVB_INPUT_HEIGHT - 1u));
            input[offset + 2] = static_cast<std::uint8_t>((x ^ y) & 0xFFu);
            input[offset + 3] = 255u;
        }
    }
}
} // namespace

int wmain(int argumentCount, wchar_t** arguments)
{
    if (argumentCount < 2 || argumentCount > 4)
    {
        std::wcerr
            << L"Usage: RtxVideoBridgeSmokeTest.exe FEATURE_DIRECTORY "
               L"[QUALITY_0_TO_4] [ADAPTER_INDEX]\n";
        return RVB_STATUS_INVALID_ARGUMENT;
    }

    const std::filesystem::path featureDirectory = arguments[1];
    const std::filesystem::path applicationDataDirectory =
        DefaultApplicationDataDirectory();
    const std::uint32_t quality = argumentCount >= 3
        ? static_cast<std::uint32_t>(std::wcstoul(arguments[2], nullptr, 10))
        : RVB_QUALITY_MEDIUM;
    const int adapterIndex = argumentCount >= 4
        ? static_cast<int>(std::wcstol(arguments[3], nullptr, 10))
        : -1;

    std::wcout << L"RtxVideoBridge ABI version: " << rvb_get_api_version() << L"\n";
    if (rvb_get_api_version() != RVB_API_VERSION)
    {
        std::wcerr << L"ABI version mismatch\n";
        return RVB_STATUS_ABI_MISMATCH;
    }

    RvbProbeOptions probeOptions{};
    probeOptions.struct_size = sizeof(probeOptions);
    probeOptions.feature_directory = featureDirectory.c_str();
    probeOptions.application_data_directory = applicationDataDirectory.c_str();
    probeOptions.adapter_index = adapterIndex;

    RvbCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    RvbStatus status = rvb_probe(&probeOptions, &capabilities);
    if (status != RVB_STATUS_OK)
    {
        std::cerr << "Probe failed [" << static_cast<int>(status)
                  << "]: " << LastError(nullptr) << "\n";
        return status;
    }

    std::wcout
        << L"Probe OK: DXGI adapter " << capabilities.adapter_index
        << L", vendor=0x" << std::hex << capabilities.adapter_vendor_id
        << L", device=0x" << capabilities.adapter_device_id << std::dec
        << L", dedicated="
        << (capabilities.adapter_dedicated_video_memory / (1024u * 1024u))
        << L" MiB\n";

    RvbCreateOptions createOptions{};
    createOptions.struct_size = sizeof(createOptions);
    createOptions.feature_directory = featureDirectory.c_str();
    createOptions.application_data_directory = applicationDataDirectory.c_str();
    createOptions.adapter_index = adapterIndex;
    createOptions.input_width = RVB_INPUT_WIDTH;
    createOptions.input_height = RVB_INPUT_HEIGHT;
    createOptions.output_width = RVB_DEFAULT_OUTPUT_WIDTH;
    createOptions.output_height = RVB_DEFAULT_OUTPUT_HEIGHT;
    createOptions.quality = quality;

    RvbHandle* handle = nullptr;
    status = rvb_create(&createOptions, &handle);
    if (status != RVB_STATUS_OK)
    {
        std::cerr << "Create failed [" << static_cast<int>(status)
                  << "]: " << LastError(nullptr) << "\n";
        return status;
    }

    std::vector<std::uint8_t> input(
        static_cast<std::size_t>(RVB_INPUT_WIDTH) * RVB_INPUT_HEIGHT * 4u);
    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(RVB_DEFAULT_OUTPUT_WIDTH) *
            RVB_DEFAULT_OUTPUT_HEIGHT * 4u);
    FillInput(input);

    status = rvb_process_rgba8(
        handle,
        input.data(),
        RVB_INPUT_WIDTH * 4u,
        output.data(),
        RVB_DEFAULT_OUTPUT_WIDTH * 4u);
    if (status != RVB_STATUS_OK)
    {
        std::cerr << "Process failed [" << static_cast<int>(status)
                  << "]: " << LastError(handle) << "\n";
        rvb_destroy(handle);
        return status;
    }

    RvbFrameTiming timing{};
    timing.struct_size = sizeof(timing);
    status = rvb_get_last_frame_timing(handle, &timing);
    if (status != RVB_STATUS_OK)
    {
        std::cerr << "Timing retrieval failed [" << static_cast<int>(status)
                  << "]: " << LastError(handle) << "\n";
        rvb_destroy(handle);
        return status;
    }

    std::uint64_t checksum = 0;
    std::uint8_t minimumAlpha = 255;
    std::uint8_t maximumAlpha = 0;
    for (std::size_t offset = 0; offset < output.size(); offset += 4u)
    {
        checksum += output[offset + 0];
        checksum += output[offset + 1];
        checksum += output[offset + 2];
        minimumAlpha = std::min(minimumAlpha, output[offset + 3]);
        maximumAlpha = std::max(maximumAlpha, output[offset + 3]);
    }

    rvb_destroy(handle);
    std::cout << "Process OK: quality=" << quality
              << ", RGB checksum=" << checksum
              << ", alpha=" << static_cast<unsigned int>(minimumAlpha)
              << ".." << static_cast<unsigned int>(maximumAlpha)
              << ", VSR GPU=" << timing.evaluate_gpu_ms
              << " ms, Map wait=" << timing.map_wait_cpu_ms << " ms\n";
    return RVB_STATUS_OK;
}
