#include <gtest/gtest.h>

#include <desktop_preview.h>
#include <overlay_renderer.h>
#include <selection_model.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <windows.h>

namespace open_st
{
// 友元只让集成测试在 Present 之前读取实际绘制结果，不向应用开放截图覆盖图导出接口。
struct OverlayRendererTestAccess final
{
    // 使用产品绘制路径生成一帧并从同一个未翻转后缓冲回读。
    static bool DrawAndRead(OverlayRenderer& renderer, SelectionSnapshot snapshot,
                            OutputPreviewFrame& readback, std::wstring& errorMessage)
    {
        return renderer.DrawFrame(snapshot, false, errorMessage) && renderer.ReadbackFrame(readback, errorMessage);
    }
};
} // namespace open_st

namespace
{
class OverlayRendererIntegrationTest : public testing::Test
{
  protected:
    // 只有不存在 Windows 显示输出的平台环境才跳过；具备显示环境后的设备或呈现失败必须报错。
    void SetUp() override
    {
        if (GetSystemMetrics(SM_CMONITORS) == 0)
        {
            GTEST_SKIP() << "当前 Windows 会话没有显示输出，无法建立 HWND 交换链。";
        }
    }
};

class HiddenWindow final
{
  public:
    // 创建不显示、不激活的无边框测试窗口，客户区与指定物理像素大小一致。
    HiddenWindow(int width, int height)
        : window_(CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                   0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr))
    {
    }

    // 测试结束后释放隐藏窗口，不对真实截图窗口执行任何操作。
    ~HiddenWindow()
    {
        if (this->window_ != nullptr)
        {
            DestroyWindow(this->window_);
        }
    }

    // 禁止复制窗口所有权。
    HiddenWindow(const HiddenWindow&) = delete;
    // 禁止复制赋值窗口所有权。
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    // 返回借用窗口句柄，供本测试渲染器初始化使用。
    HWND Get() const noexcept
    {
        return this->window_;
    }

  private:
    HWND window_{};
};

// 构造边框在目标之外的全局跨屏选区，让回读只包含未遮罩、未描边的冻结像素。
open_st::SelectionSnapshot UnobstructedSelection() noexcept
{
    open_st::SelectionSnapshot snapshot{};
    snapshot.hasSelection = true;
    snapshot.rectangle = {-100, -100, 100, 100};
    return snapshot;
}
} // namespace

// 验证实际 FP16 上传、D2D 绘制、GPU 回读保留中灰、1.0、超白和负通道，不重复乘 UI 白亮度。
TEST_F(OverlayRendererIntegrationTest, preserves_native_half_values_through_gpu_rendering)
{
    HiddenWindow window(4, 4);
    ASSERT_NE(window.Get(), nullptr);
    open_st::OutputPreviewFrame source{};
    source.bounds = {-4, 0, 0, 4};
    source.pixelFormat = open_st::CapturedPixelFormat::Rgba16FloatScRgb;
    source.stride = 32U;
    source.uiWhiteScale = 3.0F;
    source.pixels.resize(128U);
    const std::array<std::uint16_t, 4> values{0x31C3U, 0x3C00U, 0x4000U, 0xB800U};
    for (std::size_t pixel = 0U; pixel < 16U; ++pixel)
    {
        const std::uint16_t value = values[pixel % values.size()];
        const std::array<std::uint16_t, 4> channels{value, value, value, 0x3C00U};
        std::memcpy(source.pixels.data() + pixel * 8U, channels.data(), 8U);
    }
    open_st::OverlayRenderer renderer;
    std::wstring errorMessage;
    ASSERT_TRUE(renderer.Initialize(window.Get(), source, std::nullopt, errorMessage)) << errorMessage;
    open_st::OutputPreviewFrame readback;
    ASSERT_TRUE(open_st::OverlayRendererTestAccess::DrawAndRead(
        renderer, UnobstructedSelection(), readback, errorMessage)) << errorMessage;
    EXPECT_EQ(readback.pixelFormat, open_st::CapturedPixelFormat::Rgba16FloatScRgb);
    EXPECT_EQ(readback.pixels, source.pixels);
}

// 验证 SDR BGRA8 不改码值，且跨屏选区不会在本屏左右边界错误生成选区边框。
TEST_F(OverlayRendererIntegrationTest, preserves_sdr_bytes_and_has_no_internal_monitor_border)
{
    HiddenWindow window(8, 8);
    ASSERT_NE(window.Get(), nullptr);
    open_st::OutputPreviewFrame source{};
    source.bounds = {-8, 0, 0, 8};
    source.pixelFormat = open_st::CapturedPixelFormat::Bgra8Unorm;
    source.stride = 32U;
    source.pixels.resize(256U);
    for (std::size_t pixel = 0U; pixel < 64U; ++pixel)
    {
        source.pixels[pixel * 4U] = 35U;
        source.pixels[pixel * 4U + 1U] = 126U;
        source.pixels[pixel * 4U + 2U] = 230U;
        source.pixels[pixel * 4U + 3U] = 255U;
    }
    open_st::OverlayRenderer renderer;
    std::wstring errorMessage;
    ASSERT_TRUE(renderer.Initialize(window.Get(), source, std::nullopt, errorMessage)) << errorMessage;
    open_st::OutputPreviewFrame readback;
    ASSERT_TRUE(open_st::OverlayRendererTestAccess::DrawAndRead(
        renderer, UnobstructedSelection(), readback, errorMessage)) << errorMessage;
    EXPECT_EQ(readback.pixelFormat, open_st::CapturedPixelFormat::Bgra8Unorm);
    EXPECT_EQ(readback.pixels, source.pixels);
}

