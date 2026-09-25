// 声明截图会话和OCR结果的生命周期协调，不向核心传入App或窗口对象。
#pragma once
#include <memory>
#include <string>
#include <windows.h>
namespace open_st
{
class SdrSelectionFrame;
inline constexpr UINT OcrWakeMessage = WM_APP + 141;
inline constexpr UINT_PTR OcrPollTimer = 141;
class OcrSession final
{
  public:
    // 建立延迟加载的协调器。
    // 入参：owner为应用消息窗口；icon为借用图标。返回：尚未识别的对象。
    OcrSession(HWND owner, HICON icon);
    // 回收会话；正常退出须先等后台结束。
    // 入参：无。返回：无。
    ~OcrSession();
    // 接收本次正式输出及固定设置。
    // 入参：frame为同步借用图像；model/language为任务参数。返回：无，失败在结果窗口显示。
    void Begin(const SdrSelectionFrame& frame, const std::string& model, const std::string& language);
    // 激活已有结果，防止再次生成图像。
    // 入参：无。返回：已有结果窗口为true。
    bool ActivateExisting() noexcept;
    // 关闭当前结果并取消请求，后台对象继续存活。
    // 入参：无。返回：无。
    void EndCapture() noexcept;
    // 查询并分派新快照，补收丢失通知。
    // 入参：无。返回：无。
    void Poll() noexcept;
    // 转交非模态窗口输入。
    // 入参：message为主循环消息。返回：消费为true。
    bool Process(MSG& message);
    // 转交系统注册的编辑组合。
    // 入参：modifiers/key为已核验热键。返回：消费为true。
    bool RegisteredHotkey(UINT modifiers, UINT key) noexcept;
    // 开始取消退出，UI继续分派消息。
    // 入参：无。返回：无。
    void Shutdown() noexcept;
    // 查询后台释放完成。
    // 入参：无。返回：可以析构为true。
    bool ShutdownComplete() const noexcept;
    // 将宿主模态准入状态同步到结果窗口。
    // 入参：paused为暂停标志。返回：无。
    void Pause(bool paused) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
