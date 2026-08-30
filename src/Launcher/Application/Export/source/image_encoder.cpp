#include "image_encoder.h"

#include <Windows.h>
#include <cstring>
#include <limits>
#include <new>
#include <wincodec.h>
#include <wrl/client.h>

namespace open_st
{
namespace
{
// 保留具体 HRESULT 供上层记录，用户可见错误由 Application 本地化。
bool CheckResult(HRESULT result, const wchar_t* operation, std::wstring& error)
{
    if (SUCCEEDED(result))
    {
        return true;
    }
    error = std::wstring(operation) + L" HRESULT=" + std::to_wstring(static_cast<unsigned long>(result));
    return false;
}
} // namespace

// 在任何像素访问和尺寸乘法之前验证输入范围，允许最后一行不带 padding。
bool ValidateSdrImage(const SdrImageView& image, std::wstring& error)
{
    error.clear();
    constexpr std::size_t MAX_BYTES = (std::numeric_limits<UINT>::max)();
    if (image.width == 0U || image.height == 0U || image.width > MAX_BYTES / 4U ||
        image.height > static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)()))
    {
        error = L"Invalid SDR image dimensions";
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
    if (image.stride < rowBytes || image.stride > MAX_BYTES ||
        static_cast<std::size_t>(image.height) > (MAX_BYTES - sizeof(BITMAPINFOHEADER)) / rowBytes ||
        static_cast<std::size_t>(image.height - 1U) >
            ((std::numeric_limits<std::size_t>::max)() - rowBytes) / image.stride ||
        image.pixels.size() < static_cast<std::size_t>(image.height - 1U) * image.stride + rowBytes)
    {
        error = L"Invalid SDR image stride or buffer length";
        return false;
    }
    return true;
}

// 使用 WIC 内存流编码不透明 BGR24，明确记录 sRGB 色彩语义。
bool EncodeSdrImage(const SdrImageView& image, ImageFileFormat format, std::vector<std::uint8_t>& encoded,
                    std::wstring& error)
{
    if (!ValidateSdrImage(image, error))
    {
        return false;
    }
    if (format != ImageFileFormat::Jpeg && format != ImageFileFormat::Png)
    {
        error = L"Unsupported image format";
        return false;
    }
    try
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (!CheckResult(
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                L"Create WIC factory", error) ||
            !CheckResult(CreateStreamOnHGlobal(nullptr, TRUE, &stream), L"Create memory stream", error) ||
            !CheckResult(factory->CreateEncoder(format == ImageFileFormat::Png ? GUID_ContainerFormatPng
                                                                               : GUID_ContainerFormatJpeg,
                                                nullptr, &encoder),
                         L"Create encoder", error) ||
            !CheckResult(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"Initialize encoder", error) ||
            !CheckResult(encoder->CreateNewFrame(&frame, &options), L"Create frame", error))
        {
            return false;
        }
        if (format == ImageFileFormat::Jpeg)
        {
            PROPBAG2 property{};
            wchar_t propertyName[] = L"ImageQuality";
            property.pstrName = propertyName;
            VARIANT value{};
            value.vt = VT_R4;
            value.fltVal = 0.95F;
            if (!CheckResult(options->Write(1U, &property, &value), L"Set JPEG quality", error))
            {
                return false;
            }
        }
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
        if (!CheckResult(frame->Initialize(options.Get()), L"Initialize frame", error) ||
            !CheckResult(frame->SetSize(image.width, image.height), L"Set image dimensions", error) ||
            !CheckResult(frame->SetPixelFormat(&pixelFormat), L"Set opaque BGR format", error))
        {
            return false;
        }
        if (!IsEqualGUID(pixelFormat, GUID_WICPixelFormat24bppBGR))
        {
            error = L"Encoder did not accept opaque BGR24";
            return false;
        }
        ComPtr<IWICMetadataQueryWriter> metadata;
        if (!CheckResult(frame->GetMetadataQueryWriter(&metadata), L"Get color metadata writer", error))
        {
            return false;
        }
        PROPVARIANT colorSpace{};
        if (format == ImageFileFormat::Png)
        {
            colorSpace.vt = VT_UI1;
            colorSpace.bVal = 0U;
        }
        else
        {
            colorSpace.vt = VT_UI2;
            colorSpace.uiVal = 1U;
        }
        if (!CheckResult(metadata->SetMetadataByName(format == ImageFileFormat::Png ? L"/sRGB/RenderingIntent"
                                                                                    : L"/app1/ifd/exif/{ushort=40961}",
                                                     &colorSpace),
                         L"Write sRGB metadata", error))
        {
            return false;
        }
        const UINT rowBytes = image.width * 3U;
        std::vector<std::uint8_t> row(rowBytes);
        for (std::uint32_t y = 0U; y < image.height; ++y)
        {
            const std::uint8_t* source = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
            for (std::uint32_t x = 0U; x < image.width; ++x)
            {
                std::memcpy(row.data() + static_cast<std::size_t>(x) * 3U, source + static_cast<std::size_t>(x) * 4U,
                            3U);
            }
            if (!CheckResult(frame->WritePixels(1U, rowBytes, rowBytes, row.data()), L"Encode row", error))
            {
                return false;
            }
        }
        if (!CheckResult(frame->Commit(), L"Commit image frame", error) ||
            !CheckResult(encoder->Commit(), L"Commit image encoder", error))
        {
            return false;
        }
        STATSTG stat{};
        if (!CheckResult(stream->Stat(&stat, STATFLAG_NONAME), L"Measure encoded stream", error))
        {
            return false;
        }
        if (stat.cbSize.QuadPart == 0U || stat.cbSize.QuadPart > (std::numeric_limits<ULONG>::max)())
        {
            error = L"Encoded image exceeds supported stream size";
            return false;
        }
        std::vector<std::uint8_t> candidate(static_cast<std::size_t>(stat.cbSize.QuadPart));
        LARGE_INTEGER start{};
        ULONG read{};
        if (!CheckResult(stream->Seek(start, STREAM_SEEK_SET, nullptr), L"Rewind encoded stream", error) ||
            !CheckResult(stream->Read(candidate.data(), static_cast<ULONG>(candidate.size()), &read),
                         L"Read encoded image", error) ||
            read != candidate.size())
        {
            if (error.empty())
            {
                error = L"Incomplete encoded image read";
            }
            return false;
        }
        encoded = std::move(candidate);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = L"Insufficient memory to encode image";
        return false;
    }
}
} // namespace open_st
