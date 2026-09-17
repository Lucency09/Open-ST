// 独立链接样式弹窗替身，验证右键属性队列、修订和单元素事务，不显示真实窗口或写用户数据。

#include "annotation_interaction_controller.h"
#include "capture_annotation_state.h"
#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include "overlay_input_queue.h"

#include "annotation_style_dialog.h"
#include <annotation_document.h>
#include <app.h>
#include <array>
#include <gtest/gtest.h>

namespace open_st
{
namespace
{
struct PropertiesProbe
{
    int calls{};
    AnnotationStyle received{};
    bool width{};
    bool resolver{};
    std::function<std::wstring(std::string_view)> textResolver;
    AnnotationStyleResult result{AnnotationStyleResult::Cancelled};
    std::optional<AnnotationStyle> replacement;
    std::function<void(HWND)> duringDialog;
    AnnotationStylePreview preview;
    int textCalls{};
    AnnotationTextStyle receivedText;
    std::optional<AnnotationTextStyle> textReplacement;
    AnnotationTextStylePreview textPreview;
    int choiceCalls{};
    unsigned receivedChoice{};
    std::optional<unsigned> choiceReplacement;
    std::function<bool(unsigned)> choicePreview;
    std::vector<unsigned> choices;
};
PropertiesProbe probe;
} // namespace

// 替换原生属性弹窗显示，仅观察 App 固定的目标样式并按用例配置返回。
// 入参：owner 为宿主；value 为候选；allowLineWidth 为目标类型；resolver 为文本；error 为诊断；preview 为同步预览。
// 返回：配置的模态结果，不创建界面或写入设置。
AnnotationStyleResult ShowAnnotationStyleDialog(HWND owner, AnnotationStyle& value, bool allowLineWidth,
                                                const std::function<std::wstring(std::string_view)>& resolver,
                                                std::wstring& error, const AnnotationStylePreview& preview)
{
    ++probe.calls;
    EXPECT_TRUE(IsWindow(owner));
    probe.received = value;
    probe.width = allowLineWidth;
    probe.resolver = static_cast<bool>(resolver);
    probe.textResolver = resolver;
    probe.preview = preview;
    if (probe.duringDialog)
    {
        probe.duringDialog(owner);
    }
    probe.preview = {};
    EXPECT_TRUE(IsWindow(owner));
    if (probe.replacement)
    {
        value = *probe.replacement;
    }
    error = probe.result == AnnotationStyleResult::Failed ? L"模拟属性窗口失败" : L"";
    return probe.result;
}

// 替换文字属性入口，确保独立测试不会从同一产品对象文件拉入真实模态实现。
// 入参：owner：宿主；value/textStyle：候选；resolver/error：文本及诊断；preview：文字参数预览。
// 返回：配置的结果，始终不请求真实原位正文编辑。
AnnotationStyleResult ShowAnnotationTextStyleDialog(HWND owner, AnnotationStyle& value, AnnotationTextStyle& textStyle,
                                                    const std::function<std::wstring(std::string_view)>& resolver,
                                                    std::wstring& error, const AnnotationTextStylePreview& preview)
{
    ++probe.textCalls;
    EXPECT_TRUE(IsWindow(owner));
    probe.received = value;
    probe.receivedText = textStyle;
    probe.textResolver = resolver;
    probe.textPreview = preview;
    if (probe.duringDialog)
        probe.duringDialog(owner);
    probe.textPreview = {};
    if (probe.replacement)
        value = *probe.replacement;
    if (probe.textReplacement)
        textStyle = *probe.textReplacement;
    textStyle.editRequested = false;
    error = probe.result == AnnotationStyleResult::Failed ? L"模拟文字属性失败" : L"";
    return probe.result;
}

// 替换马赛克档位表单，验证 App 参数适配而不创建原生窗口。
// 入参：owner：宿主；value：当前值；choices：允许档位；titleKey/labelKey：文本键；resolver/error：文本和诊断；
// preview：同步候选准备回调。
// 返回：配置结果，是否提交仍由 App 和状态事务决定。
AnnotationStyleResult ShowAnnotationChoiceDialog(HWND owner, unsigned& value, std::span<const unsigned> choices,
                                                 std::string_view titleKey, std::string_view labelKey,
                                                 const std::function<std::wstring(std::string_view)>& resolver,
                                                 std::wstring& error, const std::function<bool(unsigned)>& preview)
{
    ++probe.choiceCalls;
    EXPECT_TRUE(IsWindow(owner));
    EXPECT_FALSE(titleKey.empty());
    EXPECT_FALSE(labelKey.empty());
    probe.receivedChoice = value;
    probe.choices.assign(choices.begin(), choices.end());
    probe.textResolver = resolver;
    probe.choicePreview = preview;
    if (probe.duringDialog)
        probe.duringDialog(owner);
    probe.choicePreview = {};
    if (probe.choiceReplacement)
        value = *probe.choiceReplacement;
    error = probe.result == AnnotationStyleResult::Failed ? L"模拟档位属性失败" : L"";
    return probe.result;
}

struct AppAnnotationTestAccess final
{
    // 按明确目标进入真实 App 属性编排，避免用字体栅格点猜测文字命中位置。
    // 入参：app：应用；id：当前文档目标。
    // 返回：无，仅表单边界由本文件替身替代。
    static void Edit(App& app, std::uint64_t id)
    {
        app.EditAnnotationElement(id, app.annotation_->Revision());
    }
    // 创建消息窗口、命令门禁和两个遮罩，准备轮廓与填充两个独立标注。
    // 入参：app 为独占应用；windows 接收借用遮罩句柄。
    // 返回：初始化成功为 true；不显示窗口或初始化真实桌面捕获。
    static bool Initialize(App& app, std::array<HWND, 2>& windows)
    {
        if (app.messageWindow_ == nullptr && !app.CreateMessageWindow())
        {
            return false;
        }
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({-1000, -1000, 1000, 1000});
        if (!app.selectionModel_->SelectRectangle({0, 0, 300, 300}))
        {
            return false;
        }
        if (!app.toolbarGate_)
        {
            app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        }
        app.annotation_ = std::make_unique<CaptureAnnotationState>();
        app.annotation_->SetTool(CaptureAnnotationTool::Rectangle);
        if (!app.annotation_->SetStyle({0x123456, 25, 5}) || !app.annotation_->BeginDraw({20, 20}, {0, 0, 300, 300}) ||
            app.annotation_->EndDraw({100, 100}) != AnnotationCommitResult::Committed)
        {
            return false;
        }
        app.annotation_->SetTool(CaptureAnnotationTool::FilledRectangle);
        if (!app.annotation_->SetStyle({0xABCDEF, 35, 3}) ||
            !app.annotation_->BeginDraw({120, 120}, {0, 0, 300, 300}) ||
            app.annotation_->EndDraw({180, 180}) != AnnotationCommitResult::Committed)
        {
            return false;
        }
        app.annotation_->SetTool(CaptureAnnotationTool::Arrow);
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = App::OverlayProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OpenST.AnnotationPropertiesTest";
        if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        app.overlayPreparing_ = true;
        for (HWND& window : windows)
        {
            window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"", WS_POPUP, 0,
                                     0, 300, 300, nullptr, nullptr, windowClass.hInstance, &app);
            if (window == nullptr)
            {
                app.overlayPreparing_ = false;
                return false;
            }
            app.overlaySession_->Add(window);
        }
        app.overlayPreparing_ = false;
        return !app.overlayInvalidated_;
    }

