// 验证 SDR 图像编码、导出参数校验及模拟系统边界下的写入与清理行为。

#include "export_system_fake.h"
#include "image_encoder.h"
#include <array>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <wincodec.h>
#include <wrl/client.h>

namespace open_st
{
namespace
{
using testing::ExportSystemFake;
constexpr std::array<std::uint8_t, 20> PIXELS{10, 20, 30, 0,  40, 50,  60,  127, 99,  99,
                                              99, 99, 70, 80, 90, 255, 100, 110, 120, 3};
// 构造最后一行不含尾部 padding 的两行 BGRX 输入。
// 入参：无显式入参。
// 返回：借用静态 PIXELS 的 2×2 BGRX 视图，行距为 12 字节。
SdrImageView Image()
{
    return {2U, 2U, 12U, PIXELS};
}
// 统计边界调用以检验释放、关闭与有限重试。
// 入参：fake 为保存系统调用轨迹的导出替身；operation 为要统计的操作名称。
// 返回：fake.calls 中 operation 的出现次数。
std::size_t Count(const ExportSystemFake& fake, const std::string& operation)
{
    return static_cast<std::size_t>(std::count(fake.calls.begin(), fake.calls.end(), operation));
}

// 验证 padding 不计入像素、行序翻转和 X 字节不进入 CF_DIB 透明度语义。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ClipboardTest, bottom_up_opaque_dib)
{
    std::vector<std::uint8_t> dib;
    std::wstring error;
    ASSERT_TRUE(BuildClipboardDib(Image(), dib, error));
    ASSERT_EQ(dib.size(), sizeof(BITMAPINFOHEADER) + 16U);
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib.data(), sizeof(header));
    EXPECT_EQ(header.biWidth, 2);
    EXPECT_EQ(header.biHeight, 2);
    EXPECT_EQ(header.biBitCount, 32U);
    EXPECT_EQ(header.biCompression, BI_RGB);
    const std::vector<std::uint8_t> expected{70, 80, 90, 0, 100, 110, 120, 0, 10, 20, 30, 0, 40, 50, 60, 0};
    EXPECT_EQ(std::vector<std::uint8_t>(dib.begin() + sizeof(header), dib.end()), expected);
}

// 验证非法尺寸、短跨度、短缓冲区和整数上界均在访问像素前拒绝，并保留调用方输出。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ImageValidationTest, rejects_invalid_layout_without_changing_output)
{
    const std::array<SdrImageView, 5> invalid{{{0, 2, 12, PIXELS},
                                               {2, 2, 7, PIXELS},
                                               {2, 3, 12, PIXELS},
                                               {(std::numeric_limits<std::uint32_t>::max)(), 1, 12, PIXELS},
                                               {2, 2, (std::numeric_limits<std::size_t>::max)(), PIXELS}}};
    for (const SdrImageView& image : invalid)
    {
        std::vector<std::uint8_t> output{42};
        std::wstring error;
        EXPECT_FALSE(BuildClipboardDib(image, output, error));
        EXPECT_EQ(output, (std::vector<std::uint8_t>{42}));
        EXPECT_FALSE(error.empty());
    }
}

// 验证发布成功后系统接管内存，且 FALSE/ERROR_SUCCESS 解锁不被误判。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ClipboardTest, success_transfers_memory_and_closes_session)
{
    ExportSystemFake fake;
    std::wstring error;
    EXPECT_TRUE(CopyImageWithApi(ExportSystemFake::Owner(), Image(), fake.Clipboard(), error));
    EXPECT_EQ(fake.publishedFormat, CF_DIB);
    std::vector<std::uint8_t> expected;
    ASSERT_TRUE(BuildClipboardDib(Image(), expected, error));
    EXPECT_EQ(fake.memory, expected);
    EXPECT_EQ(Count(fake, "free"), 0U);
    EXPECT_EQ(Count(fake, "clipboardClose"), 1U);
    EXPECT_EQ(fake.calls, (std::vector<std::string>{"owner", "allocate", "lock", "unlock", "open", "empty", "set",
                                                    "clipboardClose"}));
}

// 验证分别注入每个发布阶段的故障，验证只释放仍属于本地的内存和已打开的会话。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ClipboardTest, failures_release_only_owned_resources)
{
    const std::array<std::string, 7> stages{"owner", "allocate", "lock", "unlock", "open", "empty", "set"};
    for (const std::string& stage : stages)
    {
        SCOPED_TRACE(stage);
        ExportSystemFake fake;
        fake.failure = stage;
        std::wstring error;
        EXPECT_FALSE(CopyImageWithApi(ExportSystemFake::Owner(), Image(), fake.Clipboard(), error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(Count(fake, "free"), stage == "owner" || stage == "allocate" ? 0U : 1U);
        EXPECT_EQ(Count(fake, "clipboardClose"), stage == "empty" || stage == "set" ? 1U : 0U);
        EXPECT_LE(Count(fake, "open"), 1U);
        if (stage != "empty" && stage != "set")
        {
            EXPECT_EQ(Count(fake, "empty"), 0U);
        }
    }
}

// 验证无窗口或无效像素直接失败，不分配或打开剪贴板。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ClipboardTest, invalid_input_does_not_touch_clipboard)
{
    ExportSystemFake fake;
    std::wstring error;
    EXPECT_FALSE(CopyImageWithApi(nullptr, Image(), fake.Clipboard(), error));
    EXPECT_FALSE(CopyImageWithApi(ExportSystemFake::Owner(), {}, fake.Clipboard(), error));
    EXPECT_EQ(Count(fake, "allocate"), 0U);
    EXPECT_EQ(Count(fake, "open"), 0U);
}

// 验证短写继续推进偏移，完整交付后只刷新和关闭一次。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(FileWriterTest, short_writes_complete_in_order)
{
    ExportSystemFake fake;
    fake.writeLimit = 3;
    std::wstring error;
    ASSERT_TRUE(WriteEncodedFile(L"fake.png", PIXELS, fake.File(), error));
    EXPECT_EQ(fake.writtenBytes, (std::vector<std::uint8_t>(PIXELS.begin(), PIXELS.end())));
    EXPECT_EQ(Count(fake, "write"), 7U);
    EXPECT_EQ(Count(fake, "flush"), 1U);
    EXPECT_EQ(Count(fake, "fileClose"), 1U);
    EXPECT_FALSE(fake.deleteRequested);
    EXPECT_EQ(fake.dispositions, (std::vector<DWORD>{CREATE_NEW}));
}

// 验证新建与覆盖目标在写入及刷新失败时采用不同清理策略。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(FileWriterTest, failure_deletes_only_newly_created_file)
{
    for (const bool existing : {false, true})
    {
        for (const std::string stage : {"write", "flush"})
        {
            ExportSystemFake fake;
            fake.existing = existing;
            fake.failure = stage;
            std::wstring error;
            EXPECT_FALSE(WriteEncodedFile(L"fake.jpg", PIXELS, fake.File(), error));
            EXPECT_EQ(fake.deleteRequested, !existing);
            EXPECT_EQ(Count(fake, "fileClose"), 1U);
            EXPECT_FALSE(error.empty());
            if (existing)
            {
                EXPECT_EQ(fake.dispositions, (std::vector<DWORD>{CREATE_NEW, TRUNCATE_EXISTING}));
            }
        }
    }
}

// 验证零写不会无限循环，且清理失败保留诊断信息。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(FileWriterTest, zero_write_and_cleanup_failure_are_reported)
{
    ExportSystemFake fake;
    fake.writeLimit = 0;
    fake.cleanupFails = true;
    std::wstring error;
    EXPECT_FALSE(WriteEncodedFile(L"fake.png", PIXELS, fake.File(), error));
    EXPECT_EQ(Count(fake, "write"), 1U);
    EXPECT_EQ(Count(fake, "fileClose"), 1U);
    EXPECT_NE(error.find(L"cleanup failed"), std::wstring::npos);
}

// 验证空参数不创建文件，创建拒绝不尝试覆盖或关闭无效句柄。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(FileWriterTest, rejects_empty_input_and_create_failure)
{
    ExportSystemFake fake;
    std::wstring error;
    EXPECT_FALSE(WriteEncodedFile({}, PIXELS, fake.File(), error));
    EXPECT_FALSE(WriteEncodedFile(L"fake.png", {}, fake.File(), error));
    EXPECT_TRUE(fake.calls.empty());
    fake.failure = "create";
    EXPECT_FALSE(WriteEncodedFile(L"fake.png", PIXELS, fake.File(), error));
    EXPECT_EQ(Count(fake, "create"), 1U);
    EXPECT_EQ(Count(fake, "fileClose"), 0U);
}

// COM 只在当前测试线程初始化；编解码使用内存流，不读写系统边界。
class ImageEncoderTest : public ::testing::Test
{
  protected:
    // 为 WIC 创建当前线程 COM 环境。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        this->result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->result_));
    }
    // 配对释放初始化计数。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        if (SUCCEEDED(this->result_))
        {
            CoUninitialize();
        }
    }
    HRESULT result_{E_FAIL};
};

// 验证 PNG 无损 RGB 往返、JPEG 可解码尺寸，以及两种格式的 sRGB 元数据。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(ImageEncoderTest, round_trip_rgb_and_srgb_metadata)
{
    using Microsoft::WRL::ComPtr;
    for (const ImageFileFormat format : {ImageFileFormat::Png, ImageFileFormat::Jpeg})
    {
        std::wstring error;
        std::vector<std::uint8_t> encoded;
        ASSERT_TRUE(EncodeSdrImage(Image(), format, encoded, error));
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ASSERT_TRUE(SUCCEEDED(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
        ASSERT_TRUE(SUCCEEDED(factory->CreateStream(&stream)));
        ASSERT_TRUE(SUCCEEDED(stream->InitializeFromMemory(encoded.data(), static_cast<DWORD>(encoded.size()))));
        ASSERT_TRUE(
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
        ASSERT_TRUE(SUCCEEDED(decoder->GetFrame(0, &frame)));
        UINT width{}, height{};
        ASSERT_TRUE(SUCCEEDED(frame->GetSize(&width, &height)));
        EXPECT_EQ(width, 2U);
        EXPECT_EQ(height, 2U);
        ComPtr<IWICMetadataQueryReader> metadata;
        ASSERT_TRUE(SUCCEEDED(frame->GetMetadataQueryReader(&metadata)));
        PROPVARIANT color{};
        ASSERT_TRUE(SUCCEEDED(metadata->GetMetadataByName(
            format == ImageFileFormat::Png ? L"/sRGB/RenderingIntent" : L"/app1/ifd/exif/{ushort=40961}", &color)));
        EXPECT_EQ(color.vt, format == ImageFileFormat::Png ? VT_UI1 : VT_UI2);
        EXPECT_EQ(format == ImageFileFormat::Png ? color.bVal : color.uiVal, format == ImageFileFormat::Png ? 0 : 1);
        (void)PropVariantClear(&color);
        ASSERT_TRUE(SUCCEEDED(factory->CreateFormatConverter(&converter)));
        ASSERT_TRUE(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                    nullptr, 0.0, WICBitmapPaletteTypeCustom)));
        std::array<std::uint8_t, 16> decoded{};
        ASSERT_TRUE(SUCCEEDED(converter->CopyPixels(nullptr, 8U, static_cast<UINT>(decoded.size()), decoded.data())));
        for (std::size_t pixel = 0; pixel < 4; ++pixel)
        {
            EXPECT_EQ(decoded[pixel * 4 + 3], 255U);
            if (format == ImageFileFormat::Png)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    EXPECT_EQ(decoded[pixel * 4 + channel], PIXELS[(pixel / 2) * 12 + (pixel % 2) * 4 + channel]);
                }
            }
        }
    }
}

