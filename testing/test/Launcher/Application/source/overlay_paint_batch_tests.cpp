// 验证真实窗口上的会话绘制批次、重入请求和安全回收，仅系统区域用例显示小型无激活窗口。

#include "capture_overlay_session.h"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <stdexcept>
#include <vector>

namespace
{
constexpr wchar_t ERASE_HOOK_PROPERTY[] = L"OpenST.OverlayPaintBatchEraseHook";

struct EraseHook final
{
    HWND window{};
    std::function<void()> action;
    unsigned int calls{};

    // 在测试窗口上登记一次背景擦除回调，供 BeginPaint 同步调用。
    // 入参：target 为借用的测试窗口句柄。
    // 返回：构造函数无返回值；调用方通过 GetPropW 核验登记成功。
    explicit EraseHook(HWND target) : window(target)
    {
        SetPropW(this->window, ERASE_HOOK_PROPERTY, this);
    }

    // 在栈上回调状态销毁前解除窗口属性，避免遗留悬空指针。
    // 入参：无。
    // 返回：析构函数无返回值；窗口已随会话关闭时无需额外处理。
    ~EraseHook()
    {
        if (IsWindow(this->window) != FALSE && GetPropW(this->window, ERASE_HOOK_PROPERTY) == this)
        {
            RemovePropW(this->window, ERASE_HOOK_PROPERTY);
        }
    }

    // 禁止复制回调属性的独占生命周期。
    // 入参：未使用的同类型引用。
    // 返回：删除的构造函数不可调用。
    EraseHook(const EraseHook&) = delete;

