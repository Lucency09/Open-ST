// 使用统一提供方替身测试真实工作线程的顺序、取消、期限和本地零网络边界。
#include <algorithm>
#include <atomic>
#include <chrono>
#include "translation_test_defaults.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <translation_protocol.h>
#include <translation_provider.h>
#include <windows.h>
#include <winhttp.h>

namespace open_st
{
struct TranslationClientTestAccess
{
    // 注入提供方工厂，复用产品真实单工作线程。
    // 入参：dependencies 为测试依赖。
    // 返回：独占客户端值，使用保证省略复制的构造。
    static TranslationClient Create(std::unique_ptr<translation_detail::ClientDependencies> dependencies)
    {
        return TranslationClient({}, std::move(dependencies));
    }
};
namespace
{
using namespace std::chrono_literals;
using translation_detail::Clock;
struct State
{
    std::mutex mutex;
    std::vector<std::string> calls;
    std::vector<std::thread::id> threads;
    std::vector<TranslationError> results;
    std::vector<TranslationError> validationResults;
    std::vector<TranslationRequest> validated, executed;
    std::vector<Clock::time_point> validationStarts, deadlines;
    std::vector<std::string> outputs, detectedLanguages;
    std::atomic_bool validationEntered{}, holdValidation{};
    std::size_t holdExecuteCall{};
    std::chrono::milliseconds executeDelay{};
    std::atomic_bool entered{}, release{}, hold{}, ignoreCancel{}, overrun{}, maintained{};
    std::atomic<unsigned> creations{}, active{}, peak{};
};
class FakeProvider final : public translation_detail::ITranslationProvider
{
  public:
    // 借用共享观测状态，不产生自己的线程。
    // 入参：state 为测试观察数据。
    // 返回：替身实例。
    explicit FakeProvider(std::shared_ptr<State> state) : state_(std::move(state))
    {
        ++this->state_->creations;
    }
    // 声明测试能力，实际执行仍由产品调度控制。
    // 入参：profile 为已验证条目。
    // 返回：测试语言对。
    translation_detail::ProviderCapabilities Capabilities(const nlohmann::json&) const override
    {
        return {true, true, 10000, {{"auto", "zh-CN"}}, {}};
    }
    // 通过名称构造缺失凭据，避免改变正式 JSON 结构。
    // 入参：input 为当前片段快照，profile 为条目。
    // 返回：skip 项返回 MissingCredentials。
    TranslationError Validate(const TranslationRequest& input, const nlohmann::json& profile) const override
    {
        TranslationError error = TranslationError::None;
        {
            const std::scoped_lock lock(this->state_->mutex);
            const std::size_t index = this->state_->validated.size();
            this->state_->validated.push_back(input);
            this->state_->validationStarts.push_back(Clock::now());
            if (index < this->state_->validationResults.size())
                error = this->state_->validationResults[index];
        }
        this->state_->validationEntered = true;
        while (this->state_->holdValidation && !this->state_->release)
            std::this_thread::sleep_for(1ms);
        if (profile.at("name") == "slow-validation")
            std::this_thread::sleep_for(70ms);
        return profile.at("name") == "skip" ? TranslationError::MissingCredentials : error;
    }
    // 模拟单次同步执行，可模拟合作取消及不可中断收尾。
    // 入参：input/profile 为快照，deadline 为期限，stop 为取消。
    // 返回：预设领域结果。
    translation_detail::ProviderResult Execute(const TranslationRequest& input, const nlohmann::json& profile,
                                               Clock::time_point deadline, std::stop_token stop) override
    {
        const unsigned active = ++this->state_->active;
        this->state_->peak.store(std::max(this->state_->peak.load(), active));
        TranslationError error = TranslationError::None;
        std::string output = "译文", detectedLanguage = "en";
        std::size_t call{};
        {
            const std::scoped_lock lock(this->state_->mutex);
            const std::size_t index = this->state_->calls.size();
            this->state_->calls.push_back(profile.at("name").get<std::string>());
            this->state_->threads.push_back(std::this_thread::get_id());
            this->state_->executed.push_back(input);
            this->state_->deadlines.push_back(deadline);
            call = index + 1;
            if (index < this->state_->results.size())
                error = this->state_->results[index];
            if (index < this->state_->outputs.size())
                output = this->state_->outputs[index];
            if (index < this->state_->detectedLanguages.size())
                detectedLanguage = this->state_->detectedLanguages[index];
        }
        this->state_->entered = true;
        while ((this->state_->hold || call == this->state_->holdExecuteCall) && !this->state_->release)
        {
            if (!this->state_->ignoreCancel && stop.stop_requested())
                break;
            std::this_thread::sleep_for(1ms);
        }
        if (error == TranslationError::Timeout)
        {
            while (Clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            error = TranslationError::None;
        }
        if (this->state_->overrun)
            std::this_thread::sleep_for(70ms);
        if (this->state_->executeDelay != 0ms)
            std::this_thread::sleep_for(this->state_->executeDelay);
        --this->state_->active;
        this->maintenance_ = Clock::now() + 10ms;
        return {translation_detail::StatusOf(error), error, error == TranslationError::None ? output : "",
                detectedLanguage};
    }
    // 返回测试维护到期时刻，证明产品空闲循环负责调度。
    // 入参：无。
    // 返回：到期时间。
    Clock::time_point NextMaintenance() const noexcept override
    {
        return this->maintenance_;
    }
    // 标记到期维护执行，不创建第二个定时线程。
    // 入参：now 为当前时间。
    // 返回：无。
    void Maintain(Clock::time_point now) noexcept override
    {
        if (now >= this->maintenance_)
        {
            this->state_->maintained = true;
            this->maintenance_ = Clock::time_point::max();
        }
    }

  private:
    std::shared_ptr<State> state_;
    Clock::time_point maintenance_{Clock::time_point::max()};
};
struct TransportState
{
    std::vector<http::Request> requests;
    std::vector<http::Result> responses;
    std::vector<std::string> bodies;
    bool overrun{};
};
class FixtureTransport final : public http::Transport
{
  public:
    // 注入有界合成响应，只替换网络层而保留真实提供方和调度。
    // 入参：state 为请求观测和响应样例。
    // 返回：传输替身。
    explicit FixtureTransport(std::shared_ptr<TransportState> state) : state_(std::move(state)) {}
    // 同步消费样例正文，不创建任何系统连接或后台请求。
    // 入参：request 为真实适配器请求，stop 为取消，sink 为正文接收器。
    // 返回：预设传输结果。
    http::Result Perform(const http::Request& request, std::stop_token, const http::Sink& sink) override
    {
        const std::size_t index = this->state_->requests.size();
        this->state_->requests.push_back(request);
        if (this->state_->overrun)
            std::this_thread::sleep_until(request.deadline + 1ms);
        if (index < this->state_->bodies.size())
        {
            const std::string& body = this->state_->bodies[index];
            if (!sink(std::as_bytes(std::span(body.data(), body.size()))))
                return {http::Error::SinkRejected};
        }
        return index < this->state_->responses.size() ? this->state_->responses[index] : http::Result{};
    }

