// 文件职责：定义 Update 私有 HTTP 传输边界，供生产 WinHTTP 和无网络测试替身共用。

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

namespace open_st::update_detail
{
enum class UpdateHttpError
{
    None,
    Cancelled,
    Timeout,
    Network,
    SinkRejected,
    InvalidRequest
};

struct UpdateHttpRequest
{
    std::wstring url;
    std::wstring accept = L"application/vnd.github+json";
    std::chrono::steady_clock::time_point deadline;
    std::chrono::milliseconds idleTimeout{30000};
};

struct UpdateHttpResult
{
    UpdateHttpError error = UpdateHttpError::None;
    unsigned long systemError = 0;
    unsigned status = 0;
    std::optional<std::uint64_t> declaredLength;
    std::wstring redirectLocation;
};

using UpdateHttpSink = std::function<bool(std::span<const std::byte>)>;

class UpdateTransport
{
  public:
    // 回收传输实现；正在调用 Get 的对象须先由任务所有者取消并回收。
    // 入参：无。
    // 返回：无；虚析构允许测试替身通过基类释放。
    virtual ~UpdateTransport() = default;

    // 执行单次 HTTPS GET，不跟随跳转，仅将 200 响应体按块交给同步消费者。
    // 入参：request 提供 URL、Accept、总截止时间及无进展时限；stop 取消本次请求；sink 消费借用字节。
    // 返回：HTTP 状态、可选长度与跳转目标或错误；非 200 不传响应体，sink 拒绝立即停止。
    virtual UpdateHttpResult Get(const UpdateHttpRequest& request, std::stop_token stop,
                                 const UpdateHttpSink& sink) = 0;
};

// 创建使用系统代理及系统 TLS 验证的异步 WinHTTP 实现。
// 入参：无。
// 返回：独占传输对象；内存分配异常交由调用方处理，不执行网络操作。
std::unique_ptr<UpdateTransport> MakeWinHttpUpdateTransport();
} // namespace open_st::update_detail
