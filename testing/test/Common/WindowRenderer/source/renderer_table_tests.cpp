// 验证通用表格的稳定身份、输入门禁和布局显隐，不包含产品配置知识。
#include "renderer_model.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <window_renderer.h>

namespace
{
// 构造包含表格、编辑草稿及下方按钮的可滚动布局。
// 入参：无。
// 返回：独立通用布局。
nlohmann::json TableDocument()
{
    return nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"title","initialSize":[640,540]},
      "content":{"type":"column","id":"body","gap":12,"children":[
        {"type":"column","id":"group","gap":8,"children":[
          {"type":"table","id":"table","labelKey":"rows","visibleRows":4,"columns":[
            {"textKey":"name","width":120},{"textKey":"value","width":"fill"}]},
          {"type":"edit","id":"draft"}]},
        {"type":"button","id":"after","textKey":"after"}]},
      "footer":{"trailing":[{"type":"button","id":"close","textKey":"close"}]}})");
}
class RendererTableTest : public testing::Test
{
  protected:
    open_st::WindowRenderer renderer_;
    std::vector<open_st::RendererTableRow> rows_{{"a", {L"甲", L"one"}}, {"b", {L"乙", L"two"}}};
    std::string selected_{"b"}, draft_{"seed"};
    int changes_{}, activations_{}, defaults_{}, edits_{};
    bool reject_{};
    // 绑定完整宿主状态，并显示隐藏原生窗口以避免干扰前台。
    // 入参：无。
    // 返回：无，初始化失败由断言报告。
    void SetUp() override
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(TableDocument()));
        // 使用文字键作为测试文案，不依赖产品资源。
        // 入参：key 为文字键。
        // 返回：对应宽字符。
        ASSERT_TRUE(
            this->renderer_.SetTextResolver([](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        // 返回独立显示行，宿主仍拥有唯一数据。
        // 入参：无。
        // 返回：完整快照。
        ASSERT_TRUE(
            this->renderer_.BindRows("table", [this]() { return open_st::RendererRowsResult{true, this->rows_, {}}; }));
        ASSERT_TRUE(this->renderer_.BindString(
            "table",
            // 读取稳定选中身份。
            // 入参：无。
            // 返回：当前 ID。
            [this]() { return open_st::RendererStringResult{true, this->selected_, {}}; },
            // 可拒绝选择，验证控件恢复宿主身份。
            // 入参：value 为候选 ID。
            // 返回：接受状态。
            [this](std::string_view value)
            {
                ++this->changes_;
                if (!this->reject_)
                    this->selected_ = value;
                return open_st::RendererChangeResult{!this->reject_, {}};
            }));
        // 记录表格激活，与默认按钮分别计数。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.BindAction("table", [this]() { ++this->activations_; }));
        ASSERT_TRUE(this->renderer_.BindString(
            "draft",
            // 读取本地编辑草稿。
            // 入参：无。
            // 返回：原始文字。
            [this]() { return open_st::RendererStringResult{true, this->draft_, {}}; },
            // 记录真实编辑，不参与表格选择。
            // 入参：value 为当前文字。
            // 返回：接受。
            [this](std::string_view value)
            {
                this->draft_ = value;
                ++this->edits_;
                return open_st::RendererChangeResult{};
            }));
        // 记录下方按钮是否被隐藏或禁用输入误触发。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.BindAction("after", [this]() { ++this->defaults_; }));
        // 记录默认关闭动作，Enter 在表格内不应触发它。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.BindAction("close", [this]() { ++this->defaults_; }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("close"));
        // 关闭使用原有延迟机制。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.SetCloseHandler([this]() { (void)this->renderer_.RequestClose(); }));
        open_st::RendererWindowOptions options;
        options.showCommand = SW_HIDE;
        ASSERT_TRUE(this->renderer_.Show(options));
    }
    // 在宿主状态释放前销毁原生窗口。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        DestroyWindow(this->renderer_.NativeHandle());
    }
    // 定位统一滚动视口。
    // 入参：无。
    // 返回：借用 HWND。
    HWND Viewport() const
    {
        return FindWindowExW(this->renderer_.NativeHandle(), nullptr, L"OpenST.WindowRendererPage", nullptr);
    }
    // 定位表格原生控件。
    // 入参：无。
    // 返回：借用 HWND。
    HWND Table() const
    {
        return GetDlgItem(this->Viewport(), 100);
    }
    // 模拟原生双击通知，只传真实行索引。
    // 入参：row 为行号，负数代表空白区域。
    // 返回：无。
    void DoubleClick(int row)
    {
        NMITEMACTIVATE event{};
        event.hdr = {this->Table(), 100, NM_DBLCLK};
        event.iItem = row;
        SendMessageW(this->Viewport(), WM_NOTIFY, 100, reinterpret_cast<LPARAM>(&event));
    }
};
// 验证重排按稳定身份恢复选择，刷新不伪造用户选择事件。
// 入参：无。
// 返回：断言行索引、身份和通知次数。
TEST_F(RendererTableTest, refresh_preserves_stable_selection_across_reorder_and_empty_rows)
{
    EXPECT_EQ(ListView_GetItemCount(this->Table()), 2);
    EXPECT_EQ(ListView_GetNextItem(this->Table(), -1, LVNI_SELECTED), 1);
    std::swap(this->rows_[0], this->rows_[1]);
    ASSERT_TRUE(this->renderer_.RefreshValue("table"));
    EXPECT_EQ(ListView_GetNextItem(this->Table(), -1, LVNI_SELECTED), 0);
    EXPECT_EQ(this->selected_, "b");
    EXPECT_EQ(this->changes_, 0);
    this->rows_.clear();
    ASSERT_TRUE(this->renderer_.RefreshValue("table"));
    EXPECT_EQ(ListView_GetItemCount(this->Table()), 0);
    EXPECT_EQ(this->changes_, 0);
}
// 无效快照不得改变已经显示的行或选择。
// 入参：无。
// 返回：断言重复身份和错误列数的失败原子性。
TEST_F(RendererTableTest, invalid_rows_leave_previous_native_snapshot_intact)
{
    this->rows_.push_back(this->rows_.front());
    EXPECT_EQ(this->renderer_.RefreshValue("table").code, "duplicate_row");
    EXPECT_EQ(ListView_GetItemCount(this->Table()), 2);
    this->rows_.pop_back();
    this->rows_[0].cells.clear();
    EXPECT_EQ(this->renderer_.RefreshValue("table").code, "invalid_row");
    EXPECT_EQ(ListView_GetNextItem(this->Table(), -1, LVNI_SELECTED), 1);
}
// 拒绝选择必须恢复原稳定身份，不因行号变化误选其他行。
// 入参：无。
// 返回：断言宿主身份、原生选择和事件次数。
TEST_F(RendererTableTest, rejected_selection_restores_host_identity)
{
    this->reject_ = true;
    ListView_SetItemState(this->Table(), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    EXPECT_EQ(this->selected_, "b");
    EXPECT_EQ(ListView_GetNextItem(this->Table(), -1, LVNI_SELECTED), 1);
    EXPECT_EQ(this->changes_, 1);
}
// 双击只激活真实选中行，禁用和窗口忙态必须屏蔽同样的延后通知。
// 入参：无。
// 返回：断言激活次数与默认按钮隔离。
TEST_F(RendererTableTest, double_click_obeys_disabled_busy_and_hidden_ancestors)
{
    this->DoubleClick(1);
    EXPECT_EQ(this->activations_, 1);
    this->DoubleClick(-1);
    ASSERT_TRUE(this->renderer_.SetEnabled("table", false));
    this->DoubleClick(1);
    ASSERT_TRUE(this->renderer_.SetEnabled("table", true));
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    this->DoubleClick(1);
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    ASSERT_TRUE(this->renderer_.SetVisible("group", false));
    ASSERT_TRUE(this->renderer_.SetVisible("table", true));
    this->DoubleClick(1);
    EXPECT_EQ(this->activations_, 1);
    EXPECT_EQ(this->defaults_, 0);
}
// 表格焦点的 Enter 激活选中行而不是窗口默认按钮，空表也不会误提交。
// 入参：无。
// 返回：断言真实对话框消息路径的激活次数与默认动作隔离。
TEST_F(RendererTableTest, enter_activates_selected_row_without_submitting_default_action)
{
    SetFocus(this->Table());
    ASSERT_EQ(GetFocus(), this->Table());
    MSG message{};
    message.hwnd = this->Table();
    message.message = WM_KEYDOWN;
    message.wParam = VK_RETURN;
    ASSERT_TRUE(this->renderer_.ProcessDialogMessage(message));
    EXPECT_EQ(this->activations_, 1);
    EXPECT_EQ(this->defaults_, 0);
    this->rows_.clear();
    ASSERT_TRUE(this->renderer_.RefreshValue("table"));
    ASSERT_TRUE(this->renderer_.ProcessDialogMessage(message));
    EXPECT_EQ(this->activations_, 1);
    EXPECT_EQ(this->defaults_, 0);
}

// 选择被拒绝后双击原候选行不能激活被恢复选中的另一行。
// 入参：无。
// 返回：断言激活目标仍受最初稳定身份约束。
TEST_F(RendererTableTest, rejected_double_click_does_not_activate_restored_other_row)
{
    this->reject_ = true;
    ListView_SetItemState(this->Table(), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ASSERT_EQ(this->selected_, "b");
    this->DoubleClick(0);
    EXPECT_EQ(this->activations_, 0);
    EXPECT_EQ(this->defaults_, 0);
}
// Enter 分派时才被拒绝的原生候选，同样不能替换成旧选中行进行激活。
// 入参：无。
// 返回：断言回调恢复选择后原事件失效。
TEST_F(RendererTableTest, rejected_pending_enter_does_not_activate_restored_other_row)
{
    this->reject_ = true;
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    ListView_SetItemState(this->Table(), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    SetFocus(this->Table());
    ASSERT_EQ(GetFocus(), this->Table());
    MSG message{};
    message.hwnd = this->Table();
    message.message = WM_KEYDOWN;
    message.wParam = VK_RETURN;
    ASSERT_TRUE(this->renderer_.ProcessDialogMessage(message));
    EXPECT_EQ(this->selected_, "b");
    EXPECT_EQ(this->activations_, 0);
    EXPECT_EQ(this->defaults_, 0);
}
// 旧消息仍指向已隐藏或禁用的编辑框时，不得借当前焦点触发窗口默认动作。
// 入参：无。
// 返回：断言排队 Enter 被消费且没有提交。
TEST_F(RendererTableTest, queued_enter_from_hidden_or_disabled_control_never_submits)
{
    MSG message{};
    message.hwnd = GetDlgItem(this->Viewport(), 101);
    message.message = WM_KEYDOWN;
    message.wParam = VK_RETURN;
    ASSERT_TRUE(this->renderer_.SetVisible("group", false));
    ASSERT_TRUE(this->renderer_.ProcessDialogMessage(message));
    EXPECT_EQ(this->defaults_, 0);
    ASSERT_TRUE(this->renderer_.SetVisible("group", true));
    ASSERT_TRUE(this->renderer_.SetEnabled("draft", false));
    ASSERT_TRUE(this->renderer_.ProcessDialogMessage(message));
    EXPECT_EQ(this->defaults_, 0);
}

// 隐藏整组收回高度和间隙，恢复时保持原生编辑对象及撤销历史。
// 入参：无。
// 返回：断言实际几何、编辑 HWND 和历史不被重建。
TEST_F(RendererTableTest, hidden_group_collapses_layout_and_preserves_edit_undo)
{
    const HWND edit = GetDlgItem(this->Viewport(), 101);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"!"));
    ASSERT_NE(SendMessageW(edit, EM_CANUNDO, 0, 0), 0);
    RECT before{}, hidden{}, restored{};
    GetWindowRect(GetDlgItem(this->Viewport(), 102), &before);
    ASSERT_TRUE(this->renderer_.SetVisible("group", false));
    GetWindowRect(GetDlgItem(this->Viewport(), 102), &hidden);
    EXPECT_LT(hidden.top, before.top);
    const int edits = this->edits_;
    SendMessageW(this->Viewport(), WM_COMMAND, MAKEWPARAM(101, EN_CHANGE), reinterpret_cast<LPARAM>(edit));
    EXPECT_EQ(this->edits_, edits);
    ASSERT_TRUE(this->renderer_.SetVisible("group", true));
    GetWindowRect(GetDlgItem(this->Viewport(), 102), &restored);
    EXPECT_EQ(restored.top, before.top);
    EXPECT_EQ(GetDlgItem(this->Viewport(), 101), edit);
    EXPECT_NE(SendMessageW(edit, EM_CANUNDO, 0, 0), 0);
}
// DPI 改变后固定列按 DIP 缩放，列文本仍通过公共资源解析。
// 入参：无。
// 返回：断言实际原生列宽和标题。
TEST_F(RendererTableTest, columns_scale_with_dpi_and_keep_localized_headers)
{
    const int before = ListView_GetColumnWidth(this->Table(), 0);
    const UINT original = GetDpiForWindow(this->renderer_.NativeHandle());
    RECT bounds{};
    GetWindowRect(this->renderer_.NativeHandle(), &bounds);
    const UINT updated = original == 144 ? 192 : 144;
    SendMessageW(this->renderer_.NativeHandle(), WM_DPICHANGED, MAKELONG(updated, updated),
                 reinterpret_cast<LPARAM>(&bounds));
    EXPECT_EQ(ListView_GetColumnWidth(this->Table(), 0), MulDiv(120, static_cast<int>(updated), 96));
    EXPECT_NE(ListView_GetColumnWidth(this->Table(), 0), before);
    wchar_t title[32]{};
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    column.pszText = title;
    column.cchTextMax = 32;
    ASSERT_NE(SendMessageW(this->Table(), LVM_GETCOLUMNW, 0, reinterpret_cast<LPARAM>(&column)), FALSE);
    EXPECT_STREQ(title, L"name");
}
// 表格布局严格拒绝无列、无标题和零行高配置。
// 入参：无。
// 返回：断言合法布局成功及各无效输入不改输出。
TEST(RendererTableModel, validates_columns_and_visible_row_bounds)
{
    open_st::renderer_detail::Layout layout;
    nlohmann::json document = TableDocument();
    ASSERT_TRUE(open_st::renderer_detail::ParseLayout(document, layout));
    nlohmann::json& table = document["content"]["children"][0]["children"][0];
    table["columns"] = nlohmann::json::array();
    EXPECT_EQ(open_st::renderer_detail::ParseLayout(document, layout).code, "invalid_columns");
    table["columns"] = {{{"textKey", "name"}}};
    table["visibleRows"] = 0;
    EXPECT_EQ(open_st::renderer_detail::ParseLayout(document, layout).code, "invalid_dimension");
}
} // namespace
