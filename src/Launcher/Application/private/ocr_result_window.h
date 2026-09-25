// 声明识别结果的可编辑草稿和公共窗口绑定，不持有识别引擎。
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
class WindowRenderer;
class OcrResultWindow final
{
  public:
    // 建立结果窗口草稿；不创建原生窗口。
    // 入参：无。返回：空窗口对象。
    OcrResultWindow();
    // 回收公共窗口及草稿。
    // 入参：无。返回：无。
    ~OcrResultWindow();
    // 创建非模态结果窗口，取消通过窄回调交给协调器。
    // 入参：owner为稳定宿主；icon为借用图标；cancel为UI取消通知。返回：成功为true。
    bool Show(HWND owner, HICON icon, std::function<void()> cancel);
    // 更新阶段文字，不触碰编辑内容。
    // 入参：key为本地化状态键。返回：无。
    void Status(std::string_view key);
    // 将一次完成结果填入草稿，并开放编辑和复制。
    // 入参：text为独立识别结果。返回：无。
    void SetResult(std::wstring_view text);
    // 请求关闭，不在事件回调中析构窗口。
    // 入参：无。返回：无。
    void Close() noexcept;
    // 激活仍存在的结果窗口。
    // 入参：无。返回：无。
    void Activate() noexcept;
    // 查询原生窗口是否仍存活。
    // 入参：无。返回：存活为true。
    bool IsOpen() const noexcept;
    // 转交公共表单输入处理。
    // 入参：message为主循环消息。返回：消费为true。
    bool Process(MSG& message);
    // 将已注册的编辑组合转交公共控件。
    // 入参：modifiers/key为已核验热键。返回：消费为true。
    bool RegisteredHotkey(UINT modifiers, UINT key) noexcept;
    // 跟随宿主模态门禁暂停原生输入，关闭取消仍可由宿主请求。
    // 入参：paused为是否暂停。返回：无。
    void Pause(bool paused) noexcept;

  private:
    std::unique_ptr<WindowRenderer> renderer_;
    std::string draft_;
    std::function<void()> cancel_;
};
} // namespace open_st
