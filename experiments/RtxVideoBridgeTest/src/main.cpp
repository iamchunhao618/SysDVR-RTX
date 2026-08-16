#include "RtxVsrBenchmark.h"
#include "WicImage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct ComScope
{
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            throw AppError(
                AppExitCode::InputError,
                "CoInitializeEx failed");
        }
        uninitialize = SUCCEEDED(hr);
    }

    ~ComScope()
    {
        if (uninitialize)
        {
            CoUninitialize();
        }
    }

    bool uninitialize = false;
};

struct Options
{
    std::filesystem::path inputPath = L"results/input.png";
    std::filesystem::path outputDirectory = L"results";
    std::filesystem::path featureDirectory;
    std::optional<std::filesystem::path> prepareSource;
    std::optional<std::uint32_t> adapterIndex;
    int warmupIterations = 20;
    int measuredIterations = 100;
    std::uint32_t outputWidth = 2560;
    std::uint32_t outputHeight = 1440;
    bool listAdaptersOnly = false;
    bool probeOnly = false;
    std::vector<NVSDK_NGX_VSR_QualityLevel> qualities = {
        NVSDK_NGX_VSR_Quality_Medium,
        NVSDK_NGX_VSR_Quality_High,
        NVSDK_NGX_VSR_Quality_Ultra,
    };
};

struct Statistics
{
    bool valid = false;
    double average = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double maximum = 0.0;
};

struct AlphaSummary
{
    std::uint8_t minimum = 255;
    std::uint8_t maximum = 0;
    std::uint64_t nonOpaquePixels = 0;
};

struct QualityResult
{
    NVSDK_NGX_VSR_QualityLevel quality{};
    std::string name;
    std::filesystem::path outputPath;
    std::vector<FrameTiming> frames;
    std::map<std::string, Statistics> statistics;
    AlphaSummary alpha;
};

std::string QualityName(NVSDK_NGX_VSR_QualityLevel quality)
{
    switch (quality)
    {
    case NVSDK_NGX_VSR_Quality_Bicubic: return "bicubic";
    case NVSDK_NGX_VSR_Quality_Low: return "vsr_low";
    case NVSDK_NGX_VSR_Quality_Medium: return "vsr_medium";
    case NVSDK_NGX_VSR_Quality_High: return "vsr_high";
    case NVSDK_NGX_VSR_Quality_Ultra: return "vsr_ultra";
    }
    return "unknown";
}

NVSDK_NGX_VSR_QualityLevel ParseQuality(const std::string& text)
{
    if (text == "0" || text == "bicubic")
        return NVSDK_NGX_VSR_Quality_Bicubic;
    if (text == "1" || text == "low")
        return NVSDK_NGX_VSR_Quality_Low;
    if (text == "2" || text == "medium")
        return NVSDK_NGX_VSR_Quality_Medium;
    if (text == "3" || text == "high")
        return NVSDK_NGX_VSR_Quality_High;
    if (text == "4" || text == "ultra")
        return NVSDK_NGX_VSR_Quality_Ultra;
    throw AppError(
        AppExitCode::InvalidArguments,
        "Unknown quality '" + text + "'; use bicubic, low, medium, high, ultra, or all");
}

void ParseResolution(
    const std::string& text,
    std::uint32_t& width,
    std::uint32_t& height)
{
    if (text == "1080p" || text == "1920x1080")
    {
        width = 1920;
        height = 1080;
        return;
    }
    if (text == "1440p" || text == "2560x1440")
    {
        width = 2560;
        height = 1440;
        return;
    }
    if (text == "2160p" || text == "4k" || text == "3840x2160")
    {
        width = 3840;
        height = 2160;
        return;
    }
    throw AppError(
        AppExitCode::InvalidArguments,
        "Unknown resolution '" + text +
            "'; use 1080p, 1440p, or 2160p/4k");
}

int ParsePositiveInt(const std::string& value, const char* option, bool allowZero)
{
    std::size_t parsed = 0;
    int result = 0;
    try
    {
        result = std::stoi(value, &parsed);
    }
    catch (...)
    {
        throw AppError(
            AppExitCode::InvalidArguments,
            std::string(option) + " requires an integer");
    }
    if (parsed != value.size() || result < (allowZero ? 0 : 1))
    {
        throw AppError(
            AppExitCode::InvalidArguments,
            std::string(option) + " is outside its valid range");
    }
    return result;
}

std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr)
    {
        return {};
    }
    const std::filesystem::path result(value);
    std::free(value);
    return result;
}

