// 验证真实 OCR 单任务线程、输入所有权、取消发布屏障与异步退出；不依赖模型识别速度。
#include "ocr_engine.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <ocr_client.h>
#include <stdexcept>
#include <thread>

namespace open_st
{
struct OcrClientTestAccess
{
    // 仅替换第三方识别边界，任务线程与所有状态均使用正式实现。
    // 入参：engine 为独占替身；notify 为可控通知。
    // 返回：未提交任务的客户端。
    static std::unique_ptr<OcrClient> Create(std::unique_ptr<ocr_detail::Engine> engine,
                                             std::function<void(std::uint64_t)> notify = {})
    {
        return std::unique_ptr<OcrClient>(new OcrClient(L"C:/unused", std::move(notify), std::move(engine)));
    }
};
} // namespace open_st

namespace
{
using namespace open_st;
using namespace std::chrono_literals;
struct Control
{
    std::mutex mutex;
    std::condition_variable wake;
    bool release{true};
    bool throwFailure{};
    unsigned int entered{};
    OcrError error{OcrError::None};
    std::wstring output{L"识别结果\nOCR text"};
    std::byte observed{};
    OcrOptions options;
    std::thread::id thread;
};
class FakeEngine final : public ocr_detail::Engine
{
  public:
    // 保存测试控制块，模拟引擎不可中断区间。
    // 入参：control 为共享同步状态。
    // 返回：无。
    explicit FakeEngine(std::shared_ptr<Control> control) : control_(std::move(control)) {}
    // 在受控门闩后返回结果，故意忽略取消以验证客户端防止晚到发布。
    // 入参：image 为复制图；options 为选择；phase 为实际阶段回调；其余为未使用边界参数。
    // 返回：测试指定的成功、空白或失败结果。
    ocr_detail::Recognition Recognize(const std::filesystem::path&, const ocr_detail::Image& image,
                                      const OcrOptions& options, const std::atomic_bool&,
                                      const std::function<void(OcrPhase)>& phase) override
    {
        phase(OcrPhase::Loading);
        std::unique_lock lock(this->control_->mutex);
        ++this->control_->entered;
        this->control_->wake.notify_all();
        // 模拟无法立刻中断的第三方初始化；只能由测试显式释放。
        // 入参：无。
        // 返回：允许继续时 true。
        this->control_->wake.wait(lock, [this] { return this->control_->release; });
        this->control_->observed = image.pixels.front();
        this->control_->options = options;
        this->control_->thread = std::this_thread::get_id();
        if (this->control_->throwFailure)
            throw std::runtime_error("controlled engine failure");
        phase(OcrPhase::Recognizing);
        ocr_detail::Recognition result;
        result.error = this->control_->error;
        result.text = this->control_->output;
        return result;
    }

  private:
    std::shared_ptr<Control> control_;
};
class OcrClientTest : public testing::Test
{
  protected:
    std::shared_ptr<Control> control = std::make_shared<Control>();
    std::unique_ptr<OcrClient> client;
    std::array<std::byte, 16> bytes{};
    // 构造隔离客户端，像素缓冲区只借用至 Submit 返回。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        this->bytes.fill(std::byte{0x11});
        this->client = OcrClientTestAccess::Create(std::make_unique<FakeEngine>(this->control));
    }
    // 即使断言提前返回也释放门闩，避免测试清理阻塞或遗留后台线程。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        this->Release();
        this->client.reset();
    }
    // 提交最小合法图，测试复用真实输入复制逻辑。
    // 入参：id 输出编号；error 输出拒绝原因。
    // 返回：客户端接受时 true。
    bool Start(std::uint64_t& id, OcrError& error)
    {
        return this->client->Submit({2, 2, 8, this->bytes}, {}, id, error);
    }
    // 等待替身进入受控区间，不以睡眠推测工作线程已经开始。
    // 入参：count 为预期累计调用次数。
    // 返回：在有限时间内进入时 true。
    bool WaitEntered(unsigned int count = 1)
    {
        std::unique_lock lock(this->control->mutex);
        // 在同一同步锁下检查进入计数。
        // 入参：无。
        // 返回：目标次数到达为 true。
        return this->control->wake.wait_for(lock, 3s, [this, count] { return this->control->entered >= count; });
    }
    // 释放受控引擎且允许后续任务直接执行。
    // 入参：无。
    // 返回：无。
    void Release()
    {
        const std::scoped_lock lock(this->control->mutex);
        this->control->release = true;
        this->control->wake.notify_all();
    }
    // 用公开快照等待真正结束，模拟宿主补收未投递成功的通知。
    // 入参：无。
    // 返回：任务在有限时间内结束时 true。
    bool WaitIdle()
    {
        const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + 3s;
        while (this->client->Snapshot().busy && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        return !this->client->Snapshot().busy;
    }
};

