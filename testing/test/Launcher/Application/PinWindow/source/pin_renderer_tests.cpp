// 文件职责：验证贴图渲染器的输入拒绝、隐藏首帧、缩放透明度及资源回收契约。

#include "pin_renderer.h"
#include <dwmapi.h>
#include <gtest/gtest.h>
#include <limits>
#include <objbase.h>
#include <pin_image.h>
#include <vector>

namespace
{
// 验证未初始化对象拒绝渲染且多次回收不会破坏后续输入校验。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；无效操作均提供诊断。
TEST(PinRendererInputTest, rejects_uninitialized_render_and_invalid_window)
{
    open_st::PinRenderer renderer;
    std::wstring error;
    EXPECT_FALSE(renderer.Render(10, 10, 1.0F, error));
    EXPECT_FALSE(error.empty());
    renderer.Reset();
    renderer.Reset();
    EXPECT_FALSE(renderer.Initialize(nullptr, {}, error));
    EXPECT_FALSE(error.empty());
}

class PinRendererTest : public testing::Test
{
  protected:
    // 创建不显示的真实窗口并初始化 COM，避免测试获取桌面焦点。
    // 入参：无。
    // 返回：无返回值；缺少桌面合成时显式跳过 GPU 集成测试。
    void SetUp() override
    {
        BOOL enabled = FALSE;
        if (FAILED(DwmIsCompositionEnabled(&enabled)) || !enabled)
        {
            GTEST_SKIP() << "Desktop composition is unavailable for PinRenderer integration tests.";
        }
        this->com_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->com_) || this->com_ == RPC_E_CHANGED_MODE);
        this->window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, L"STATIC", L"PinRendererTest",
                                        WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->window_, nullptr);
        const std::vector<std::uint8_t> pixels(16U * 16U * 4U, 0);
        std::wstring error;
        this->image_ = open_st::PinImage::Create(16, 16, 64, pixels, error);
        ASSERT_NE(this->image_, nullptr);
        this->renderer_ = std::make_unique<open_st::PinRenderer>();
    }
    // 按渲染器、窗口、COM 的顺序回收测试资源。
    // 入参：无。
    // 返回：无返回值；也覆盖初始化中途断言失败的清理路径。
    void TearDown() override
    {
        this->renderer_.reset();
        this->image_.reset();
        if (this->window_)
            DestroyWindow(this->window_);
        if (SUCCEEDED(this->com_))
            CoUninitialize();
    }
    HRESULT com_{E_FAIL};
    HWND window_{};
    std::shared_ptr<const open_st::PinImage> image_;
    std::unique_ptr<open_st::PinRenderer> renderer_;
};

// 验证隐藏首帧和后续尺寸透明度调整成功，上传显示副本不修改 BGRX 原图。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；窗口保持隐藏且原图第四字节始终为零。
TEST_F(PinRendererTest, renders_hidden_frame_then_resizes_without_mutating_image)
{
    std::wstring error;
    ASSERT_TRUE(this->renderer_->Initialize(this->window_, this->image_, error)) << error;
    EXPECT_FALSE(IsWindowVisible(this->window_));
    EXPECT_TRUE(this->renderer_->Render(32, 24, 0.35F, error)) << error;
    EXPECT_TRUE(this->renderer_->Render(16, 16, 1.0F, error)) << error;
    EXPECT_EQ(this->image_->Pixels()[3], 0);
    EXPECT_FALSE(IsWindowVisible(this->window_));
}

// 验证无效尺寸和非有限透明度不影响随后有效绘制。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；非法输入失败后仍可提交正常帧。
TEST_F(PinRendererTest, rejects_invalid_geometry_and_opacity_without_losing_resources)
{
    std::wstring error;
    ASSERT_TRUE(this->renderer_->Initialize(this->window_, this->image_, error));
    EXPECT_FALSE(this->renderer_->Render(0, 16, 1.0F, error));
    EXPECT_FALSE(this->renderer_->Render(16385, 16, 1.0F, error));
    EXPECT_FALSE(this->renderer_->Render(16, 16, -0.1F, error));
    EXPECT_FALSE(this->renderer_->Render(16, 16, 1.1F, error));
    EXPECT_FALSE(this->renderer_->Render(16, 16, std::numeric_limits<float>::quiet_NaN(), error));
    EXPECT_FALSE(this->renderer_->Render(16, 16, std::numeric_limits<float>::infinity(), error));
    EXPECT_TRUE(this->renderer_->Render(16, 16, 0.5F, error));
}

// 验证回收释放图像所有权并允许同一窗口重新建立独立合成树。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；回收后渲染被拒绝，重新初始化恢复正常。
TEST_F(PinRendererTest, reset_releases_snapshot_and_allows_reinitialize)
{
    std::wstring error;
    ASSERT_TRUE(this->renderer_->Initialize(this->window_, this->image_, error));
    std::weak_ptr<const open_st::PinImage> weak = this->image_;
    this->image_.reset();
    EXPECT_FALSE(weak.expired());
    this->renderer_->Reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(this->renderer_->Render(16, 16, 1.0F, error));
    const std::vector<std::uint8_t> pixels(4, 255);
    this->image_ = open_st::PinImage::Create(1, 1, 4, pixels, error);
    EXPECT_TRUE(this->renderer_->Initialize(this->window_, this->image_, error));
}
} // namespace