    // 借用当前标注状态以断言事务与默认样式。
    // 入参：app 为独占应用。
    // 返回：会话状态引用。
    static CaptureAnnotationState& State(App& app)
    {
        return *app.annotation_;
    }

    // 观察全部遮罩统一使用的实际绘制快照，区分临时属性预览与已提交文档。
    // 入参：app 为应用。
    // 返回：当前用于绘制的只读快照；已关闭会话应为空。
    static AnnotationSnapshot Drawing(const App& app)
    {
        return app.AnnotationForDrawing();
    }

    // 调用正式右键按下入口，固定窗口 DPI 和对象修订。
    // 入参：app 为应用；window 为遮罩；point 为物理点击位置。
    // 返回：无。
    static void Down(App& app, HWND window, PointI point)
    {
        app.BeginAnnotationPropertyClick(window, point);
    }

    // 推进正式右键拖动容差判定。
    // 入参：app 为应用；point 为当前物理位置。
    // 返回：无。
    static void Move(App& app, PointI point)
    {
        app.UpdateAnnotationPropertyClick(point);
    }

    // 调用正式右键抬起入口，实际投递属性请求。
    // 入参：app 为应用；point 为抬起位置。
    // 返回：本次投递后的 pending 序号。
    static std::uint64_t Up(App& app, PointI point)
    {
        app.EndAnnotationPropertyClick(point);
        return app.annotationInteraction_->PendingProperty();
    }

    // 取得实际投递到消息窗口的属性消息并交由系统分派。
    // 入参：app 为应用。
    // 返回：确实取得并分派消息时 true。
    static bool Pump(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 8, WM_APP + 8, PM_REMOVE))
        {
            return false;
        }
        DispatchMessageW(&message);
        return true;
    }

    // 模拟迟到消息进入生产分派，检查不能清除新 pending。
    // 入参：app 为应用；serial 为旧或当前消息序号。
    // 返回：无。
    static void Dispatch(App& app, std::uint64_t serial)
    {
        app.DispatchAnnotationProperties(serial);
    }

    // 查询当前属性请求序号。
    // 入参：app 为应用。
    // 返回：零表示没有请求。
    static std::uint64_t Pending(const App& app)
    {
        return app.annotationInteraction_->PendingProperty();
    }

    // 使工具栏代次失效而保留已经排队的系统消息。
    // 入参：app 为应用。
    // 返回：无。
    static void ChangeToken(App& app)
    {
        app.toolbarGate_->Invalidate();
    }

    // 通过真实编辑历史入口执行撤销或重做。
    // 入参：app 为应用；redo 为重做方向。
    // 返回：无。
    static void Restore(App& app, bool redo)
    {
        app.RestoreAnnotationEdit(redo);
    }

    // 清空或恢复正式选区，供无区域与有效区域准入对照。
    // 入参：app 为应用；selected 决定是否恢复测试选区。
    // 返回：无。
    static void Selection(App& app, bool selected)
    {
        app.selectionModel_->Reset();
        if (selected)
        {
            EXPECT_TRUE(app.selectionModel_->SelectRectangle({0, 0, 300, 300}));
        }
    }

    // 查询跨屏输入和输出共同使用的完成忙状态。
    // 入参：app 为应用。
    // 返回：模态保护存在为 true。
    static bool Busy(const App& app)
    {
        return app.CompletionBusy();
    }

    // 查询会话及标注是否尚未被释放。
    // 入参：app 为应用。
    // 返回：两者均存活为 true。
    static bool Active(const App& app)
    {
        return app.overlaySession_ != nullptr && app.annotation_ != nullptr;
    }

    // 读取正式 crop 验证属性事务不改变截图区域。
    // 入参：app 为应用。
    // 返回：当前裁剪矩形值副本。
    static RectI Crop(const App& app)
    {
        return app.selectionModel_->Snapshot().rectangle;
    }

    // 结束真实截图会话，保留进程消息窗口用于新会话旧消息测试。
    // 入参：app 为应用。
    // 返回：无。
    static void Close(App& app)
    {
        app.CloseOverlay();
    }

    // 临时替换投递目的句柄来制造 PostMessage 失败，不销毁实际消息窗口。
    // 入参：app 为应用；point 为有效点击的抬起位置。
    // 返回：投递后 pending 序号，失败应为零。
    static std::uint64_t FailPost(App& app, PointI point)
    {
        const HWND original = app.messageWindow_;
        app.messageWindow_ = reinterpret_cast<HWND>(static_cast<INT_PTR>(-7));
        EXPECT_FALSE(IsWindow(app.messageWindow_));
        app.EndAnnotationPropertyClick(point);
        app.messageWindow_ = original;
        return app.annotationInteraction_->PendingProperty();
    }
};
} // namespace open_st