  private:
    std::shared_ptr<TransportState> state_;
};
// 创建真实协议工厂，本地模型缺失使用专属不存在的路径，在线只走合成传输。
// 入参：state 为合成传输，item 为单项预算。
// 返回：统一生产适配器依赖。
std::unique_ptr<translation_detail::ClientDependencies> ProtocolDependencies(std::shared_ptr<TransportState> state,
                                                                             std::chrono::milliseconds item = 1s)
{
    auto dependencies = std::make_unique<translation_detail::ClientDependencies>();
    dependencies->factory = [state](std::string_view kind) -> std::unique_ptr<translation_detail::ITranslationProvider>
    {
        if (kind == "ctranslate2_local")
            return translation_detail::CreateProvider(kind, std::filesystem::temp_directory_path() /
                                                                L"open-st-nonexistent-model-fixture");
        return translation_detail::CreateOnlineProvider(kind, std::make_unique<FixtureTransport>(state));
    };
    dependencies->itemBudget = item;
    dependencies->totalBudget = 5s;
    return dependencies;
}
// 配置合成凭据的自定义条目，使用无外网的固定示例地址。
// 入参：无。
// 返回：可执行配置。
nlohmann::json CustomProfile()
{
    nlohmann::json profile = TestTranslationProfile("custom_http");
    profile["enabled"] = true;
    profile["secrets"]["api_key"] = "fixture-key";
    return profile;
}
// 建立多条同类型配置，身份由领域创建而不是靠位置。
// 入参：names 为条目名称。
// 返回：完整整轮请求。
TranslationRequest Request(std::initializer_list<std::string_view> names)
{
    TranslationRequest request = TestTranslationRequest();
    request.text = "hello";
    request.sessionId = 7;
    request.sourceRevision = 9;
    request.configurationRevision = 11;
    request.configuration.interfaces = nlohmann::json::array();
    for (const std::string_view name : names)
    {
        nlohmann::json profile = TestTranslationProfile("google");
        profile["name"] = name;
        profile["enabled"] = true;
        request.configuration.interfaces.push_back(std::move(profile));
    }
    return request;
}
// 为不同测试注入预算与唯一提供方工厂。
// 入参：state 为记录，item/total 为测试短预算。
// 返回：客户端依赖。
std::unique_ptr<translation_detail::ClientDependencies> Dependencies(std::shared_ptr<State> state,
                                                                     std::chrono::milliseconds item = 1s,
                                                                     std::chrono::milliseconds total = 5s)
{
    auto dependencies = std::make_unique<translation_detail::ClientDependencies>();
    dependencies->factory = [state](std::string_view) { return std::make_unique<FakeProvider>(state); };
    dependencies->itemBudget = item;
    dependencies->totalBudget = total;
    return dependencies;
}
// 等待后台确实收尾，设置有界测试等待避免挂起测试进程。
// 入参：client 为任务客户端；budget为最长等待，真实模型用例放宽至60秒。
// 返回：最终快照或超时仍忙快照。
TranslationSnapshot Finish(TranslationClient& client, std::chrono::milliseconds budget = 3s)
{
    const Clock::time_point end = Clock::now() + budget;
    while (client.Snapshot().busy && Clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return client.Snapshot();
}
// 等待指定原子标记，测试线程只做同步不负责提供方执行。
// 入参：flag 为跨线程事件。
// 返回：一秒内发生时 true。
bool Wait(std::atomic_bool& flag)
{
    const Clock::time_point end = Clock::now() + 1s;
    while (!flag && Clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return flag.load();
}
// 创建每段有独立字节身份、固定安全句末及换行的长文。
// 入参：count 为正文段数量。返回：每个正文176字符、段间一个LF的完整原文。
std::string SegmentedText(std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
            text += '\n';
        text += std::string(175, static_cast<char>('a' + index % 26)) + ".";
    }
    return text;
}
// 等待指定片段实际进入Execute；读取记录时持锁，避免与工作线程竞争。
// 入参：state为共享记录，count为至少进入的调用数。返回：一秒内达到时true。
bool WaitCalls(const std::shared_ptr<State>& state, std::size_t count)
{
    const Clock::time_point end = Clock::now() + 1s;
    while (Clock::now() < end)
    {
        {
            const std::scoped_lock lock(state->mutex);
            if (state->calls.size() >= count)
                return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
class SegmentedLocalClientTest : public testing::Test
{
  protected:
    std::filesystem::path root;
    // 为真实客户端准备完整双向资源副本，只读构建缓存，不访问用户data。
    // 入参：无。返回：无；缺少缓存按测试失败处理。
    void SetUp() override
    {
        this->root = std::filesystem::path(OPEN_ST_TRANSLATION_CLIENT_TEST_OUTPUT) /
                     (L"分段 客户端 " + std::to_wstring(GetCurrentProcessId()));
        ASSERT_FALSE(std::filesystem::exists(this->root));
        ASSERT_TRUE(std::filesystem::create_directories(this->root));
        for (const std::string_view direction : {"en-zh", "zh-en"})
        {
            const std::filesystem::path destination = this->root / "resources/translation" / direction;
            std::filesystem::create_directories(destination);
            std::filesystem::copy(std::filesystem::path(OPEN_ST_TRANSLATION_CACHE) / direction, destination,
                                  std::filesystem::copy_options::recursive);
        }
    }
    // 真实客户端已析构后清理本例专属模型副本。
    // 入参：无。返回：无。
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(this->root, error);
        EXPECT_FALSE(error);
    }
};
} // namespace
// 验证按启用顺序执行、成功后零后续调用及同类型共享唯一实例。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, OrderedFallbackStopsAfterSuccess)
{
    auto state = std::make_shared<State>();
    state->results = {TranslationError::Authentication, TranslationError::Quota, TranslationError::None};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"first", "second", "third", "never"}), id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_FALSE(snapshot.busy);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(snapshot.result);
    EXPECT_EQ(snapshot.result->profileName, "third");
    EXPECT_EQ(snapshot.result->sessionId, 7U);
    EXPECT_EQ(snapshot.result->sourceRevision, 9U);
    EXPECT_EQ(snapshot.result->configurationRevision, 11U);
    EXPECT_EQ(snapshot.result->requestId, id);
    EXPECT_EQ(snapshot.attempts.size(), 3U);
    EXPECT_EQ(snapshot.unattempted, 1U);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"first", "second", "third"}));
    EXPECT_EQ(state->creations.load(), 1U);
    EXPECT_EQ(state->peak.load(), 1U);
    EXPECT_EQ(state->threads[0], state->threads[2]);
}
// 验证明确输入错误停止整轮，而停用项和缺凭据项不执行。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, SkipsIncompleteAndStopsOnInputError)
{
    auto state = std::make_shared<State>();
    state->results = {TranslationError::InvalidInput};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"disabled", "skip", "input", "never"});
    request.configuration.interfaces[0]["enabled"] = false;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::InvalidInput);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_TRUE(snapshot.attempts[0].skipped);
    EXPECT_EQ(snapshot.totalInterfaces, 3U);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"input"}));
}
// 验证取消立即撤销结果、旧不可中断执行结束前拒绝新任务并不切换下家。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, CancelBlocksLateResultAndKeepsSingleSlot)
{
    auto state = std::make_shared<State>();
    state->hold = true;
    state->ignoreCancel = true;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"first", "never"}), id, error));
    ASSERT_TRUE(Wait(state->entered));
    client.Cancel();
    EXPECT_TRUE(client.Snapshot().busy);
    EXPECT_FALSE(client.Snapshot().result);
    EXPECT_FALSE(client.Submit(Request({"new"}), id, error));
    EXPECT_EQ(error, TranslationError::Busy);
    state->release = true;
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.phase, TranslationPhase::Cancelled);
    EXPECT_FALSE(snapshot.result);
    EXPECT_EQ(state->calls.size(), 1U);
    ASSERT_TRUE(client.Submit(Request({"new"}), id, error));
    EXPECT_EQ(Finish(client).phase, TranslationPhase::Succeeded);
}
// 验证单项超时后顺序继续，迟到成功也不能当成本项成功。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, ItemDeadlineFallsBackAfterActualReturn)
{
    auto state = std::make_shared<State>();
    state->results = {TranslationError::Timeout, TranslationError::None};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 20ms, 1s));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"slow", "next"}), id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::Timeout);
    EXPECT_EQ(state->peak.load(), 1U);
}
// 验证本地不可中断收尾超过整轮期限时，后续在线项不会启动。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, RoundBudgetPreventsAnyLaterProvider)
{
    auto state = std::make_shared<State>();
    state->overrun = true;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 10ms, 30ms));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"loading", "never"}), id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::BudgetExhausted);
    EXPECT_EQ(snapshot.unattempted, 1U);
    EXPECT_EQ(state->calls.size(), 1U);
}
// 验证校验和提供方初始化计入单项预算，过期项不会进入 Execute。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, ExpiredValidationCannotStartProviderExecution)
{
    auto state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 20ms, 1s));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"slow-validation", "next"}), id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::Timeout);
    EXPECT_TRUE(snapshot.attempts[0].skipped);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"next"}));
}
// 验证资源失败、明确输入错误不会因为同时过期而错误回退到其他服务。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, FatalErrorsAfterDeadlineStillStopTheRound)
{
    for (const TranslationError fatal :
         {TranslationError::OutOfMemory, TranslationError::InvalidInput, TranslationError::InvalidConfiguration})
    {
        auto state = std::make_shared<State>();
        state->results = {fatal};
        state->overrun = true;
        TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 10ms, 1s));
        std::uint64_t id{};
        TranslationError error{};
        ASSERT_TRUE(client.Submit(Request({"late-fatal", "never"}), id, error));
        const TranslationSnapshot snapshot = Finish(client);
        EXPECT_EQ(snapshot.error, fatal);
        EXPECT_FALSE(snapshot.result);
        EXPECT_EQ(snapshot.unattempted, 1U);
        EXPECT_EQ(state->calls.size(), 1U);
    }
}
// 验证非法全局输入/列表在创建任何提供方前拒绝。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, GlobalValidationSendsNothing)
{
    auto state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    std::uint64_t id{};
    TranslationError error{};
    TranslationRequest request = Request({"first"});
    request.text = "";
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::InvalidInput);
    request.text.assign(100001, 'a');
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::InputTooLarge);
    request = Request({"first"});
    request.configuration.interfaces.push_back(request.configuration.interfaces[0]);
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::InvalidConfiguration);
    request = Request({});
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::NoInterfaces);
    EXPECT_EQ(state->creations.load(), 0U);
}
// 验证所有片段沿用原始语言、代理和整篇身份，只有完整成功才发布。
// 入参：三段原文和逐段合成译文。返回：断言结果。
TEST(TranslationChain, SegmentsUseOriginalOptionsAndPublishOneCompleteIdentity)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->outputs = {"第一段", "第二段", "第三段"};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "never"});
    request.text = SegmentedText(3);
    request.options.sourceLanguage = "en";
    request.configuration.proxyMode = "custom";
    request.configuration.proxyAddress = "localhost:8765";
    const TranslationRequest original = request;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(snapshot.result);
    EXPECT_EQ(snapshot.result->text, "第一段\n第二段\n第三段");
    EXPECT_EQ(snapshot.result->requestId, id);
    EXPECT_EQ(snapshot.result->sessionId, original.sessionId);
    EXPECT_EQ(snapshot.result->sourceRevision, original.sourceRevision);
    EXPECT_EQ(snapshot.result->configurationRevision, original.configurationRevision);
    EXPECT_EQ(snapshot.result->options.sourceLanguage, original.options.sourceLanguage);
    EXPECT_EQ(snapshot.result->options.targetLanguage, original.options.targetLanguage);
    EXPECT_EQ(snapshot.result->detectedLanguage, "en");
    ASSERT_EQ(snapshot.attempts.size(), 1U);
    EXPECT_FALSE(snapshot.attempts.front().skipped);
    EXPECT_EQ(snapshot.unattempted, 1U);
    ASSERT_EQ(state->executed.size(), 3U);
    ASSERT_EQ(state->validated.size(), state->executed.size());
    for (std::size_t index = 0; index < state->executed.size(); ++index)
    {
        const TranslationRequest& piece = state->executed[index];
        EXPECT_EQ(piece.text, std::string(175, static_cast<char>('a' + index)) + ".");
        EXPECT_EQ(state->validated[index].text, piece.text);
        EXPECT_EQ(piece.options.sourceLanguage, original.options.sourceLanguage);
        EXPECT_EQ(piece.options.targetLanguage, original.options.targetLanguage);
        EXPECT_EQ(piece.configuration.interfaces, original.configuration.interfaces);
        EXPECT_EQ(piece.configuration.proxyAddress, original.configuration.proxyAddress);
        EXPECT_EQ(piece.sessionId, original.sessionId);
    }
    EXPECT_EQ(request.text, original.text);
    EXPECT_EQ(state->peak.load(), 1U);
    EXPECT_EQ(state->creations.load(), 1U);
}
// 验证中段失败后整家弃用，下家从相同计划第一段重做，来源不混合。
// 入参：第一家第二段网络失败。返回：断言结果。
TEST(TranslationChain, PartialFailureRestartsIdenticalPlanForFallback)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->results = {TranslationError::None, TranslationError::Network};
    state->outputs = {"必须丢弃", "", "后备一", "后备二", "后备三"};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "fallback", "never"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(snapshot.result);
    EXPECT_EQ(snapshot.result->text, "后备一\n后备二\n后备三");
    EXPECT_EQ(snapshot.result->profileName, "fallback");
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::Network);
    EXPECT_FALSE(snapshot.attempts[0].skipped);
    EXPECT_FALSE(snapshot.attempts[1].skipped);
    EXPECT_EQ(snapshot.unattempted, 1U);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"first", "first", "fallback", "fallback", "fallback"}));
    ASSERT_EQ(state->executed.size(), 5U);
    EXPECT_EQ(state->executed[0].text, state->executed[2].text);
    EXPECT_EQ(state->executed[1].text, state->executed[3].text);
    EXPECT_EQ(state->executed[4].text, std::string(175, 'c') + ".");
}
// 验证已执行前段后校验失败仍属于已尝试，每家只有一个摘要。
// 入参：第一家第二段校验返回缺少凭据。返回：断言结果。
TEST(TranslationChain, LaterValidationFailureIsNotMarkedSkipped)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->validationResults = {TranslationError::None, TranslationError::MissingCredentials};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "fallback"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts.front().error, TranslationError::MissingCredentials);
    EXPECT_FALSE(snapshot.attempts.front().skipped);
    ASSERT_EQ(state->executed.size(), 4U);
    EXPECT_EQ(state->executed[0].text, state->executed[1].text);
    EXPECT_EQ(state->validated.size(), 5U);
}
// 验证中段阻塞时不发布半篇，取消后不进入下一段或后备。
// 入参：第二次Execute模拟不可中断调用。返回：断言结果。
TEST(TranslationChain, CancellationDuringPieceDiscardsAllPartialResults)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->holdExecuteCall = 2;
    state->ignoreCancel = true;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "never"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    EXPECT_TRUE(WaitCalls(state, 2));
    EXPECT_TRUE(client.Snapshot().busy);
    EXPECT_FALSE(client.Snapshot().result);
    client.Cancel();
    state->release = true;
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.phase, TranslationPhase::Cancelled);
    EXPECT_FALSE(snapshot.result);
    EXPECT_EQ(state->calls.size(), 2U);
}
// 验证Validate返回之前的取消优先于后续Execute和后备。
// 入参：可控阻塞校验。返回：断言结果。
TEST(TranslationChain, CancellationAfterValidationCannotEnterExecute)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->holdValidation = true;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "never"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    EXPECT_TRUE(Wait(state->validationEntered));
    client.Cancel();
    state->release = true;
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.phase, TranslationPhase::Cancelled);
    EXPECT_TRUE(state->calls.empty());
    EXPECT_FALSE(snapshot.result);
}
// 验证每段重新取得单次预算，Validate和Execute共用同一截止。
// 入参：每段20ms工作、100ms单次预算。返回：断言结果。
TEST(TranslationChain, EachPieceSharesValidationDeadlineAndRenewsItemBudget)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->executeDelay = 20ms;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 100ms, 2s));
    TranslationRequest request = Request({"first"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    ASSERT_EQ(Finish(client).phase, TranslationPhase::Succeeded);
    ASSERT_EQ(state->deadlines.size(), 3U);
    ASSERT_EQ(state->validationStarts.size(), 3U);
    for (std::size_t index = 0; index < state->deadlines.size(); ++index)
    {
        EXPECT_LE(state->deadlines[index], state->validationStarts[index] + 100ms);
        EXPECT_GT(state->deadlines[index], state->validationStarts[index]);
        if (index != 0)
            EXPECT_GT(state->deadlines[index], state->deadlines[index - 1]);
    }
}
// 验证整轮截止覆盖多个片段，单次预算较长时所有调用被同一整轮截止裁剪。
// 入参：单段35ms、整轮60ms。返回：断言结果。
TEST(TranslationChain, RoundDeadlineDoesNotRestartBetweenPieces)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->executeDelay = 35ms;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state, 1s, 60ms));
    TranslationRequest request = Request({"first", "never"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::BudgetExhausted);
    EXPECT_FALSE(snapshot.result);
    ASSERT_EQ(state->deadlines.size(), 2U);
    EXPECT_EQ(state->deadlines[0], state->deadlines[1]);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"first", "first"}));
}
// 验证收集第二段即发现聚合预算耗尽，不发布半篇，已有完整结果对象不变。
// 入参：先成功一次，再返回两份各600000字节译文。返回：断言结果。
TEST(TranslationChain, AggregateOutputBudgetRejectsEarlyAndKeepsPriorCompleteValue)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->outputs = {"上一份完整译文", std::string(600000, 'x'), std::string(600000, 'y')};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"first"}), id, error));
    const std::shared_ptr<const TranslationResult> previous = Finish(client).result;
    ASSERT_TRUE(previous);
    const std::uint64_t previousId = previous->requestId;
    TranslationRequest request = Request({"first"});
    request.text = SegmentedText(3);
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::Truncated);
    EXPECT_FALSE(snapshot.result);
    EXPECT_EQ(state->calls.size(), 3U);
    EXPECT_EQ(previous->text, "上一份完整译文");
    EXPECT_EQ(previous->requestId, previousId);
    EXPECT_GT(id, previousId);
}
// 验证保留原文空白时，纯Unicode空白译文按空结果后备，不误判为致命原文错误。
// 入参：第一家仅返回NBSP，第二家返回有效译文并可控阻塞。返回：断言结果。
TEST(TranslationChain, WhitespaceOnlyAssembledTranslationFallsBackAsEmptyResult)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->outputs = {"\xC2\xA0", "后备完整译文"};
    state->holdExecuteCall = 2;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first", "fallback"});
    request.text = " hello ";
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    EXPECT_TRUE(WaitCalls(state, 2));
    const TranslationSnapshot running = client.Snapshot();
    EXPECT_TRUE(running.busy);
    EXPECT_FALSE(running.result);
    state->release = true;
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::EmptyResult);
    EXPECT_FALSE(snapshot.attempts[0].skipped);
    EXPECT_EQ(state->calls, (std::vector<std::string>{"first", "fallback"}));
    ASSERT_TRUE(snapshot.result);
    EXPECT_EQ(snapshot.result->text, " 后备完整译文 ");
    EXPECT_EQ(snapshot.result->profileName, "fallback");
}
// 验证全文放宽到100000 Unicode标量，超过旧门禁的输入进入分段，越界则零调用拒绝。
// 入参：50000组中文及空格。返回：断言结果。
TEST(TranslationChain, OrdinaryAdmissionAcceptsOneHundredThousandUnicodeScalars)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first"});
    request.text.clear();
    for (std::size_t index = 0; index < 50000; ++index)
        request.text += "界 ";
    const std::string original = request.text;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    EXPECT_GT(state->calls.size(), 50U);
    EXPECT_EQ(request.text, original);
    const std::size_t completed = state->calls.size();
    request.text += "界";
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::InputTooLarge);
    EXPECT_EQ(state->calls.size(), completed);
}
// 验证Unicode空白没有可翻译正文，分隔内容不能构成成功结果。
// 入参：ASCII空白、NBSP和全角空格。返回：断言结果。
TEST(TranslationChain, UnicodeWhitespaceOnlyHasNoProviderExecution)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first"});
    request.text = " \t\r\n\xC2\xA0\xE3\x80\x80";
    std::uint64_t id{};
    TranslationError error{};
    if (client.Submit(request, id, error))
        EXPECT_EQ(Finish(client).error, TranslationError::InvalidInput);
    else
        EXPECT_EQ(error, TranslationError::InvalidInput);
    EXPECT_TRUE(state->calls.empty());
    EXPECT_FALSE(client.Snapshot().result);
}
// 验证诊断绕过分段且保留10000标量门禁，恰好执行一次完整请求。
// 入参：超过200的诊断原文及10000/10001边界。返回：断言结果。
TEST(TranslationChain, DiagnosticKeepsSingleFullRequestAndOriginalAdmissionLimit)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first"});
    request.diagnostic = true;
    std::uint64_t id{};
    TranslationError error{};
    for (const std::string text : {SegmentedText(3), std::string(10000, 'a')})
    {
        request.text = text;
        ASSERT_TRUE(client.Submit(request, id, error));
        ASSERT_EQ(Finish(client).phase, TranslationPhase::Succeeded);
        ASSERT_FALSE(state->executed.empty());
        EXPECT_EQ(state->executed.back().text, text);
        EXPECT_TRUE(state->executed.back().diagnostic);
    }
    ASSERT_EQ(state->calls.size(), 2U);
    request.text.push_back('a');
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::InputTooLarge);
    EXPECT_EQ(state->calls.size(), 2U);
}
// 验证本地auto逐段原样交给统一接口，不能被父层推断或改写为整篇语言。
// 入参：本地类型替身及不一致的逐段检测结果。返回：断言结果。
TEST(TranslationChain, LocalAutoIsForwardedPerPieceAndMixedDetectionIsCleared)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->detectedLanguages = {"en", "zh-CN", "en"};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({});
    nlohmann::json local = TestTranslationProfile("ctranslate2_local");
    local["enabled"] = true;
    request.configuration.interfaces.push_back(local);
    request.options.sourceLanguage = "auto";
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(snapshot.result);
    EXPECT_TRUE(snapshot.result->detectedLanguage.empty());
    EXPECT_EQ(snapshot.result->options.sourceLanguage, "auto");
    ASSERT_EQ(state->executed.size(), 3U);
    for (const TranslationRequest& piece : state->executed)
        EXPECT_EQ(piece.options.sourceLanguage, "auto");
}
// 验证只要一段未报告检测语言，整篇结果就不能沿用其余片段的语言。
// 入参：一段空检测值。返回：断言结果。
TEST(TranslationChain, MissingPieceDetectionClearsWholeDetectedLanguage)
{
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->detectedLanguages = {"en", "", "en"};
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first"});
    request.text = SegmentedText(3);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_TRUE(snapshot.result);
    EXPECT_TRUE(snapshot.result->detectedLanguage.empty());
}
// 验证真实本地模型经过统一客户端完成超过200字符的长文，保留完整段间换行。
// 入参：固定缓存的完整双向资源副本；没有在线条目。返回：断言结果。
TEST_F(SegmentedLocalClientTest, RealLocalLongTextMatchesRepeatedCompleteTranslation)
{
    TranslationClient client(this->root, {});
    TranslationRequest request = Request({});
    nlohmann::json local = TestTranslationProfile("ctranslate2_local");
    local["enabled"] = true;
    request.configuration.interfaces.push_back(local);
    request.options.sourceLanguage = "auto";
    request.options.targetLanguage = "zh-CN";
    const std::string sentence =
        "The students are learning new words and writing their homework every day while their teacher explains "
        "how to use the library and how to find the books that they need for study.";
    ASSERT_GT(sentence.size(), 100U);
    ASSERT_LE(sentence.size(), 200U);
    request.text = sentence;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot shortResult = Finish(client, 60s);
    ASSERT_EQ(shortResult.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(shortResult.result);
    ASSERT_FALSE(shortResult.result->text.empty());
    request.text = sentence + "\n" + sentence + "\n" + sentence;
    ASSERT_GT(request.text.size(), 200U);
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client, 60s);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_TRUE(snapshot.result);
    EXPECT_EQ(snapshot.result->text,
              shortResult.result->text + "\n" + shortResult.result->text + "\n" + shortResult.result->text);
    EXPECT_EQ(snapshot.result->requestId, id);
    EXPECT_EQ(snapshot.result->sessionId, request.sessionId);
    EXPECT_EQ(snapshot.result->sourceRevision, request.sourceRevision);
    EXPECT_EQ(snapshot.result->detectedLanguage, "en");
    EXPECT_EQ(snapshot.result->options.sourceLanguage, "auto");
    ASSERT_EQ(snapshot.attempts.size(), 1U);
    EXPECT_FALSE(snapshot.attempts.front().network);
    EXPECT_FALSE(snapshot.attempts.front().skipped);
    EXPECT_FALSE(std::filesystem::exists(this->root / "data"));
}
// 验证后台空闲维护会到期执行，关停后准入永久关闭。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, IdleMaintenanceAndShutdown)
{
    auto state = std::make_shared<State>();
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(Request({"first"}), id, error));
    EXPECT_EQ(Finish(client).phase, TranslationPhase::Succeeded);
    EXPECT_TRUE(Wait(state->maintained));
    client.RequestShutdown();
    const Clock::time_point end = Clock::now() + 1s;
    while (!client.ShutdownComplete() && Clock::now() < end)
        std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(client.ShutdownComplete());
    EXPECT_FALSE(client.Submit(Request({"new"}), id, error));
}
// 验证本地自动识别进入真实引擎，纯本地链从不创建在线提供方。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, LocalOnlyAutoReachesEngine)
{
    TranslationClient client(std::filesystem::temp_directory_path() / L"open-st-nonexistent-model-fixture", {});
    TranslationRequest request = Request({});
    nlohmann::json local = TestTranslationProfile("ctranslate2_local");
    local["enabled"] = true;
    request.text = "This is an English paragraph about reading books in the library. "
                   "The students are learning new words and writing their homework every day.";
    request.configuration.interfaces.push_back(local);
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::ModelMissing);
    ASSERT_EQ(snapshot.attempts.size(), 1U);
    EXPECT_FALSE(snapshot.attempts[0].network);
    EXPECT_FALSE(snapshot.attempts[0].skipped);
}
// 验证显式或自动源语言的本地模型缺失后，只有启用的后备在线项发送一次。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, RealLocalAdapterFallsBackThroughTheSameOnlineProtocol)
{
    for (const std::string source : {"auto", "en"})
    {
        auto transport = std::make_shared<TransportState>();
        http::Result response;
        response.status = 200;
        transport->responses.push_back(response);
        transport->bodies.push_back(R"({"translation":"完整译文"})");
        TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
        TranslationRequest request = Request({});
        request.options.sourceLanguage = source;
        request.text = "This is an English paragraph about reading books in the library. "
                       "The students are learning new words and writing their homework every day.";
        nlohmann::json local = TestTranslationProfile("ctranslate2_local");
        local["enabled"] = true;
        nlohmann::json disabled = CustomProfile();
        disabled["enabled"] = false;
        request.configuration.interfaces = nlohmann::json::array({local, disabled, CustomProfile(), CustomProfile()});
        std::uint64_t id{};
        TranslationError error{};
        ASSERT_TRUE(client.Submit(request, id, error));
        const TranslationSnapshot snapshot = Finish(client);
        ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
        ASSERT_EQ(snapshot.attempts.size(), 2U);
        EXPECT_EQ(snapshot.attempts[0].error, TranslationError::ModelMissing);
        EXPECT_FALSE(snapshot.attempts[0].skipped);
        EXPECT_FALSE(snapshot.attempts[0].network);
        EXPECT_TRUE(snapshot.attempts[1].network);
        ASSERT_TRUE(snapshot.result);
        EXPECT_EQ(snapshot.result->text, "完整译文");
        EXPECT_EQ(transport->requests.size(), 1U);
        EXPECT_EQ(snapshot.unattempted, 1U);
    }
}
// 验证真实在线协议的业务错误优先级与整轮代理快照在切换中保持一致。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, OnlineBusinessFallbackKeepsCapturedProxyAndInput)
{
    auto transport = std::make_shared<TransportState>();
    http::Result quota;
    quota.status = 400;
    http::Result success;
    success.status = 200;
    transport->responses = {quota, success};
    transport->bodies = {R"({"code":"quota"})", R"({"translation":"complete"})"};
    TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
    TranslationRequest request = Request({});
    nlohmann::json first = CustomProfile();
    first["configuration"]["response"]["error_pointer"] = "/code";
    first["configuration"]["response"]["error_map"] = {{"quota", "quota"}};
    request.configuration.interfaces = nlohmann::json::array({first, CustomProfile(), CustomProfile()});
    request.configuration.proxyMode = "custom";
    request.configuration.proxyAddress = "localhost:8765";
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    request.text = "later edit";
    request.configuration.proxyMode = "system";
    request.configuration.interfaces.clear();
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
    ASSERT_EQ(snapshot.attempts.size(), 2U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::Quota);
    ASSERT_EQ(transport->requests.size(), 2U);
    for (const http::Request& sent : transport->requests)
    {
        EXPECT_EQ(sent.proxy.mode, http::ProxyMode::Custom);
        EXPECT_EQ(sent.proxy.address, L"localhost:8765");
        EXPECT_EQ(nlohmann::json::parse(sent.body).at("text"), "hello");
        EXPECT_EQ(sent.maxResponseBytes, 1048576U);
        EXPECT_EQ(sent.maxErrorBytes, 16384U);
    }
}
// 验证传输层分配失败即使迟到仍停止整轮，不再次发送原文。
// 入参：无。
// 返回：断言结果。
TEST(TranslationChain, OnlineAllocationFailureAfterDeadlineStopsFallback)
{
    auto transport = std::make_shared<TransportState>();
    http::Result response;
    response.error = http::Error::SinkRejected;
    response.systemError = ERROR_NOT_ENOUGH_MEMORY;
    transport->responses.push_back(response);
    transport->overrun = true;
    TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport, 20ms));
    TranslationRequest request = Request({});
    request.configuration.interfaces = nlohmann::json::array({CustomProfile(), CustomProfile()});
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.error, TranslationError::OutOfMemory);
    EXPECT_FALSE(snapshot.result);
    EXPECT_EQ(transport->requests.size(), 1U);
    EXPECT_EQ(snapshot.unattempted, 1U);
}
// 验证提供方独立边界对已取消/到期请求零发送，不依赖上层检查才能正确。
// 入参：无。
// 返回：断言结果。
TEST(TranslationProvider, CancellationAndDeadlineNeverCallTransport)
{
    auto transport = std::make_shared<TransportState>();
    auto provider =
        translation_detail::CreateOnlineProvider("custom_http", std::make_unique<FixtureTransport>(transport));
    std::stop_source stop;
    stop.request_stop();
    const TranslationRequest request = Request({});
    EXPECT_EQ(provider->Execute(request, CustomProfile(), Clock::now(), stop.get_token()).error,
              TranslationError::Cancelled);
    EXPECT_EQ(provider->Execute(request, CustomProfile(), Clock::now(), {}).error, TranslationError::Timeout);
    EXPECT_TRUE(transport->requests.empty());
}

