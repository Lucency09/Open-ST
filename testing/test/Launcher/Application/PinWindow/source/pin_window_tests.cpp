// 使用隐藏真实 HWND 验证贴图稳定标识、图像所有权及跨模态关闭边界。

#include "pin_window.h"
#include <dwmapi.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <vector>

namespace open_st
{
struct PinWindowTestAccess final
{
    // 请求管理器按稳定标识提升贴图，避免依赖隐藏窗口的系统排序。
    // 入参：manager 为被测管理器；id 为目标贴图。
    // 返回：无返回值；只改变产品已有的排序状态。
    static void Raise(PinWindowManager& manager, PinId id)
    {
        manager.Raise(id);
    }
    // 查询业务顺序中的最高有效贴图。
    // 入参：manager 为被测管理器；id 为待比较标识。
    // 返回：目标位于业务排序顶部时为 true。
    static bool Highest(const PinWindowManager& manager, PinId id)
    {
        return manager.IsHighest(id);
    }
    // 模拟外层窗口回调仍在执行，以检查同步关闭不会释放当前记录。
    // 入参：manager 为被测管理器。
    // 返回：无返回值；增加窗口分派深度。
    static void Enter(PinWindowManager& manager)
    {
        manager.EnterDispatch();
    }
    // 结束模拟窗口回调并允许管理器处理延迟关闭。
    // 入参：manager 为被测管理器。
    // 返回：无返回值；与 Enter 成对使用。
    static void Leave(PinWindowManager& manager)
    {
        manager.LeaveDispatch();
    }
    // 检查失效或隐藏贴图不会向宿主投递业务命令。
    // 入参：manager、id、command 为管理器和拟提交命令。
    // 返回：产品回调成功接收命令时为 true。
    static bool Submit(PinWindowManager& manager, PinId id, PinCommand command)
    {
        return manager.Submit(id, command);
    }
    // 查询正式输入门禁，区别可见但暂停的贴图与正常交互状态。
    // 入参：manager 为被测管理器；id 为目标标识。
    // 返回：当前允许交互时 true。
    static bool CanInteract(const PinWindowManager& manager, PinId id)
    {
        return manager.CanInteract(id);
    }
    // 查询正式滚轮准入，不修改真实前台或键盘焦点。
    // 入参：manager 为被测管理器；id 为目标标识；point 为屏幕物理坐标。
    // 返回：当前点击、焦点、命中与暂停条件均满足时 true。
    static bool CanWheel(const PinWindowManager& manager, PinId id, POINT point)
    {
        return manager.CanWheel(id, point);
    }
};
} // namespace open_st

namespace
{
class PinWindowTest : public testing::Test
{
  protected:
    // 初始化当前测试线程 COM，并显式检查桌面合成前置条件。
    // 入参：无。
    // 返回：无返回值；不支持桌面合成的环境明确跳过窗口集成用例。
    void SetUp() override
    {
        BOOL enabled = FALSE;
        if (FAILED(DwmIsCompositionEnabled(&enabled)) || !enabled)
        {
            GTEST_SKIP() << "Desktop composition is unavailable for PinWindow integration tests.";
        }
        this->com_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->com_) || this->com_ == RPC_E_CHANGED_MODE);
        open_st::PinWindowCallbacks callbacks;
        // 提供固定测试文字，避免隐藏窗口测试读取产品资源。
        // 入参：key 为模块查询的文本键。
        // 返回：以原键构成的可识别测试文字。
        callbacks.text = [](std::string_view key) { return std::wstring(key.begin(), key.end()); };
        // 记录业务投递次数，不修改系统剪贴板或磁盘。
        // 入参：id、command 为拟提交的稳定标识和命令。
        // 返回：true，表示测试宿主已接收。
        callbacks.command = [this](open_st::PinId id, open_st::PinCommand command)
        {
            ++this->commands_;
            this->lastId_ = id;
            this->lastCommand_ = command;
            return true;
        };
        this->manager_ = std::make_unique<open_st::PinWindowManager>(GetModuleHandleW(nullptr), std::move(callbacks));
        std::vector<std::uint8_t> pixels(32U * 24U * 4U, 127);
        std::wstring error;
        this->image_ = open_st::PinImage::Create(32, 24, 32U * 4U, pixels, error);
        ASSERT_NE(this->image_, nullptr);
    }
    // 在 COM 结束前释放窗口、GPU 资源及测试图像。
    // 入参：无。
    // 返回：无返回值；与当前测试初始化配对。
    void TearDown() override
    {
        this->manager_.reset();
        this->image_.reset();
        if (SUCCEEDED(this->com_))
            CoUninitialize();
    }
    // 准备一张隐藏贴图，保存首帧而不激活桌面窗口。
    // 入参：无。
    // 返回：成功发布的稳定 ID；失败记录断言并返回零。
    open_st::PinId Prepare()
    {
        open_st::PinId id{};
        std::wstring error;
        EXPECT_TRUE(this->manager_->Prepare(this->image_, {30, 30}, id, error));
        return id;
    }
    HRESULT com_{E_FAIL};
    std::unique_ptr<open_st::PinWindowManager> manager_;
    std::shared_ptr<const open_st::PinImage> image_;
    int commands_{};
    open_st::PinId lastId_{};
    open_st::PinCommand lastCommand_{open_st::PinCommand::Copy};
};

