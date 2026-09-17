// 验证通用数值色块、单字段刷新与原生置顶窗口，不读取业务样式或用户配置。

#include "renderer_model.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace
{
using namespace open_st;

// 创建包含只读色样、可点击色块和两类编辑框的独立通用布局。
// 入参：无。
// 返回：不含业务键或颜色字符串的布局 JSON。
nlohmann::json ColorDocument()
{
    return nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"title"},
      "content":{"type":"column","id":"content","children":[
        {"type":"row","id":"colors","children":[
          {"type":"swatch","id":"sample","textKey":"sample","width":64},
          {"type":"swatch","id":"pick","textKey":"pick","width":64}]},
        {"type":"edit","id":"draft","labelKey":"draft"},
        {"type":"integer","id":"number","labelKey":"number","minimum":0,"maximum":100}
      ]},"footer":{"trailing":[{"type":"button","id":"close","textKey":"close"}]}
    })");
}

struct ChildSearch
{
    std::wstring_view name;
    HWND window{};
    std::vector<HWND> edits;
};

// 根据可访问名称找控件，并按原生创建顺序记录编辑框供刷新测试使用。
// 入参：window 为当前子窗口；parameter 为测试局部查询状态。
// 返回：继续枚举所有子窗口。
BOOL CALLBACK SearchChild(HWND window, LPARAM parameter)
{
    ChildSearch& search = *reinterpret_cast<ChildSearch*>(parameter);
    wchar_t text[256]{};
    wchar_t type[32]{};
    GetWindowTextW(window, text, 256);
    GetClassNameW(window, type, 32);
    if (search.name == text)
        search.window = window;
    if (std::wstring_view(type) == L"Edit")
        search.edits.push_back(window);
    return TRUE;
}