// 用稳定测试标记检查字节预算及秘密不外泄，不模拟生产界面的翻译逻辑。
// 入参：结构化诊断。返回：测试可观察的安全摘要。
std::string DiagnosticTextForTest(const TranslationDiagnosticText& parts)
{
    std::string result;
    for (const TranslationDiagnosticPart& part : parts)
        switch (part.kind)
        {
        case TranslationDiagnosticPartKind::Text:
            result += part.text;
            break;
        case TranslationDiagnosticPartKind::Redacted:
            result += "[redacted]";
            break;
        case TranslationDiagnosticPartKind::Omitted:
            result += "[omitted]";
            break;
        case TranslationDiagnosticPartKind::TooLong:
            result += "[omitted: too long]";
            break;
        }
    return result;
}

// 验证百度 HTTP 200 业务失败可诊断，普通翻译不保留诊断正文。
// 入参：合成错误响应。返回：断言结果，不访问网络。
TEST(TranslationChain, DiagnosticBaiduErrorIsOptInAndRedacted)
{
    for (const bool diagnostic : {false, true})
    {
        auto transport = std::make_shared<TransportState>();
        http::Result response;
        response.status = 200;
        transport->responses.push_back(response);
        transport->bodies.push_back(
            R"({"error_code":"54001","error_msg":"Invalid signature fixture-secret app-fixture","echo":"do not publish"})");
        TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
        TranslationRequest request = Request({});
        nlohmann::json profile = TestTranslationProfile("baidu");
        profile["enabled"] = true;
        profile["configuration"]["app_id"] = "app-fixture";
        profile["secrets"]["api_key"] = "fixture-secret";
        request.configuration.interfaces.push_back(profile);
        request.diagnostic = diagnostic;
        std::uint64_t id{};
        TranslationError error{};
        ASSERT_TRUE(client.Submit(request, id, error));
        const TranslationSnapshot snapshot = Finish(client);
        EXPECT_EQ(snapshot.phase, TranslationPhase::Failed);
        ASSERT_EQ(snapshot.attempts.size(), 1U);
        EXPECT_EQ(snapshot.attempts[0].diagnostic.has_value(), diagnostic);
        if (diagnostic)
        {
            const TranslationDiagnostic& details = *snapshot.attempts[0].diagnostic;
            EXPECT_EQ(details.httpStatus, 200U);
            EXPECT_EQ(DiagnosticTextForTest(details.providerCode), "54001");
            EXPECT_NE(DiagnosticTextForTest(details.providerMessage).find("Invalid signature"), std::string::npos);
            EXPECT_EQ(DiagnosticTextForTest(details.response).find("fixture-secret"), std::string::npos);
            EXPECT_EQ(DiagnosticTextForTest(details.response).find("app-fixture"), std::string::npos);
            EXPECT_EQ(DiagnosticTextForTest(details.response).find("do not publish"), std::string::npos);
            EXPECT_LE(DiagnosticTextForTest(details.response).size(), 4096U);
            EXPECT_GE(details.elapsedMilliseconds, 0);
        }
        EXPECT_FALSE(snapshot.result);
        EXPECT_EQ(transport->requests.size(), 1U);
    }
}
// 验证自定义服务任意回显及非 JSON 正文不会泄漏静态凭据，仍保留系统错误。
// 入参：合成自定义响应。返回：断言结果。
TEST(TranslationChain, DiagnosticCustomResponseFailsClosed)
{
    auto transport = std::make_shared<TransportState>();
    http::Result response;
    response.status = 502;
    response.error = http::Error::Network;
    response.systemError = ERROR_WINHTTP_CANNOT_CONNECT;
    transport->responses.push_back(response);
    transport->bodies.push_back("header-secret query-secret form-secret fixture-key");
    TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
    TranslationRequest request = Request({});
    request.configuration.interfaces.push_back(CustomProfile());
    request.diagnostic = true;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.attempts.size(), 1U);
    ASSERT_TRUE(snapshot.attempts[0].diagnostic);
    const TranslationDiagnostic& details = *snapshot.attempts[0].diagnostic;
    EXPECT_EQ(details.systemError, ERROR_WINHTTP_CANNOT_CONNECT);
    EXPECT_EQ(details.httpStatus, 502U);
    EXPECT_TRUE(details.providerMessage.empty());
    ASSERT_EQ(details.response.size(), 1U);
    EXPECT_EQ(details.response.front().kind, TranslationDiagnosticPartKind::Omitted);
    EXPECT_TRUE(details.response.front().text.empty());
}
// 验证无法执行的测试仍发布耗时和稳定错误，绝不进入传输。
// 入参：未配置凭据的百度条目。返回：断言结果。
TEST(TranslationChain, DiagnosticMissingCredentialsHasAttemptWithoutNetwork)
{
    auto transport = std::make_shared<TransportState>();
    TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
    TranslationRequest request = Request({});
    nlohmann::json profile = TestTranslationProfile("baidu");
    profile["enabled"] = true;
    request.configuration.interfaces.push_back(profile);
    request.diagnostic = true;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    const TranslationSnapshot snapshot = Finish(client);
    ASSERT_EQ(snapshot.attempts.size(), 1U);
    EXPECT_EQ(snapshot.attempts[0].error, TranslationError::MissingCredentials);
    EXPECT_TRUE(snapshot.attempts[0].skipped);
    ASSERT_TRUE(snapshot.attempts[0].diagnostic);
    EXPECT_EQ(snapshot.attempts[0].diagnostic->httpStatus, 0U);
    EXPECT_TRUE(transport->requests.empty());
}

