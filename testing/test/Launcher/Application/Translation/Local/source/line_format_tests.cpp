// 验证生产逐行组装边界的格式、统一预算和原子发布，不构造网络或模型替身流程。
#include <gtest/gtest.h>
#include <thread>
#include <translation_lines.h>
#include <vector>

namespace
{
using namespace open_st::translation_local;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
// 硬换行与缩进逐字节保留，空白行不进入译文回调，模型额外前导空白不改变原缩进。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, preserves_mixed_separators_indentation_and_empty_lines)
{
    const std::string text = "\r\n \tHello\r　\tworld\n\xc2\xa0\xe2\x80\x89\r\n\n";
    std::vector<std::string> calls;
    const Result result =
        detail::TranslateLines(text, Clock::now() + 1s, {},
                               // 记录真正送入模型的内容，故意返回额外空白验证原缩进的唯一归属。
                               // 入参：line 为内容行。返回：确定性译文。
                               [&calls](std::string_view line)
                               {
                                   calls.emplace_back(line);
                                   return Result{Error::None, calls.size() == 1 ? "  第一行" : "第二行"};
                               });
    ASSERT_EQ(result.error, Error::None);
    EXPECT_EQ(calls, (std::vector<std::string>{"Hello", "world"}));
    EXPECT_EQ(result.text, "\r\n \t第一行\r　\t第二行\n\xc2\xa0\xe2\x80\x89\r\n\n");
}
// 纯空白内容由组装层原样交回，不为每个空行调用推理。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, blank_lines_never_call_model)
{
    const std::string text = "\t \r\n　\n\xc2\xa0\r\n\n";
    unsigned calls{};
    const Result result = detail::TranslateLines(text, Clock::now() + 1s, {},
                                                 // 任何调用都表示错误进入空白行推理。
                                                 // 入参：未使用行。返回：失败结果。
                                                 [&calls](std::string_view)
                                                 {
                                                     ++calls;
                                                     return Result{Error::Inference};
                                                 });
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.text, text);
    EXPECT_EQ(calls, 0U);
}
// 第二行任意领域失败都丢弃第一行草稿，且不尝试第三行。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, later_line_failure_never_publishes_partial_translation)
{
    for (const Error failure : {Error::InputTooLong, Error::Truncated, Error::Inference, Error::OutOfMemory,
                                Error::InvalidInput, Error::Cancelled, Error::TimedOut})
    {
        unsigned calls{};
        const Result result = detail::TranslateLines(
            "first\nsecond\nthird", Clock::now() + 1s, {}, // 在首行完成后注入已分类错误。
            // 入参：未使用行。返回：完整首行或指定失败。
            [&calls, failure](std::string_view)
            { return ++calls == 1 ? Result{Error::None, "complete first line"} : Result{failure}; });
        EXPECT_EQ(result.error, failure);
        EXPECT_TRUE(result.text.empty());
        EXPECT_EQ(calls, 2U);
    }
}
// 第二行合作取消覆盖迟到成功，已组装第一行也不得外泄。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, cancellation_during_later_line_discards_all_output)
{
    unsigned calls{};
    std::stop_source stop;
    const Result result = detail::TranslateLines("first\nsecond\nthird", Clock::now() + 1s, stop.get_token(),
                                                 // 第二行返回前触发整轮取消，模拟迟到成功。
                                                 // 入参：未使用行。返回：必须被丢弃的文字。
                                                 [&calls, &stop](std::string_view)
                                                 {
                                                     if (++calls == 2)
                                                         stop.request_stop();
                                                     return Result{Error::None, "late success"};
                                                 });
    EXPECT_EQ(result.error, Error::Cancelled);
    EXPECT_TRUE(result.text.empty());
    EXPECT_EQ(calls, 2U);
}
// 原始期限在第二行仍生效，不给每一行重新分配完整预算。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, all_lines_share_the_original_deadline)
{
    unsigned calls{};
    const Clock::time_point deadline = Clock::now() + 50ms;
    const Result result = detail::TranslateLines("first\nsecond\nthird", deadline, {},
                                                 // 第一行成功后让第二行跨过原始截止时刻。
                                                 // 入参：未使用行。返回：迟到的完整文字。
                                                 [&calls, deadline](std::string_view)
                                                 {
                                                     if (++calls == 2)
                                                         std::this_thread::sleep_until(deadline + 1ms);
                                                     return Result{Error::None, "late success"};
                                                 });
    EXPECT_EQ(result.error, Error::TimedOut);
    EXPECT_TRUE(result.text.empty());
    EXPECT_EQ(calls, 2U);
}
// 内存不足即使同时到期也不能变成允许自动后备的超时。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, late_memory_failure_keeps_its_fatal_classification)
{
    const Clock::time_point deadline = Clock::now() + 50ms;
    const Result result = detail::TranslateLines("first\nnever", deadline, {},
                                                 // 原始期限后报告进程级资源失败。
                                                 // 入参：未使用行。返回：内存不足。
                                                 [deadline](std::string_view)
                                                 {
                                                     std::this_thread::sleep_until(deadline + 1ms);
                                                     return Result{Error::OutOfMemory};
                                                 });
    EXPECT_EQ(result.error, Error::OutOfMemory);
    EXPECT_TRUE(result.text.empty());
}
// 最终预算包括每条分隔符及原始缩进，边界内完整发布，超一字节整项拒绝。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, final_output_budget_includes_separators_and_indentation)
{
    constexpr std::size_t OUTPUT_BUDGET = 1024 * 1024;
    for (const bool exceeds : {false, true})
    {
        unsigned calls{};
        const Result result = detail::TranslateLines(
            "first\n \tsecond", Clock::now() + 1s, {},
            // 原文短行产生长输出，用格式字节跨越整项边界。
            // 入参：未使用行。返回：精确长度译文。
            [&calls, exceeds](std::string_view)
            {
                return ++calls == 1 ? Result{Error::None, std::string(OUTPUT_BUDGET - (exceeds ? 4U : 5U), 'x')}
                                    : Result{Error::None, "ok"};
            });
        EXPECT_EQ(result.error, exceeds ? Error::Truncated : Error::None);
        EXPECT_EQ(result.text.size(), exceeds ? 0U : OUTPUT_BUDGET);
        EXPECT_EQ(calls, 2U);
    }
}
// 模型若意外增加硬换行，作为协议失败处理，不能悄悄改变原始行结构。
// 入参：无。返回：断言结果。
TEST(LocalTranslationLines, generated_extra_line_breaks_are_not_published)
{
    const Result result = detail::TranslateLines("one line", Clock::now() + 1s, {},
                                                 // 模拟异常模型输出改变硬换行数量。
                                                 // 入参：未使用行。返回：含额外换行的文字。
                                                 [](std::string_view) { return Result{Error::None, "first\nextra"}; });
    EXPECT_EQ(result.error, Error::Inference);
    EXPECT_TRUE(result.text.empty());
}
} // namespace
