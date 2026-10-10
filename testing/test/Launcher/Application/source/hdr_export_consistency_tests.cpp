// 文件职责：验证同一次原生 HDR 转换的 SDR 像素经 DIB 和 PNG 导出后没有额外 RGB 改动。

#include <gtest/gtest.h>

#include <clipboard_api.h>
#include <color_conversion.h>
#include <image_encoder.h>
#include <native_tone_mapper.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <dxgi1_2.h>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace
{
// 只在当前线程初始化 COM；独立检查硬件图形平台，不访问桌面、剪贴板或文件系统。
class HdrExportConsistencyTest : public testing::Test
{
  protected:
    // 为 WIC 初始化 COM，并核对真实硬件适配器是否可用。
    // 入参：无。
    // 返回：无返回值；无硬件适配器时跳过，平台查询或 COM 初始化失败则报告失败。
    void SetUp() override
    {
        this->comResult_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->comResult_));
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))));
        for (UINT index = 0U;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory->EnumAdapters1(index, adapter.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                GTEST_SKIP() << "No hardware graphics adapter is available.";
            }
            ASSERT_TRUE(SUCCEEDED(result));
            DXGI_ADAPTER_DESC1 description{};
            ASSERT_TRUE(SUCCEEDED(adapter->GetDesc1(&description)));
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U)
            {
                return;
            }
        }
    }

    // 配对释放本用例在当前线程取得的 COM 初始化计数。
    // 入参：无。
    // 返回：无返回值；初始化失败时不调用 CoUninitialize。
    void TearDown() override
    {
        if (SUCCEEDED(this->comResult_))
        {
            CoUninitialize();
        }
    }

    HRESULT comResult_{E_FAIL};
};
} // namespace