// 验证恶意超长错误和非 JSON 回显不影响业务分类，也不能扩大诊断预算。
// 入参：有界合成服务正文。返回：断言结果。
TEST(TranslationProvider, DiagnosticMalformedAndLongMessagesAreBounded)
{
    for (const std::string body :
         {std::string("<html>fixture-secret</html>"),
          nlohmann::json{{"error_code", "54001"}, {"error_msg", std::string(8000, 'x') + "fixture-secret"}}.dump()})
    {
        auto transport = std::make_shared<TransportState>();
        http::Result response;
        response.status = 200;
        transport->responses.push_back(response);
        transport->bodies.push_back(body);
        std::unique_ptr<translation_detail::ITranslationProvider> provider =
            translation_detail::CreateOnlineProvider("baidu", std::make_unique<FixtureTransport>(transport));
        nlohmann::json profile = TestTranslationProfile("baidu");
        profile["configuration"]["app_id"] = "app-fixture";
        profile["secrets"]["api_key"] = "fixture-secret";
        TranslationRequest request = Request({});
        request.diagnostic = true;
        const translation_detail::ProviderResult result = provider->Execute(request, profile, Clock::now() + 1s, {});
        ASSERT_TRUE(result.diagnostic);
        EXPECT_NE(result.error, TranslationError::None);
        EXPECT_LE(DiagnosticTextForTest(result.diagnostic->response).size(), 4096U);
        EXPECT_LE(DiagnosticTextForTest(result.diagnostic->providerMessage).size(), 1024U);
        EXPECT_EQ(DiagnosticTextForTest(result.diagnostic->response).find("fixture-secret"), std::string::npos);
        if (body.starts_with("<"))
        {
            ASSERT_EQ(result.diagnostic->response.size(), 1U);
            EXPECT_EQ(result.diagnostic->response.front().kind, TranslationDiagnosticPartKind::Omitted);
        }
        else
        {
            ASSERT_EQ(result.diagnostic->providerMessage.size(), 1U);
            EXPECT_EQ(result.diagnostic->providerMessage.front().kind, TranslationDiagnosticPartKind::TooLong);
            EXPECT_TRUE(result.diagnostic->providerMessage.front().text.empty());
        }
    }
}
// 验证诊断任务共用忙门禁，取消时不能发布迟到诊断或成功结果。
// 入参：可控单线程替身。返回：断言结果。
TEST(TranslationChain, DiagnosticUsesSameBusyAndCancellationGate)
{
    auto state = std::make_shared<State>();
    state->hold = true;
    state->ignoreCancel = true;
    TranslationClient client = TranslationClientTestAccess::Create(Dependencies(state));
    TranslationRequest request = Request({"first"});
    request.diagnostic = true;
    std::uint64_t id{};
    TranslationError error{};
    ASSERT_TRUE(client.Submit(request, id, error));
    ASSERT_TRUE(Wait(state->entered));
    EXPECT_FALSE(client.Submit(Request({"other"}), id, error));
    EXPECT_EQ(error, TranslationError::Busy);
    client.Cancel();
    EXPECT_FALSE(client.Submit(request, id, error));
    EXPECT_EQ(error, TranslationError::Busy);
    state->release = true;
    const TranslationSnapshot snapshot = Finish(client);
    EXPECT_EQ(snapshot.phase, TranslationPhase::Cancelled);
    EXPECT_FALSE(snapshot.result);
    EXPECT_TRUE(snapshot.attempts.empty());
    EXPECT_EQ(state->peak.load(), 1U);
}