void PrintUsage()
{
    std::cout
        << "RtxVideoBridgeTest - standalone D3D11 RTX Video VSR benchmark\n\n"
        << "Options:\n"
        << "  --input PATH             1280x720 RGBA-decodable image\n"
        << "  --output-dir PATH        Output PNG/CSV/JSON directory\n"
        << "  --feature-dir PATH       Directory containing official nvngx_vsr.dll\n"
        << "  --prepare-input PATH     Crop/scale a source image to input.png first\n"
        << "  --adapter-index N        DXGI adapter index (default: first NVIDIA)\n"
        << "  --warmup N               Warm-up frames per quality (default: 20)\n"
        << "  --iterations N           Measured frames per quality (default: 100)\n"
        << "  --resolution NAME        1080p, 1440p, or 2160p/4k (default: 1440p)\n"
        << "  --quality NAME|all       bicubic, low, medium, high, ultra\n"
        << "  --list-adapters          List DXGI adapters and exit\n"
        << "  --probe-only             Initialize VSR and exit\n"
        << "  --help                   Show this message\n";
}

Options ParseArguments(int argc, wchar_t** argv)
{
    Options options;
    const auto sdkDirectory = EnvironmentPath(L"RTX_VIDEO_SDK_DIR");
    if (!sdkDirectory.empty())
    {
        options.featureDirectory =
            sdkDirectory / L"bin" / L"Windows" / L"x64" / L"dev";
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        auto requireValue = [&]() -> std::wstring {
            if (++index >= argc)
            {
                throw AppError(
                    AppExitCode::InvalidArguments,
                    Utf8(argument) + " requires a value");
            }
            return argv[index];
        };

        if (argument == L"--input")
            options.inputPath = requireValue();
        else if (argument == L"--output-dir")
            options.outputDirectory = requireValue();
        else if (argument == L"--feature-dir")
            options.featureDirectory = requireValue();
        else if (argument == L"--prepare-input")
            options.prepareSource = std::filesystem::path(requireValue());
        else if (argument == L"--adapter-index")
            options.adapterIndex = static_cast<std::uint32_t>(
                ParsePositiveInt(Utf8(requireValue()), "--adapter-index", true));
        else if (argument == L"--warmup")
            options.warmupIterations =
                ParsePositiveInt(Utf8(requireValue()), "--warmup", true);
        else if (argument == L"--iterations")
            options.measuredIterations =
                ParsePositiveInt(Utf8(requireValue()), "--iterations", false);
        else if (argument == L"--resolution")
            ParseResolution(
                Utf8(requireValue()),
                options.outputWidth,
                options.outputHeight);
        else if (argument == L"--quality")
        {
            const std::string value = Utf8(requireValue());
            if (value == "all")
            {
                options.qualities = {
                    NVSDK_NGX_VSR_Quality_Bicubic,
                    NVSDK_NGX_VSR_Quality_Low,
                    NVSDK_NGX_VSR_Quality_Medium,
                    NVSDK_NGX_VSR_Quality_High,
                    NVSDK_NGX_VSR_Quality_Ultra,
                };
            }
            else
            {
                options.qualities = {ParseQuality(value)};
            }
        }
        else if (argument == L"--list-adapters")
            options.listAdaptersOnly = true;
        else if (argument == L"--probe-only")
            options.probeOnly = true;
        else if (argument == L"--help" || argument == L"-h")
        {
            PrintUsage();
            std::exit(0);
        }
        else
        {
            throw AppError(
                AppExitCode::InvalidArguments,
                "Unknown option: " + Utf8(argument));
        }
    }
    return options;
}

void PrintAdapters(const std::vector<AdapterInfo>& adapters)
{
    std::cout << "DXGI adapters:\n";
    for (const auto& adapter : adapters)
    {
        std::cout
            << "  [" << adapter.index << "] " << Utf8(adapter.name)
            << " | vendor=0x" << std::hex << std::uppercase
            << adapter.vendorId
            << " device=0x" << adapter.deviceId << std::dec
            << " | dedicated="
            << (adapter.dedicatedVideoMemory / (1024 * 1024)) << " MiB"
            << " | LUID=" << adapter.luid.HighPart << ":"
            << adapter.luid.LowPart
            << (adapter.software ? " | software" : "")
            << "\n";
    }
}

