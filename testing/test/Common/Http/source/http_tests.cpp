// 验证真实 WinHTTP 对本机测试代理的有界请求、取消和单次传输语义。
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <http_transport.h>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
using namespace open_st::http;
using namespace std::chrono_literals;
class LocalServer
{
  public:
    // 在回环临时端口启动单请求代理，使用真实 HTTP 响应验证边界。
    // 入参：response 为原始响应，hold 指定收完请求后持续等待取消。返回：无。
    explicit LocalServer(std::string response, bool hold = false) : response_(std::move(response)), hold_(hold)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        this->started_ = true;
        this->listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (this->listener_ == INVALID_SOCKET ||
            bind(this->listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(this->listener_, 4) != 0)
            return;
        int size = sizeof(address);
        if (getsockname(this->listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            return;
        this->port_ = ntohs(address.sin_port);
        this->thread_ = std::jthread([this](std::stop_token stop) { this->Run(stop); });
    }
    // 停止测试线程并关闭自身 socket，不触碰系统代理设置。
    // 入参：无。返回：无。
    ~LocalServer()
    {
        this->thread_.request_stop();
        if (this->thread_.joinable())
            this->thread_.join();
        if (this->listener_ != INVALID_SOCKET)
            closesocket(this->listener_);
        if (this->started_)
            WSACleanup();
    }
    // 返回指向回环测试代理的 HTTP 请求，不依赖用户系统代理。
    // 入参：无。返回：本次隔离请求。
    Request MakeRequest() const
    {
        Request request;
        request.url = L"http://request.invalid/sample";
        request.allowHttp = true;
        request.proxy = {ProxyMode::Custom, L"http://127.0.0.1:" + std::to_wstring(this->port_)};
        request.deadline = std::chrono::steady_clock::now() + 5s;
        return request;
    }
    // 查询服务器捕获的完整请求。
    // 入参：无。返回：同步独立副本。
    std::string Captured()
    {
        const std::scoped_lock lock(this->mutex_);
        return this->captured_;
    }
    std::atomic<unsigned> connections{};
    std::atomic<bool> received{};
    unsigned short port_{};

  private:
    // 轮询 socket 可读状态，使测试清理不依赖网络关闭或长超时。
    // 入参：socket 为借用 socket。返回：可读为 true。
    static bool Readable(SOCKET socket)
    {
        fd_set reads{};
        FD_ZERO(&reads);
        FD_SET(socket, &reads);
        timeval timeout{0, 20000};
        return select(0, &reads, nullptr, nullptr, &timeout) > 0;
    }
    // 接收完整头和 Content-Length 正文，再发送用例给定响应。
    // 入参：stop 为服务器生命周期。返回：无。
    void Run(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            if (!Readable(this->listener_))
                continue;
            const SOCKET client = accept(this->listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;
            ++this->connections;
            std::string request;
            while (!stop.stop_requested() && request.size() < 2U * 1024U * 1024U)
            {
                if (!Readable(client))
                    continue;
                std::array<char, 4096> bytes{};
                const int count = recv(client, bytes.data(), static_cast<int>(bytes.size()), 0);
                if (count <= 0)
                    break;
                request.append(bytes.data(), static_cast<std::size_t>(count));
                const auto end = request.find("\r\n\r\n");
                if (end == std::string::npos)
                    continue;
                std::size_t bodyLength = 0;
                const auto length = request.find("Content-Length:");
                if (length != std::string::npos)
                    bodyLength = std::stoul(request.substr(length + 15));
                if (request.size() >= end + 4 + bodyLength)
                    break;
            }
            {
                const std::scoped_lock lock(this->mutex_);
                this->captured_ = request;
            }
            this->received = true;
            while (this->hold_ && !stop.stop_requested())
                std::this_thread::sleep_for(10ms);
            if (!stop.stop_requested())
            {
                std::size_t sent = 0;
                while (sent < this->response_.size())
                {
                    const int count =
                        send(client, this->response_.data() + sent, static_cast<int>(this->response_.size() - sent), 0);
                    if (count <= 0)
                        break;
                    sent += static_cast<std::size_t>(count);
                }
            }
            closesocket(client);
        }
    }
    bool started_{};
    SOCKET listener_{INVALID_SOCKET};
    std::string response_;
    bool hold_{};
    std::mutex mutex_;
    std::string captured_;
    std::jthread thread_;
};

// 验证安全参数在建立连接前被拒绝，HTTP 仅明确允许后可用。
// 入参：无。返回：测试断言。
TEST(HttpTest, validates_tls_proxy_and_header_parameters)
{
    Request request;
    request.url = L"http://example.invalid";
    EXPECT_FALSE(IsValidRequest(request));
    request.allowHttp = true;
    EXPECT_TRUE(IsValidRequest(request));
    request.proxy = {ProxyMode::Custom, L"127.0.0.1:7897"};
    EXPECT_TRUE(IsValidRequest(request));
    request.proxy.address = L"http://user:password@127.0.0.1:7897";
    EXPECT_FALSE(IsValidRequest(request));
    request.proxy = {};
    request.headers = {{L"X-Test", L"ok\r\nInjected: yes"}};
    EXPECT_FALSE(IsValidRequest(request));
    request.headers = {{L"Content-Length", L"1"}};
    EXPECT_FALSE(IsValidRequest(request));
}

// 验证 POST 字节和自定义头真正到达本机服务，并完整读取响应。
// 入参：无。返回：测试断言。
TEST(HttpTest, posts_owned_body_and_headers_through_custom_proxy)
{
    LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    ASSERT_NE(server.port_, 0);
    auto request = server.MakeRequest();
    request.method = Method::Post;
    request.body = std::string(70000, 'p');
    request.headers = {{L"Content-Type", L"application/json"}, {L"X-Test", L"owned"}};
    std::string body;
    const auto result =
        MakeWinHttpTransport()->Perform(request, {},
                                        [&body](auto bytes)
                                        {
                                            body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                            return true;
                                        });
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.status, 200U);
    EXPECT_EQ(body, "hello");
    EXPECT_TRUE(server.Captured().ends_with(request.body));
    EXPECT_NE(server.Captured().find("X-Test: owned"), std::string::npos);
    EXPECT_EQ(server.connections.load(), 1U);
}

// 验证普通 HTTP 错误响应可交给业务解释，但不自动重发同一业务 HTTP。
// 系统连接层的多 IP 候选仍由 WinHTTP 在期限内处理，不属于业务重试。
// 入参：无。返回：测试断言。
TEST(HttpTest, exposes_error_body_without_retry)
{
    LocalServer server("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 4\r\nConnection: close\r\n\r\nslow");
    ASSERT_NE(server.port_, 0);
    std::string body;
    const auto result =
        MakeWinHttpTransport()->Perform(server.MakeRequest(), {},
                                        [&body](auto bytes)
                                        {
                                            body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                            return true;
                                        });
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.status, 429U);
    EXPECT_EQ(body, "slow");
    EXPECT_EQ(server.connections.load(), 1U);
}