namespace
{
using namespace open_st;
using Access = AppAnnotationTestAccess;
constexpr RectI CROP{0, 0, 300, 300};

class AnnotationPropertiesTest : public testing::Test
{
  protected:
    // 建立每例独立的真实窗口和内存文档。
    // 入参：无。
    // 返回：无；不使用真实弹窗或用户设置。
    void SetUp() override
    {
        probe = {};
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    }

    // 先销毁应用资源，再丢弃已解绑窗口的剩余线程消息。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        this->app_.reset();
        probe = {};
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    // 对同一位置执行匹配的右键按下和抬起，不直接调用属性编辑函数。
    // 入参：point 为裁剪中的物理位置。
    // 返回：生产入口生成的请求序号。
    std::uint64_t Click(PointI point)
    {
        Access::Down(*this->app_, this->windows_[0], point);
        return Access::Up(*this->app_, point);
    }

    std::unique_ptr<App> app_;
    std::array<HWND, 2> windows_{};
};

// 验证右键空白、区外、无选区和活动手势均不打开属性，也不执行原来的分层取消。
// 入参：无。
// 返回：无；已有文档、当前工具与活动草稿保持。
TEST_F(AnnotationPropertiesTest, irrelevant_right_clicks_never_cancel_selection_or_gesture)
{
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    EXPECT_EQ(this->Click({250, 250}), 0U);
    EXPECT_EQ(this->Click({-10, 20}), 0U);
    Access::Selection(*this->app_, false);
    EXPECT_EQ(this->Click({30, 20}), 0U);
    Access::Selection(*this->app_, true);
    ASSERT_TRUE(Access::State(*this->app_).BeginDraw({30, 30}, CROP));
    EXPECT_EQ(this->Click({30, 20}), 0U);
    EXPECT_TRUE(Access::State(*this->app_).Active());
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    EXPECT_EQ(Access::State(*this->app_).Tool(), CaptureAnnotationTool::Arrow);
    EXPECT_EQ(probe.calls, 0);
}

// 验证真实排队点击使用目标元素的颜色透明度，线宽开关取决于目标种类而非当前绘图工具。
// 入参：无。
// 返回：无；没有匹配 down 的 up 不生成请求，每个有效请求只显示一次。
TEST_F(AnnotationPropertiesTest, queued_click_opens_target_style_and_type)
{
    EXPECT_EQ(Access::Up(*this->app_, {150, 150}), 0U);
    ASSERT_NE(this->Click({150, 150}), 0U);
    EXPECT_EQ(probe.calls, 0);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.received.rgb, 0xABCDEFU);
    EXPECT_EQ(probe.received.transparency, 35U);
    EXPECT_FALSE(probe.width);
    EXPECT_TRUE(probe.resolver);
    Access::State(*this->app_).SetTool(CaptureAnnotationTool::FilledRectangle);
    ASSERT_NE(this->Click({30, 20}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 2);
    EXPECT_TRUE(probe.width);
    EXPECT_EQ(probe.received.rgb, 0x123456U);
    EXPECT_EQ(probe.received.transparency, 25U);
    EXPECT_EQ(probe.received.lineWidth, 5U);
}

// 验证取消、失败和无实际变化的确认都保留 redo 及单调修订，不产生空历史。
// 入参：无。
// 返回：无；原先撤销的第二元素仍可正常重做。
TEST_F(AnnotationPropertiesTest, cancelled_failed_and_unchanged_results_preserve_redo)
{
    Access::Restore(*this->app_, false);
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    const std::uint64_t revision = Access::State(*this->app_).Revision();
    // 在取消、失败或无变化确认之前先发布明显不同的有效预览。
    // 入参：未命名参数为同步弹窗 owner。
    // 返回：无；临时预览不应提前进入文档或历史。
    probe.duringDialog = [this, original](HWND)
    {
        ASSERT_TRUE(probe.preview);
        EXPECT_TRUE(probe.preview({0xFF9900, 75, 8}));
        EXPECT_EQ(Access::Drawing(*this->app_)->front().style.rgb, 0xFF9900U);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        if (probe.result == AnnotationStyleResult::Accepted)
        {
            EXPECT_TRUE(probe.preview(probe.received));
            EXPECT_EQ(Access::Drawing(*this->app_)->front().style.rgb, original->front().style.rgb);
        }
    };
    for (AnnotationStyleResult result :
         {AnnotationStyleResult::Cancelled, AnnotationStyleResult::Failed, AnnotationStyleResult::Accepted})
    {
        probe.result = result;
        ASSERT_NE(this->Click({30, 20}), 0U);
        ASSERT_TRUE(Access::Pump(*this->app_));
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        EXPECT_EQ(Access::Drawing(*this->app_), original);
        EXPECT_EQ(Access::State(*this->app_).Revision(), revision);
        EXPECT_TRUE(Access::State(*this->app_).CanRedo());
    }
    Access::Restore(*this->app_, true);
    EXPECT_EQ(Access::State(*this->app_).Committed()->size(), 2U);
}

// 验证一次属性确认只修改目标样式，保留其他元素、顺序、几何和工具默认值，且只需一次撤销重做。
// 入参：无。
// 返回：无；ID 及 crop 保持，事务可完整回退。
TEST_F(AnnotationPropertiesTest, accepted_change_is_one_edit_without_default_or_geometry_changes)
{
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    const AnnotationStyle defaults = Access::State(*this->app_).Style();
    probe.result = AnnotationStyleResult::Accepted;
    probe.replacement = AnnotationStyle{0xAA2200, 55, 8};
    // 多次临时变化后确认最后值，只允许最终确认形成一次历史。
    // 入参：未命名参数为同步弹窗 owner。
    // 返回：无。
    probe.duringDialog = [this, original](HWND)
    {
        ASSERT_TRUE(probe.preview);
        EXPECT_TRUE(probe.preview({0x00AA22, 15, 1}));
        EXPECT_TRUE(probe.preview({0xAA2200, 55, 8}));
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        EXPECT_EQ(Access::Drawing(*this->app_)->front().style.rgb, 0xAA2200U);
    };
    ASSERT_NE(this->Click({30, 20}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    const AnnotationSnapshot changed = Access::State(*this->app_).Committed();
    EXPECT_EQ(Access::Drawing(*this->app_), changed);
    ASSERT_EQ(changed->size(), 2U);
    EXPECT_EQ(changed->front().style.rgb, 0xAA2200U);
    EXPECT_EQ(changed->front().style.transparency, 55U);
    EXPECT_DOUBLE_EQ(changed->front().style.lineWidth, 8);
    for (std::size_t index = 0; index < 2; ++index)
    {
        EXPECT_EQ((*changed)[index].id, (*original)[index].id);
        EXPECT_EQ((*changed)[index].kind, (*original)[index].kind);
        EXPECT_DOUBLE_EQ((*changed)[index].origin.x, (*original)[index].origin.x);
        EXPECT_DOUBLE_EQ((*changed)[index].origin.y, (*original)[index].origin.y);
        EXPECT_DOUBLE_EQ((*changed)[index].extent.x, (*original)[index].extent.x);
        EXPECT_DOUBLE_EQ((*changed)[index].extent.y, (*original)[index].extent.y);
    }
    EXPECT_EQ(changed->back().style.rgb, original->back().style.rgb);
    EXPECT_EQ(changed->back().style.transparency, original->back().style.transparency);
    EXPECT_EQ(Access::State(*this->app_).Style().rgb, defaults.rgb);
    EXPECT_EQ(Access::State(*this->app_).Style().transparency, defaults.transparency);
    EXPECT_DOUBLE_EQ(Access::State(*this->app_).Style().lineWidth, defaults.lineWidth);
    const RectI crop = Access::Crop(*this->app_);
    EXPECT_EQ(crop.left, CROP.left);
    EXPECT_EQ(crop.top, CROP.top);
    EXPECT_EQ(crop.right, CROP.right);
    EXPECT_EQ(crop.bottom, CROP.bottom);
    Access::Restore(*this->app_, false);
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    Access::Restore(*this->app_, true);
    EXPECT_EQ(Access::State(*this->app_).Committed(), changed);
}

// 验证改为全透明保留对象 ID 和历史，同时后续右键命中跳过不可见对象。
// 入参：无。
// 返回：无；撤销后对象恢复可见属性及点击资格。
TEST_F(AnnotationPropertiesTest, fully_transparent_object_keeps_identity_and_can_be_undone)
{
    const std::uint64_t id = Access::State(*this->app_).Committed()->back().id;
    probe.result = AnnotationStyleResult::Accepted;
    probe.replacement = AnnotationStyle{0xABCDEF, 100, 3};
    ASSERT_NE(this->Click({150, 150}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed()->size(), 2U);
    EXPECT_EQ(Access::State(*this->app_).Committed()->back().id, id);
    EXPECT_EQ(Access::State(*this->app_).Committed()->back().style.transparency, 100U);
    EXPECT_EQ(this->Click({150, 150}), 0U);
    Access::Restore(*this->app_, false);
    EXPECT_EQ(Access::State(*this->app_).Committed()->back().id, id);
    EXPECT_EQ(Access::State(*this->app_).Committed()->back().style.transparency, 35U);
    EXPECT_NE(this->Click({150, 150}), 0U);
}

// 验证撤销回相同对象快照后修订仍递增，旧版本不能因 ABA 重新获得属性修改资格。
// 入参：无。
// 返回：无；拒绝旧修订不改变 redo 和当前文档。
TEST_F(AnnotationPropertiesTest, stale_revision_is_rejected_after_undo_returns_same_snapshot)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    const std::uint64_t id = original->front().id;
    ASSERT_TRUE(state.BeginObjectPreview(id, revision, CROP));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x990000, 25, 5}, {}, {}}));
    ASSERT_EQ(state.EndObjectPreview(CROP, true), AnnotationCommitResult::Committed);
    RectI restored{};
    ASSERT_TRUE(state.Restore(false, CROP, restored));
    ASSERT_EQ(state.Committed(), original);
    EXPECT_GT(state.Revision(), revision);
    EXPECT_FALSE(state.BeginObjectPreview(id, revision, CROP));
    EXPECT_EQ(state.Committed(), original);
    EXPECT_TRUE(state.CanRedo());
}