    // 禁止复制赋值导致多个对象解除同一窗口属性。
    // 入参：未使用的同类型引用。
    // 返回：删除的赋值操作不可调用。
    EraseHook& operator=(const EraseHook&) = delete;
};

// 为批次调度建立隐藏 Win32 窗口，并保留正常系统绘制验证行为。
// 入参：window、message、wParam、lParam 为 Windows 窗口消息参数。
// 返回：默认窗口过程处理结果。
LRESULT CALLBACK BatchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND)
    {
        EraseHook* hook = static_cast<EraseHook*>(GetPropW(window, ERASE_HOOK_PROPERTY));
        if (hook != nullptr)
        {
            ++hook->calls;
            if (hook->calls == 1 && hook->action)
            {
                hook->action();
            }
            return 1;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class OverlayPaintBatchTest : public testing::Test
{
  protected:
    open_st::CaptureOverlaySession session_;
    std::array<HWND, 3> windows_{};
    std::wstring error_;

    // 创建三个未显示的真实窗口并交给会话管理生命周期。
    // 入参：无。
    // 返回：无；创建失败通过断言结束本例。
    void SetUp() override
    {
        WNDCLASSW description{};
        description.lpfnWndProc = BatchWindowProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = L"OpenST.OverlayPaintBatchTest";
        ASSERT_TRUE(RegisterClassW(&description) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        for (std::size_t index = 0; index < this->windows_.size(); ++index)
        {
            this->windows_[index] =
                CreateWindowExW(0, description.lpszClassName, L"", WS_POPUP, static_cast<int>(index) * 40, 0, 32, 32,
                                nullptr, nullptr, description.hInstance, nullptr);
            ASSERT_NE(this->windows_[index], nullptr);
            this->session_.Add(this->windows_[index]);
            ASSERT_NE(ValidateRect(this->windows_[index], nullptr), FALSE);
        }
    }

    // 仅为真实系统区域用例显示小型无激活测试窗口，避免隐藏窗口不保留更新区域。
    // 入参：无。
    // 返回：无；断言确保三个窗口已可见并清除显示时产生的初始区域。
    void ShowRegionWindows()
    {
        for (HWND window : this->windows_)
        {
            ASSERT_NE(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW),
                      FALSE);
            ASSERT_NE(IsWindowVisible(window), FALSE);
            ASSERT_NE(ValidateRect(window, nullptr), FALSE);
        }
    }
};

struct DrawRecorder final
{
    std::vector<HWND> drawn;
    std::function<void(open_st::CaptureOverlayOutput&)> action;
    HWND failing{};

    // 记录批次绘制顺序并执行受测试控制的同步边界。
    // 入参：output 为当前借用输出；error 接收模拟渲染失败原因。
    // 返回：当前窗口不是指定失败目标时 true。
    bool operator()(open_st::CaptureOverlayOutput& output, std::wstring& error)
    {
        this->drawn.push_back(output.window);
        if (this->action)
        {
            this->action(output);
        }
        if (output.window == this->failing)
        {
            error = L"第二屏模拟失败。";
            return false;
        }
        return true;
    }
};
} // namespace

// 验证三个输出无论由哪个窗口触发，始终按登记顺序各绘制一次，多次失效合并。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录调度顺序及后续无效区域状态。
TEST_F(OverlayPaintBatchTest, merges_requests_for_every_trigger_in_registration_order)
{
    for (HWND trigger : this->windows_)
    {
        DrawRecorder recorder;
        this->session_.Invalidate();
        this->session_.Invalidate();
        EXPECT_EQ(this->session_.Paint(trigger, std::ref(recorder), {}, this->error_),
                  open_st::OverlayPaintResult::Completed);
        EXPECT_EQ(recorder.drawn, (std::vector<HWND>(this->windows_.begin(), this->windows_.end())));
        for (HWND window : this->windows_)
        {
            EXPECT_EQ(GetUpdateRect(window, nullptr, FALSE), FALSE);
        }
    }
}

// 验证反向登记三个输出时仍使用会话登记顺序，不依赖窗口句柄顺序或触发位置。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录独立会话的完整绘制次序。
TEST_F(OverlayPaintBatchTest, respects_reversed_registration_order)
{
    open_st::CaptureOverlaySession reversed;
    for (std::size_t index = this->windows_.size(); index > 0; --index)
    {
        reversed.Add(this->windows_[index - 1]);
    }
    DrawRecorder recorder;
    reversed.Invalidate();
    EXPECT_EQ(reversed.Paint(this->windows_[1], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn, (std::vector<HWND>{this->windows_[2], this->windows_[1], this->windows_[0]}));
    // 测试窗口仍由原会话持有记录；先将其 HWND 清空，避免跨两个会话重复管理。
    for (HWND window : this->windows_)
    {
        this->session_.Find(window)->window = nullptr;
    }
}

// 验证无触发不会主动绘制；空无效区域的有效触发仍消费一次内部绘制请求。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录静止与 RDW_INTERNALPAINT 等价入口的差异。
TEST_F(OverlayPaintBatchTest, skips_without_trigger_but_honors_internal_paint)
{
    DrawRecorder recorder;
    EXPECT_EQ(this->session_.Paint(nullptr, std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Skipped);
    EXPECT_TRUE(recorder.drawn.empty());
    EXPECT_EQ(this->session_.Paint(this->windows_[1], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn, (std::vector<HWND>{this->windows_[1]}));
}

// 验证独立系统失效区域进入同一批次，且在首个绘制回调前全批区域已经验证。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录暴露恢复绘制和预先验证边界。
TEST_F(OverlayPaintBatchTest, validates_entire_exposed_batch_before_first_draw)
{
    ASSERT_NO_FATAL_FAILURE(this->ShowRegionWindows());
    ASSERT_NE(InvalidateRect(this->windows_[0], nullptr, FALSE), FALSE);
    ASSERT_NE(InvalidateRect(this->windows_[2], nullptr, FALSE), FALSE);
    ASSERT_NE(GetUpdateRect(this->windows_[0], nullptr, FALSE), FALSE);
    ASSERT_NE(GetUpdateRect(this->windows_[2], nullptr, FALSE), FALSE);
    DrawRecorder recorder;
    // 在每个受控绘制边界查询真实 Windows 无效区域。
    // 入参：未使用的当前输出；捕获测试对象提供三个窗口。
    // 返回：无；断言验证收集批次已经整体清理。
    recorder.action = [this](open_st::CaptureOverlayOutput&)
    {
        for (HWND window : this->windows_)
        {
            EXPECT_EQ(GetUpdateRect(window, nullptr, FALSE), FALSE);
        }
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn, (std::vector<HWND>{this->windows_[0], this->windows_[2]}));
}

// 验证回调期间新的全屏失效保留到下一批，不被后续窗口验证吞掉。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录后续批次三屏仍完整绘制。
TEST_F(OverlayPaintBatchTest, preserves_invalidation_created_during_draw)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 只在第一屏追加一个会话失效请求。
    // 入参：output 为当前输出记录。
    // 返回：无；失效请求由会话记录。
    recorder.action = [this](open_st::CaptureOverlayOutput& output)
    {
        if (output.window == this->windows_[0])
        {
            this->session_.Invalidate();
        }
    };
    ASSERT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    recorder.action = {};
    recorder.drawn.clear();
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn.size(), 3U);
}

// 验证重入绘制消费消息但不嵌套回调，已消费的重入窗口仍获得下一批请求。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录重入结果、调用次数和后续区域。
TEST_F(OverlayPaintBatchTest, defers_reentrant_paint_without_losing_request)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    DrawRecorder nested;
    // 在第一屏绘制时模拟第二屏同步重入。
    // 入参：output 为外层当前输出。
    // 返回：无；断言验证内层不调用 renderer。
    recorder.action = [this, &nested](open_st::CaptureOverlayOutput& output)
    {
        if (output.window == this->windows_[0])
        {
            std::wstring nestedError;
            EXPECT_EQ(this->session_.Paint(this->windows_[1], std::ref(nested), {}, nestedError),
                      open_st::OverlayPaintResult::Skipped);
        }
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn.size(), 3U);
    EXPECT_TRUE(nested.drawn.empty());
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(nested), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(nested.drawn, (std::vector<HWND>{this->windows_[0], this->windows_[1]}));
}

// 验证第二屏失败立即结束批次并保留渲染器诊断，第三屏不会继续执行。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录失败位置和错误文本。
TEST_F(OverlayPaintBatchTest, stops_after_second_output_failure)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    recorder.failing = this->windows_[1];
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Failed);
    EXPECT_EQ(recorder.drawn.size(), 2U);
    EXPECT_EQ(this->error_, L"第二屏模拟失败。");
}