// 验证准备只创建隐藏窗口，关闭贴图后原图快照仍有效且 ID 不复用。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言窗口、共享图像及稳定标识。
TEST_F(PinWindowTest, hidden_prepare_snapshot_and_non_reused_ids)
{
    const open_st::PinId first = this->Prepare();
    ASSERT_NE(first, 0U);
    const HWND window = this->manager_->Window(first);
    ASSERT_TRUE(IsWindow(window));
    EXPECT_FALSE(IsWindowVisible(window));
    const std::shared_ptr<const open_st::PinImage> snapshot = this->manager_->Image(first);
    ASSERT_NE(snapshot, nullptr);
    this->manager_->Close(first);
    EXPECT_FALSE(IsWindow(window));
    EXPECT_EQ(this->manager_->Image(first), nullptr);
    EXPECT_EQ(snapshot->Pixels()[0], 127);
    const open_st::PinId second = this->Prepare();
    EXPECT_GT(second, first);
}

// 验证无效图像失败不发布半初始化窗口或增加贴图数量。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言失败结果与管理器计数。
TEST_F(PinWindowTest, rejected_image_does_not_publish_window)
{
    open_st::PinId id{};
    std::wstring error;
    EXPECT_FALSE(this->manager_->Prepare({}, {0, 0}, id, error));
    EXPECT_EQ(this->manager_->Count(), 0U);
    EXPECT_EQ(this->manager_->Window(id), nullptr);
}

// 验证默认后建在上，显式提层只移动目标，关闭后保持其余图的相对顺序。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；逐步检查业务排序顶部。
TEST_F(PinWindowTest, order_survives_raise_close_and_new_image)
{
    const open_st::PinId a = this->Prepare();
    const open_st::PinId b = this->Prepare();
    const open_st::PinId c = this->Prepare();
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, c));
    open_st::PinWindowTestAccess::Raise(*this->manager_, a);
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, a));
    const open_st::PinId d = this->Prepare();
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, d));
    this->manager_->Close(d);
    this->manager_->Close(a);
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, c));
    this->manager_->Close(c);
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, b));
}

// 验证嵌套模态内关闭 owner 只使 ID 失效，真实 HWND 在最外层退出前保持存活。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言两层暂停和实际资源释放时点。
TEST_F(PinWindowTest, nested_modal_defers_owner_destruction)
{
    const open_st::PinId id = this->Prepare();
    const HWND window = this->manager_->Window(id);
    ASSERT_TRUE(this->manager_->BeginModal(id));
    ASSERT_TRUE(this->manager_->BeginModal(0));
    this->manager_->Close(id);
    EXPECT_EQ(this->manager_->Window(id), nullptr);
    EXPECT_TRUE(IsWindow(window));
    EXPECT_TRUE(this->manager_->IsBusy());
    this->manager_->EndModal();
    EXPECT_TRUE(IsWindow(window));
    this->manager_->EndModal();
    EXPECT_FALSE(this->manager_->IsBusy());
    EXPECT_FALSE(IsWindow(window));
}

// 验证取消模态作用域不会关闭贴图或改变排序，等价于菜单没有选中业务项。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言退出暂停后原窗口仍然有效。
TEST_F(PinWindowTest, cancelled_modal_preserves_window_and_order)
{
    const open_st::PinId a = this->Prepare();
    const open_st::PinId b = this->Prepare();
    ASSERT_TRUE(this->manager_->BeginModal(a));
    this->manager_->EndModal();
    EXPECT_NE(this->manager_->Window(a), nullptr);
    EXPECT_EQ(this->manager_->Count(), 2U);
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, b));
}