// 验证跳转只返回元信息，Update 的仅 200 策略不会消费重定向正文。
// 入参：无。返回：测试断言。
TEST(HttpTest, does_not_redirect_and_can_restrict_consumption_to_200)
{
    LocalServer server(
        "HTTP/1.1 302 Found\r\nLocation: http://other.invalid/\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc");
    ASSERT_NE(server.port_, 0);
    auto request = server.MakeRequest();
    request.consumeStatus = 200;
    bool consumed = false;
    const auto result = MakeWinHttpTransport()->Perform(request, {},
                                                        [&consumed](auto)
                                                        {
                                                            consumed = true;
                                                            return true;
                                                        });
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.status, 302U);
    EXPECT_EQ(result.redirectLocation, L"http://other.invalid/");
    EXPECT_FALSE(consumed);
    EXPECT_EQ(server.connections.load(), 1U);
}

// 验证声明长度和实际块大小都有界，错误正文使用独立更小预算。
// 入参：无。返回：测试断言。
TEST(HttpTest, limits_success_and_error_responses)
{
    for (const bool error : {false, true})
    {
        LocalServer server(std::string("HTTP/1.1 ") + (error ? "500 Error" : "200 OK") +
                           "\r\nConnection: close\r\n\r\n12345678");
        ASSERT_NE(server.port_, 0);
        auto request = server.MakeRequest();
        request.maxResponseBytes = error ? 100 : 4;
        request.maxErrorBytes = 4;
        const auto result = MakeWinHttpTransport()->Perform(request, {}, [](auto) { return true; });
        EXPECT_EQ(result.error, Error::ResponseTooLarge);
    }
}

// 验证声明超限时正文不交给消费者，拒绝 sink 时立即停止。
// 入参：无。返回：测试断言。
TEST(HttpTest, rejects_declared_size_before_consumption_and_stops_rejected_sink)
{
    for (const bool oversized : {false, true})
    {
        LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n12345678");
        ASSERT_NE(server.port_, 0);
        auto request = server.MakeRequest();
        request.maxResponseBytes = oversized ? 4 : 100;
        bool consumed = false;
        const auto result = MakeWinHttpTransport()->Perform(request, {},
                                                            [&consumed](auto)
                                                            {
                                                                consumed = true;
                                                                return false;
                                                            });
        EXPECT_EQ(result.error, oversized ? Error::ResponseTooLarge : Error::SinkRejected);
        EXPECT_EQ(consumed, !oversized);
    }
}