// 验证绘制回调中关闭只标记延迟回收，当前记录在回调返回前仍存活。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录批次中断和安全销毁结果。
TEST_F(OverlayPaintBatchTest, defers_close_until_draw_callback_returns)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 在首屏回调请求关闭并验证记录和窗口仍可访问。
    // 入参：output 为当前输出。
    // 返回：无；断言在外层批次回收前观察窗口存活。
    recorder.action = [this](open_st::CaptureOverlayOutput& output)
    {
        this->session_.Close();
        EXPECT_EQ(this->session_.Find(output.window), &output);
        EXPECT_NE(IsWindow(output.window), FALSE);
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Interrupted);
    EXPECT_EQ(recorder.drawn.size(), 1U);
    EXPECT_EQ(this->session_.Find(this->windows_[0]), nullptr);
    EXPECT_EQ(IsWindow(this->windows_[2]), FALSE);
}

// 验证首屏回调销毁下一屏时停止访问该窗口，不继续调用其绘制边界。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录失效窗口检测结果。
TEST_F(OverlayPaintBatchTest, detects_window_destroyed_during_previous_draw)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 销毁当前批次中的第二个隐藏窗口，模拟外部窗口失效。
    // 入参：未使用的首屏输出；捕获当前测试会话。
    // 返回：无；断言记录 Win32 销毁结果。
    recorder.action = [this](open_st::CaptureOverlayOutput&) { EXPECT_NE(DestroyWindow(this->windows_[1]), FALSE); };
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Failed);
    EXPECT_EQ(recorder.drawn.size(), 1U);
}

// 验证宿主中断在首屏后立即阻止其余输出继续绘制。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录 shouldStop 回调边界。
TEST_F(OverlayPaintBatchTest, honors_host_stop_after_callback)
{
    this->session_.Invalidate();
    bool stopped = false;
    DrawRecorder recorder;
    // 模拟首屏绘制引发宿主显示布局失效。
    // 入参：未使用的输出。
    // 返回：无；捕获布尔值改为中断状态。
    recorder.action = [&stopped](open_st::CaptureOverlayOutput&) { stopped = true; };
    // 查询宿主共享的关闭条件。
    // 入参：无。
    // 返回：首屏回调执行后为 true。
    const std::function<bool()> shouldStop = [&stopped]() { return stopped; };
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), shouldStop, this->error_),
              open_st::OverlayPaintResult::Interrupted);
    EXPECT_EQ(recorder.drawn.size(), 1U);
}

