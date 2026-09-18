// 验证更新响应校验与真实任务编排；网络完全由私有传输替身提供，不启动安装器。

#include "update_model.h"
#include "update_transport.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <update_client.h>
#include <vector>
#include <windows.h>

namespace open_st
{
struct UpdateClientTestAccess
{
    // 为真实任务注入私有传输，保留其线程、状态与取消实现。
    // 入参：path：隔离缓存；notify：通知；transport：独占替身。
    // 返回：未启动的真实任务。
    static std::unique_ptr<UpdateClient> Create(std::filesystem::path path, std::function<void()> notify,
                                                std::unique_ptr<update_detail::UpdateTransport> transport)
    {
        return std::unique_ptr<UpdateClient>(
            new UpdateClient(std::move(path), std::move(notify), std::move(transport)));
    }
};
} // namespace open_st

namespace
{
using namespace open_st;
using namespace open_st::update_detail;
constexpr std::string_view HASH_ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view SETUP = "Open-ST-0.10.0-win-x64-Setup.exe";
constexpr std::wstring_view RELEASE_URL = L"https://api.github.com/repos/Lucency09/Open-ST/releases/latest";
constexpr std::wstring_view SUMS_URL = L"https://api.github.com/repos/Lucency09/Open-ST/releases/assets/13";

// 构造正式发行响应，所有名称、地址和版本取自相同测试版本。
// 入参：checksums：是否附带校验清单资产。
// 返回：独立 JSON 响应文档。
nlohmann::json ReleaseDocument(bool checksums = false)
{
    nlohmann::json result{
        {"id", 123U},
        {"draft", false},
        {"prerelease", false},
        {"tag_name", "v0.10.0"},
        {"html_url", "https://github.com/Lucency09/Open-ST/releases/tag/v0.10.0"},
        {"assets", nlohmann::json::array(
                       {{{"name", SETUP},
                         {"id", 11U},
                         {"size", 3U},
                         {"state", "uploaded"},
                         {"digest", "sha256:" + std::string(HASH_ABC)}},
                        {{"name", "Open-ST-0.10.0-win-x64.zip"}, {"id", 12U}, {"size", 3U}, {"state", "uploaded"}}})}};
    if (checksums)
        result["assets"].push_back(
            {{"name", "SHA256SUMS.txt"}, {"id", 13U}, {"size", 67U + SETUP.size()}, {"state", "uploaded"}});
    return result;
}

// 验证严格数值三元组排序，拒绝后缀、溢出、缺段及非规范数字。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(UpdateModelTest, versions_compare_numerically_and_reject_ambiguous_values)
{
    const auto old = ParseVersion("v0.9.0");
    const auto next = ParseVersion("0.10.0");
    ASSERT_TRUE(old.has_value());
    ASSERT_TRUE(next.has_value());
    EXPECT_LT(*old, *next);
    EXPECT_EQ(ParseVersion("v0.10.0"), next);
    for (const std::string_view invalid : {"0.10", "0.10.0.1", "0.10.0-beta", "0.10.0+meta", "v00.10.0", "0.-1.0",
                                           "+0.10.0", "0.4294967296.0", "V0.10.0", " 0.10.0", "0.10.0 "})
    {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(ParseVersion(invalid).has_value());
    }
}

// 验证受信 URL 必须精确命中 HTTPS 主机和固定元数据入口。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(UpdateModelTest, trusted_urls_reject_suffixes_credentials_and_downgrades)
{
    EXPECT_TRUE(TrustedUrl(RELEASE_URL, false));
    EXPECT_TRUE(TrustedUrl(SUMS_URL, true));
    EXPECT_TRUE(TrustedUrl(L"https://release-assets.githubusercontent.com/asset?token=test", true));
    EXPECT_TRUE(TrustedUrl(L"https://objects.githubusercontent.com/asset", true));
    EXPECT_FALSE(TrustedUrl(L"https://release-assets.githubusercontent.com/asset", false));
    for (const std::wstring_view invalid :
         {L"http://api.github.com/repos/Lucency09/Open-ST/releases/latest",
          L"https://api.github.com.evil.example/repos/Lucency09/Open-ST/releases/latest",
          L"https://user@api.github.com/repos/Lucency09/Open-ST/releases/latest",
          L"https://api.github.com:443/repos/Lucency09/Open-ST/releases/latest",
          L"https://api.github.com/repos/Other/Open-ST/releases/latest",
          L"https://release-assets.githubusercontent.com.evil.example/asset",
          L"https://objects.githubusercontent.com/asset#fragment",
          L"https://objects.githubusercontent.com\\evil.example/asset",
          L"https://objects.githubusercontent.com/asset\r\nInjected:1"})
    {
        EXPECT_FALSE(TrustedUrl(invalid, false));
        EXPECT_FALSE(TrustedUrl(invalid, true));
    }
}

// 验证清单精确匹配且唯一，拒绝重复目标和同前缀的其他文件名。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(UpdateModelTest, checksums_require_one_exact_target)
{
    const std::string line = std::string(HASH_ABC) + "  " + std::string(SETUP) + "\n";
    EXPECT_EQ(ChecksumFor(line, SETUP), HASH_ABC);
    EXPECT_EQ(ChecksumFor(std::string(HASH_ABC) + " *" + std::string(SETUP) + "\r\n", SETUP), HASH_ABC);
    EXPECT_FALSE(ChecksumFor(line + line, SETUP).has_value());
    EXPECT_FALSE(ChecksumFor(std::string(HASH_ABC) + "  " + std::string(SETUP) + ".zip\n", SETUP).has_value());
    EXPECT_FALSE(ChecksumFor(std::string(64, 'g') + "  " + std::string(SETUP) + "\n", SETUP).has_value());
    EXPECT_EQ(ChecksumFor(std::string(HASH_ABC) + "  " + std::string(SETUP) + ".zip\n" + line, SETUP), HASH_ABC);
}

// 验证正式发行字段完整且页面固定，失败不得发布半份候选。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(UpdateModelTest, release_rejects_wrong_page_prerelease_and_missing_required_fields)
{
    const auto baseline = ReleaseDocument();
    UpdateRelease release;
    ASSERT_TRUE(ParseRelease(baseline.dump(), release));
    EXPECT_EQ(release.versionText, "0.10.0");
    EXPECT_EQ(release.id, 123U);
    ASSERT_TRUE(release.installer.has_value());
    EXPECT_EQ(release.installer->name, SETUP);
    for (const char* field : {"id", "draft", "prerelease", "tag_name", "html_url", "assets"})
    {
        auto bad = baseline;
        bad.erase(field);
        EXPECT_FALSE(ParseRelease(bad.dump(), release)) << field;
        EXPECT_EQ(release.versionText, "0.10.0");
    }
    for (const char* field : {"draft", "prerelease"})
    {
        auto bad = baseline;
        bad[field] = true;
        EXPECT_FALSE(ParseRelease(bad.dump(), release));
    }
    auto bad = baseline;
    bad["id"] = 0U;
    EXPECT_FALSE(ParseRelease(bad.dump(), release));
    bad["id"] = -1;
    EXPECT_FALSE(ParseRelease(bad.dump(), release));
    bad["id"] = "123";
    EXPECT_FALSE(ParseRelease(bad.dump(), release));
    bad = baseline;
    bad["html_url"] = "https://github.com/Other/Open-ST/releases/tag/v0.10.0";
    EXPECT_FALSE(ParseRelease(bad.dump(), release));
    bad["html_url"] = "http://github.com/Lucency09/Open-ST/releases/tag/v0.10.0";
    EXPECT_FALSE(ParseRelease(bad.dump(), release));
}

// 验证资产名称严格含版本和架构，重复资产或不合法大小不能被悄悄挑选。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(UpdateModelTest, release_assets_require_exact_names_unique_entries_and_valid_sizes)
{
    auto document = ReleaseDocument();
    UpdateRelease release;
    document["assets"].push_back(document["assets"][0]);
    EXPECT_FALSE(ParseRelease(document.dump(), release));
    for (const std::string_view invalidName :
         {"setup.exe", "Open-ST-0.9.0-win-x64-Setup.exe", "Open-ST-0.10.0-win-arm64-Setup.exe",
          "Open-ST-0.10.0-win-x64-Setup.exe.zip"})
    {
        document = ReleaseDocument();
        document["assets"][0]["name"] = invalidName;
        ASSERT_TRUE(ParseRelease(document.dump(), release));
        EXPECT_FALSE(release.installer.has_value());
    }
    for (const std::uint64_t size : {0ULL, MAX_INSTALLER_BYTES + 1ULL})
    {
        document = ReleaseDocument();
        document["assets"][0]["size"] = size;
        EXPECT_FALSE(ParseRelease(document.dump(), release));
    }
    document = ReleaseDocument();
    document["assets"][0]["id"] = -1;
    EXPECT_FALSE(ParseRelease(document.dump(), release));
    document = ReleaseDocument();
    document["assets"][0]["state"] = "new";
    EXPECT_FALSE(ParseRelease(document.dump(), release));
    document = ReleaseDocument();
    document["assets"][0]["digest"] = "sha256:not-a-digest";
    EXPECT_FALSE(ParseRelease(document.dump(), release));
}

struct Reply
{
    std::string body;
    unsigned status{200};
    UpdateHttpError error{UpdateHttpError::None};
    std::optional<std::uint64_t> declared;
    std::wstring redirect;
    bool block{};
};
struct FakeState
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Reply> replies;
    std::vector<UpdateHttpRequest> requests;
    bool release{};
    std::size_t completed{};
};
class FakeTransport final : public UpdateTransport
{
  public:
    // 接收线程安全的测试响应队列。
    // 入参：state：测试与真实工作线程共享的替身状态。
    // 返回：无返回值。
    explicit FakeTransport(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    // 返回脚本指定响应，可故意在取消后发送迟到字节验证任务自身保护。
    // 入参：request：真实任务请求；忽略的 stop 用于模拟不合作的迟到传输；sink：真实任务消费者。
    // 返回：脚本状态或明确替身失败，不访问网络。
    UpdateHttpResult Get(const UpdateHttpRequest& request, std::stop_token, const UpdateHttpSink& sink) override
    {
        Reply reply;
        {
            std::unique_lock lock(this->state_->mutex);
            this->state_->requests.push_back(request);
            if (this->state_->replies.empty())
                reply.error = UpdateHttpError::Network;
            else
            {
                reply = std::move(this->state_->replies.front());
                this->state_->replies.pop_front();
            }
            this->state_->changed.notify_all();
            if (reply.block)
                this->state_->changed.wait_for(lock, std::chrono::seconds(5),
                                               // 只由测试释放，不把 stop 当成传输一定合作的假设。
                                               // 入参：无。
                                               // 返回：允许返回迟到响应时 true。
                                               [this]() { return this->state_->release; });
        }
        if (reply.error == UpdateHttpError::None && reply.status == 200 &&
            !sink(std::as_bytes(std::span(reply.body.data(), reply.body.size()))))
            reply.error = UpdateHttpError::SinkRejected;
        {
            const std::scoped_lock lock(this->state_->mutex);
            ++this->state_->completed;
        }
        this->state_->changed.notify_all();
        return {reply.error, 0, reply.status, reply.declared, reply.redirect};
    }

  private:
    std::shared_ptr<FakeState> state_;
};

class UpdateClientTest : public testing::Test
{
  protected:
    // 创建隔离缓存与真实异步任务，只注入网络边界。
    // 入参：无。
    // 返回：无返回值。
    void SetUp() override
    {
        const auto name = testing::UnitTest::GetInstance()->current_test_info()->name();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_update_" + std::to_string(GetCurrentProcessId()) + "_" + name);
        std::filesystem::create_directories(this->root_);
        this->state_ = std::make_shared<FakeState>();
        this->client_ = UpdateClientTestAccess::Create(
            this->root_ / "cache",
            // 唤醒等待快照的测试线程，不操作 UI。
            // 入参：无。
            // 返回：无返回值。
            [state = this->state_]()
            {
                const std::scoped_lock lock(state->mutex);
                state->changed.notify_all();
            },
            std::make_unique<FakeTransport>(this->state_));
    }
    // 先释放可能阻塞的替身，再取消并回收任务，最后清理隔离目录。
    // 入参：无。
    // 返回：无返回值。
    void TearDown() override
    {
        this->ReleaseBlocked();
        this->client_.reset();
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }
    // 追加响应并通知替身。
    // 入参：reply：一次请求的响应。
    // 返回：无返回值。
    void Push(Reply reply)
    {
        const std::scoped_lock lock(this->state_->mutex);
        this->state_->replies.push_back(std::move(reply));
    }
    // 等待真实任务离开正在处理的阶段。
    // 入参：无。
    // 返回：已发布稳定阶段的快照；超时用测试断言失败。
    UpdateSnapshot WaitResult()
    {
        std::unique_lock lock(this->state_->mutex);
        const bool completed = this->state_->changed.wait_for(lock, std::chrono::seconds(5),
                                                              // 读取真实状态，不以替身响应返回冒充任务已完成。
                                                              // 入参：无。
                                                              // 返回：任务已到非活动状态时 true。
                                                              [this]()
                                                              {
                                                                  const auto phase = this->client_->Snapshot().phase;
                                                                  return phase != UpdatePhase::Checking &&
                                                                         phase != UpdatePhase::Downloading;
                                                              });
        EXPECT_TRUE(completed);
        return this->client_->Snapshot();
    }
    // 等待指定数量请求进入传输边界。
    // 入参：count：期望至少收到的请求数。
    // 返回：在测试截止前达到数量为 true。
    bool WaitRequests(std::size_t count)
    {
        std::unique_lock lock(this->state_->mutex);
        return this->state_->changed.wait_for(lock, std::chrono::seconds(5),
                                              // 检查受互斥保护的请求集合。
                                              // 入参：无。
                                              // 返回：目标请求已进入时 true。
                                              [this, count]() { return this->state_->requests.size() >= count; });
    }
    // 复制请求集合供断言，不借用工作线程仍可能修改的容器。
    // 入参：无。
    // 返回：独立请求副本。
    std::vector<UpdateHttpRequest> Requests()
    {
        const std::scoped_lock lock(this->state_->mutex);
        return this->state_->requests;
    }
    // 显式释放迟到响应，断言中途退出也由 TearDown 调用。
    // 入参：无。
    // 返回：无返回值。
    void ReleaseBlocked()
    {
        {
            const std::scoped_lock lock(this->state_->mutex);
            this->state_->release = true;
        }
        this->state_->changed.notify_all();
    }
    std::filesystem::path root_;
    std::shared_ptr<FakeState> state_;
    std::unique_ptr<UpdateClient> client_;
};

// 验证安装版查询只读元数据，明确确认后才发资产请求。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, installed_query_waits_for_confirmation_before_requesting_assets)
{
    this->Push({ReleaseDocument(true).dump()});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    ASSERT_EQ(this->WaitResult().phase, UpdatePhase::Available);
    auto requests = this->Requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].url, RELEASE_URL);
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "cache"));
    this->Push({{}, 404});
    ASSERT_TRUE(this->client_->DownloadConfirmed());
    EXPECT_EQ(this->WaitResult().error, UpdateError::NotFound);
    requests = this->Requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[1].url, SUMS_URL);
    EXPECT_EQ(requests[1].accept, L"application/octet-stream");
}