// 验证正文消费者内存不足保留系统错误，普通异常仍归拒绝，且两者均停止单次传输。
// 入参：无。返回：测试断言。
TEST(HttpTest, preserves_sink_allocation_failure_without_retry)
{
    for (const bool allocationFailure : {false, true})
    {
        LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n12345678");
        ASSERT_NE(server.port_, 0);
        unsigned calls = 0;
        const Result result = MakeWinHttpTransport()->Perform(server.MakeRequest(), {},
                                                              [&calls, allocationFailure](auto) -> bool
                                                              {
                                                                  ++calls;
                                                                  if (allocationFailure)
                                                                      throw std::bad_alloc();
                                                                  throw std::runtime_error("test sink rejected");
                                                              });
        EXPECT_EQ(result.error, Error::SinkRejected);
        EXPECT_EQ(result.systemError, allocationFailure ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS);
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(server.connections.load(), 1U);
    }
}

// 验证已取消的请求不建立连接，即使参数和代理均有效。
// 入参：无。返回：测试断言。
TEST(HttpTest, precancelled_request_does_not_connect)
{
    LocalServer server("");
    ASSERT_NE(server.port_, 0);
    std::stop_source stop;
    stop.request_stop();
    const auto result =
        MakeWinHttpTransport()->Perform(server.MakeRequest(), stop.get_token(), [](auto) { return true; });
    EXPECT_EQ(result.error, Error::Cancelled);
    EXPECT_EQ(server.connections.load(), 0U);
}

// 验证响应等待期间取消能快速返回，关闭回调继续独立保活请求上下文。
// 入参：无。返回：测试断言。
TEST(HttpTest, cancels_waiting_post_without_waiting_for_server)
{
    LocalServer server("", true);
    ASSERT_NE(server.port_, 0);
    auto request = server.MakeRequest();
    request.method = Method::Post;
    request.body = std::string(80000, 'x');
    std::stop_source stop;
    auto future =
        std::async(std::launch::async, [&]
                   { return MakeWinHttpTransport()->Perform(request, stop.get_token(), [](auto) { return true; }); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!server.received && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(server.received);
    stop.request_stop();
    EXPECT_EQ(future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(future.get().error, Error::Cancelled);
}

// 验证绝对截止时间包含等待响应阶段，不因服务持续占用而无限等待。
// 入参：无。返回：测试断言。
TEST(HttpTest, total_deadline_bounds_response_wait)
{
    LocalServer server("", true);
    ASSERT_NE(server.port_, 0);
    auto request = server.MakeRequest();
    request.deadline = std::chrono::steady_clock::now() + 250ms;
    const auto result = MakeWinHttpTransport()->Perform(request, {}, [](auto) { return true; });
    EXPECT_EQ(result.error, Error::Timeout);
}

// 验证 PUT/PATCH 使用真实方法且 Content-Type 作为有界元信息供协议层校验。
// 入参：无。返回：测试断言。
TEST(HttpTest, supports_put_patch_and_content_type)
{
    for (const Method method : {Method::Put, Method::Patch})
    {
        LocalServer server("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
                           "2\r\nConnection: close\r\n\r\nok");
        ASSERT_NE(server.port_, 0);
        Request request = server.MakeRequest();
        request.method = method;
        request.body = "payload";
        const Result result = MakeWinHttpTransport()->Perform(request, {}, [](auto) { return true; });
        EXPECT_EQ(result.error, Error::None);
        EXPECT_EQ(result.contentType, L"text/plain; charset=utf-8");
        EXPECT_TRUE(server.Captured().starts_with(method == Method::Put ? "PUT " : "PATCH "));
        EXPECT_TRUE(server.Captured().ends_with("payload"));
    }
}

// 验证 Content-Type 超过 256 字符时拒绝响应，而不是截断后接受伪造文本。
// 入参：无。返回：测试断言。
TEST(HttpTest, rejects_oversized_content_type)
{
    LocalServer server("HTTP/1.1 200 OK\r\nContent-Type: " + std::string(257, 'a') +
                       "\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
    ASSERT_NE(server.port_, 0);
    bool consumed = false;
    const Result result = MakeWinHttpTransport()->Perform(server.MakeRequest(), {},
                                                          [&consumed](auto)
                                                          {
                                                              consumed = true;
                                                              return true;
                                                          });
    EXPECT_NE(result.error, Error::None);
    EXPECT_FALSE(consumed);
}
} // namespace
