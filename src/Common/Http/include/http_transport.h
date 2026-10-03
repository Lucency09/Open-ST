// 文件职责：提供独立于业务协议的有界、可取消 HTTP 传输契约。
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace open_st::http
{
enum class Error
{
    None,
    Cancelled,
    Timeout,
    Network,
    SinkRejected,
    InvalidRequest,
    ResponseTooLarge
};
enum class Method
{
    Get,
    Post,
    Put,
    Patch
};
enum class ProxyMode
{
    System,
    Custom
};
struct Proxy
{
    ProxyMode mode = ProxyMode::System;
    std::wstring address;
};
struct Header
{
    std::wstring name;
    std::wstring value;
};
struct Request
{
    std::wstring url;
    Method method = Method::Get;
    std::vector<Header> headers;
    std::string body;
    Proxy proxy;
    bool allowHttp = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds idleTimeout{30000};
    std::uint64_t maxResponseBytes = 4U * 1024U * 1024U;
    std::uint64_t maxErrorBytes = 64U * 1024U;
    // 业务可只消费某个状态；零表示消费所有状态的有界正文。
    unsigned consumeStatus = 0;
};
struct Result
{
    Error error = Error::None;
    unsigned long systemError = 0;
    unsigned status = 0;
    std::optional<std::uint64_t> declaredLength;
    std::wstring redirectLocation;
    std::wstring contentType;
};
using Sink = std::function<bool(std::span<const std::byte>)>;
class Transport
{
  public:
    // 释放传输对象，活动调用须由任务所有者先取消并回收。
    // 入参：无。返回：无。
    virtual ~Transport() = default;
    // 执行单次请求，不自动跳转或重发业务 HTTP；系统可在连接期限内尝试不同 IP。
    // 正文借用仅在同步 sink 调用内有效。
    // 入参：request 拥有参数，stop 用于取消，sink 消费有界响应正文。
    // 返回：HTTP 元信息或结构化错误；非成功 HTTP 状态保留供业务判断。
    virtual Result Perform(const Request& request, std::stop_token stop, const Sink& sink) = 0;
};
// 校验通用 URL、请求头、代理和预算，不访问网络或判断业务路径。
// 入参：request 为候选参数。返回：有效为 true。
bool IsValidRequest(const Request& request) noexcept;
// 建立使用系统 TLS 校验的 WinHTTP 传输，不立即访问网络。
// 入参：无。返回：独占传输对象。
std::unique_ptr<Transport> MakeWinHttpTransport();
} // namespace open_st::http