// 验证未知格式在编码前拒绝并保持上次输出，不返回伪成功。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(ImageEncoderTest, invalid_format_preserves_output)
{
    std::vector<std::uint8_t> encoded{42};
    std::wstring error;
    EXPECT_FALSE(EncodeSdrImage(Image(), static_cast<ImageFileFormat>(99), encoded, error));
    EXPECT_EQ(encoded, (std::vector<std::uint8_t>{42}));
    EXPECT_FALSE(error.empty());
}

// 验证真实编码失败后完全不进入文件 API，已有目标不会被截断。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(ImageEncoderTest, encoding_failure_never_opens_destination)
{
    ExportSystemFake fake;
    fake.existing = true;
    std::wstring error;
    EXPECT_FALSE(WriteImageWithApi({}, L"fake.png", ImageFileFormat::Png, fake.File(), error));
    EXPECT_FALSE(WriteImageWithApi(Image(), L"fake.png", static_cast<ImageFileFormat>(99), fake.File(), error));
    EXPECT_TRUE(fake.calls.empty());
}
// 比较旧式整帧临时 DIB 加复制与直接填充，测试替身避免触碰系统剪贴板。
// 入参：仅 Release 且 OPEN_ST_EXPORT_BENCHMARKS=1 时执行；固定 4K 像素和九次采样。
// 返回：输出中位毫秒和明确省去的临时字节数；不设置机器相关的耗时断言。
TEST(ClipboardBenchmark, compares_4k_preparation)
{
#if !defined(NDEBUG)
    GTEST_SKIP() << "Release benchmark only";
#else
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"OPEN_ST_EXPORT_BENCHMARKS", enabled, 2) != 1 || enabled[0] != L'1')
        GTEST_SKIP() << "Set OPEN_ST_EXPORT_BENCHMARKS=1 to measure";
    const std::vector<std::uint8_t> pixels(3840U * 2160U * 4U, 37);
    const SdrImageView image{3840U, 2160U, 3840U * 4U, pixels};
    std::array<double, 9> before{};
    std::array<double, 9> after{};
    for (std::size_t sample = 0; sample < before.size(); ++sample)
    {
        std::wstring error;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> dib;
        ASSERT_TRUE(BuildClipboardDib(image, dib, error));
        std::vector<std::uint8_t> published(dib.size());
        std::memcpy(published.data(), dib.data(), dib.size());
        const std::chrono::steady_clock::time_point copied = std::chrono::steady_clock::now();
        ExportSystemFake fake;
        ASSERT_TRUE(CopyImageWithApi(ExportSystemFake::Owner(), image, fake.Clipboard(), error));
        const std::chrono::steady_clock::time_point direct = std::chrono::steady_clock::now();
        ASSERT_EQ(fake.memory, published);
        before[sample] = std::chrono::duration<double, std::milli>(copied - start).count();
        after[sample] = std::chrono::duration<double, std::milli>(direct - copied).count();
    }
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::cout << "4K DIB preparation: temporary+copy=" << before[4] << "ms direct=" << after[4]
              << "ms temporary_bytes_removed=" << pixels.size() + sizeof(BITMAPINFOHEADER) << '\n';
#endif
}
} // namespace
} // namespace open_st