class RendererColorTest : public testing::Test
{
  protected:
    // 注册通用读取、动作及关闭回调，不绑定任何业务模块。
    // 入参：无。
    // 返回：无；绑定失败终止用例。
    void SetUp() override
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(ColorDocument()));
        ASSERT_TRUE(this->renderer_.SetTextResolver(
            // 将通用测试键作为可访问名称。
            // 入参：key 为布局文本键。
            // 返回：测试名称。
            [](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        ASSERT_TRUE(this->renderer_.BindColor("sample",
                                              // 读取独立数值颜色，并按开关注入宿主失败或异常。
                                              // 入参：无。
                                              // 返回：当前测试颜色及宿主错误。
                                              [this]()
                                              {
                                                  ++this->colorReads_;
                                                  if (this->throwRead_)
                                                      throw std::runtime_error("color read failed");
                                                  return this->color_;
                                              }));
        ASSERT_TRUE(this->renderer_.BindColor(
            "pick",
            // 提供固定可点击色块的数值 RGB。
            // 入参：无。
            // 返回：独立绿色色样。
            []() { return RendererColorResult{true, 0x00AA22, {}}; },
            // 累计原生按钮触发次数。
            // 入参：无。
            // 返回：无。
            [this]() { ++this->actions_; }));
        ASSERT_TRUE(this->renderer_.BindString(
            "draft",
            // 返回已接受的宿主草稿，统计是否被无关刷新误读。
            // 入参：无。
            // 返回：固定已接受文本。
            [this]()
            {
                ++this->textReads_;
                return RendererStringResult{true, "saved", {}};
            },
            // 保留被测试的未完成字符串。
            // 入参：未命名参数为编辑原始文本。
            // 返回：拒绝提交，保持原始输入供用户继续编辑。
            [](std::string_view) { return RendererChangeResult{false, L"unfinished text"}; }));
        ASSERT_TRUE(this->renderer_.BindInteger(
            "number",
            // 返回已接受整数并记录读取次数。
            // 入参：无。
            // 返回：固定合法整数。
            [this]()
            {
                ++this->integerReads_;
                return RendererIntegerResult{true, 50, {}};
            },
            // 保留未完成数值输入，不将其改写为旧值。
            // 入参：未命名参数为可选解析整数。
            // 返回：模拟宿主字段校验失败。
            [](std::optional<std::int64_t>) { return RendererChangeResult{false, L"unfinished number"}; }));
        ASSERT_TRUE(this->renderer_.BindAction("close",
                                               // 使用既有异步关闭流程结束测试窗口。
                                               // 入参：无。
                                               // 返回：无。
                                               [this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetCloseHandler(
            // 标题栏关闭仍复用通用关闭接口。
            // 入参：无。
            // 返回：无。
            [this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("close"));
    }

    // 销毁测试自有窗口，避免向后续用例泄漏消息。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        if (this->renderer_.NativeHandle() != nullptr)
            DestroyWindow(this->renderer_.NativeHandle());
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    // 创建隐藏通用窗口，可按用例要求选择置顶。
    // 入参：topmost 为原生置顶标志。
    // 返回：无；窗口失败终止辅助函数并记录失败。
    void Show(bool topmost = false)
    {
        RendererWindowOptions options;
        options.showCommand = SW_HIDE;
        options.topmost = topmost;
        ASSERT_TRUE(this->renderer_.Show(options));
    }

    // 在当前窗口子树中查询可访问名称对应的控件。
    // 入参：name 为期望名称。
    // 返回：查询到的借用 HWND。
    HWND Find(std::wstring_view name)
    {
        ChildSearch search{name};
        EnumChildWindows(this->renderer_.NativeHandle(), SearchChild, reinterpret_cast<LPARAM>(&search));
        return search.window;
    }

    // 在内存位图中执行真实原生自绘，观察中心像素以排除屏幕遮挡因素。
    // 入参：window 为测试色块控件。
    // 返回：中心 RGB 值；资源创建失败时返回 CLR_INVALID。
    COLORREF Pixel(HWND window)
    {
        const HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
            return CLR_INVALID;
        const HBITMAP bitmap = CreateBitmap(64, 32, 1, 32, nullptr);
        if (bitmap == nullptr)
        {
            DeleteDC(dc);
            return CLR_INVALID;
        }
        const HGDIOBJ previous = SelectObject(dc, bitmap);
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_STATIC;
        draw.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
        draw.hwndItem = window;
        draw.hDC = dc;
        draw.rcItem = {0, 0, 64, 32};
        SendMessageW(GetParent(window), WM_DRAWITEM, draw.CtlID, reinterpret_cast<LPARAM>(&draw));
        const COLORREF pixel = GetPixel(dc, 32, 16);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return pixel;
    }

    WindowRenderer renderer_;
    RendererColorResult color_{true, 0x123456, {}};
    int colorReads_{};
    int textReads_{};
    int integerReads_{};
    int actions_{};
    bool throwRead_{};
};

// 验证 swatch 复用通用叶节点的文本键和宽度协议，不接受私有颜色字符串属性。
// 入参：无。
// 返回：无；缺少可访问文本键或含业务值字段时解析失败。
TEST(RendererColorModelTest, swatch_requires_accessible_text_and_rejects_embedded_color_values)
{
    renderer_detail::Layout layout;
    nlohmann::json document = ColorDocument();
    ASSERT_TRUE(renderer_detail::ParseLayout(document, layout));
    EXPECT_EQ(layout.pages[0].content.children[0].children[0].type, renderer_detail::NodeType::Swatch);
    EXPECT_EQ(layout.pages[0].content.children[0].children[0].width, 64);
    document["content"]["children"][0]["children"][0].erase("textKey");
    EXPECT_FALSE(renderer_detail::ParseLayout(document, layout));
    document = ColorDocument();
    document["content"]["children"][0]["children"][0]["rgb"] = "#123456";
    EXPECT_FALSE(renderer_detail::ParseLayout(document, layout));
}

// 验证 BindColor 类型和绑定约束，只读色样无需动作回调即可通过完整绑定校验。
// 入参：无。
// 返回：无；空读取、重复绑定与错误控件类型被拒绝。
TEST_F(RendererColorTest, validates_color_bindings_and_readonly_contract)
{
    EXPECT_TRUE(this->renderer_.ValidateBindings());
    // 给重复或错误目标提供有效读取回调，确保错误来自绑定目标本身。
    // 入参：无。
    // 返回：合法颜色。
    const std::function<RendererColorResult()> read = []() { return RendererColorResult{}; };
    EXPECT_EQ(this->renderer_.BindColor("sample", read).code, "duplicate_binding");
    EXPECT_EQ(this->renderer_.BindColor("draft", read).code, "wrong_control_type");
    EXPECT_EQ(this->renderer_.BindColor("missing", read).code, "unknown_id");
    EXPECT_EQ(this->renderer_.BindColor("sample", {}).code, "empty_callback");
    EXPECT_EQ(this->renderer_.RefreshValue("sample").code, "window_missing");
}

// 验证原生色块实际绘制 RGB，读取刷新不触发动作，并保留可访问名称和交互差异。
// 入参：无。
// 返回：无；只读色样不参与 Tab 导航，可点击色块遵守忙状态和禁用门禁。
TEST_F(RendererColorTest, native_swatch_draws_rgb_and_optional_action_obeys_common_gates)
{
    this->Show();
    const HWND sample = this->Find(L"sample");
    const HWND pick = this->Find(L"pick");
    ASSERT_NE(sample, nullptr);
    ASSERT_NE(pick, nullptr);
    EXPECT_EQ(GetWindowLongPtrW(sample, GWL_STYLE) & WS_TABSTOP, 0);
    EXPECT_NE(GetWindowLongPtrW(pick, GWL_STYLE) & WS_TABSTOP, 0);
    EXPECT_EQ(this->Pixel(sample), RGB(0x12, 0x34, 0x56));
    EXPECT_EQ(this->Pixel(pick), RGB(0, 0xAA, 0x22));
    SendMessageW(pick, BM_CLICK, 0, 0);
    EXPECT_EQ(this->actions_, 1);
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    SendMessageW(pick, BM_CLICK, 0, 0);
    EXPECT_EQ(this->actions_, 1);
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    ASSERT_TRUE(this->renderer_.SetEnabled("pick", false));
    SendMessageW(pick, BM_CLICK, 0, 0);
    EXPECT_EQ(this->actions_, 1);
    this->color_.rgb = 0xFEDCBA;
    ASSERT_TRUE(this->renderer_.RefreshValues());
    EXPECT_EQ(this->Pixel(sample), RGB(0xFE, 0xDC, 0xBA));
    EXPECT_EQ(this->actions_, 1);
}

// 验证宿主读取失败显示公共字段错误并保留旧颜色，非法 RGB 与回调异常返回结构化错误。
// 入参：无。
// 返回：无；错误后恢复读取可以刷新同一控件。
TEST_F(RendererColorTest, color_errors_preserve_last_value_and_use_shared_error_reporting)
{
    this->Show();
    const HWND sample = this->Find(L"sample");
    this->color_ = {false, 0, L"color unavailable"};
    ASSERT_TRUE(this->renderer_.RefreshValue("sample"));
    EXPECT_NE(this->Find(L"color unavailable"), nullptr);
    EXPECT_EQ(this->Pixel(sample), RGB(0x12, 0x34, 0x56));
    this->color_ = {true, 0x1000000, {}};
    EXPECT_EQ(this->renderer_.RefreshValue("sample").code, "invalid_color");
    EXPECT_EQ(this->Pixel(sample), RGB(0x12, 0x34, 0x56));
    this->throwRead_ = true;
    EXPECT_EQ(this->renderer_.RefreshValue("sample").code, "callback_failed");
    this->throwRead_ = false;
    this->color_ = {true, 0, {}};
    ASSERT_TRUE(this->renderer_.RefreshValue("sample"));
    EXPECT_EQ(this->Pixel(sample), RGB(0, 0, 0));
    EXPECT_TRUE(this->renderer_.SetFieldError("sample", L"host error"));
    EXPECT_NE(this->Find(L"host error"), nullptr);
}

// 验证单字段刷新只读取目标，不覆盖其他编辑框的未完成文字、非法整数或光标选区。
// 入参：无。
// 返回：无；同接口可刷新普通编辑字段，仍保持未指定的整数原样。
TEST_F(RendererColorTest, refresh_value_preserves_unfinished_sibling_inputs_and_selection)
{
    this->Show();
    ChildSearch search{};
    EnumChildWindows(this->renderer_.NativeHandle(), SearchChild, reinterpret_cast<LPARAM>(&search));
    ASSERT_EQ(search.edits.size(), 2U);
    SetWindowTextW(search.edits[0], L"unfinished");
    SetWindowTextW(search.edits[1], L"-");
    SendMessageW(search.edits[0], EM_SETSEL, 2, 5);
    const int textReads = this->textReads_;
    const int integerReads = this->integerReads_;
    this->color_.rgb = 0x112233;
    ASSERT_TRUE(this->renderer_.RefreshValue("sample"));
    EXPECT_EQ(this->textReads_, textReads);
    EXPECT_EQ(this->integerReads_, integerReads);
    wchar_t text[64]{};
    GetWindowTextW(search.edits[0], text, 64);
    EXPECT_EQ(std::wstring_view(text), L"unfinished");
    GetWindowTextW(search.edits[1], text, 64);
    EXPECT_EQ(std::wstring_view(text), L"-");
    DWORD start{};
    DWORD end{};
    SendMessageW(search.edits[0], EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    EXPECT_EQ(start, 2U);
    EXPECT_EQ(end, 5U);
    ASSERT_TRUE(this->renderer_.RefreshValue("draft"));
    GetWindowTextW(search.edits[0], text, 64);
    EXPECT_EQ(std::wstring_view(text), L"saved");
    GetWindowTextW(search.edits[1], text, 64);
    EXPECT_EQ(std::wstring_view(text), L"-");
    EXPECT_EQ(this->renderer_.RefreshValue("close").code, "wrong_control_type");
    EXPECT_EQ(this->renderer_.RefreshValue("unknown").code, "unknown_id");
}

// 验证单控件刷新继续遵守所属 UI 线程约束，不跨线程执行宿主读取。
// 入参：无。
// 返回：无；非所属线程获得明确错误。
TEST_F(RendererColorTest, refresh_value_rejects_foreign_thread)
{
    this->Show();
    RendererResult result;
    // 只从外部线程调用公开接口，不发送窗口消息。
    // 入参：无。
    // 返回：无；结果在 join 后读取。
    std::thread worker([this, &result]() { result = this->renderer_.RefreshValue("sample"); });
    worker.join();
    EXPECT_EQ(result.code, "wrong_thread");
}

// 验证非模态创建可选择置顶，隐藏测试窗口不因此激活或显示。
// 入参：无。
// 返回：无；原生扩展样式准确包含置顶标记。
TEST_F(RendererColorTest, show_honors_topmost_without_overriding_hidden_state)
{
    this->Show(true);
    ASSERT_NE(this->renderer_.NativeHandle(), nullptr);
    EXPECT_NE(GetWindowLongPtrW(this->renderer_.NativeHandle(), GWL_EXSTYLE) & WS_EX_TOPMOST, 0);
    EXPECT_FALSE(IsWindowVisible(this->renderer_.NativeHandle()));
}

// 验证模态入口沿用相同置顶选项及既有消息循环，不另行创建窗口或改变关闭机制。
// 入参：无。
// 返回：无；测试消息同步观察置顶后通过 RequestClose 结束模态。
TEST_F(RendererColorTest, show_modal_honors_topmost_and_existing_close_flow)
{
    MSG queued{};
    PeekMessageW(&queued, nullptr, 0, 0, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 778, 0, 0));
    RendererWindowOptions options;
    options.showCommand = SW_HIDE;
    options.topmost = true;
    bool checked{};
    EXPECT_TRUE(this->renderer_.ShowModal(
        options,
        // 在既有线程消息回调中观察窗口属性并请求正常关闭。
        // 入参：message 为模态循环当前取得的消息。
        // 返回：仅测试消息被消费。
        [this, &checked](MSG& message)
        {
            if (message.message != WM_APP + 778)
                return false;
            checked = true;
            EXPECT_NE(GetWindowLongPtrW(this->renderer_.NativeHandle(), GWL_EXSTYLE) & WS_EX_TOPMOST, 0);
            EXPECT_FALSE(IsWindowVisible(this->renderer_.NativeHandle()));
            EXPECT_TRUE(this->renderer_.RequestClose());
            return true;
        }));
    EXPECT_TRUE(checked);
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
} // namespace