// 验证右键一旦超过拖动容差就永久撤销本次点击资格，移回原位置也不能打开属性。
// 入参：无。
// 返回：无；裁剪和对象文档保持。
TEST_F(AnnotationPropertiesTest, right_drag_cannot_rearm_by_returning_to_origin)
{
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    Access::Down(*this->app_, this->windows_[0], {30, 20});
    Access::Move(*this->app_, {90, 80});
    Access::Move(*this->app_, {30, 20});
    EXPECT_EQ(Access::Up(*this->app_, {30, 20}), 0U);
    EXPECT_EQ(probe.calls, 0);
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
}

// 验证按下或排队后的 token、文档修订变化均拒绝旧请求，不打开错误对象。
// 入参：无。
// 返回：无；有效的新点击仍能正常排队。
TEST_F(AnnotationPropertiesTest, token_and_revision_changes_reject_stale_clicks_and_messages)
{
    Access::Down(*this->app_, this->windows_[0], {30, 20});
    Access::ChangeToken(*this->app_);
    EXPECT_EQ(Access::Up(*this->app_, {30, 20}), 0U);
    ASSERT_NE(this->Click({30, 20}), 0U);
    Access::ChangeToken(*this->app_);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 0);
    ASSERT_NE(this->Click({30, 20}), 0U);
    CaptureAnnotationState& state = Access::State(*this->app_);
    ASSERT_TRUE(state.BeginObjectPreview(state.Committed()->front().id, state.Revision(), CROP));
    ASSERT_TRUE(state.UpdateObjectPreview({{0xFF0000, 25, 5}, {}, {}}));
    ASSERT_EQ(state.EndObjectPreview(CROP, true), AnnotationCommitResult::Committed);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 0);
    ASSERT_NE(this->Click({30, 20}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 1);
}

