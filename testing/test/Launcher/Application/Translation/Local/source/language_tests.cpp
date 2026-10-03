// 验证自动语言检测的首选语义、规范化副本、预算和真实离线系统服务。
#include <gtest/gtest.h>
#include <translation_language.h>
#include <thread>

namespace
{
using namespace open_st::translation_local;
using Clock = std::chrono::steady_clock;
// 包装系统注册表式结果，保留末尾双 NUL。
// 入参：preferred 为首选，other 为次选。返回：检测器原始结果。
std::wstring Languages(std::wstring_view preferred, std::wstring_view other = {})
{
    std::wstring result(preferred);
    result.push_back(L'\0');
    result.append(other);
    result.push_back(L'\0');
    return result;
}
// 首选不受支持时不搜索后面的中英文，也不把繁体中文强行视为简体。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, only_first_supported_language_is_selected)
{
    for (const std::wstring_view preferred : {L"ja", L"zh-Hant", L"fr", L""})
    {
        const Result result =
            detail::DetectSourceLanguage("hello", Clock::now() + std::chrono::seconds(10), {},
                                         [preferred](std::wstring_view) { return Languages(preferred, L"en"); });
        EXPECT_EQ(result.error, Error::NotApplicable);
        EXPECT_TRUE(result.detectedLanguage.empty());
    }
    for (const std::wstring_view preferred : {L"en", L"en-US", L"zh", L"zh-Hans", L"zh-CN", L"zh-SG"})
    {
        const Result result =
            detail::DetectSourceLanguage("hello", Clock::now() + std::chrono::seconds(10), {},
                                         [preferred](std::wstring_view) { return Languages(preferred); });
        EXPECT_EQ(result.error, Error::None);
        EXPECT_EQ(result.detectedLanguage, preferred.starts_with(L"en") ? "en" : "zh-CN");
    }
}
// 检测器看到全文 NFC 副本，原始空白和分解编码不被改写。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, normalizes_only_detection_copy)
{
    const std::string text = "  Cafe\xcc\x81\r\n\r\n\ttext";
    std::wstring seen;
    const Result result = detail::DetectSourceLanguage(text, Clock::now() + std::chrono::seconds(10), {},
                                                       [&seen](std::wstring_view normalized)
                                                       {
                                                           seen = normalized;
                                                           return Languages(L"en");
                                                       });
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(seen, L"  Caf\u00e9\r\n\r\n\ttext");
    EXPECT_EQ(text, "  Cafe\xcc\x81\r\n\r\n\ttext");
}
// 无结果、损坏结果、非法输入均不允许猜测源语言。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, missing_results_and_invalid_input_are_not_guessed)
{
    for (const std::wstring& raw : {std::wstring{}, std::wstring(L"en")})
        EXPECT_EQ(detail::DetectSourceLanguage("12345", Clock::now() + std::chrono::seconds(10), {},
                                               [&raw](std::wstring_view) { return raw; })
                      .error,
                  Error::NotApplicable);
    EXPECT_EQ(detail::DetectSourceLanguage("\xff", Clock::now() + std::chrono::seconds(10), {}).error,
              Error::InvalidInput);
}
// 检测前后均服从统一停止规则，不因系统调用返回而发布过期结果。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, cancellation_and_deadline_cover_detection)
{
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(detail::DetectSourceLanguage("hello", Clock::now(), stop.get_token()).error, Error::Cancelled);
    EXPECT_EQ(detail::DetectSourceLanguage("hello", Clock::now(), {}).error, Error::TimedOut);
    std::stop_source during;
    EXPECT_EQ(detail::DetectSourceLanguage("hello", Clock::now() + std::chrono::seconds(10), during.get_token(),
                                           [&during](std::wstring_view)
                                           {
                                               during.request_stop();
                                               return Languages(L"en");
                                           })
                  .error,
              Error::Cancelled);
}
// 检测耗尽同一截止时间后，成功的系统结果也不恢复任务资格。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, deadline_is_checked_after_detector_returns)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(20);
    const Result result = detail::DetectSourceLanguage("hello", deadline, {},
                                                       [deadline](std::wstring_view)
                                                       {
                                                           std::this_thread::sleep_until(deadline);
                                                           return Languages(L"en");
                                                       });
    EXPECT_EQ(result.error, Error::TimedOut);
    EXPECT_TRUE(result.detectedLanguage.empty());
}
// 实际系统识别代表性中英文；短文本/数字/混合文字仅观察系统结果，不承诺必准。
// 入参：无。返回：断言结果。
TEST(LocalLanguageTest, windows_detects_representative_bilingual_text)
{
    EXPECT_EQ(detail::DetectSourceLanguage("The screenshot area can be moved across monitors.",
                                           Clock::now() + std::chrono::seconds(10), {})
                  .detectedLanguage,
              "en");
    EXPECT_EQ(
        detail::DetectSourceLanguage("你好，世界！", Clock::now() + std::chrono::seconds(10), {}).detectedLanguage,
        "zh-CN");
    for (const std::string_view text : {"Hello", "12345", "Hello 世界", "\t\r\n"})
    {
        const Result result = detail::DetectSourceLanguage(text, Clock::now() + std::chrono::seconds(10), {});
        EXPECT_TRUE(result.error == Error::None || result.error == Error::NotApplicable);
        EXPECT_TRUE(result.detectedLanguage.empty() || result.detectedLanguage == "en" ||
                    result.detectedLanguage == "zh-CN");
    }
}
} // namespace