// 验证 HDR 控制点白只应用 UI 白亮度比例，实际线性 FP16 画刷可以输出大于 1.0 的值。
TEST_F(OverlayRendererIntegrationTest, scales_hdr_handle_white_without_clamping)
{
    HiddenWindow window(16, 16);
    ASSERT_NE(window.Get(), nullptr);
    open_st::OutputPreviewFrame source{};
    source.bounds = {0, 0, 16, 16};
    source.pixelFormat = open_st::CapturedPixelFormat::Rgba16FloatScRgb;
    source.stride = 128U;
    source.uiWhiteScale = 3.0F;
    source.pixels.resize(2048U);
    open_st::SelectionSnapshot snapshot = UnobstructedSelection();
    snapshot.showHandles = true;
    for (open_st::SelectionHandlePosition& handle : snapshot.handles)
    {
        handle.center = {8, 8};
    }
    open_st::OverlayRenderer renderer;
    std::wstring errorMessage;
    ASSERT_TRUE(renderer.Initialize(window.Get(), source, std::nullopt, errorMessage)) << errorMessage;
    open_st::OutputPreviewFrame readback;
    ASSERT_TRUE(open_st::OverlayRendererTestAccess::DrawAndRead(
        renderer, snapshot, readback, errorMessage)) << errorMessage;
    std::array<std::uint16_t, 4> channels{};
    std::memcpy(channels.data(), readback.pixels.data() + 8U * readback.stride + 8U * 8U, 8U);
    EXPECT_EQ(channels[0], 0x4200U);
    EXPECT_EQ(channels[1], 0x4200U);
    EXPECT_EQ(channels[2], 0x4200U);
    EXPECT_EQ(channels[3], 0x3C00U);
}

// 验证交换链实际显示器与捕获身份不一致时初始化明确失败，不在错误的显示输出上继续呈现。
TEST_F(OverlayRendererIntegrationTest, rejects_stale_output_identity)
{
    HiddenWindow window(4, 4);
    ASSERT_NE(window.Get(), nullptr);
    open_st::OutputPreviewFrame source{};
    source.bounds = {0, 0, 4, 4};
    source.stride = 16U;
    source.pixels.resize(64U);
    source.colorMetadata.deviceName[0] = L'!';
    open_st::OverlayRenderer renderer;
    std::wstring errorMessage;
    EXPECT_FALSE(renderer.Initialize(window.Get(), source, std::nullopt, errorMessage));
    EXPECT_NE(errorMessage.find(L"显示输出与冻结帧不一致"), std::wstring::npos);
    source.colorMetadata = {};
    EXPECT_TRUE(renderer.Initialize(window.Get(), source, std::nullopt, errorMessage)) << errorMessage;
}

// 验证调用方注入的 RGB 用于实际边框，且初始化返回后不再借用配置字符串。
TEST_F(OverlayRendererIntegrationTest, uses_injected_border_color_without_retaining_string)
{
    HiddenWindow window(32, 32);
    ASSERT_NE(window.Get(), nullptr);
    open_st::OutputPreviewFrame source{};
    source.bounds = {0, 0, 32, 32};
    source.pixelFormat = open_st::CapturedPixelFormat::Bgra8Unorm;
    source.stride = 128U;
    source.pixels.resize(4096U);
    open_st::SelectionSnapshot snapshot{};
    snapshot.hasSelection = true;
    snapshot.rectangle = {8, 8, 24, 24};
    open_st::OverlayRenderer renderer;
    std::wstring errorMessage;
    std::string configuredColor = "#12ABCF";
    ASSERT_TRUE(renderer.Initialize(window.Get(), source, std::string_view(configuredColor), errorMessage));
    configuredColor.assign("#000000");
    open_st::OutputPreviewFrame readback;
    ASSERT_TRUE(open_st::OverlayRendererTestAccess::DrawAndRead(
        renderer, snapshot, readback, errorMessage)) << errorMessage;
    bool foundConfiguredColor = false;
    for (std::size_t offset = 0U; offset < readback.pixels.size(); offset += 4U)
    {
        if (readback.pixels[offset] == 0xCFU && readback.pixels[offset + 1U] == 0xABU &&
            readback.pixels[offset + 2U] == 0x12U)
        {
            foundConfiguredColor = true;
            break;
        }
    }
    EXPECT_TRUE(foundConfiguredColor);
}