// 验证截图关闭并新建会话后旧系统消息不能清除新 pending，即使对象 ID 在新文档中重用。
// 入参：无。
// 返回：无；只有新序号匹配的消息打开属性。
TEST_F(AnnotationPropertiesTest, old_session_message_does_not_consume_new_request)
{
    const std::uint64_t old = this->Click({30, 20});
    ASSERT_NE(old, 0U);
    Access::Close(*this->app_);
    ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    const std::uint64_t current = this->Click({150, 150});
    ASSERT_NE(current, 0U);
    ASSERT_NE(current, old);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Pending(*this->app_), current);
    EXPECT_EQ(probe.calls, 0);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.received.rgb, 0xABCDEFU);
}

// 验证属性模态期间另一屏输入与有效撤销入口均被阻止，显示失效和关闭延迟到返回后回收。
// 入参：无；显示与关闭两种失效分别执行，替身不创建真实窗口。
// 返回：无；模态返回后候选不提交到已失效会话。
TEST_F(AnnotationPropertiesTest, modal_busy_blocks_undo_and_defers_cross_overlay_invalidation)
{
    for (UINT message : {WM_DISPLAYCHANGE, WM_CLOSE})
    {
        if (!Access::Active(*this->app_))
        {
            ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
        }
        const AnnotationSnapshot original = Access::State(*this->app_).Committed();
        ASSERT_TRUE(Access::State(*this->app_).CanUndo());
        probe.result = AnnotationStyleResult::Accepted;
        probe.replacement = AnnotationStyle{0, 0, 3};
        // 在真实 App 模态栈内模拟另一屏消息，直接撤销确保测试有效业务前提。
        // 入参：owner 为弹窗 owner；捕获对象均只在本次同步模态期间借用。
        // 返回：无；不假设 Z 消息携带真实 Ctrl 修饰键。
        probe.duringDialog = [this, original, message](HWND owner)
        {
            EXPECT_TRUE(Access::Busy(*this->app_));
            const HWND other = owner == this->windows_[0] ? this->windows_[1] : this->windows_[0];
            SendMessageW(other, WM_LBUTTONDOWN, MK_LBUTTON, 0);
            SendMessageW(other, WM_LBUTTONUP, 0, 0);
            SendMessageW(other, WM_KEYDOWN, 'Z', 0);
            Access::Restore(*this->app_, false);
            EXPECT_EQ(Access::State(*this->app_).Committed(), original);
            EXPECT_FALSE(Access::State(*this->app_).CanUndo());
            EXPECT_TRUE(Access::State(*this->app_).PreviewingObject());
            EXPECT_FALSE(Access::State(*this->app_).Drawing());
            SendMessageW(other, message, 0, 0);
            EXPECT_TRUE(Access::Active(*this->app_));
            EXPECT_TRUE(IsWindow(this->windows_[0]));
            EXPECT_TRUE(IsWindow(this->windows_[1]));
        };
        ASSERT_NE(this->Click({30, 20}), 0U);
        ASSERT_TRUE(Access::Pump(*this->app_));
        EXPECT_FALSE(Access::Busy(*this->app_));
        EXPECT_FALSE(Access::Active(*this->app_));
        EXPECT_FALSE(IsWindow(this->windows_[0]));
        EXPECT_FALSE(IsWindow(this->windows_[1]));
    }
    EXPECT_EQ(probe.calls, 2);
}

// 验证属性消息投递失败解除 pending，恢复有效句柄后新点击可以重试。
// 入参：无；只临时替换句柄值，不销毁或解绑真实 App 消息窗口。
// 返回：无；旧失败序号不能消费新的有效请求。
TEST_F(AnnotationPropertiesTest, post_failure_releases_pending_and_allows_retry)
{
    Access::Down(*this->app_, this->windows_[0], {30, 20});
    EXPECT_EQ(Access::FailPost(*this->app_, {30, 20}), 0U);
    ASSERT_NE(this->Click({30, 20}), 0U);
    const std::uint64_t current = Access::Pending(*this->app_);
    ASSERT_GT(current, 1U);
    Access::Dispatch(*this->app_, current - 1);
    EXPECT_EQ(Access::Pending(*this->app_), current);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 1);
}

