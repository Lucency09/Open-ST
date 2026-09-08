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
    // 显式释放后置空，使停止和重试不遗留旧的管道实例。
    void Reset()
    {
        if (this->value != nullptr && this->value != INVALID_HANDLE_VALUE)
            CloseHandle(this->value);
        this->value = nullptr;
    }
    // 关闭拥有的 Win32 句柄。
    ~Handle()
    {
        this->Reset();
    }
};
struct LocalMemory
{
    HLOCAL value{};
    // 释放 Windows 分配的 SID 字符串或安全描述符。
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
    // 接收线程只校验并投递命令，不访问业务模块或用户界面。
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
// 参数列表不包含程序文件名；解析失败不修改输出。
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
// 延迟系统访问到 Acquire，方便调用者先处理参数错误。
SingleInstance::SingleInstance(std::wstring name) : impl_(std::make_unique<Impl>())
{
    this->impl_->name = std::move(name);
}
// 保证线程退出早于其依赖的句柄销毁。
SingleInstance::~SingleInstance()
{
    this->Stop();
}
// 使用当前用户专属 DACL 创建 Global 互斥体，并保留句柄占有实例名称。
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
// 不允许运行期替换回调，避免接收线程与调用者同时访问函数对象。
void SingleInstance::SetCaptureGate(std::function<std::optional<LPARAM>()> gate)
{
    if (this->impl_->worker.joinable())
        throw std::logic_error("Capture gate must be set before listening starts");
    this->impl_->captureGate = std::move(gate);
}
// 同步建立首个管道实例，防止命名抢占；线程的所有 IO 都可取消。
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
    this->impl_->worker = std::thread([this, target, message]() { this->impl_->Listen(target, message); });
    return true;
}
// 等待服务器建管道期间不放行第二主实例；协议消息及回复都有总截止时间。
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
// 信号打断连接或读写等待；不使用阻塞 FlushFileBuffers 等待客户端消费。
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
