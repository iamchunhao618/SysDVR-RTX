#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

struct ImageRgba
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

Microsoft::WRL::ComPtr<IWICImagingFactory> CreateWicFactory();

ImageRgba LoadImageRgba(
    IWICImagingFactory* factory,
    const std::filesystem::path& path);

void SavePngRgba(
    IWICImagingFactory* factory,
    const std::filesystem::path& path,
    const ImageRgba& image);

void PrepareInputImage(
    IWICImagingFactory* factory,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight);