// 验证两行相反排列的黑白灰及异色通道样本在真实 HDR 转换后，DIB 和 PNG 保留所有 RGB 与尺寸。
// 入参：无；全部像素自造于内存，亮度为 100%，不捕屏、不发布剪贴板、不写图片文件。
// 返回：无返回值；只验证导出未继续改色，不能据此判定 HDR 映射的文字粗细或画质正确。
TEST_F(HdrExportConsistencyTest, dib_and_png_preserve_tone_mapped_rgb)
{
    constexpr std::uint32_t WIDTH = 8U;
    constexpr std::uint32_t HEIGHT = 2U;
    // scRGB 的 1.0 对应 80 nit；灰阶模拟浅底暗字与深底亮字，异色样本防止 RGB 通道交换漏检。
    constexpr std::array<std::array<float, 3>, WIDTH> RGB_LEVELS{{
        {3.25F, 3.25F, 3.25F},
        {2.0F, 2.0F, 2.0F},
        {1.0F, 1.0F, 1.0F},
        {0.18F, 0.18F, 0.18F},
        {0.0F, 0.0F, 0.0F},
        {2.5F, 1.0F, 0.1F},
        {0.1F, 2.5F, 1.0F},
        {1.0F, 0.1F, 2.5F},
    }};
    std::vector<std::uint8_t> hdrPixels(static_cast<std::size_t>(WIDTH) * HEIGHT * 8U);
    for (std::uint32_t y = 0U; y < HEIGHT; ++y)
    {
        for (std::uint32_t x = 0U; x < WIDTH; ++x)
        {
            const std::array<float, 3>& rgb = RGB_LEVELS[y == 0U ? x : WIDTH - 1U - x];
            const std::array<std::uint16_t, 4> rgba{open_st::EncodeFloat16(rgb[0]), open_st::EncodeFloat16(rgb[1]),
                                                    open_st::EncodeFloat16(rgb[2]), open_st::EncodeFloat16(1.0F)};
            const std::size_t offset = (static_cast<std::size_t>(y) * WIDTH + x) * 8U;
            std::memcpy(hdrPixels.data() + offset, rgba.data(), sizeof(rgba));
        }
    }

    open_st::NativeToneMapper mapper;
    ASSERT_TRUE(mapper.SetBrightnessPercent(100U));
    std::wstring error;
    std::vector<std::uint8_t> bgra;
    ASSERT_TRUE(
        mapper.Convert({WIDTH, HEIGHT, WIDTH * 8U, hdrPixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 260.0F},
                       {4000U}, bgra, error))
        << error;
    ASSERT_EQ(bgra.size(), static_cast<std::size_t>(WIDTH) * HEIGHT * 4U);
    // 确认转换基准仍包含可区分通道，避免全灰结果让通道顺序检查失去意义。
    const std::size_t colorOffset = 5U * 4U;
    ASSERT_NE(bgra[colorOffset], bgra[colorOffset + 1U]);
    ASSERT_NE(bgra[colorOffset + 1U], bgra[colorOffset + 2U]);
    ASSERT_NE(bgra[colorOffset], bgra[colorOffset + 2U]);
    const open_st::SdrImageView image{WIDTH, HEIGHT, WIDTH * 4U, bgra};

    std::vector<std::uint8_t> dib;
    ASSERT_TRUE(open_st::BuildClipboardDib(image, dib, error)) << error;
    ASSERT_EQ(dib.size(), sizeof(BITMAPINFOHEADER) + bgra.size());
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib.data(), sizeof(header));
    ASSERT_EQ(header.biSize, sizeof(header));
    ASSERT_EQ(header.biWidth, static_cast<LONG>(WIDTH));
    ASSERT_EQ(header.biHeight, static_cast<LONG>(HEIGHT));
    ASSERT_EQ(header.biPlanes, 1U);
    ASSERT_EQ(header.biBitCount, 32U);
    ASSERT_EQ(header.biCompression, BI_RGB);
    ASSERT_EQ(header.biSizeImage, static_cast<DWORD>(bgra.size()));

    std::vector<std::uint8_t> png;
    ASSERT_TRUE(open_st::EncodeSdrImage(image, {open_st::ImageFileFormat::Png, 95}, png, error)) << error;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ASSERT_TRUE(
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
    ASSERT_TRUE(SUCCEEDED(factory->CreateStream(&stream)));
    ASSERT_TRUE(SUCCEEDED(stream->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size()))));
    ASSERT_TRUE(
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
    ASSERT_TRUE(SUCCEEDED(decoder->GetFrame(0U, &frame)));
    UINT width{}, height{};
    ASSERT_TRUE(SUCCEEDED(frame->GetSize(&width, &height)));
    ASSERT_EQ(width, WIDTH);
    ASSERT_EQ(height, HEIGHT);
    ASSERT_TRUE(SUCCEEDED(factory->CreateFormatConverter(&converter)));
    ASSERT_TRUE(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                nullptr, 0.0, WICBitmapPaletteTypeCustom)));
    std::vector<std::uint8_t> decoded(bgra.size());
    ASSERT_TRUE(
        SUCCEEDED(converter->CopyPixels(nullptr, WIDTH * 4U, static_cast<UINT>(decoded.size()), decoded.data())));

    for (std::uint32_t y = 0U; y < HEIGHT; ++y)
    {
        for (std::uint32_t x = 0U; x < WIDTH; ++x)
        {
            const std::size_t sourceOffset = (static_cast<std::size_t>(y) * WIDTH + x) * 4U;
            const std::size_t dibOffset = sizeof(header) + (static_cast<std::size_t>(HEIGHT - 1U - y) * WIDTH + x) * 4U;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                SCOPED_TRACE(testing::Message() << "x=" << x << " y=" << y << " BGR channel=" << channel);
                EXPECT_EQ(dib[dibOffset + channel], bgra[sourceOffset + channel]);
                EXPECT_EQ(decoded[sourceOffset + channel], bgra[sourceOffset + channel]);
                EXPECT_EQ(dib[dibOffset + channel], decoded[sourceOffset + channel]);
            }
        }
    }
}
