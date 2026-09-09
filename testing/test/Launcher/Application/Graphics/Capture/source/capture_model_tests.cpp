// 文件职责：验证虚拟桌面矩形并集及 BGRA 帧的尺寸、行跨度和清理行为。

#include <gtest/gtest.h>

#include <bgra_frame.h>
#include <geometry.h>

#include <array>

// 验证虚拟桌面矩形合并：支持负坐标和跨显示器范围，并忽略宽度为零的空矩形；同时确认合并结果的左右上下边界以及最终宽高均符合半开区间语义。
// 入参：无运行时形参；宏参数 GeometryTest 为测试套件，union_rectangles 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(GeometryTest, union_rectangles)
{
    constexpr std::array rectangles{
        open_st::RectI{-1920, 0, 0, 1080},
        open_st::RectI{0, 0, 2560, 1440},
        open_st::RectI{100, 100, 100, 200},
    };

    const open_st::RectI combined = open_st::UnionRectangles(rectangles);

    EXPECT_EQ(combined.left, -1920);
    EXPECT_EQ(combined.top, 0);
    EXPECT_EQ(combined.right, 2560);
    EXPECT_EQ(combined.bottom, 1440);
    EXPECT_EQ(combined.Width(), 4480);
    EXPECT_EQ(combined.Height(), 1440);
}

// 验证 BGRA 帧的基础内存布局：根据带负坐标的边界正确计算宽、高、stride 和缓冲区大小；同时验证 Clear() 会释放像素缓冲区，并将帧恢复为无效状态。
// 入参：无运行时形参；宏参数 BgraFrameTest 为测试套件，constructs_and_clears 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(BgraFrameTest, constructs_and_clears)
{
    open_st::BgraFrame frame(open_st::RectI{-10, -20, 90, 30});

    ASSERT_TRUE(frame.IsValid());
    EXPECT_EQ(frame.Width(), 100);
    EXPECT_EQ(frame.Height(), 50);
    EXPECT_EQ(frame.Stride(), 400);
    EXPECT_EQ(frame.Pixels().size(), 20000U);

    frame.Clear();

    EXPECT_FALSE(frame.IsValid());
    EXPECT_TRUE(frame.Pixels().empty());
}
