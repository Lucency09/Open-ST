// 声明按用户隔离的单实例占用、启动命令解析与命名管道转发接口。

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
// 入参：arguments 为不含程序名的命令行参数列表；command 为解析成功后写入的启动命令。
// 返回：参数为无开关、--capture 或 --startup 时为 true；非法组合返回 false，command 保持原值。
[[nodiscard]] bool ParseLaunchCommand(const std::vector<std::wstring>& arguments, LaunchCommand& command);
class SingleInstance
{
  public:
    // 建立单实例协调对象的命名配置，测试可注入独立基础名称。
    // 入参：name 为实例命名空间的基础名称，取得实例资格时再附加当前用户 SID。
    // 返回：无返回值。
    explicit SingleInstance(std::wstring name = L"Open-ST");
    // 停止单实例接收线程并释放实例占有权。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~SingleInstance();
    // 实例及线程不可复制。
    // 入参：未命名 SingleInstance 引用为被禁止复制或赋值的实例与线程所有权来源。
    // 返回：无可调用实现；该操作已删除，尝试调用会导致编译错误。
    SingleInstance(const SingleInstance&) = delete;
    // 实例及线程不可赋值。
    // 入参：未命名 SingleInstance 引用为被禁止复制或赋值的实例与线程所有权来源。
    // 返回：无可调用实现；该操作已删除，尝试调用会导致编译错误。
    SingleInstance& operator=(const SingleInstance&) = delete;
    // 按当前用户 SID 申请实例名称并判定是否为主实例。
    // 入参：无显式入参。
    // 返回：Primary 表示取得主实例资格；Forwarded 表示已有实例但尚未转发；Failed 附带系统错误码。
    [[nodiscard]] InstanceStatus Acquire();
    // 在监听前设置跨进程截图请求的准入回调。
    // 入参：gate 为监听线程调用的截图准入回调；返回空 optional 拒绝请求，返回代次则写入消息 lParam；必须在监听前设置。
    // 返回：无返回值。
    void SetCaptureGate(std::function<std::optional<LPARAM>()> gate);
    // 启动主实例管道监听，将接收到的有效命令异步投递给目标窗口。
    // 入参：target 为借用的接收窗口句柄；message 为投递到窗口的自定义消息编号。
    // 返回：成功创建管道并启动监听线程时为 true；身份、窗口或系统资源条件不满足时为 false。
    [[nodiscard]] bool StartListening(HWND target, UINT message);
    // 限时转发启动命令到已存在的主实例，并取得接收确认。
    // 入参：command 为待转发的启动命令；timeoutMs 为等待就绪及消息交换共用的超时毫秒数。
    // 返回：转发成功为 Forwarded；对端位于其他登录会话为 OtherSession；失败为 Failed 并附错误码。
    [[nodiscard]] InstanceStatus Forward(LaunchCommand command, DWORD timeoutMs = 2000);
    // 取消后台监听和未完成 IO，并等待接收线程结束。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
