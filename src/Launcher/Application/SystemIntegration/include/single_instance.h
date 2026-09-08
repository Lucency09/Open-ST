#pragma once
#include <Windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace open_st
{
enum class LaunchCommand : DWORD
{
    Normal,
    Capture,
    Startup
};
enum class InstanceResult
{
    Primary,
    Forwarded,
    OtherSession,
    Failed
};
struct InstanceStatus
{
    InstanceResult result{InstanceResult::Failed};
    DWORD error{};
};
// 只接受零个参数或一个已知开关；拒绝未知及组合参数。
[[nodiscard]] bool ParseLaunchCommand(const std::vector<std::wstring>& arguments, LaunchCommand& command);
class SingleInstance
{
  public:
    // 名称按用户 SID 隔离；测试必须使用独立名称。
    explicit SingleInstance(std::wstring name = L"Open-ST");
    // 取消管道接收并释放实例占有权。
    ~SingleInstance();
    // 实例及线程不可复制。
    SingleInstance(const SingleInstance&) = delete;
    // 实例及线程不可赋值。
    SingleInstance& operator=(const SingleInstance&) = delete;
    // 在业务文件初始化前抢占单实例；非主实例返回 Forwarded，尚未发送命令。
    [[nodiscard]] InstanceStatus Acquire();
    // 监听前设置线程安全的截图门禁；空值拒绝请求，代次传入消息 lParam，监听中修改抛出异常。
    void SetCaptureGate(std::function<std::optional<LPARAM>()> gate);
    // 主实例开始监听；后台只向借用窗口异步投递固定命令到 wParam。
    [[nodiscard]] bool StartListening(HWND target, UINT message);
    // 限时等待首实例就绪并转发；跨登录会话返回 OtherSession。
    [[nodiscard]] InstanceStatus Forward(LaunchCommand command, DWORD timeoutMs = 2000);
    // 停止监听并取消未完成 IO；保持互斥体直到对象析构。
    void Stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
