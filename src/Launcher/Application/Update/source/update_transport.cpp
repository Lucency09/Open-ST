// 文件职责：以可取消的异步 WinHTTP 完成单次 HTTPS 传输，不承担 Release、文件或界面业务。

#include "update_transport.h"

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace open_st::update_detail
{
namespace
{
struct EventCloser
{
    // 释放本对象独占的 Windows 事件句柄。
    // 入参：handle 为有效事件或空指针。
    // 返回：无；不等待任何网络操作。
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr)
            CloseHandle(handle);
    }
};
using EventHandle = std::unique_ptr<void, EventCloser>;

struct InternetCloser
{
    // 关闭异步 WinHTTP 句柄；请求上下文另由最终 HANDLE_CLOSING 通知回收。
    // 入参：handle 为 WinHTTP 句柄或空指针。
    // 返回：无；调用后不得继续使用该句柄。
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;

struct CallbackState
{
    EventHandle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    EventHandle cancelled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex;
    DWORD notification = 0;
    DWORD error = ERROR_SUCCESS;
    DWORD byteCount = 0;
    // 未完成的 ReadData 可在 Get 返回后仍引用缓冲区，必须与回调上下文一起保活。
    std::array<std::byte, 65536> buffer{};
};

// 记录异步操作完成；最终关闭通知回收 WinHTTP 持有的共享上下文引用。
// 入参：context 为共享状态引用的独占包装地址，其余为 WinHTTP 提供的通知与只借用信息。
// 返回：无；不调用业务 sink、不访问窗口，不让异常越过系统回调边界。
void CALLBACK HttpCallback(HINTERNET, DWORD_PTR context, DWORD notification, void* information, DWORD length) noexcept
{
    if (context == 0)
        return;
    std::shared_ptr<CallbackState>* holder = reinterpret_cast<std::shared_ptr<CallbackState>*>(context);
    if (notification == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        const std::unique_ptr<std::shared_ptr<CallbackState>> finalReference(holder);
        return;
    }
    if (notification != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
        notification != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE &&
        notification != WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE &&
        notification != WINHTTP_CALLBACK_STATUS_READ_COMPLETE && notification != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        return;
    try
    {
        const std::shared_ptr<CallbackState> state = *holder;
        const std::lock_guard<std::mutex> guard(state->mutex);
        state->notification = notification;
        state->error = ERROR_SUCCESS;
        state->byteCount = 0;
        if (notification == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            state->error = information != nullptr && length >= sizeof(WINHTTP_ASYNC_RESULT)
                               ? static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError
                               : ERROR_WINHTTP_INTERNAL_ERROR;
        else if (notification == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)
            state->byteCount =
                information != nullptr && length >= sizeof(DWORD) ? *static_cast<DWORD*>(information) : 0;
        else if (notification == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
            state->byteCount = length;
        SetEvent(state->completed.get());
    }
    catch (...)
    {
        // 内部同步失败时唤醒取消分支；上下文仍由最终关闭通知释放。
        SetEvent((*holder)->cancelled.get());
    }
}

// 保存 Win32 网络失败，保留此前已取得的 HTTP 响应信息。
// 入参：result 为本次结果；error 为 Win32 错误码。
// 返回：false，便于作为失败分支统一返回条件。
bool NetworkFailure(UpdateHttpResult& result, DWORD error)
{
    result.error = error == ERROR_WINHTTP_TIMEOUT ? UpdateHttpError::Timeout : UpdateHttpError::Network;
    result.systemError = error;
    return false;
}

// 检查用户取消和统一截止时间，不自行重试网络调用。
// 入参：request 提供截止时间；stop 为任务取消令牌；result 接收失败原因。
// 返回：仍允许执行时 true，否则 false。
bool CheckRequest(const UpdateHttpRequest& request, std::stop_token stop, UpdateHttpResult& result)
{
    if (stop.stop_requested())
    {
        result.error = UpdateHttpError::Cancelled;
        result.systemError = ERROR_CANCELLED;
        return false;
    }
    if (std::chrono::steady_clock::now() >= request.deadline)
    {
        result.error = UpdateHttpError::Timeout;
        result.systemError = ERROR_WINHTTP_TIMEOUT;
        return false;
    }
    return true;
}

// 在发起下一异步操作前重置完成槽；上一操作已完成，不存在同时在途的 API。
// 入参：state 为本请求共享回调状态。
// 返回：无；不改变取消事件。
void PrepareOperation(CallbackState& state)
{
    const std::lock_guard<std::mutex> guard(state.mutex);
    state.notification = 0;
    state.error = ERROR_SUCCESS;
    state.byteCount = 0;
    ResetEvent(state.completed.get());
}

// 等待单次异步操作、取消或期限，结束后只允许调用方顺序启动下一操作。
// 入参：state 为回调状态；request 为期限；stop 为取消令牌；expected 为完成通知；result 接收错误。
// 返回：收到预期完成通知时 true；取消、网络错误或超时为 false，不等待关闭回调。
bool AwaitOperation(CallbackState& state, const UpdateHttpRequest& request, std::stop_token stop, DWORD expected,
                    UpdateHttpResult& result)
{
    if (!CheckRequest(request, stop, result))
        return false;
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(request.deadline - std::chrono::steady_clock::now());
    const std::chrono::milliseconds bounded = std::min(remaining, request.idleTimeout);
    const DWORD timeout = static_cast<DWORD>(std::clamp<std::int64_t>(bounded.count(), 1, MAXDWORD - 1));
    const std::array<HANDLE, 2> events{state.cancelled.get(), state.completed.get()};
    const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, timeout);
    if (!CheckRequest(request, stop, result))
        return false;
    if (waited == WAIT_OBJECT_0)
    {
        result.error = UpdateHttpError::Cancelled;
        result.systemError = ERROR_CANCELLED;
        return false;
    }
    if (waited == WAIT_TIMEOUT)
        return NetworkFailure(result, ERROR_WINHTTP_TIMEOUT);
    if (waited != WAIT_OBJECT_0 + 1)
        return NetworkFailure(result, waited == WAIT_FAILED ? GetLastError() : ERROR_WINHTTP_INTERNAL_ERROR);
    const std::lock_guard<std::mutex> guard(state.mutex);
    if (state.error != ERROR_SUCCESS)
        return NetworkFailure(result, state.error);
    if (state.notification != expected)
        return NetworkFailure(result, ERROR_WINHTTP_INTERNAL_ERROR);
    return true;
}

// 判断异步 API 是否成功启动；立即成功仍须等待完成通知。
// 入参：started 为 API 返回值；result 接收失败结果。
// 返回：已启动或报告异步待完成时 true，否则 false。
bool Started(BOOL started, UpdateHttpResult& result)
{
    if (started != FALSE)
        return true;
    const DWORD error = GetLastError();
    return error == ERROR_IO_PENDING || NetworkFailure(result, error);
}

// 有界读取单个响应头，缺失返回空值，重复头由上层协议或调用方另行约束。
// 入参：handle 为已接收响应头的请求；query 为 WinHTTP 查询常量；value 输出头值；result 接收失败。
// 返回：读取成功或头不存在时 true；头超过 32 KiB 或查询失败时 false。
bool ReadHeader(HINTERNET handle, DWORD query, std::wstring& value, UpdateHttpResult& result)
{
    DWORD bytes = 0;
    if (!WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_WINHTTP_HEADER_NOT_FOUND)
        {
            value.clear();
            return true;
        }
        if (error != ERROR_INSUFFICIENT_BUFFER)
            return NetworkFailure(result, error);
    }
    if (bytes > 32768 || bytes % sizeof(wchar_t) != 0)
        return NetworkFailure(result, ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX))
        return NetworkFailure(result, GetLastError());
    value.assign(buffer.data(), bytes / sizeof(wchar_t));
    return true;
}

// 读取状态、长度和跳转地址，不判断业务允许的主机或 Release 资产。
// 入参：handle 为收到响应头的请求；result 输出元信息及错误。
// 返回：头格式有效时 true；数字溢出、无效长度或读取失败为 false。
bool ReadResponseHeaders(HINTERNET handle, UpdateHttpResult& result)
{
    DWORD status = 0;
    DWORD bytes = sizeof(status);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
        return NetworkFailure(result, GetLastError());
    result.status = status;
    std::wstring length;
    if (!ReadHeader(handle, WINHTTP_QUERY_CONTENT_LENGTH, length, result) ||
        !ReadHeader(handle, WINHTTP_QUERY_LOCATION, result.redirectLocation, result))
        return false;
    if (!length.empty())
    {
        std::uint64_t parsed = 0;
        for (const wchar_t character : length)
        {
            if (character < L'0' || character > L'9' ||
                parsed > (std::numeric_limits<std::uint64_t>::max() - (character - L'0')) / 10)
                return NetworkFailure(result, ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
            parsed = parsed * 10 + static_cast<unsigned>(character - L'0');
        }
        result.declaredLength = parsed;
    }
    return true;
}

class WinHttpUpdateTransport final : public UpdateTransport
{
  public:
    // 同步服务任务线程，内部所有可能在途的网络操作均使用异步 WinHTTP 与有界事件等待。
    // 入参：request 为单次 HTTPS 参数；stop 为取消令牌；sink 同步消费 200 响应体且不得保留 span。
    // 返回：单次 HTTP 结果，不自动跳转、重试或访问本地文件；异常转为结构化错误。
    UpdateHttpResult Get(const UpdateHttpRequest& request, std::stop_token stop, const UpdateHttpSink& sink) override
    {
        UpdateHttpResult result;
        try
        {
            this->Perform(request, stop, sink, result);
        }
        catch (...)
        {
            result.error = UpdateHttpError::Network;
            result.systemError = ERROR_NOT_ENOUGH_MEMORY;
        }
        return result;
    }

  private:
    // 创建单次请求并按发送、收头、读块顺序执行，所有句柄和待完成缓冲区具备独立寿命。
    // 入参：request、stop、sink 为 Get 的借用参数；result 累积结果。
    // 返回：无；任何失败立即停止，由 RAII 关闭异步句柄触发取消。
    void Perform(const UpdateHttpRequest& request, std::stop_token stop, const UpdateHttpSink& sink,
                 UpdateHttpResult& result)
    {
        if (!CheckRequest(request, stop, result))
            return;
        if (!sink || request.url.empty() || request.url.size() > 32768 || request.idleTimeout.count() <= 0 ||
            request.accept.empty() || request.accept.size() > 1024 ||
            request.accept.find_first_of(L"\r\n") != std::wstring::npos ||
            request.accept.find(L'\0') != std::wstring::npos || request.url.find(L'\0') != std::wstring::npos ||
            request.url.find(L'#') != std::wstring::npos)
        {
            result.error = UpdateHttpError::InvalidRequest;
            return;
        }
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        components.dwUserNameLength = static_cast<DWORD>(-1);
        components.dwPasswordLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(request.url.c_str(), static_cast<DWORD>(request.url.size()), 0, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS || components.dwHostNameLength == 0 ||
            components.dwUserNameLength != 0 || components.dwPasswordLength != 0)
        {
            result.error = UpdateHttpError::InvalidRequest;
            return;
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path =
            components.dwUrlPathLength == 0 ? L"/" : std::wstring(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength != 0)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        const InternetHandle session(WinHttpOpen(L"Open-ST-Update/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!session)
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        const int timeout = static_cast<int>(std::min<std::int64_t>(request.idleTimeout.count(), INT_MAX));
        if (!WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout))
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        const InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (!connection)
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        const std::shared_ptr<CallbackState> state = std::make_shared<CallbackState>();
        if (!state->completed || !state->cancelled)
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        // state 中的固定缓冲区与事件由最终句柄关闭回调保活，取消线程只唤醒事件。
        const std::stop_callback cancel(stop,
                                        // 唤醒本次有界等待，由任务线程统一关闭异步请求。
                                        // 入参：无；捕获共享状态以保活事件句柄。
                                        // 返回：无；不在取消线程调用 WinHTTP。
                                        [state]() noexcept { SetEvent(state->cancelled.get()); });
        const InternetHandle handle(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       WINHTTP_FLAG_SECURE));
        if (!handle)
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            WinHttpSetStatusCallback(handle.get(), HttpCallback,
                                     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                     0) == WINHTTP_INVALID_STATUS_CALLBACK)
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        auto callbackReference = std::make_unique<std::shared_ptr<CallbackState>>(state);
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(callbackReference.get());
        if (!WinHttpSetOption(handle.get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        {
            NetworkFailure(result, GetLastError());
            return;
        }
        // 从此该引用只在 HANDLE_CLOSING 回调中回收；不能因 Get 取消而提前释放。
        (void)callbackReference.release();
        const std::wstring headers = L"Accept: " + request.accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        PrepareOperation(*state);
        if (!CheckRequest(request, stop, result) ||
            !Started(WinHttpSendRequest(handle.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, context),
                     result) ||
            !AwaitOperation(*state, request, stop, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, result))
            return;
        PrepareOperation(*state);
        if (!Started(WinHttpReceiveResponse(handle.get(), nullptr), result) ||
            !AwaitOperation(*state, request, stop, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, result) ||
            !ReadResponseHeaders(handle.get(), result) || result.status != 200)
            return;
        for (;;)
        {
            PrepareOperation(*state);
            if (!CheckRequest(request, stop, result) ||
                !Started(WinHttpQueryDataAvailable(handle.get(), nullptr), result) ||
                !AwaitOperation(*state, request, stop, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, result))
                return;
            DWORD available = 0;
            {
                const std::lock_guard<std::mutex> guard(state->mutex);
                available = state->byteCount;
            }
            if (available == 0)
                return;
            PrepareOperation(*state);
            if (!Started(WinHttpReadData(handle.get(), state->buffer.data(),
                                         std::min<DWORD>(available, static_cast<DWORD>(state->buffer.size())), nullptr),
                         result) ||
                !AwaitOperation(*state, request, stop, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, result))
                return;
            DWORD received = 0;
            {
                const std::lock_guard<std::mutex> guard(state->mutex);
                received = state->byteCount;
            }
            if (received == 0)
                return;
            try
            {
                if (received <= state->buffer.size() && sink({state->buffer.data(), received}))
                    continue;
            }
            catch (...)
            {
            }
            result.error = UpdateHttpError::SinkRejected;
            return;
        }
    }
};
} // namespace

// 创建无全局状态的 WinHTTP 获取器；每次请求独立持有系统句柄及取消上下文。
// 入参：无。
// 返回：独占传输边界，可由任务测试替换成不访问网络的实现。
std::unique_ptr<UpdateTransport> MakeWinHttpUpdateTransport()
{
    return std::make_unique<WinHttpUpdateTransport>();
}
} // namespace open_st::update_detail