// 验证便携版只返回页面信息，下载确认入口始终拒绝。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, portable_release_never_downloads_setup)
{
    this->Push({ReleaseDocument().dump()});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Portable));
    const auto result = this->WaitResult();
    EXPECT_EQ(result.phase, UpdatePhase::Available);
    EXPECT_EQ(result.releasePage, L"https://github.com/Lucency09/Open-ST/releases/tag/v0.10.0");
    EXPECT_FALSE(this->client_->DownloadConfirmed());
    EXPECT_EQ(this->Requests().size(), 1U);
    bool launched{};
    // 检查无 Ready 时绝不调用外部执行回调。
    // 入参：忽略的路径不得传入。
    // 返回：若误调用返回 true 以便断言暴露缺陷。
    EXPECT_FALSE(this->client_->LaunchReady(
        [&launched](const std::filesystem::path&)
        {
            launched = true;
            return true;
        }));
    EXPECT_FALSE(launched);
}

// 验证同版和本地较新版本均不能进入下载流程。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, current_and_local_ahead_versions_have_no_download)
{
    this->Push({ReleaseDocument().dump()});
    ASSERT_TRUE(this->client_->Check("0.10.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().phase, UpdatePhase::Current);
    EXPECT_FALSE(this->client_->DownloadConfirmed());
    this->Push({ReleaseDocument().dump()});
    ASSERT_TRUE(this->client_->Check("0.11.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().phase, UpdatePhase::LocalAhead);
    EXPECT_FALSE(this->client_->DownloadConfirmed());
    EXPECT_EQ(this->Requests().size(), 2U);
}

// 验证缺少本发行类型所需资产报告 MissingPackage，不用近似文件名替代。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, missing_required_package_is_an_explicit_failure)
{
    auto release = ReleaseDocument();
    release["assets"][0]["name"] = "setup.exe";
    this->Push({release.dump()});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    const auto result = this->WaitResult();
    EXPECT_EQ(result.phase, UpdatePhase::Failed);
    EXPECT_EQ(result.error, UpdateError::MissingPackage);
    EXPECT_FALSE(this->client_->DownloadConfirmed());
}

// 验证 HTTP 404、限流和传输超时分别归类，失败不执行资产请求。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, request_failures_are_classified_without_asset_requests)
{
    const std::vector<std::pair<Reply, UpdateError>> cases{{{"", 404}, UpdateError::NotFound},
                                                           {{"", 429}, UpdateError::RateLimited},
                                                           {{"", 403}, UpdateError::RateLimited},
                                                           {{"", 0, UpdateHttpError::Timeout}, UpdateError::Timeout},
                                                           {{"", 0, UpdateHttpError::Network}, UpdateError::Network}};
    for (const auto& [reply, expected] : cases)
    {
        this->Push(reply);
        ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
        const auto result = this->WaitResult();
        EXPECT_EQ(result.phase, UpdatePhase::Failed);
        EXPECT_EQ(result.error, expected);
        EXPECT_FALSE(this->client_->DownloadConfirmed());
    }
    EXPECT_EQ(this->Requests().size(), cases.size());
}

// 验证元数据声明长度和实际体积必须一致，超大响应不能发布发行信息。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, metadata_length_mismatch_and_oversized_body_cannot_be_available)
{
    const std::string body = ReleaseDocument().dump();
    this->Push({body, 200, UpdateHttpError::None, body.size() + 1U});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().error, UpdateError::InvalidRelease);
    this->Push({std::string(MAX_METADATA_BYTES + 1U, 'x')});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().phase, UpdatePhase::Failed);
    EXPECT_FALSE(this->client_->DownloadConfirmed());
}

// 验证校验清单的声明长度不符时提前失败，不进入二进制下载或文件执行。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, checksum_length_mismatch_blocks_binary_download)
{
    this->Push({ReleaseDocument(true).dump()});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    ASSERT_EQ(this->WaitResult().phase, UpdatePhase::Available);
    const std::string sums = std::string(HASH_ABC) + "  " + std::string(SETUP) + "\n";
    this->Push({sums, 200, UpdateHttpError::None, sums.size() + 1U});
    ASSERT_TRUE(this->client_->DownloadConfirmed());
    const auto result = this->WaitResult();
    EXPECT_EQ(result.phase, UpdatePhase::Failed);
    EXPECT_EQ(result.error, UpdateError::Integrity);
    EXPECT_EQ(this->Requests().size(), 2U);
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "cache"));
}