const AdapterInfo& SelectAdapter(
    const std::vector<AdapterInfo>& adapters,
    const std::optional<std::uint32_t>& requestedIndex)
{
    if (requestedIndex.has_value())
    {
        const auto match = std::find_if(
            adapters.begin(), adapters.end(),
            [&](const AdapterInfo& adapter) {
                return adapter.index == *requestedIndex;
            });
        if (match == adapters.end())
        {
            throw AppError(
                AppExitCode::InvalidArguments,
                "Requested DXGI adapter index does not exist");
        }
        return *match;
    }

    const auto match = std::find_if(
        adapters.begin(), adapters.end(),
        [](const AdapterInfo& adapter) {
            return !adapter.software && adapter.vendorId == 0x10DE;
        });
    if (match == adapters.end())
    {
        throw AppError(
            AppExitCode::WrongAdapter,
            "No NVIDIA hardware DXGI adapter was found");
    }
    return *match;
}

Statistics CalculateStatistics(std::vector<double> values)
{
    values.erase(
        std::remove_if(values.begin(), values.end(),
                       [](double value) { return !std::isfinite(value); }),
        values.end());
    if (values.empty())
    {
        return {};
    }

    std::sort(values.begin(), values.end());
    Statistics result;
    result.valid = true;
    result.average = std::accumulate(values.begin(), values.end(), 0.0) /
                     values.size();
    const std::size_t middle = values.size() / 2;
    result.median = values.size() % 2 == 0
        ? (values[middle - 1] + values[middle]) / 2.0
        : values[middle];
    const std::size_t p95Index = std::min(
        values.size() - 1,
        static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1);
    result.p95 = values[p95Index];
    result.maximum = values.back();
    return result;
}

template <typename Getter>
Statistics StatisticsFor(
    const std::vector<FrameTiming>& frames,
    Getter getter)
{
    std::vector<double> values;
    values.reserve(frames.size());
    for (const auto& frame : frames)
    {
        values.push_back(getter(frame));
    }
    return CalculateStatistics(std::move(values));
}

