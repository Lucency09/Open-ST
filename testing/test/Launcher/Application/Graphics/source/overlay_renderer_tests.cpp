// 文件职责：以隐藏窗口验证遮罩渲染器初始化、资源释放及选区内外的实际像素结果。

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
    // 使用正式渲染路径绘制选区，再在交换链翻转前读取像素以便测试比对。
    // 入参：renderer：已初始化的渲染器；snapshot：物理像素选区快照；readback：输出参数，接收后缓冲预览；errorMessage：输出参数，接收绘制或回读失败原因。
    // 返回：绘制和回读均成功时为 true；任一步失败时为 false，并保留对应诊断。
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
    // 在每个覆盖绘制用例运行前确认当前 Windows 会话具备显示输出。
    // 入参：无。
    // 返回：无返回值；没有显示输出时标记跳过，有输出时继续运行实际绘制检查。
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
    // 创建供 GPU 绘制与回读测试使用的隐藏无边框窗口。
    // 入参：width、height：客户区宽高，单位物理像素。
    // 返回：无返回值；窗口创建失败时 Get() 返回 nullptr。
    HiddenWindow(int width, int height)
        : window_(CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                   0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr))
    {
    }

    // 销毁本用例创建的隐藏窗口并归还窗口资源。
    // 入参：无。
    // 返回：无返回值；析构完成对应资源清理。
    ~HiddenWindow()
    {
        if (this->window_ != nullptr)
        {
            DestroyWindow(this->window_);
        }
    }

    // 禁止复制窗口所有权。
    // 入参：未命名 const HiddenWindow 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    HiddenWindow(const HiddenWindow&) = delete;
    // 禁止复制赋值窗口所有权。
    // 入参：未命名 const HiddenWindow 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    // 向测试渲染器提供隐藏窗口句柄，不转移窗口所有权。
    // 入参：无。
    // 返回：当前隐藏窗口的借用 HWND；窗口创建失败时为 nullptr。
    HWND Get() const noexcept
    {
        return this->window_;
    }

  private:
    HWND window_{};
};

// 生成边框位于测试窗口外的跨屏选区，使像素回读不受暗层和描边影响。
// 入参：无。
// 返回：覆盖测试窗口且不显示控制点的选区快照，矩形为 -100 至 100 的物理像素范围。
open_st::SelectionSnapshot UnobstructedSelection() noexcept
{
    open_st::SelectionSnapshot snapshot{};
    snapshot.hasSelection = true;
    snapshot.rectangle = {-100, -100, 100, 100};
    return snapshot;
}
} // namespace

// 验证实际 FP16 上传、D2D 绘制、GPU 回读保留中灰、1.0、超白和负通道，不重复乘 UI 白亮度。
// 入参：无运行时形参；宏参数 OverlayRendererIntegrationTest 为测试套件，preserves_native_half_values_through_gpu_rendering 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
// 入参：无运行时形参；宏参数 OverlayRendererIntegrationTest 为测试套件，preserves_sdr_bytes_and_has_no_internal_monitor_border
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
// 入参：无运行时形参；宏参数 OverlayRendererIntegrationTest 为测试套件，scales_hdr_handle_white_without_clamping 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
// 入参：无运行时形参；宏参数 OverlayRendererIntegrationTest 为测试套件，rejects_stale_output_identity 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
// 入参：无运行时形参；宏参数 OverlayRendererIntegrationTest 为测试套件，uses_injected_border_color_without_retaining_string 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
