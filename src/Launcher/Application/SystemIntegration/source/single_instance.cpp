// 实现用户身份校验、可取消管道通信及单实例监听线程的资源管理。

#include "single_instance.h"

#include <array>
#include <sddl.h>
#include <stdexcept>
#include <thread>
#include <utility>

namespace open_st
{
namespace
{
struct Handle
{
    HANDLE value{};
    // 释放并清空拥有的 Win32 句柄，使停止后可以安全重试。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Reset()
    {
        if (this->value != nullptr && this->value != INVALID_HANDLE_VALUE)
            CloseHandle(this->value);
        this->value = nullptr;
    }
    // 关闭拥有的 Win32 句柄。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~Handle()
    {
        this->Reset();
    }
};
struct LocalMemory
{
    HLOCAL value{};
    // 释放 Windows 分配的 SID 字符串或安全描述符。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~LocalMemory()
    {
        if (this->value != nullptr)
            LocalFree(this->value);
    }
};
struct Packet
{
    DWORD version{1};
    DWORD command{};
};
// 从指定进程读取 SID 文本，失败时不猜测用户身份。
// 入参：process 为借用的目标进程句柄，需要允许读取进程令牌。
// 返回：进程用户 SID 的字符串形式；系统查询或转换失败返回空字符串。
std::wstring ProcessSid(HANDLE process)
{
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
        return {};
    DWORD size{};
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (size == 0)
        return {};
    std::vector<BYTE> storage(size);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), size, &size))
        return {};
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text))
        return {};
    LocalMemory memory{text};
    return text;
}
// 校验命名管道对端进程身份，避免仅信任公开的管道名称。
// 入参：pipe 为借用管道句柄；serverSide 表示当前端是服务器；sid 为期望的对端用户 SID。
// 返回：成功读取对端进程且用户 SID 匹配时为 true；查询失败或不匹配为 false。
bool PeerMatches(HANDLE pipe, bool serverSide, const std::wstring& sid)
{
    ULONG processId{};
    const BOOL found =
        serverSide ? GetNamedPipeClientProcessId(pipe, &processId) : GetNamedPipeServerProcessId(pipe, &processId);
    if (!found)
        return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)};
    return process.value != nullptr && ProcessSid(process.value) == sid;
}
// 等待异步 IO，可由退出事件取消；取消后收回完成结果才能释放 OVERLAPPED。
// 入参：pipe 为管道句柄；operation 为未完成的重叠操作；stop 为可空取消事件；timeout 为等待毫秒数；bytes 输出实际传输字节数。
// 返回：异步操作完成且结果读取成功时为 true；超时、取消或 IO 失败为 false。
bool CompleteIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& bytes)
{
    const HANDLE events[]{operation.hEvent, stop};
    const DWORD wait = WaitForMultipleObjects(stop != nullptr ? 2u : 1u, events, FALSE, timeout);
    if (wait != WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
// 对固定长度消息执行有截止时间的读取或写入。
// 入参：pipe 为管道句柄；data 为读入缓冲区或写出数据；size 为字节数；writing 为写入方向标志；stop 为可空取消事件；timeout 为等待毫秒数。
// 返回：一次读写成功且传输字节数恰好为 size 时为 true，否则为 false。
bool Transfer(HANDLE pipe, void* data, DWORD size, bool writing, HANDLE stop, DWORD timeout)
{
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (event.value == nullptr)
        return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD bytes{};
    const BOOL complete =
        writing ? WriteFile(pipe, data, size, &bytes, &operation) : ReadFile(pipe, data, size, &bytes, &operation);
    if (!complete && (GetLastError() != ERROR_IO_PENDING || !CompleteIo(pipe, operation, stop, timeout, bytes)))
        return false;
    return bytes == size;
}
} // namespace
struct SingleInstance::Impl
{
    std::wstring name;
    std::wstring sid;
    std::wstring pipeName;
    Handle mutex;
    Handle stop;
    Handle pipe;
    LocalMemory security;
    std::thread worker;
    std::function<std::optional<LPARAM>()> captureGate;
    bool primary{};
    DWORD session{};
    // 运行单实例命名管道接收循环，校验用户、会话和命令后异步通知目标窗口。
    // 入参：target 为借用的接收窗口句柄；message 为投递到窗口的自定义消息编号。
    // 返回：无返回值。
    void Listen(HWND target, UINT message)
    {
        while (WaitForSingleObject(this->stop.value, 0) == WAIT_TIMEOUT)
        {
            Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (event.value == nullptr)
                return;
            OVERLAPPED operation{};
            operation.hEvent = event.value;
            BOOL connected = ConnectNamedPipe(this->pipe.value, &operation);
            if (!connected)
            {
                const DWORD error = GetLastError();
                DWORD bytes{};
                connected = error == ERROR_PIPE_CONNECTED ||
                            (error == ERROR_IO_PENDING &&
                             CompleteIo(this->pipe.value, operation, this->stop.value, INFINITE, bytes));
            }
            if (!connected)
                return;
            Packet packet{};
            DWORD reply = ERROR_INVALID_DATA;
            ULONG clientSession{};
            if (PeerMatches(this->pipe.value, true, this->sid) &&
                GetNamedPipeClientSessionId(this->pipe.value, &clientSession) &&
                Transfer(this->pipe.value, &packet, sizeof(packet), false, this->stop.value, 2000) &&
                packet.version == 1 && packet.command <= static_cast<DWORD>(LaunchCommand::Startup))
            {
                if (clientSession != this->session)
                    reply = ERROR_CTX_WINSTATION_NOT_FOUND;
                else
                {
                    std::optional<LPARAM> token = 0;
                    if (packet.command == static_cast<DWORD>(LaunchCommand::Capture) && this->captureGate)
                    {
                        try
                        {
                            token = this->captureGate();
                        }
                        catch (...)
                        {
                            token.reset();
                        }
                    }
                    if (!token)
                        reply = ERROR_BUSY;
                    else if (PostMessageW(target, message, static_cast<WPARAM>(packet.command), *token))
                        reply = ERROR_SUCCESS;
                    else
                        reply = GetLastError();
                }
            }
            if (Transfer(this->pipe.value, &reply, sizeof(reply), true, this->stop.value, 2000))
            {
                DWORD acknowledgement{};
                Transfer(this->pipe.value, &acknowledgement, sizeof(acknowledgement), false, this->stop.value, 2000);
            }
            DisconnectNamedPipe(this->pipe.value);
        }
    }
};
// 将程序启动参数解析为普通启动、截图或开机启动命令。
// 入参：arguments 为不含程序名的命令行参数列表；command 为解析成功后写入的启动命令。
// 返回：参数为无开关、--capture 或 --startup 时为 true；非法组合返回 false，command 保持原值。
bool ParseLaunchCommand(const std::vector<std::wstring>& arguments, LaunchCommand& command)
{
    if (arguments.empty())
    {
        command = LaunchCommand::Normal;
        return true;
    }
    if (arguments.size() != 1)
        return false;
    if (arguments.front() == L"--capture")
    {
        command = LaunchCommand::Capture;
        return true;
    }
    if (arguments.front() == L"--startup")
    {
        command = LaunchCommand::Startup;
        return true;
    }
    return false;
}
// 创建单实例协调对象并保存基础名称，将系统资源申请延后到 Acquire。
// 入参：name 为实例命名空间的基础名称，取得实例资格时再附加当前用户 SID。
// 返回：无返回值。
SingleInstance::SingleInstance(std::wstring name) : impl_(std::make_unique<Impl>())
{
    this->impl_->name = std::move(name);
}
// 停止监听线程并释放单实例协调对象拥有的系统资源。
// 入参：无显式入参。
// 返回：无返回值。
SingleInstance::~SingleInstance()
{
    this->Stop();
}
// 使用当前用户专属 DACL 创建 Global 互斥体，并保留句柄占有实例名称。
// 入参：无显式入参。
// 返回：Primary 表示取得主实例资格；Forwarded 表示已有实例但尚未转发；Failed 附带系统错误码。
InstanceStatus SingleInstance::Acquire()
{
    if (this->impl_->mutex.value != nullptr)
        return {this->impl_->primary ? InstanceResult::Primary : InstanceResult::Forwarded, 0};
    this->impl_->sid = ProcessSid(GetCurrentProcess());
    if (this->impl_->sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &this->impl_->session))
        return {InstanceResult::Failed, GetLastError()};
    const std::wstring descriptor = L"D:P(A;;GA;;;" + this->impl_->sid + L")";
    PSECURITY_DESCRIPTOR security{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr))
        return {InstanceResult::Failed, GetLastError()};
    this->impl_->security.value = security;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
    const std::wstring identity = this->impl_->name + L"." + this->impl_->sid;
    this->impl_->pipeName = L"\\\\.\\pipe\\" + identity;
    this->impl_->mutex.value = CreateMutexW(&attributes, FALSE, (L"Global\\" + identity).c_str());
    const DWORD error = GetLastError();
    if (this->impl_->mutex.value == nullptr)
        return {InstanceResult::Failed, error};
    this->impl_->primary = error != ERROR_ALREADY_EXISTS;
    return {this->impl_->primary ? InstanceResult::Primary : InstanceResult::Forwarded, 0};
}
// 在开始监听前安装用于决定跨进程截图请求是否准入的回调。
// 入参：gate 为监听线程调用的截图准入回调；返回空 optional 拒绝请求，返回代次则写入消息 lParam；必须在监听前设置。
// 返回：无返回值。
void SingleInstance::SetCaptureGate(std::function<std::optional<LPARAM>()> gate)
{
    if (this->impl_->worker.joinable())
        throw std::logic_error("Capture gate must be set before listening starts");
    this->impl_->captureGate = std::move(gate);
}
// 为主实例建立首个命名管道并启动可取消的后台监听线程。
// 入参：target 为借用的接收窗口句柄；message 为投递到窗口的自定义消息编号。
// 返回：成功创建管道并启动监听线程时为 true；身份、窗口或系统资源条件不满足时为 false。
bool SingleInstance::StartListening(HWND target, UINT message)
{
    if (!this->impl_->primary || this->impl_->worker.joinable() || !IsWindow(target))
        return false;
    this->impl_->stop.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (this->impl_->stop.value == nullptr)
        return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), this->impl_->security.value, FALSE};
    this->impl_->pipe.value = CreateNamedPipeW(
        this->impl_->pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, sizeof(DWORD),
        sizeof(Packet), 0, &attributes);
    if (this->impl_->pipe.value == INVALID_HANDLE_VALUE)
    {
        this->impl_->pipe.Reset();
        this->impl_->stop.Reset();
        return false;
    }
    // 在后台执行管道监听；Stop 等待线程结束后才释放其借用的对象和句柄。
    // 入参：无显式入参。
    // 返回：无返回值。
    this->impl_->worker = std::thread([this, target, message]() { this->impl_->Listen(target, message); });
    return true;
}
// 向已存在的主实例转发启动命令并等待协议确认。
// 入参：command 为待转发的启动命令；timeoutMs 为等待就绪及消息交换共用的超时毫秒数。
// 返回：转发成功为 Forwarded；对端位于其他登录会话为 OtherSession；失败为 Failed 并附错误码。
InstanceStatus SingleInstance::Forward(LaunchCommand command, DWORD timeoutMs)
{
    if (this->impl_->pipeName.empty() || static_cast<DWORD>(command) > static_cast<DWORD>(LaunchCommand::Startup))
        return {InstanceResult::Failed, ERROR_INVALID_PARAMETER};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    Handle pipe;
    do
    {
        pipe.value = CreateFileW(this->impl_->pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE)
            break;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
            return {InstanceResult::Failed, error};
        Sleep(10);
    } while (GetTickCount64() < deadline);
    if (pipe.value == INVALID_HANDLE_VALUE)
        return {InstanceResult::Failed, ERROR_TIMEOUT};
    if (!PeerMatches(pipe.value, false, this->impl_->sid))
        return {InstanceResult::Failed, ERROR_ACCESS_DENIED};
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr))
        return {InstanceResult::Failed, GetLastError()};
    Packet packet{1, static_cast<DWORD>(command)};
    DWORD reply{};
    // 每次阶段开始重新计算剩余时间，不能因两次 IO 延长调用者给出的期限。
    // 入参：无显式入参。
    // 返回：距捕获的 deadline 尚余的毫秒数；已到期限时为 0。
    const auto remaining = [deadline]() -> DWORD
    {
        const ULONGLONG now = GetTickCount64();
        return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    };
    if (!Transfer(pipe.value, &packet, sizeof(packet), true, nullptr, remaining()) ||
        !Transfer(pipe.value, &reply, sizeof(reply), false, nullptr, remaining()))
        return {InstanceResult::Failed, GetLastError()};
    DWORD acknowledgement = 1;
    if (!Transfer(pipe.value, &acknowledgement, sizeof(acknowledgement), true, nullptr, remaining()))
        return {InstanceResult::Failed, GetLastError()};
    if (reply == ERROR_CTX_WINSTATION_NOT_FOUND)
        return {InstanceResult::OtherSession, reply};
    return {reply == ERROR_SUCCESS ? InstanceResult::Forwarded : InstanceResult::Failed, reply};
}
// 停止后台命名管道监听并收回未完成 IO，保留实例互斥体直到对象析构。
// 入参：无显式入参。
// 返回：无返回值。
void SingleInstance::Stop()
{
    if (this->impl_->stop.value != nullptr)
        SetEvent(this->impl_->stop.value);
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
    this->impl_->pipe.Reset();
    this->impl_->stop.Reset();
}
} // namespace open_st