// 验证用户取消后迟到成功响应不能恢复 Available、Ready 或执行资格。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, cancellation_rejects_late_query_and_download_responses)
{
    Reply query{ReleaseDocument(true).dump()};
    query.block = true;
    this->Push(query);
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    ASSERT_TRUE(this->WaitRequests(1));
    EXPECT_FALSE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    this->client_->Cancel();
    EXPECT_EQ(this->client_->Snapshot().phase, UpdatePhase::Cancelled);
    this->ReleaseBlocked();
    this->Push({ReleaseDocument(true).dump()});
    // 新操作必须先回收旧工作线程，从而确定迟到响应已返回。
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    ASSERT_EQ(this->WaitResult().phase, UpdatePhase::Available);
    {
        const std::scoped_lock lock(this->state_->mutex);
        this->state_->release = false;
    }
    Reply sums{std::string(HASH_ABC) + "  " + std::string(SETUP) + "\n"};
    sums.block = true;
    this->Push(sums);
    ASSERT_TRUE(this->client_->DownloadConfirmed());
    ASSERT_TRUE(this->WaitRequests(3));
    this->client_->Cancel();
    EXPECT_EQ(this->client_->Snapshot().phase, UpdatePhase::Cancelled);
    bool launched{};
    // 取消后即使传输尚未合作退出，也不授予执行资格。
    // 入参：忽略的路径。
    // 返回：若错误进入则标记并接受，供断言捕获。
    EXPECT_FALSE(this->client_->LaunchReady(
        [&launched](const std::filesystem::path&)
        {
            launched = true;
            return true;
        }));
    this->ReleaseBlocked();
    this->Push({ReleaseDocument().dump()});
    ASSERT_TRUE(this->client_->Check("0.10.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().phase, UpdatePhase::Current);
    EXPECT_FALSE(launched);
    EXPECT_EQ(this->Requests().size(), 4U);
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "cache"));
}

// 验证跨主机重定向被阻止，传输不会收到攻击者 URL。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(UpdateClientTest, untrusted_redirect_is_rejected_before_transport)
{
    this->Push({{}, 302, UpdateHttpError::None, {}, L"https://api.github.com.evil.example/releases/latest"});
    ASSERT_TRUE(this->client_->Check("0.9.0", UpdateDistribution::Installed));
    EXPECT_EQ(this->WaitResult().phase, UpdatePhase::Failed);
    EXPECT_EQ(this->Requests().size(), 1U);
}
} // namespace
