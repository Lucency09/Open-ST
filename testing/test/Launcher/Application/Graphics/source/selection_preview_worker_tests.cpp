// 验证实时预览的有界队列、取消资格、冻结输入寿命及通知无锁契约。
#include <selection_preview_worker.h>
#include <selection_output_renderer.h>
#include <color_conversion.h>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace open_st
{
struct SelectionPreviewWorkerTestAccess final
{
    // 为并发协议测试注入可控制的转换屏障。
    // 入参：notify 为通知，convert 为测试转换。返回：独立后台实例。
    static std::unique_ptr<SelectionPreviewWorker> Create(std::function<void()> notify,
                                                          SelectionPreviewWorker::Converter convert)
    {
        return std::unique_ptr<SelectionPreviewWorker>(
            new SelectionPreviewWorker(std::move(notify), std::move(convert)));
    }
};
namespace
{
// 构造一像素不可变桌面，避免测试访问真实屏幕。
// 入参：无。返回：共享只读冻结帧。
std::shared_ptr<const FrozenDesktopFrame> Desktop()
{
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 1, 1}, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709,
                        OutputColorMetadata{}, std::vector<std::uint8_t>{10, 20, 30, 0});
    return std::make_shared<const FrozenDesktopFrame>(RectI{0, 0, 1, 1}, std::move(planes));
}
// 使用有限超时收集通知，测试失败也不会永久等待工作线程。
struct Notifications final
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned int count{};
    // 记录一次后台通知并唤醒测试等待方。
    // 入参：无。返回：无。
    void Notify()
    {
        {
            const std::lock_guard<std::mutex> lock(this->mutex);
            ++this->count;
        }
        this->changed.notify_one();
    }
    // 等待给定次数通知，禁止无限等待。
    // 入参：expected 为最低次数。返回：期限内到达为true。
    bool Wait(unsigned int expected)
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        return this->changed.wait_for(lock, std::chrono::seconds(5),
                                      [this, expected]() { return this->count >= expected; });
    }
};
} // namespace

// 验证忙时仅保留最后待办，完成中的旧结果也不会进入结果邮箱。
// 入参：无。返回：断言执行次序和唯一结果版本。
TEST(SelectionPreviewWorkerTest, retains_only_latest_pending_and_discards_old_completion)
{
    Notifications notifications;
    std::promise<void> started, release;
    std::future<void> startedFuture = started.get_future();
    std::shared_future<void> released = release.get_future().share();
    std::vector<unsigned int> executed;
    std::unique_ptr<SelectionPreviewWorker> worker = SelectionPreviewWorkerTestAccess::Create(
        [&notifications]() { notifications.Notify(); },
        [&](const FrozenDesktopFrame&, RectI bounds, unsigned int brightness, HdrToneMappingOptions options,
            SdrSelectionFrame& frame, std::wstring&)
        {
            executed.push_back(brightness);
            EXPECT_EQ(options.highlightCeilingNits, 4000U);
            if (executed.size() == 1U)
            {
                started.set_value();
                (void)released.wait_for(std::chrono::seconds(3));
            }
            frame = SdrSelectionFrame(bounds, {1, 2, 3, 255});
            return true;
        });
    const std::shared_ptr<const FrozenDesktopFrame> desktop = Desktop();
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 100U, 1U, {4000U}));
    ASSERT_EQ(startedFuture.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 50U, 2U, {4000U}));
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 75U, 3U, {4000U}));
    release.set_value();
    ASSERT_TRUE(notifications.Wait(2U));
    std::optional<SelectionPreviewResult> result = worker->TakeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->requestId, 3U);
    EXPECT_TRUE(result->success);
    EXPECT_FALSE(worker->HasWork());
    EXPECT_EQ(executed, (std::vector<unsigned int>{100U, 75U}));
    EXPECT_FALSE(worker->TakeResult().has_value());
}

// 验证取消立即失去资格，运行任务安全持有冻结像素，取消后可以再次提交。
// 入参：无。返回：断言输入寿命、空邮箱和新请求完成。
TEST(SelectionPreviewWorkerTest, cancel_preserves_running_input_and_allows_next_session)
{
    Notifications notifications;
    std::promise<void> started, release;
    std::future<void> startedFuture = started.get_future();
    std::shared_future<void> released = release.get_future().share();
    bool first = true;
    std::unique_ptr<SelectionPreviewWorker> worker =
        SelectionPreviewWorkerTestAccess::Create([&notifications]() { notifications.Notify(); },
                                                 [&](const FrozenDesktopFrame&, RectI bounds, unsigned int,
                                                     HdrToneMappingOptions, SdrSelectionFrame& frame, std::wstring&)
                                                 {
                                                     if (first)
                                                     {
                                                         first = false;
                                                         started.set_value();
                                                         (void)released.wait_for(std::chrono::seconds(3));
                                                     }
                                                     frame = SdrSelectionFrame(bounds, {1, 2, 3, 255});
                                                     return true;
                                                 });
    std::shared_ptr<const FrozenDesktopFrame> desktop = Desktop();
    std::weak_ptr<const FrozenDesktopFrame> weak = desktop;
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 100U, 10U, {4000U}));
    ASSERT_EQ(startedFuture.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    worker->Cancel();
    desktop.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(worker->HasWork());
    EXPECT_FALSE(worker->TakeResult().has_value());
    release.set_value();
    ASSERT_TRUE(notifications.Wait(1U));
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(worker->HasWork());
    EXPECT_FALSE(worker->TakeResult().has_value());
    desktop = Desktop();
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 50U, 11U, {4000U}));
    ASSERT_TRUE(notifications.Wait(2U));
    const std::optional<SelectionPreviewResult> result = worker->TakeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->requestId, 11U);
}

