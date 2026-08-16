#include "WicImage.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
std::string HexHr(HRESULT hr)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(hr);
    return stream.str();
}

void CheckHr(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(
            std::string(operation) + " failed with HRESULT " + HexHr(hr));
    }
}

ComPtr<IWICBitmapFrameDecode> DecodeFirstFrame(
    IWICImagingFactory* factory,
    const std::filesystem::path& path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder),
            "IWICImagingFactory::CreateDecoderFromFilename");

    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");
    return frame;
}

ImageRgba CopySourceToRgba(
    IWICImagingFactory* factory,
    IWICBitmapSource* source,
    std::uint32_t width,
    std::uint32_t height)
{
    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(&converter),
            "IWICImagingFactory::CreateFormatConverter");
    CheckHr(converter->Initialize(
                source,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "IWICFormatConverter::Initialize(RGBA8)");

    ImageRgba result;
    result.width = width;
    result.height = height;
    const std::uint64_t byteCount =
        static_cast<std::uint64_t>(width) * height * 4u;
    if (byteCount > std::numeric_limits<UINT>::max())
    {
        throw std::runtime_error("Image is too large for WIC CopyPixels");
    }
    result.pixels.resize(static_cast<std::size_t>(byteCount));
    CheckHr(converter->CopyPixels(
                nullptr,
                width * 4u,
                static_cast<UINT>(result.pixels.size()),
                result.pixels.data()),
            "IWICBitmapSource::CopyPixels");
    return result;
}
} // namespace

ComPtr<IWICImagingFactory> CreateWicFactory()
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        CheckHr(CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory)),
                "CoCreateInstance(CLSID_WICImagingFactory)");
    }
    return factory;
}

ImageRgba LoadImageRgba(
    IWICImagingFactory* factory,
    const std::filesystem::path& path)
{
    auto frame = DecodeFirstFrame(factory, path);
    UINT width = 0;
    UINT height = 0;
    CheckHr(frame->GetSize(&width, &height), "IWICBitmapSource::GetSize");
    return CopySourceToRgba(factory, frame.Get(), width, height);
}

void SavePngRgba(
    IWICImagingFactory* factory,
    const std::filesystem::path& path,
    const ImageRgba& image)
{
    if (image.pixels.size() !=
        static_cast<std::size_t>(image.width) * image.height * 4u)
    {
        throw std::runtime_error("RGBA image buffer has an invalid size");
    }

    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);

    ComPtr<IWICStream> stream;
    CheckHr(factory->CreateStream(&stream), "IWICImagingFactory::CreateStream");
    CheckHr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
            "IWICStream::InitializeFromFilename");

    ComPtr<IWICBitmapEncoder> encoder;
    CheckHr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
            "IWICImagingFactory::CreateEncoder(PNG)");
    CheckHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
            "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    CheckHr(encoder->CreateNewFrame(&frame, &properties),
            "IWICBitmapEncoder::CreateNewFrame");
    CheckHr(frame->Initialize(properties.Get()),
            "IWICBitmapFrameEncode::Initialize");
    CheckHr(frame->SetSize(image.width, image.height),
            "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    CheckHr(frame->SetPixelFormat(&format),
            "IWICBitmapFrameEncode::SetPixelFormat");

    const BYTE* encodedPixels = image.pixels.data();
    std::vector<BYTE> bgraPixels;
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
    {
        bgraPixels = image.pixels;
        for (std::size_t index = 0; index < bgraPixels.size(); index += 4)
        {
            std::swap(bgraPixels[index], bgraPixels[index + 2]);
        }
        encodedPixels = bgraPixels.data();
    }
    else if (!IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA))
    {
        throw std::runtime_error(
            "The WIC PNG encoder requested an unsupported pixel format");
    }

    CheckHr(frame->WritePixels(
                image.height,
                image.width * 4u,
                static_cast<UINT>(image.pixels.size()),
                const_cast<BYTE*>(encodedPixels)),
            "IWICBitmapFrameEncode::WritePixels");
    CheckHr(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    CheckHr(encoder->Commit(), "IWICBitmapEncoder::Commit");
}

void PrepareInputImage(
    IWICImagingFactory* factory,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    auto frame = DecodeFirstFrame(factory, sourcePath);

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    CheckHr(frame->GetSize(&sourceWidth, &sourceHeight),
            "IWICBitmapSource::GetSize");

    const double targetAspect =
        static_cast<double>(targetWidth) / targetHeight;
    const double sourceAspect =
        static_cast<double>(sourceWidth) / sourceHeight;

    WICRect crop{};
    if (sourceAspect > targetAspect)
    {
        crop.Height = static_cast<INT>(sourceHeight);
        crop.Width = static_cast<INT>(sourceHeight * targetAspect);
        crop.X = static_cast<INT>((sourceWidth - crop.Width) / 2u);
        crop.Y = 0;
    }
    else
    {
        crop.Width = static_cast<INT>(sourceWidth);
        crop.Height = static_cast<INT>(sourceWidth / targetAspect);
        crop.X = 0;
        crop.Y = static_cast<INT>((sourceHeight - crop.Height) / 2u);
    }

    ComPtr<IWICBitmapClipper> clipper;
    CheckHr(factory->CreateBitmapClipper(&clipper),
            "IWICImagingFactory::CreateBitmapClipper");
    CheckHr(clipper->Initialize(frame.Get(), &crop),
            "IWICBitmapClipper::Initialize");

    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(factory->CreateBitmapScaler(&scaler),
            "IWICImagingFactory::CreateBitmapScaler");
    CheckHr(scaler->Initialize(
                clipper.Get(), targetWidth, targetHeight,
                WICBitmapInterpolationModeFant),
            "IWICBitmapScaler::Initialize");

    auto image = CopySourceToRgba(
        factory, scaler.Get(), targetWidth, targetHeight);
    SavePngRgba(factory, destinationPath, image);
}
