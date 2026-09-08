#include <window_renderer.h>

#include <gtest/gtest.h>
#include <windows.h>

#include <array>
#include <string>
#include <string_view>

namespace
{
// 只有明确启用环境开关才等待真人关闭；普通回归测试跳过，不能用定时器冒充人工验收。
TEST(RendererManualTest, waits_for_user_close)
{
    std::array<wchar_t, 4> enabled{};
    const DWORD length =
        GetEnvironmentVariableW(L"OPEN_ST_INTERACTIVE_UI_TESTS", enabled.data(), static_cast<DWORD>(enabled.size()));
    if (length != 1 || enabled[0] != L'1')
    {
        GTEST_SKIP() << "Set OPEN_ST_INTERACTIVE_UI_TESTS=1 to enable the interactive window test.";
    }

    const nlohmann::json layout = nlohmann::json::parse(R"({
        "schemaVersion": 1,
        "window": {
            "titleKey": "title",
            "initialSize": [520, 240],
            "minSize": [360, 200],
            "resizable": true
        },
        "content": {
            "type": "column",
            "id": "body",
            "padding": 20,
            "gap": 12,
            "children": [
                {"type": "text", "id": "instructions", "textKey": "instructions"}
            ]
        },
        "footer": {
            "leading": [],
            "trailing": [{"type": "button", "id": "confirm", "textKey": "confirm"}]
        }
    })");

    open_st::WindowRenderer renderer;
    ASSERT_TRUE(renderer.LoadLayout(layout));
    ASSERT_TRUE(renderer.SetTextResolver(
        [](std::string_view key) -> std::wstring
        {
            if (key == "title")
            {
                return L"WindowRenderer 手动弹窗测试";
            }
            if (key == "instructions")
            {
                return L"请检查窗口文字、布局和缩放。完成后点击“确定”，或按 Esc、点击右上角关闭按钮。"
                       L"窗口会一直等待你的操作，不会自动关闭。";
            }
            if (key == "confirm")
            {
                return L"确定";
            }
            return std::wstring(key.begin(), key.end());
        }));
    bool closeRequested = false;
    ASSERT_TRUE(renderer.BindAction("confirm",
                                    [&renderer, &closeRequested]()
                                    {
                                        closeRequested = true;
                                        EXPECT_TRUE(renderer.RequestClose());
                                    }));
    ASSERT_TRUE(renderer.SetCloseHandler(
        [&renderer, &closeRequested]()
        {
            closeRequested = true;
            EXPECT_TRUE(renderer.RequestClose());
        }));
    ASSERT_TRUE(renderer.SetDefaultAction("confirm"));
    ASSERT_TRUE(renderer.ValidateBindings());

    const open_st::RendererWindowOptions options;
    const open_st::RendererResult result = renderer.ShowModal(options);
    EXPECT_TRUE(result) << result.code << " " << result.path;
    EXPECT_TRUE(closeRequested);
    EXPECT_EQ(renderer.NativeHandle(), nullptr);
}
} // namespace
