// 文件职责：验证贴图图像的独立所有权、BGRX 字节契约和尺寸跨度边界。

#include <gtest/gtest.h>
#include <limits>
#include <pin_image.h>
#include <vector>

namespace
{
// 验证行填充被移除，末行不要求填充且原始第四字节不被修改。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言检查紧密像素、尺寸和清空的错误信息。
TEST(PinImageTest, copies_rows_without_padding_and_preserves_bgrx)
{
    std::vector<std::uint8_t> pixels{1, 2, 3, 0, 9, 9, 9, 9, 4, 5, 6, 17};
    std::wstring error = L"previous";
    const std::shared_ptr<const open_st::PinImage> image = open_st::PinImage::Create(1, 2, 8, pixels, error);
    ASSERT_NE(image, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(image->Width(), 1);
    EXPECT_EQ(image->Height(), 2);
    EXPECT_EQ(image->Stride(), 4U);
    EXPECT_EQ(image->Pixels().size(), 8U);
    EXPECT_EQ(image->Pixels()[3], 0);
    EXPECT_EQ(image->Pixels()[4], 4);
    EXPECT_EQ(image->Pixels()[7], 17);
    pixels.assign(pixels.size(), 255);
    EXPECT_EQ(image->Pixels()[0], 1);
    EXPECT_EQ(image->Pixels()[7], 17);
}

// 验证负数、零及超出 GPU 上限的尺寸均不会创建图像。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；每种非法尺寸返回空指针并提供诊断。
TEST(PinImageTest, rejects_invalid_dimensions)
{
    const std::vector<std::uint8_t> pixels(16, 0);
    std::wstring error;
    EXPECT_EQ(open_st::PinImage::Create(0, 1, 4, pixels, error), nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(open_st::PinImage::Create(1, -1, 4, pixels, error), nullptr);
    EXPECT_EQ(open_st::PinImage::Create(16385, 1, 4, pixels, error), nullptr);
    EXPECT_EQ(open_st::PinImage::Create(1, 16385, 4, pixels, error), nullptr);
}

// 验证不足行跨度和不足末行缓冲区被拒绝。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言确认不会读取输入范围外的像素。
TEST(PinImageTest, rejects_short_stride_and_incomplete_last_row)
{
    const std::vector<std::uint8_t> pixels(15, 0);
    std::wstring error;
    EXPECT_EQ(open_st::PinImage::Create(2, 1, 7, pixels, error), nullptr);
    EXPECT_EQ(open_st::PinImage::Create(2, 2, 8, pixels, error), nullptr);
    EXPECT_FALSE(error.empty());
}

// 验证极大输入跨度不会在计算末行地址时整数溢出。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；溢出跨度必须返回空指针。
TEST(PinImageTest, rejects_stride_overflow)
{
    const std::vector<std::uint8_t> pixels(16, 0);
    std::wstring error;
    EXPECT_EQ(open_st::PinImage::Create(1, 3, (std::numeric_limits<std::size_t>::max)(), pixels, error), nullptr);
    EXPECT_FALSE(error.empty());
}

// 验证图像在输入缓冲区和创建时局部所有者销毁后仍可使用。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；共享快照保留已复制的原图。
TEST(PinImageTest, snapshot_outlives_source_buffer)
{
    std::shared_ptr<const open_st::PinImage> snapshot;
    {
        const std::vector<std::uint8_t> pixels{1, 2, 3, 4};
        std::wstring error;
        snapshot = open_st::PinImage::Create(1, 1, 4, pixels, error);
    }
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->Pixels()[2], 3);
    EXPECT_EQ(snapshot->Pixels()[3], 4);
}
} // namespace