// 验证窗口回调执行期间的关闭请求不会使当前 HWND 提前失效。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查分派深度归零后的实际关闭。
TEST_F(PinWindowTest, dispatch_scope_defers_close)
{
    const open_st::PinId id = this->Prepare();
    const HWND window = this->manager_->Window(id);
    open_st::PinWindowTestAccess::Enter(*this->manager_);
    this->manager_->Close(id);
    EXPECT_TRUE(IsWindow(window));
    open_st::PinWindowTestAccess::Leave(*this->manager_);
    EXPECT_FALSE(IsWindow(window));
}

// 验证截图结束不会把原来隐藏或暂停期间刚准备的窗口意外显示。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言图像存活、初始可见性与排序。
TEST_F(PinWindowTest, capture_end_keeps_prepared_images_hidden)
{
    const open_st::PinId oldId = this->Prepare();
    std::wstring error;
    ASSERT_TRUE(this->manager_->BeginCapture(error));
    const open_st::PinId newId = this->Prepare();
    this->manager_->Show(newId);
    EXPECT_FALSE(IsWindowVisible(this->manager_->Window(newId)));
    this->manager_->EndCapture();
    EXPECT_FALSE(IsWindowVisible(this->manager_->Window(oldId)));
    EXPECT_FALSE(IsWindowVisible(this->manager_->Window(newId)));
    EXPECT_TRUE(open_st::PinWindowTestAccess::Highest(*this->manager_, newId));
}

// 验证捕获准备保持原可见性但拒绝输入，捕获失败直接恢复后原图重新接受业务命令。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；只检查暂停门禁，不要求系统授予真实前台焦点。
TEST_F(PinWindowTest, capture_pause_keeps_visible_image_and_failure_restores_input)
{
    const open_st::PinId visibleId = this->Prepare();
    const open_st::PinId hiddenId = this->Prepare();
    ASSERT_NE(visibleId, 0U);
    ASSERT_NE(hiddenId, 0U);
    const HWND visibleWindow = this->manager_->Window(visibleId);
    const HWND hiddenWindow = this->manager_->Window(hiddenId);
    this->manager_->Show(visibleId);
    ASSERT_TRUE(IsWindowVisible(visibleWindow));
    ASSERT_FALSE(IsWindowVisible(hiddenWindow));
    ASSERT_TRUE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, visibleId));
    ASSERT_TRUE(open_st::PinWindowTestAccess::Submit(*this->manager_, visibleId, open_st::PinCommand::Copy));
    ASSERT_EQ(this->commands_, 1);

    std::wstring error;
    ASSERT_TRUE(this->manager_->BeginCapture(error)) << error;
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_FALSE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, visibleId));
    EXPECT_FALSE(open_st::PinWindowTestAccess::CanWheel(*this->manager_, visibleId, {32, 32}));
    EXPECT_FALSE(open_st::PinWindowTestAccess::Submit(*this->manager_, visibleId, open_st::PinCommand::Copy));
    EXPECT_FALSE(open_st::PinWindowTestAccess::Submit(*this->manager_, visibleId, open_st::PinCommand::Save));
    EXPECT_EQ(this->commands_, 1);

    this->manager_->EndCapture();
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_TRUE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, visibleId));
    EXPECT_FALSE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, hiddenId));
    EXPECT_TRUE(open_st::PinWindowTestAccess::Submit(*this->manager_, visibleId, open_st::PinCommand::Save));
    EXPECT_EQ(this->commands_, 2);
    EXPECT_EQ(this->lastId_, visibleId);
    EXPECT_EQ(this->lastCommand_, open_st::PinCommand::Save);
    EXPECT_EQ(this->manager_->Image(visibleId), this->image_);
}