// 验证真实遮罩右键路由不再执行 Esc 分层取消，空闲和活动草稿均保留工具、文档及裁剪。
// 入参：无；系统鼠标位置可能命中或不命中元素，本例不假设命中结果。
// 返回：无；只验证右键消息本身不会清空选区、切工具或回滚已有手势。
TEST_F(AnnotationPropertiesTest, overlay_right_button_messages_preserve_tool_crop_and_active_gesture)
{
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    const CaptureAnnotationTool tool = Access::State(*this->app_).Tool();
    for (bool active : {false, true})
    {
        if (active)
        {
            ASSERT_TRUE(Access::State(*this->app_).BeginDraw({40, 40}, CROP));
            ASSERT_TRUE(Access::State(*this->app_).UpdateDraw({80, 90}));
        }
        const AnnotationSnapshot preview = Access::State(*this->app_).Preview();
        SendMessageW(this->windows_[0], WM_RBUTTONDOWN, MK_RBUTTON, 0);
        SendMessageW(this->windows_[0], WM_RBUTTONUP, 0, 0);
        ASSERT_TRUE(Access::Active(*this->app_));
        EXPECT_EQ(Access::State(*this->app_).Tool(), tool);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        EXPECT_EQ(Access::State(*this->app_).Preview(), preview);
        EXPECT_EQ(Access::State(*this->app_).Active(), active);
        const RectI crop = Access::Crop(*this->app_);
        EXPECT_EQ(crop.left, CROP.left);
        EXPECT_EQ(crop.top, CROP.top);
        EXPECT_EQ(crop.right, CROP.right);
        EXPECT_EQ(crop.bottom, CROP.bottom);
    }
    EXPECT_EQ(probe.calls, 0);
}

// 验证离开、取消模式和失焦通过真实窗口过程撤销尚未抬起的右键点击，已排队请求不被离开清除。
// 入参：无；有效 down 使用显式元素位置，后续取消使用真实 Win32 消息。
// 返回：无；取消后的 up 不排队，已排队请求仍可正常显示属性。
TEST_F(AnnotationPropertiesTest, window_cancellation_discards_armed_click_but_preserves_queued_request)
{
    for (UINT message : {WM_MOUSELEAVE, WM_CANCELMODE, WM_KILLFOCUS})
    {
        Access::Down(*this->app_, this->windows_[0], {30, 20});
        SendMessageW(this->windows_[0], message, 0, 0);
        EXPECT_EQ(Access::Up(*this->app_, {30, 20}), 0U);
    }
    EXPECT_EQ(probe.calls, 0);
    const std::uint64_t request = this->Click({30, 20});
    ASSERT_NE(request, 0U);
    SendMessageW(this->windows_[0], WM_MOUSELEAVE, 0, 0);
    EXPECT_EQ(Access::Pending(*this->app_), request);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(probe.calls, 1);
}

// 验证连续属性预览更新实际绘制快照，但不改变已提交文档、修订、redo、其他对象或工具默认值。
// 入参：无；预先构造可重做编辑，并保留首个预览引用检查后续更新没有原地修改它。
// 返回：无；取消后绘制回退原文档，旧 redo 仍可恢复。
TEST_F(AnnotationPropertiesTest, successive_previews_are_immutable_and_leave_document_and_history_unchanged)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    ASSERT_TRUE(state.BeginObjectPreview(state.Committed()->back().id, state.Revision(), CROP));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x111111, 35, 3}, {}, {}}));
    ASSERT_EQ(state.EndObjectPreview(CROP, true), AnnotationCommitResult::Committed);
    Access::Restore(*this->app_, false);
    ASSERT_TRUE(state.CanRedo());
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    const AnnotationStyle defaults = state.Style();
    // 同步模拟三个有效输入事件，比较渲染快照与固定原文档。
    // 入参：未命名参数为弹窗 owner。
    // 返回：无；每次预览只改变目标样式。
    probe.duringDialog = [this, original, revision, defaults](HWND)
    {
        ASSERT_TRUE(probe.preview);
        ASSERT_TRUE(probe.preview({0xAA0000, 10, 1}));
        const AnnotationSnapshot first = Access::Drawing(*this->app_);
        ASSERT_NE(first, nullptr);
        for (const AnnotationStyle candidate : {AnnotationStyle{0x00BB00, 50, 5}, AnnotationStyle{0x0000CC, 90, 8}})
        {
            ASSERT_TRUE(probe.preview(candidate));
            const AnnotationSnapshot drawing = Access::Drawing(*this->app_);
            ASSERT_NE(drawing, nullptr);
            ASSERT_EQ(drawing->size(), original->size());
            EXPECT_EQ(drawing->front().style.rgb, candidate.rgb);
            EXPECT_EQ(drawing->front().style.transparency, candidate.transparency);
            EXPECT_DOUBLE_EQ(drawing->front().style.lineWidth, candidate.lineWidth);
            EXPECT_EQ(drawing->front().id, original->front().id);
            EXPECT_DOUBLE_EQ(drawing->front().origin.x, original->front().origin.x);
            EXPECT_DOUBLE_EQ(drawing->front().origin.y, original->front().origin.y);
            EXPECT_DOUBLE_EQ(drawing->front().extent.x, original->front().extent.x);
            EXPECT_DOUBLE_EQ(drawing->front().extent.y, original->front().extent.y);
            EXPECT_EQ(drawing->back().id, original->back().id);
            EXPECT_EQ(drawing->back().style.rgb, original->back().style.rgb);
            EXPECT_EQ(drawing->back().style.transparency, original->back().style.transparency);
            EXPECT_DOUBLE_EQ(drawing->back().style.lineWidth, original->back().style.lineWidth);
            EXPECT_EQ(Access::State(*this->app_).Committed(), original);
            EXPECT_EQ(Access::State(*this->app_).Revision(), revision);
            EXPECT_FALSE(Access::State(*this->app_).CanRedo());
            EXPECT_TRUE(Access::State(*this->app_).PreviewingObject());
            EXPECT_EQ(Access::State(*this->app_).Style().rgb, defaults.rgb);
            EXPECT_EQ(Access::State(*this->app_).Style().transparency, defaults.transparency);
            EXPECT_DOUBLE_EQ(Access::State(*this->app_).Style().lineWidth, defaults.lineWidth);
        }
        EXPECT_EQ(first->front().style.rgb, 0xAA0000U);
        EXPECT_EQ(first->front().style.transparency, 10U);
        EXPECT_DOUBLE_EQ(first->front().style.lineWidth, 1);
    };
    ASSERT_NE(this->Click({30, 20}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Drawing(*this->app_), original);
    EXPECT_EQ(state.Revision(), revision);
    Access::Restore(*this->app_, true);
    EXPECT_EQ(state.Committed()->back().style.rgb, 0x111111U);
}