// 验证实际转换保留SDR像素，通知回调允许查询状态和取结果且不会锁重入。
// 入参：无。返回：断言实际输出、非法请求拒绝及取消清邮箱。
TEST(SelectionPreviewWorkerTest, real_sdr_conversion_and_notification_are_safe)
{
    std::promise<SelectionPreviewResult> completed;
    std::future<SelectionPreviewResult> future = completed.get_future();
    std::unique_ptr<SelectionPreviewWorker> worker;
    worker = std::make_unique<SelectionPreviewWorker>(
        [&]()
        {
            (void)worker->HasWork();
            std::optional<SelectionPreviewResult> result = worker->TakeResult();
            if (result)
                completed.set_value(std::move(*result));
        });
    const std::shared_ptr<const FrozenDesktopFrame> desktop = Desktop();
    EXPECT_FALSE(worker->Submit(nullptr, desktop->Bounds(), 100U, 1U, {4000U}));
    EXPECT_FALSE(worker->Submit(desktop, desktop->Bounds(), 0U, 1U, {4000U}));
    EXPECT_FALSE(worker->Submit(desktop, {-1, 0, 1, 1}, 100U, 1U, {4000U}));
    ASSERT_TRUE(worker->Submit(desktop, desktop->Bounds(), 25U, 12U, {4000U}));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const SelectionPreviewResult result = future.get();
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.requestId, 12U);
    EXPECT_EQ((std::vector<std::uint8_t>(result.frame.Pixels().begin(), result.frame.Pixels().end())),
              (std::vector<std::uint8_t>{10, 20, 30, 0}));
    worker->Cancel();
    EXPECT_FALSE(worker->TakeResult().has_value());
}
// 验证实际后台转换接收请求中的策略和亮度，与独立正式输出相同。
// 入参：无。返回：断言生产路径像素完全一致。
TEST(SelectionPreviewWorkerTest, real_hdr_conversion_preserves_explicit_policy_snapshot)
{
    const std::array<std::uint16_t, 4> channels{EncodeFloat16(12.0F), EncodeFloat16(5.0F), EncodeFloat16(2.0F),
                                                EncodeFloat16(1.0F)};
    std::vector<std::uint8_t> pixels(8U);
    std::memcpy(pixels.data(), channels.data(), 8U);
    OutputColorMetadata metadata{};
    metadata.hasSdrWhiteLevel = true;
    metadata.sdrWhiteLevelNits = 240.0F;
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 1, 1}, CapturedPixelFormat::Rgba16FloatScRgb, CapturedColorSpace::ScRgb, metadata,
                        std::move(pixels));
    auto desktop = std::make_shared<const FrozenDesktopFrame>(RectI{0, 0, 1, 1}, std::move(planes));
    for (const unsigned int ceiling : {1000U, 10000U})
    {
        Notifications notifications;
        SelectionPreviewWorker worker([&] { notifications.Notify(); });
        EXPECT_FALSE(worker.Submit(desktop, desktop->Bounds(), 125U, 1U, {0U}));
        ASSERT_TRUE(worker.Submit(desktop, desktop->Bounds(), 125U, 2U, {ceiling}));
        ASSERT_TRUE(notifications.Wait(1U));
        const auto result = worker.TakeResult();
        ASSERT_TRUE(result && result->success);
        SelectionOutputRenderer renderer;
        ASSERT_TRUE(renderer.SetToneMappingOptions({ceiling}));
        ASSERT_TRUE(renderer.SetBrightnessPercent(125U));
        SdrSelectionFrame expected;
        std::wstring error;
        ASSERT_TRUE(renderer.Render(*desktop, desktop->Bounds(), expected, error)) << error;
        EXPECT_EQ((std::vector<std::uint8_t>(result->frame.Pixels().begin(), result->frame.Pixels().end())),
                  (std::vector<std::uint8_t>(expected.Pixels().begin(), expected.Pixels().end())));
    }
}
} // namespace open_st