// 验证回调异常结束批次后可再次接受绘制，同时禁止回调中追加输出破坏固定批次。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录异常恢复和 Add 准入保护。
TEST_F(OverlayPaintBatchTest, restores_batch_state_after_exception_and_rejects_add)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 在批次中尝试追加输出，随后抛出受控异常。
    // 入参：未使用的当前输出。
    // 返回：无正常返回；异常由会话转换为失败。
    recorder.action = [this](open_st::CaptureOverlayOutput&)
    {
        EXPECT_THROW(this->session_.Add(nullptr), std::logic_error);
        throw std::runtime_error("Controlled draw exception.");
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Failed);
    recorder.action = {};
    recorder.drawn.clear();
    this->session_.Invalidate();
    EXPECT_EQ(this->session_.Paint(this->windows_[1], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn.size(), 3U);
}

// 验证空准备记录不会调用全桌面失效，正常输出仍能独立完成批次。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录空句柄查询与有效输出数量。
TEST_F(OverlayPaintBatchTest, ignores_null_preparation_records)
{
    this->session_.Add(nullptr);
    this->session_.Invalidate();
    DrawRecorder recorder;
    EXPECT_EQ(this->session_.Find(nullptr), nullptr);
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn.size(), 3U);
}

// 验证绘制期间来自系统的独立失效区域，不会在外批返回时被清除。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录没有内部 Invalidate 代次时仍保留窗口恢复请求。
TEST_F(OverlayPaintBatchTest, preserves_system_region_created_during_draw)
{
    ASSERT_NO_FATAL_FAILURE(this->ShowRegionWindows());
    this->session_.Invalidate();
    for (HWND window : this->windows_)
    {
        ASSERT_NE(GetUpdateRect(window, nullptr, FALSE), FALSE);
    }
    DrawRecorder recorder;
    // 首屏绘制期间单独使最后一屏失效，模拟暴露消息。
    // 入参：output 为当前输出。
    // 返回：无；断言验证真实 Windows API 成功。
    recorder.action = [this](open_st::CaptureOverlayOutput& output)
    {
        if (output.window == this->windows_[0])
        {
            EXPECT_NE(InvalidateRect(this->windows_[2], nullptr, FALSE), FALSE);
            EXPECT_NE(GetUpdateRect(this->windows_[2], nullptr, FALSE), FALSE);
        }
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[1], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_NE(GetUpdateRect(this->windows_[2], nullptr, FALSE), FALSE);
    recorder.action = {};
    recorder.drawn.clear();
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn, (std::vector<HWND>{this->windows_[2]}));
}

// 验证绘制回调改变当前窗口绑定时立即失败，即使 HWND 仍然存活也不继续后续屏。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录存活窗口身份变化的防护。
TEST_F(OverlayPaintBatchTest, detects_binding_change_after_draw)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 模拟窗口被重新绑定给另一宿主。
    // 入参：output 为当前窗口记录。
    // 返回：无；测试只修改本例隐藏窗口。
    recorder.action = [](open_st::CaptureOverlayOutput& output)
    { SetWindowLongPtrW(output.window, GWLP_USERDATA, 42); };
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Failed);
    EXPECT_EQ(recorder.drawn.size(), 1U);
}

// 验证关闭后抛出的绘制异常仍完成延迟回收，不留下批次锁或隐藏窗口。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言记录异常与关闭同时发生时的资源结局。
TEST_F(OverlayPaintBatchTest, releases_deferred_close_when_callback_throws)
{
    this->session_.Invalidate();
    DrawRecorder recorder;
    // 请求关闭后抛出受控异常，要求退出路径同样清理会话。
    // 入参：未使用的当前输出。
    // 返回：无正常返回；异常由会话处理。
    recorder.action = [this](open_st::CaptureOverlayOutput&)
    {
        this->session_.Close();
        throw std::runtime_error("Close followed by draw exception.");
    };
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Failed);
    EXPECT_EQ(this->session_.Find(this->windows_[0]), nullptr);
    EXPECT_EQ(IsWindow(this->windows_[2]), FALSE);
}