// 验证全透明属性预览不删除目标，仍能在同一次弹窗中恢复可见透明度并取消回退。
// 入参：无；目标为填充元素，当前绘图工具保持不变。
// 返回：无；提交文档及 ID 始终保留。
TEST_F(AnnotationPropertiesTest, fully_transparent_preview_can_be_adjusted_back_before_cancel)
{
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    // 将目标先设为不可见再调回，模拟同一滑条会话中的连续有效输入。
    // 入参：未命名参数为弹窗 owner。
    // 返回：无；预览期间对象始终存在。
    probe.duringDialog = [this, original](HWND)
    {
        ASSERT_TRUE(probe.preview);
        ASSERT_TRUE(probe.preview({0xABCDEF, 100, 3}));
        ASSERT_EQ(Access::Drawing(*this->app_)->size(), 2U);
        EXPECT_EQ(Access::Drawing(*this->app_)->back().id, original->back().id);
        EXPECT_EQ(Access::Drawing(*this->app_)->back().style.transparency, 100U);
        ASSERT_TRUE(probe.preview({0xABCDEF, 20, 3}));
        EXPECT_EQ(Access::Drawing(*this->app_)->back().id, original->back().id);
        EXPECT_EQ(Access::Drawing(*this->app_)->back().style.transparency, 20U);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    };
    ASSERT_NE(this->Click({150, 150}), 0U);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Drawing(*this->app_), original);
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
}

// 验证显示失效或关闭后预览回调拒绝继续发布，弹窗返回清理临时快照及整个会话。
// 入参：无；分别模拟另一屏显示变化和关闭，确认返回也不能提交失效候选。
// 返回：无；所有遮罩销毁后绘制入口不再返回属性预览。
TEST_F(AnnotationPropertiesTest, invalidated_session_rejects_preview_and_releases_temporary_snapshot)
{
    for (UINT message : {WM_DISPLAYCHANGE, WM_CLOSE})
    {
        if (!Access::Active(*this->app_))
        {
            ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
        }
        const AnnotationSnapshot original = Access::State(*this->app_).Committed();
        probe.result = AnnotationStyleResult::Accepted;
        probe.replacement = AnnotationStyle{0, 0, 3};
        // 在成功预览后使另一屏失效，再尝试更新同一临时属性。
        // 入参：owner 为真实属性窗口 owner。
        // 返回：无；已提交文档必须在整个同步回调内保持原值。
        probe.duringDialog = [this, original, message](HWND owner)
        {
            ASSERT_TRUE(probe.preview);
            ASSERT_TRUE(probe.preview({0xFFAA00, 10, 3}));
            EXPECT_EQ(Access::Drawing(*this->app_)->front().style.rgb, 0xFFAA00U);
            const HWND other = owner == this->windows_[0] ? this->windows_[1] : this->windows_[0];
            SendMessageW(other, message, 0, 0);
            EXPECT_FALSE(probe.preview({0x00FFAA, 50, 8}));
            ASSERT_TRUE(Access::Active(*this->app_));
            EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        };
        ASSERT_NE(this->Click({30, 20}), 0U);
        ASSERT_TRUE(Access::Pump(*this->app_));
        EXPECT_FALSE(Access::Active(*this->app_));
        EXPECT_EQ(Access::Drawing(*this->app_), nullptr);
        EXPECT_FALSE(IsWindow(this->windows_[0]));
        EXPECT_FALSE(IsWindow(this->windows_[1]));
    }
}