// 验证领域候选与准入采用同一规则，拒绝路径穿越、额外模型和任意参数。
// 入参：无。
// 返回：无；断言覆盖所有发布候选组合。
TEST(OcrInputTest, fixed_choices_and_options)
{
    EXPECT_EQ(OcrModelChoices().size(), 2U);
    EXPECT_EQ(OcrLanguageChoices().size(), 4U);
    for (std::string_view model : OcrModelChoices())
        for (std::string_view language : OcrLanguageChoices())
            EXPECT_TRUE(AreOcrOptionsValid({std::string(model), std::string(language)}));
    EXPECT_FALSE(AreOcrOptionsValid({"../best", "eng"}));
    EXPECT_FALSE(AreOcrOptionsValid({"fast", "eng+osd"}));
    EXPECT_FALSE(AreOcrOptionsValid({"FAST", "eng"}));
}
// 验证跨度与最后一行边界，不让乘法溢出、空图或超预算进入复制。
// 入参：无。
// 返回：无。
TEST(OcrInputTest, validates_pixels_stride_and_overflow)
{
    std::array<std::byte, 20> bytes{};
    EXPECT_TRUE(ocr_detail::ValidImage({2, 2, 12, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({2, 2, 13, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({2, 2, 7, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({0, 2, 8, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({16385, 1, 65540, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({8000, 8000, 32000, bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({2, 2, std::numeric_limits<std::size_t>::max(), bytes}));
    EXPECT_FALSE(ocr_detail::ValidImage({2, 2, 8, {}}));
}
// 验证构造不会初始化引擎或启动识别，空闲关闭可立即完成且不再接受任务。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, lazy_start_and_shutdown)
{
    EXPECT_TRUE(this->client->ShutdownComplete());
    EXPECT_EQ(this->control->entered, 0U);
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Idle);
    this->client->RequestShutdown();
    EXPECT_TRUE(this->client->ShutdownComplete());
    std::uint64_t id{};
    OcrError error{};
    EXPECT_FALSE(this->Start(id, error));
    EXPECT_EQ(error, OcrError::Unavailable);
}
// 验证提交复制像素和选项，后台使用独立线程；成功快照中的正文不可变。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, copies_input_before_return_and_runs_in_background)
{
    this->control->release = false;
    OcrOptions options{"best", "jpn"};
    std::uint64_t id{};
    OcrError error{};
    ASSERT_TRUE(this->client->Submit({2, 2, 8, this->bytes}, options, id, error));
    ASSERT_TRUE(this->WaitEntered());
    this->bytes.fill(std::byte{0xFF});
    options.language = "eng";
    this->Release();
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->control->observed, std::byte{0x11});
    EXPECT_EQ(this->control->options.language, "jpn");
    EXPECT_NE(this->control->thread, std::this_thread::get_id());
    const OcrSnapshot snapshot = this->client->Snapshot();
    ASSERT_TRUE(snapshot.text);
    EXPECT_EQ(*snapshot.text, L"识别结果\nOCR text");
    this->client->Cancel();
    EXPECT_FALSE(this->client->Snapshot().text);
    EXPECT_EQ(*snapshot.text, L"识别结果\nOCR text");
}
// 验证不可中断引擎期间取消立即返回、忙状态仍保留，晚成功不能复活且下一请求编号增长。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, cancellation_rejects_late_results_and_busy_restart)
{
    this->control->release = false;
    std::uint64_t first{}, second{};
    OcrError error{};
    ASSERT_TRUE(this->Start(first, error));
    ASSERT_TRUE(this->WaitEntered());
    this->client->Cancel();
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Cancelled);
    EXPECT_TRUE(this->client->Snapshot().busy);
    EXPECT_FALSE(this->Start(second, error));
    EXPECT_EQ(error, OcrError::Busy);
    this->Release();
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_FALSE(this->client->Snapshot().text);
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Cancelled);
    ASSERT_TRUE(this->Start(second, error));
    EXPECT_GT(second, first);
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Succeeded);
}
// 验证停止请求不 join 不可中断阶段，并在引擎完成后提供可查询退出标记。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, shutdown_is_nonblocking_and_queryable)
{
    this->control->release = false;
    std::uint64_t id{};
    OcrError error{};
    ASSERT_TRUE(this->Start(id, error));
    ASSERT_TRUE(this->WaitEntered());
    this->client->RequestShutdown();
    EXPECT_FALSE(this->client->ShutdownComplete());
    EXPECT_FALSE(this->Start(id, error));
    EXPECT_EQ(error, OcrError::Unavailable);
    this->Release();
    const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + 3s;
    while (!this->client->ShutdownComplete() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(this->client->ShutdownComplete());
    EXPECT_FALSE(this->client->Snapshot().text);
}
// 验证空白、识别失败及引擎异常分别表达，失败不泄漏部分正文。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, empty_failure_and_exception_are_distinct)
{
    this->control->output.clear();
    std::uint64_t id{};
    OcrError error{};
    ASSERT_TRUE(this->Start(id, error));
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Empty);
    this->control->error = OcrError::Recognition;
    this->control->output = L"must not be published";
    ASSERT_TRUE(this->Start(id, error));
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Failed);
    EXPECT_FALSE(this->client->Snapshot().text);
    this->control->throwFailure = true;
    ASSERT_TRUE(this->Start(id, error));
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->client->Snapshot().error, OcrError::Unavailable);
}
// 验证通知抛异常也不会终止线程，宿主轮询仍能得到完整结果。
// 入参：无。
// 返回：无。
TEST_F(OcrClientTest, failed_notification_preserves_pollable_completion)
{
    // 模拟消息接收者暂不可用；不将通知异常当作引擎失败。
    // 入参：未使用任务编号。
    // 返回：无，受控抛异常。
    this->client = OcrClientTestAccess::Create(std::make_unique<FakeEngine>(this->control), [](std::uint64_t)
                                               { throw std::runtime_error("notification unavailable"); });
    std::uint64_t id{};
    OcrError error{};
    ASSERT_TRUE(this->Start(id, error));
    ASSERT_TRUE(this->WaitIdle());
    EXPECT_EQ(this->client->Snapshot().phase, OcrPhase::Succeeded);
}
} // namespace