// 验证重复开始与结束捕获不改变旧图可见性，暂停期间的显示请求被拒绝且不会延迟执行。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查输入暂停幂等，候选图只能在结束捕获后显式显示。
TEST_F(PinWindowTest, capture_begin_end_are_idempotent_and_reject_show_while_paused)
{
    const open_st::PinId visibleId = this->Prepare();
    const open_st::PinId hiddenId = this->Prepare();
    ASSERT_NE(visibleId, 0U);
    ASSERT_NE(hiddenId, 0U);
    const HWND visibleWindow = this->manager_->Window(visibleId);
    const HWND hiddenWindow = this->manager_->Window(hiddenId);
    this->manager_->Show(visibleId);
    ASSERT_TRUE(IsWindowVisible(visibleWindow));
    ASSERT_FALSE(IsWindowVisible(hiddenWindow));
    std::wstring error;
    ASSERT_TRUE(this->manager_->BeginCapture(error)) << error;
    ASSERT_TRUE(this->manager_->BeginCapture(error)) << error;
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    this->manager_->Show(hiddenId);
    this->manager_->Show(visibleId);
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_FALSE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, visibleId));

    this->manager_->EndCapture();
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    this->manager_->EndCapture();
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_EQ(this->manager_->Count(), 2U);
    EXPECT_TRUE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, visibleId));
    this->manager_->Show(hiddenId);
    EXPECT_TRUE(IsWindowVisible(hiddenWindow));
    EXPECT_TRUE(open_st::PinWindowTestAccess::CanInteract(*this->manager_, hiddenId));
}

// 验证退出期间拒绝新图与命令，模态 owner 在退出暂停后才释放。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言退出不恢复窗口或调用业务回调。
TEST_F(PinWindowTest, shutdown_defers_owner_and_rejects_new_work)
{
    const open_st::PinId id = this->Prepare();
    const HWND window = this->manager_->Window(id);
    ASSERT_TRUE(this->manager_->BeginModal(id));
    this->manager_->Shutdown();
    EXPECT_TRUE(IsWindow(window));
    EXPECT_FALSE(open_st::PinWindowTestAccess::Submit(*this->manager_, id, open_st::PinCommand::Copy));
    open_st::PinId next{};
    std::wstring error;
    EXPECT_FALSE(this->manager_->Prepare(this->image_, {0, 0}, next, error));
    this->manager_->EndCapture();
    this->manager_->EndModal();
    EXPECT_FALSE(IsWindow(window));
    EXPECT_EQ(this->commands_, 0);
}
// 验证模态期间排队的点击、滚轮和快捷键不会在恢复后回放。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查本窗口输入已移除，普通业务消息仍留在队列。
TEST_F(PinWindowTest, modal_discards_queued_input_without_dropping_other_messages)
{
    const open_st::PinId id = this->Prepare();
    const HWND window = this->manager_->Window(id);
    ASSERT_TRUE(this->manager_->BeginModal(id));
    ASSERT_TRUE(PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2)));
    ASSERT_TRUE(PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(32, 32)));
    ASSERT_TRUE(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0));
    ASSERT_TRUE(PostMessageW(window, WM_APP + 42, 0, 0));
    this->manager_->EndModal();
    MSG message{};
    EXPECT_FALSE(PeekMessageW(&message, window, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE));
    EXPECT_FALSE(PeekMessageW(&message, window, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
    EXPECT_TRUE(PeekMessageW(&message, window, WM_APP + 42, WM_APP + 42, PM_REMOVE));
    EXPECT_TRUE(IsWindow(window));
}

// 验证截图期间积压的窗口输入在结束暂停前被清除，新图仍保持隐藏。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；只检查暂停恢复的消息边界，不替代真实桌面点击验收。
TEST_F(PinWindowTest, capture_end_discards_queued_clicks)
{
    const open_st::PinId id = this->Prepare();
    const HWND window = this->manager_->Window(id);
    std::wstring error;
    ASSERT_TRUE(this->manager_->BeginCapture(error));
    ASSERT_TRUE(PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2)));
    this->manager_->EndCapture();
    MSG message{};
    EXPECT_FALSE(PeekMessageW(&message, window, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE));
    EXPECT_FALSE(IsWindowVisible(window));
}
// 验证输入清理保留不受 PeekMessage 范围过滤限制的线程退出通知。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；模态返回后 WM_QUIT 及原退出码仍能被主循环取得。
TEST_F(PinWindowTest, modal_input_cleanup_preserves_quit_code)
{
    const open_st::PinId id = this->Prepare();
    ASSERT_TRUE(this->manager_->BeginModal(id));
    PostQuitMessage(37);
    this->manager_->EndModal();
    MSG message{};
    ASSERT_TRUE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_EQ(message.message, WM_QUIT);
    EXPECT_EQ(message.wParam, 37U);
}
} // namespace