// 验证文字属性入口读取目标字号，实时预览不改默认值，确认只产生一条可撤销历史。
// 入参：无。
// 返回：App 适配的种类专属参数、正文共享、其他对象与历史断言。
TEST_F(AnnotationPropertiesTest, text_parameters_preview_and_commit_as_one_edit)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({200, 20}, CROP));
    ASSERT_TRUE(state.UpdateText(u"文字属性"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    ASSERT_TRUE(state.SetTextFontSize(32U));
    state.SetTool(CaptureAnnotationTool::Arrow);
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t id = original->back().id;
    const std::uint64_t revision = state.Revision();
    const AnnotationStyle defaults = state.Style();
    probe.result = AnnotationStyleResult::Accepted;
    probe.replacement = AnnotationStyle{0x654321U, 60U, original->back().style.lineWidth};
    probe.textReplacement = AnnotationTextStyle{48U, true, false};
    // 多次字号预览后确认，正文和旧对象始终使用原共享载荷。
    // 入参：owner：同步属性窗口宿主。
    // 返回：无。
    probe.duringDialog = [this, original](HWND owner)
    {
        EXPECT_TRUE(IsWindow(owner));
        EXPECT_EQ(probe.receivedText.fontSize, 24U);
        EXPECT_TRUE(probe.receivedText.allowEdit);
        EXPECT_FALSE(probe.receivedText.editRequested);
        ASSERT_TRUE(probe.textPreview);
        EXPECT_TRUE(probe.textPreview(probe.received, 32U));
        EXPECT_TRUE(probe.textPreview(*probe.replacement, 48U));
        const AnnotationSnapshot drawing = Access::Drawing(*this->app_);
        EXPECT_EQ(std::get<AnnotationText>(drawing->back().payload).fontSize, 48.0);
        EXPECT_EQ(std::get<AnnotationText>(drawing->back().payload).text,
                  std::get<AnnotationText>(original->back().payload).text);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        EXPECT_EQ(Access::State(*this->app_).TextFontSize(), 32U);
    };
    Access::Edit(*this->app_, id);
    EXPECT_EQ(probe.textCalls, 1);
    EXPECT_EQ(probe.calls, 0);
    const AnnotationSnapshot changed = state.Committed();
    EXPECT_EQ(state.Revision(), revision + 1U);
    EXPECT_EQ(std::get<AnnotationText>(changed->back().payload).fontSize, 48.0);
    EXPECT_EQ(changed->back().style.rgb, 0x654321U);
    EXPECT_EQ(changed->back().style.transparency, 60U);
    EXPECT_EQ(changed->front().id, original->front().id);
    EXPECT_EQ(changed->front().style.rgb, original->front().style.rgb);
    EXPECT_EQ(state.TextFontSize(), 32U);
    EXPECT_EQ(state.Style().rgb, defaults.rgb);
    EXPECT_EQ(state.Style().transparency, defaults.transparency);
    EXPECT_EQ(state.Tool(), CaptureAnnotationTool::Arrow);
    Access::Restore(*this->app_, false);
    EXPECT_EQ(state.Committed(), original);
    Access::Restore(*this->app_, true);
    EXPECT_EQ(state.Committed(), changed);
}

// 验证文字属性取消与失败回收统一预览，不提交字号或破坏已有 redo 分支。
// 入参：无。
// 返回：模态退出恢复、原字号和默认值保持断言。
TEST_F(AnnotationPropertiesTest, text_parameter_cancel_and_failure_preserve_redo)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({200, 20}, CROP));
    ASSERT_TRUE(state.UpdateText(u"保留正文"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t id = original->back().id;
    ASSERT_TRUE(state.BeginObjectPreview(id, state.Revision(), CROP));
    AnnotationProperties properties = PropertiesOf(original->back());
    properties.style = {0U, 40U, 3.0};
    ASSERT_TRUE(state.UpdateObjectPreview(properties));
    ASSERT_EQ(state.EndObjectPreview(CROP, true), AnnotationCommitResult::Committed);
    RectI restored;
    ASSERT_TRUE(state.Restore(false, CROP, restored));
    ASSERT_EQ(state.Committed(), original);
    const std::uint64_t revision = state.Revision();
    // 先发布有效字号预览，随后由配置的表单结果取消或报错。
    // 入参：未命名参数为宿主。
    // 返回：无。
    probe.duringDialog = [this, original](HWND)
    {
        ASSERT_TRUE(probe.textPreview);
        EXPECT_TRUE(probe.textPreview(probe.received, 48U));
        EXPECT_EQ(std::get<AnnotationText>(Access::Drawing(*this->app_)->back().payload).fontSize, 48.0);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    };
    for (AnnotationStyleResult result : {AnnotationStyleResult::Cancelled, AnnotationStyleResult::Failed})
    {
        probe.result = result;
        Access::Edit(*this->app_, id);
        EXPECT_EQ(state.Committed(), original);
        EXPECT_EQ(Access::Drawing(*this->app_), original);
        EXPECT_EQ(state.Revision(), revision);
        EXPECT_TRUE(state.CanRedo());
    }
    EXPECT_EQ(probe.textCalls, 2);
}

// 验证马赛克档位表单只改目标块大小，取消不改文档，确认一条历史且工具默认档位不变。
// 入参：无。
// 返回：四档参数适配、预览、默认值及撤销恢复断言。
TEST_F(AnnotationPropertiesTest, mosaic_block_preview_is_independent_and_commits_once)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    state.SetTool(CaptureAnnotationTool::Mosaic);
    ASSERT_TRUE(state.BeginDraw({200, 100}, CROP));
    ASSERT_EQ(state.EndDraw({250, 150}), AnnotationCommitResult::Committed);
    ASSERT_TRUE(state.SetMosaicBlockSize(32U));
    state.SetTool(CaptureAnnotationTool::Arrow);
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t id = original->back().id;
    const std::uint64_t revision = state.Revision();
    // 预览细档位，目标原值来自对象而非当前工具默认值。
    // 入参：未命名参数为宿主。
    // 返回：无。
    probe.duringDialog = [this, original](HWND)
    {
        EXPECT_EQ(probe.receivedChoice, 16U);
        EXPECT_EQ(probe.choices, (std::vector<unsigned>{4U, 8U, 16U, 32U}));
        ASSERT_TRUE(probe.choicePreview);
        EXPECT_TRUE(probe.choicePreview(4U));
        EXPECT_EQ(std::get<AnnotationMosaic>(Access::Drawing(*this->app_)->back().payload).blockSize, 4U);
        EXPECT_EQ(Access::State(*this->app_).Committed(), original);
        EXPECT_EQ(Access::State(*this->app_).MosaicBlockSize(), 32U);
    };
    probe.result = AnnotationStyleResult::Cancelled;
    Access::Edit(*this->app_, id);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Revision(), revision);
    probe.result = AnnotationStyleResult::Accepted;
    probe.choiceReplacement = 4U;
    Access::Edit(*this->app_, id);
    EXPECT_EQ(probe.choiceCalls, 2);
    EXPECT_EQ(probe.calls, 0);
    const AnnotationSnapshot changed = state.Committed();
    EXPECT_EQ(std::get<AnnotationMosaic>(changed->back().payload).blockSize, 4U);
    EXPECT_EQ(changed->back().style.transparency, 0U);
    EXPECT_EQ(changed->back().origin.x, original->back().origin.x);
    EXPECT_EQ(changed->back().extent.y, original->back().extent.y);
    EXPECT_EQ(changed->front().style.rgb, original->front().style.rgb);
    EXPECT_EQ(state.MosaicBlockSize(), 32U);
    EXPECT_EQ(state.Revision(), revision + 1U);
    Access::Restore(*this->app_, false);
    EXPECT_EQ(state.Committed(), original);
}
} // namespace