// 验证 BeginPaint 的真实背景擦除重入中新增失效，仍在当前批次完成后保留下一批请求。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言先核验擦除回调确实发生，再验证非触发输出继续参加下一批。
TEST_F(OverlayPaintBatchTest, preserves_invalidation_from_begin_paint_erase_callback)
{
    ASSERT_NO_FATAL_FAILURE(this->ShowRegionWindows());
    EraseHook hook(this->windows_[0]);
    ASSERT_EQ(GetPropW(this->windows_[0], ERASE_HOOK_PROPERTY), &hook);
    DrawRecorder recorder;
    // 仅由 BeginPaint 的 WM_ERASEBKGND 回调追加会话失效，不通过绘制闭包触发。
    // 入参：无；捕获测试会话和绘制记录器。
    // 返回：无；断言此时尚未执行任何输出绘制。
    hook.action = [this, &recorder]()
    {
        EXPECT_TRUE(recorder.drawn.empty());
        this->session_.Invalidate();
    };
    ASSERT_NE(InvalidateRect(this->windows_[0], nullptr, TRUE), FALSE);
    ASSERT_NE(GetUpdateRect(this->windows_[0], nullptr, FALSE), FALSE);
    ASSERT_EQ(hook.calls, 0U);
    ASSERT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    ASSERT_EQ(hook.calls, 1U);
    EXPECT_EQ(recorder.drawn.size(), 3U);
    recorder.drawn.clear();
    EXPECT_EQ(this->session_.Paint(this->windows_[2], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Completed);
    EXPECT_EQ(recorder.drawn, (std::vector<HWND>(this->windows_.begin(), this->windows_.end())));
}

// 验证 BeginPaint 同步背景擦除回调中关闭会话时，记录保留到回调返回且不执行任何绘制。
// 入参：无；测试宏参数标识用例。
// 返回：无；断言真实擦除前置、回调期记录存活以及外层返回后的统一回收。
TEST_F(OverlayPaintBatchTest, defers_close_from_begin_paint_erase_callback)
{
    ASSERT_NO_FATAL_FAILURE(this->ShowRegionWindows());
    EraseHook hook(this->windows_[0]);
    ASSERT_EQ(GetPropW(this->windows_[0], ERASE_HOOK_PROPERTY), &hook);
    DrawRecorder recorder;
    open_st::CaptureOverlayOutput* const first = this->session_.Find(this->windows_[0]);
    ASSERT_NE(first, nullptr);
    // 在背景擦除回调内部关闭会话并核验当前栈使用的记录仍保持有效。
    // 入参：无；捕获借用的输出记录和测试对象。
    // 返回：无；断言所有窗口直到回调结束仍存活。
    hook.action = [this, first, &recorder]()
    {
        EXPECT_TRUE(recorder.drawn.empty());
        this->session_.Close();
        EXPECT_EQ(this->session_.Find(this->windows_[0]), first);
        EXPECT_EQ(first->window, this->windows_[0]);
        for (HWND window : this->windows_)
        {
            EXPECT_NE(IsWindow(window), FALSE);
        }
    };
    ASSERT_NE(InvalidateRect(this->windows_[0], nullptr, TRUE), FALSE);
    ASSERT_NE(GetUpdateRect(this->windows_[0], nullptr, FALSE), FALSE);
    ASSERT_EQ(hook.calls, 0U);
    EXPECT_EQ(this->session_.Paint(this->windows_[0], std::ref(recorder), {}, this->error_),
              open_st::OverlayPaintResult::Interrupted);
    ASSERT_EQ(hook.calls, 1U);
    EXPECT_TRUE(recorder.drawn.empty());
    for (HWND window : this->windows_)
    {
        EXPECT_EQ(this->session_.Find(window), nullptr);
        EXPECT_EQ(IsWindow(window), FALSE);
    }
}