// 验证成功译文也经过诊断脱敏，普通翻译保留服务原始译文。
// 入参：模拟服务在译文回显凭据、请求头和静态参数。返回：断言结果。
TEST(TranslationChain, DiagnosticSuccessfulTranslationRedactsEchoedCredentials)
{
    for (const bool diagnostic : {false, true})
    {
        auto transport = std::make_shared<TransportState>();
        http::Result response;
        response.status = 200;
        transport->responses.push_back(response);
        const std::string echo = "translation fixture-key header-secret query-secret form-secret";
        transport->bodies.push_back(nlohmann::json{{"translation", echo}}.dump());
        TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
        TranslationRequest request = Request({});
        nlohmann::json profile = CustomProfile();
        profile["configuration"]["headers"]["X-Token"] = "header-secret";
        profile["configuration"]["query"]["token"] = "query-secret";
        profile["configuration"]["body"]["secret"] = "form-secret";
        request.configuration.interfaces.push_back(profile);
        request.diagnostic = diagnostic;
        std::uint64_t id{};
        TranslationError error{};
        ASSERT_TRUE(client.Submit(request, id, error));
        const TranslationSnapshot snapshot = Finish(client);
        ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
        ASSERT_TRUE(snapshot.result);
        if (!diagnostic)
            EXPECT_EQ(snapshot.result->text, echo);
        else
            for (const std::string_view secret : {"fixture-key", "header-secret", "query-secret", "form-secret"})
                EXPECT_EQ(snapshot.result->text.find(secret), std::string::npos);
    }
}
// 服务字面占位保持原文；只有实际凭据匹配才产生结构化脱敏标记。
// 入参：合成百度错误，包含交叠秘密。返回：断言结果，不联网。
TEST(TranslationProvider, DiagnosticMarkersDistinguishRemoteTextAndOverlappingSecrets)
{
    auto transport = std::make_shared<TransportState>();
    http::Result response;
    response.status = 200;
    transport->responses.push_back(response);
    transport->bodies.push_back(R"({"error_code":"54001","error_msg":"[redacted] [message omitted] abcdef"})");
    auto provider = translation_detail::CreateOnlineProvider("baidu", std::make_unique<FixtureTransport>(transport));
    nlohmann::json profile = TestTranslationProfile("baidu");
    profile["configuration"]["app_id"] = "abcd";
    profile["secrets"]["api_key"] = "cdef";
    TranslationRequest request = Request({});
    request.diagnostic = true;
    const translation_detail::ProviderResult result = provider->Execute(request, profile, Clock::now() + 1s, {});
    ASSERT_TRUE(result.diagnostic);
    const TranslationDiagnosticText& message = result.diagnostic->providerMessage;
    ASSERT_EQ(message.size(), 2U);
    EXPECT_EQ(message[0].kind, TranslationDiagnosticPartKind::Text);
    EXPECT_EQ(message[0].text, "[redacted] [message omitted] ");
    EXPECT_EQ(message[1].kind, TranslationDiagnosticPartKind::Redacted);
    EXPECT_TRUE(message[1].text.empty());
    EXPECT_EQ(DiagnosticTextForTest(result.diagnostic->response).find("abcdef"), std::string::npos);
}
// 成功诊断的译文也按片段发布；普通翻译保持服务原文和既有成功分类。
// 入参：合成成功百度响应。返回：断言结果。
TEST(TranslationChain, DiagnosticSuccessfulTranslationKeepsStructuredRedaction)
{
    for (const bool diagnostic : {false, true})
    {
        auto transport = std::make_shared<TransportState>();
        http::Result response;
        response.status = 200;
        transport->responses.push_back(response);
        transport->bodies.push_back(R"({"from":"en","to":"zh","trans_result":[{"dst":"[redacted] fixture-secret"}]})");
        TranslationClient client = TranslationClientTestAccess::Create(ProtocolDependencies(transport));
        TranslationRequest request = Request({});
        nlohmann::json profile = TestTranslationProfile("baidu");
        profile["enabled"] = true;
        profile["configuration"]["app_id"] = "app-fixture";
        profile["secrets"]["api_key"] = "fixture-secret";
        request.configuration.interfaces.push_back(profile);
        request.diagnostic = diagnostic;
        std::uint64_t id{};
        TranslationError error{};
        ASSERT_TRUE(client.Submit(request, id, error));
        const TranslationSnapshot snapshot = Finish(client);
        ASSERT_EQ(snapshot.phase, TranslationPhase::Succeeded);
        ASSERT_TRUE(snapshot.result);
        if (diagnostic)
        {
            ASSERT_TRUE(snapshot.attempts.front().diagnostic);
            const TranslationDiagnosticText& parts = snapshot.attempts.front().diagnostic->translatedText;
            ASSERT_EQ(parts.size(), 2U);
            EXPECT_EQ(parts[0].kind, TranslationDiagnosticPartKind::Text);
            EXPECT_EQ(parts[0].text, "[redacted] ");
            EXPECT_EQ(parts[1].kind, TranslationDiagnosticPartKind::Redacted);
            EXPECT_TRUE(parts[1].text.empty());
            EXPECT_EQ(snapshot.result->text.find("fixture-secret"), std::string::npos);
        }
        else
            EXPECT_EQ(snapshot.result->text, "[redacted] fixture-secret");
    }
}
} // namespace open_st
