// 管理本次截图显示参数、实时预览请求及非模态设置面板，不写默认设置。
#pragma once
#include <geometry.h>
#include <native_tone_mapper.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
class FrozenDesktopFrame;
class SdrSelectionFrame;
class SelectionPreviewWorker;
class WindowRenderer;
struct CaptureVisualDefaults
{
    unsigned brightness{};
    unsigned mask{};
    HdrToneMappingOptions toneMapping{};
};
// 校验截图百分比设置，复用图形领域规则。
// 入参：key 为完整设置键；value 为整数。返回：合法为 true。
bool IsCaptureVisualSetting(std::string_view key, std::int64_t value) noexcept;
// 读取用户优先的默认参数；百分比沿用默认回退，映射策略非法则拒绝。
// 入参：无。返回：完整有效的会话快照，策略或资源无效时为空。
std::optional<CaptureVisualDefaults> ReadCaptureVisualDefaults() noexcept;
class CaptureVisualSession final
{
  public:
    // 创建唯一预览工作线程；通知仅用于投递 UI 消息。
    // 入参：wake 为线程安全唤醒回调。返回：无。
    explicit CaptureVisualSession(std::function<void()> wake);
    // 取消并等待工作线程，宿主须保持通知窗口存活到析构完成。
    // 入参：无。返回：无。
    ~CaptureVisualSession();
    // 禁止复制拥有的窗口和工作线程。
    // 入参：源对象。返回：已删除。
    CaptureVisualSession(const CaptureVisualSession&) = delete;
    // 禁止赋值拥有的窗口和工作线程。
    // 入参：源对象。返回：已删除。
    CaptureVisualSession& operator=(const CaptureVisualSession&) = delete;
    // 绑定不可变冻结帧及默认快照，不共享可修改参数状态。
    // 入参：frame 为共享只读来源；defaults 为已校验默认；changed 通知宿主请求更新。返回：无。
    void Begin(std::shared_ptr<const FrozenDesktopFrame> frame, CaptureVisualDefaults defaults,
               std::function<void()> changed);
    // 立即取消资格并收起面板；后台仍安全持有正在使用的输入。
    // 入参：无。返回：无，不等待后台。
    void End() noexcept;
    // 查询本次唯一亮度值。
    // 入参：无。返回：百分比。
    unsigned Brightness() const noexcept;
    // 查询本次唯一遮罩值。
    // 入参：无。返回：百分比。
    unsigned Mask() const noexcept;
    // 查询截图开始时冻结的策略。
    // 入参：无。返回：会话策略。
    HdrToneMappingOptions ToneMappingOptions() const noexcept;
    // 查询选区是否包含可调节的原生 HDR 像素。
    // 入参：selection 为正式物理选区。返回：至少一块 HDR 来源时为 true。
    bool HasHdr(RectI selection) const noexcept;
    // 提交最新选区及参数；重复目标不重复转换。
    // 入参：selection 为物理选区。返回：已接受请求或相同请求为 true。
    bool Request(RectI selection);
    // 消费最新版本结果，过期结果不发布。
    // 入参：error 接收本次失败。返回：有新成功结果为 true，失败写 error。
    bool Poll(std::wstring& error);
    // 借用当前版本自有底图，待更新时为空。
    // 入参：无。返回：只读借用，不转移所有权。
    const SdrSelectionFrame* Preview() const noexcept;
    // 共享当前不可变底图，马赛克缓存可安全持有其寿命。
    // 入参：无。返回：同一版本的共享只读底图。
    std::shared_ptr<const SdrSelectionFrame> SharedPreview() const noexcept;
    // 为未完成底图保留唯一输出动作，宿主暂停输入直到发布。
    // 入参：action 为输出回调。返回：底图仍待处理且成功保留时为 true。
    bool DeferOutput(std::function<void()> action);
    // 查询是否存在等待最新底图的输出动作。
    // 入参：无。返回：已保留动作时为 true。
    bool HasDeferredOutput() const noexcept;
    // 接管唯一输出动作，之后结束会话不会再次调用。
    // 入参：无。返回：待完成动作或空。
    std::function<void()> TakeDeferredOutput() noexcept;
    // 查询仍在读取输入的后台任务。
    // 入参：无。返回：存在执行或待处理任务为 true。
    bool HasWork() const noexcept;
    // 查询当前目标版本，宿主据此只在变化时请求绘制。
    // 入参：无。返回：单调请求身份。
    std::uint64_t Version() const noexcept;
    // 打开或激活公共非模态面板。
    // 入参：owner/anchor 为借用窗口；icon 借用图标；text 为本地化入口。返回：成功为 true；无会话返回
    // false，公共渲染失败抛诊断。
    bool Show(HWND owner, HWND anchor, HICON icon, std::function<std::wstring(std::string_view)> text);
    // 转交属于面板的键盘消息。
    // 入参：message 为线程消息。返回：已消费为 true。
    bool Process(MSG& message);
    // 暂停面板交互，模态业务结束后恢复。
    // 入参：enabled 为可用状态。返回：无。
    void SetInteractive(bool enabled);
    // 查询当前面板句柄以协调层级。
    // 入参：无。返回：借用 HWND，关闭时为空。
    HWND Window() const noexcept;

  private:
    // 应用面板合法输入，只更新本次参数并通知宿主。
    // 入参：brightness/mask 为候选。返回：接受时为 true。
    bool Change(unsigned brightness, unsigned mask);
    // 更新面板状态和允许项，保留原生输入缓冲。
    // 入参：无。返回：无。
    void RefreshPanel();
    std::unique_ptr<SelectionPreviewWorker> worker_;
    std::unique_ptr<WindowRenderer> panel_;
    std::shared_ptr<const FrozenDesktopFrame> frame_;
    std::shared_ptr<const SdrSelectionFrame> preview_;
    CaptureVisualDefaults defaults_{};
    unsigned brightness_{}, mask_{};
    std::uint64_t serial_{}, requested_{};
    std::optional<RectI> selection_;
    std::function<void()> changed_;
    std::function<void()> deferredOutput_;
    std::function<std::wstring(std::string_view)> text_;
    bool hdr_{}, pending_{}, interactive_{true};
};
} // namespace open_st