std::map<std::string, Statistics> BuildStatistics(
    const std::vector<FrameTiming>& frames)
{
    std::map<std::string, Statistics> result;
    result["upload_submit_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.uploadSubmitCpuMs; });
    result["evaluate_call_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.evaluateCallCpuMs; });
    result["copy_submit_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.copySubmitCpuMs; });
    result["map_wait_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.mapWaitCpuMs; });
    result["cpu_row_copy_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.cpuRowCopyMs; });
    result["readback_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.readbackCpuMs; });
    result["process_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.processCpuMs; });
    result["query_resolve_cpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) { return f.queryResolveCpuMs; });
    result["upload_gpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) {
            return f.gpuTimingValid ? f.uploadGpuMs : NAN;
        });
    result["evaluate_gpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) {
            return f.gpuTimingValid ? f.evaluateGpuMs : NAN;
        });
    result["readback_copy_gpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) {
            return f.gpuTimingValid ? f.readbackCopyGpuMs : NAN;
        });
    result["total_gpu_ms"] = StatisticsFor(
        frames, [](const FrameTiming& f) {
            return f.gpuTimingValid ? f.totalGpuMs : NAN;
        });
    return result;
}

AlphaSummary AnalyzeAlpha(const ImageRgba& image)
{
    AlphaSummary result;
    for (std::size_t index = 3; index < image.pixels.size(); index += 4)
    {
        const auto alpha = image.pixels[index];
        result.minimum = std::min(result.minimum, alpha);
        result.maximum = std::max(result.maximum, alpha);
        if (alpha != 255)
        {
            ++result.nonOpaquePixels;
        }
    }
    return result;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream output;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(character)
                       << std::dec;
            }
            else
            {
                output << character;
            }
        }
    }
    return output.str();
}

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<QualityResult>& results)
{
    std::ofstream output(path);
    if (!output)
    {
        throw AppError(AppExitCode::InputError, "Could not create benchmark CSV");
    }
    output << "quality,metric,average_ms,median_ms,p95_ms,maximum_ms\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& result : results)
    {
        for (const auto& [metric, statistics] : result.statistics)
        {
            output << result.name << ',' << metric << ',';
            if (statistics.valid)
            {
                output << statistics.average << ',' << statistics.median << ','
                       << statistics.p95 << ',' << statistics.maximum;
            }
            output << '\n';
        }
    }
}

void WriteJson(
    const std::filesystem::path& path,
    const RtxVsrBenchmark& benchmark,
    int warmupIterations,
    int measuredIterations,
    const std::vector<QualityResult>& results)
{
    std::ofstream output(path);
    if (!output)
    {
        throw AppError(AppExitCode::InputError, "Could not create benchmark JSON");
    }

    const auto& adapter = benchmark.SelectedAdapter();
    output << std::fixed << std::setprecision(6);
    output << "{\n"
           << "  \"sdk\": \"NVIDIA RTX Video SDK 1.1.0\",\n"
           << "  \"api\": \"D3D11 NGX VSR\",\n"
           << "  \"adapter\": {\n"
           << "    \"index\": " << adapter.index << ",\n"
           << "    \"name\": \"" << JsonEscape(Utf8(adapter.name)) << "\",\n"
           << "    \"vendor_id\": " << adapter.vendorId << ",\n"
           << "    \"device_id\": " << adapter.deviceId << ",\n"
           << "    \"dedicated_video_memory_bytes\": "
           << adapter.dedicatedVideoMemory << "\n"
           << "  },\n"
           << "  \"loaded_feature_module\": \""
           << JsonEscape(Utf8(benchmark.LoadedFeatureModule().wstring()))
           << "\",\n"
           << "  \"input\": {\"width\": 1280, \"height\": 720, "
              "\"format\": \"DXGI_FORMAT_R8G8B8A8_UNORM\"},\n"
           << "  \"output\": {\"width\": " << benchmark.OutputWidth()
           << ", \"height\": " << benchmark.OutputHeight() << ", "
              "\"format\": \"DXGI_FORMAT_R8G8B8A8_UNORM\"},\n"
           << "  \"warmup_iterations\": " << warmupIterations << ",\n"
           << "  \"measured_iterations\": " << measuredIterations << ",\n"
           << "  \"qualities\": [\n";

    for (std::size_t qualityIndex = 0;
         qualityIndex < results.size(); ++qualityIndex)
    {
        const auto& result = results[qualityIndex];
        output << "    {\n"
               << "      \"name\": \"" << result.name << "\",\n"
               << "      \"quality_value\": "
               << static_cast<int>(result.quality) << ",\n"
               << "      \"output_file\": \""
               << JsonEscape(Utf8(result.outputPath.wstring())) << "\",\n"
               << "      \"alpha\": {\"minimum\": "
               << static_cast<int>(result.alpha.minimum)
               << ", \"maximum\": "
               << static_cast<int>(result.alpha.maximum)
               << ", \"non_opaque_pixels\": "
               << result.alpha.nonOpaquePixels << "},\n"
               << "      \"timings_ms\": {\n";

        std::size_t metricIndex = 0;
        for (const auto& [metric, statistics] : result.statistics)
        {
            output << "        \"" << metric << "\": ";
            if (!statistics.valid)
            {
                output << "null";
            }
            else
            {
                output << "{\"average\": " << statistics.average
                       << ", \"median\": " << statistics.median
                       << ", \"p95\": " << statistics.p95
                       << ", \"maximum\": " << statistics.maximum << "}";
            }
            output << (++metricIndex == result.statistics.size() ? "\n" : ",\n");
        }
        output << "      }\n"
               << "    }"
               << (qualityIndex + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void PrintPrimaryStatistics(const QualityResult& result)
{
    const auto print = [&](const char* metric) {
        const auto& value = result.statistics.at(metric);
        std::cout << std::setw(11) << std::fixed << std::setprecision(3)
                  << value.average << std::setw(11) << value.median
                  << std::setw(11) << value.p95 << std::setw(11)
                  << value.maximum;
    };

    std::cout << "\n" << result.name << " timing (ms)\n"
              << "metric                 average     median        p95        max\n";
    std::cout << "GPU upload       "; print("upload_gpu_ms"); std::cout << '\n';
    std::cout << "GPU VSR          "; print("evaluate_gpu_ms"); std::cout << '\n';
    std::cout << "GPU readback copy"; print("readback_copy_gpu_ms"); std::cout << '\n';
    std::cout << "CPU Map wait     "; print("map_wait_cpu_ms"); std::cout << '\n';
    std::cout << "CPU output rows  "; print("cpu_row_copy_ms"); std::cout << '\n';
    std::cout << "CPU readback all "; print("readback_cpu_ms"); std::cout << '\n';
    std::cout << "CPU ProcessFrame "; print("process_cpu_ms"); std::cout << '\n';
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        ComScope com;
        Options options = ParseArguments(argc, argv);

        const auto adapters = RtxVsrBenchmark::EnumerateAdapters();
        PrintAdapters(adapters);
        if (options.listAdaptersOnly)
        {
            return 0;
        }

        if (options.featureDirectory.empty())
        {
            throw AppError(
                AppExitCode::MissingFeatureDll,
                "Set RTX_VIDEO_SDK_DIR or pass --feature-dir pointing to the "
                "official SDK dev/rel feature directory");
        }

        std::filesystem::create_directories(options.outputDirectory);
        if (options.prepareSource.has_value())
        {
            auto factory = CreateWicFactory();
            PrepareInputImage(
                factory.Get(), *options.prepareSource, options.inputPath,
                RtxVsrBenchmark::InputWidth,
                RtxVsrBenchmark::InputHeight);
            std::cout << "Prepared 1280x720 RGBA input PNG: "
                      << Utf8(std::filesystem::absolute(options.inputPath).wstring())
                      << "\n";
        }

        const auto& selectedAdapter =
            SelectAdapter(adapters, options.adapterIndex);
        RtxVsrBenchmark benchmark;
        benchmark.Initialize(
            selectedAdapter,
            options.featureDirectory,
            options.outputDirectory / L"ngx-cache",
            options.outputWidth,
            options.outputHeight);

        std::cout << "Selected adapter: " << Utf8(selectedAdapter.name) << "\n"
                  << "VSR.Available: " << benchmark.VsrAvailable() << "\n"
                  << "VSR.NeedsUpdatedDriver: "
                  << benchmark.NeedsUpdatedDriver() << "\n"
                  << "VSR scratch buffer: "
                  << benchmark.ScratchBufferBytes() << " bytes\n"
                  << "Loaded feature module: "
                  << Utf8(benchmark.LoadedFeatureModule().wstring()) << "\n"
                  << "Output resolution: " << benchmark.OutputWidth() << "x"
                  << benchmark.OutputHeight() << " (direct NGX output)\n";

        if (options.probeOnly)
        {
            benchmark.Shutdown();
            std::cout << "Probe completed successfully.\n";
            return 0;
        }

        auto factory = CreateWicFactory();
        const ImageRgba input = LoadImageRgba(factory.Get(), options.inputPath);
        if (input.width != RtxVsrBenchmark::InputWidth ||
            input.height != RtxVsrBenchmark::InputHeight)
        {
            throw AppError(
                AppExitCode::InputError,
                "Input image must be exactly 1280x720; use --prepare-input "
                "to create the test input");
        }

        std::vector<QualityResult> results;
        for (const auto quality : options.qualities)
        {
            const std::string name = QualityName(quality);
            std::cout << "\nRunning " << name << ": "
                      << options.warmupIterations << " warm-up + "
                      << options.measuredIterations << " measured frames...\n";

            ImageRgba output;
            for (int iteration = 0;
                 iteration < options.warmupIterations; ++iteration)
            {
                benchmark.ProcessFrame(input, quality, output);
            }

            QualityResult result;
            result.quality = quality;
            result.name = name;
            result.frames.reserve(options.measuredIterations);
            for (int iteration = 0;
                 iteration < options.measuredIterations; ++iteration)
            {
                result.frames.push_back(
                    benchmark.ProcessFrame(input, quality, output));
            }

            result.outputPath =
                std::filesystem::absolute(
                    options.outputDirectory / ("output_" + name + ".png"));
            SavePngRgba(factory.Get(), result.outputPath, output);
            result.statistics = BuildStatistics(result.frames);
            result.alpha = AnalyzeAlpha(output);
            PrintPrimaryStatistics(result);
            std::cout << "Output: " << Utf8(result.outputPath.wstring()) << "\n"
                      << "Alpha range: "
                      << static_cast<int>(result.alpha.minimum) << ".."
                      << static_cast<int>(result.alpha.maximum)
                      << "; non-opaque pixels="
                      << result.alpha.nonOpaquePixels << "\n";
            results.push_back(std::move(result));
        }

        WriteCsv(options.outputDirectory / L"benchmark.csv", results);
        WriteJson(
            options.outputDirectory / L"benchmark.json",
            benchmark,
            options.warmupIterations,
            options.measuredIterations,
            results);
        benchmark.Shutdown();

        std::cout << "\nBenchmark completed successfully.\n"
                  << "CSV: "
                  << Utf8(std::filesystem::absolute(
                         options.outputDirectory / L"benchmark.csv").wstring())
                  << "\nJSON: "
                  << Utf8(std::filesystem::absolute(
                         options.outputDirectory / L"benchmark.json").wstring())
                  << "\n";
        return 0;
    }
    catch (const AppError& error)
    {
        std::cerr << "ERROR[" << static_cast<int>(error.Code())
                  << "]: " << error.what() << "\n";
        return static_cast<int>(error.Code());
    }
    catch (const std::exception& error)
    {
        std::cerr << "ERROR[1]: " << error.what() << "\n";
        return 1;
    }
}
