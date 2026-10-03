// 验证本地真实权重、双向标记、长文本拒绝、缓存卸载及取消边界，测试不创建网络请求。
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <translation_local.h>
#include <windows.h>

namespace
{
using namespace open_st::translation_local;
using Clock = std::chrono::steady_clock;
class LocalTranslationTest : public testing::Test
{
  protected:
    std::filesystem::path root;
    // 每例建立自己的中文资源根，不写入产品配置。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        this->root = std::filesystem::path(OPEN_ST_TRANSLATION_TEST_OUTPUT) /
                     (L"中英 模型 " + std::to_wstring(GetCurrentProcessId()));
        ASSERT_FALSE(std::filesystem::exists(this->root));
        ASSERT_TRUE(std::filesystem::create_directories(this->root));
    }
    // 只删除当前测试创建的专属根。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(this->root, error);
        EXPECT_FALSE(error);
    }
    // 从构建固定缓存复制指定方向，失败作为测试失败而非环境跳过。
    // 入参：direction 为固定方向。
    // 返回：无。
    void Copy(std::string_view direction)
    {
        const std::filesystem::path path = this->root / "resources/translation" / direction;
        std::filesystem::create_directories(path);
        std::filesystem::copy(std::filesystem::path(OPEN_ST_TRANSLATION_CACHE) / direction, path,
                              std::filesystem::copy_options::recursive);
    }
};
// 验证轻量状态查询区分缺少固定模型与不受支持的相对根，不尝试加载或下载。
// 入参：无。返回：无。
TEST_F(LocalTranslationTest, metadata_status_reports_missing_and_invalid_root)
{
    EXPECT_EQ(QueryModelFiles(this->root), Error::ModelMissing);
    EXPECT_EQ(QueryModelFiles("relative-model-root"), Error::Unavailable);
}
// 验证两方向全部存在时只承诺尺寸匹配；相同长度坏内容不会被误称为完整性已校验。
// 入参：无。返回：无。
TEST_F(LocalTranslationTest, metadata_status_does_not_hash_or_load_model_contents)
{
    this->Copy("en-zh");
    EXPECT_EQ(QueryModelFiles(this->root), Error::ModelMissing);
    this->Copy("zh-en");
    EXPECT_EQ(QueryModelFiles(this->root), Error::None);
    const std::filesystem::path path = this->root / "resources/translation/en-zh/config.json";
    const auto size = std::filesystem::file_size(path);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.put('!');
    file.close();
    ASSERT_EQ(std::filesystem::file_size(path), size);
    EXPECT_EQ(QueryModelFiles(this->root), Error::None);
}
// 验证尺寸异常和伪装为文件的目录被状态查询拒绝，无须读取模型正文。
// 入参：无。返回：无。
TEST_F(LocalTranslationTest, metadata_status_rejects_changed_size_and_directory)
{
    this->Copy("en-zh");
    this->Copy("zh-en");
    const std::filesystem::path path = this->root / "resources/translation/en-zh/config.json";
    std::filesystem::resize_file(path, 1);
    EXPECT_EQ(QueryModelFiles(this->root), Error::ModelIntegrity);
    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    EXPECT_EQ(QueryModelFiles(this->root), Error::ModelIntegrity);
}
// 验证显式日语在模型读取前不适用，不返回模型缺失。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, unsupported_source_never_loads_models)
{
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    EXPECT_EQ(engine.Execute("hello", "ja", "en", deadline, {}).error, Error::NotApplicable);
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
}
// 自动识别独立于目标；相同源目标不反推另一种源语言，不触碰缺失模型。
// 入参：无。返回：断言结果。
TEST_F(LocalTranslationTest, auto_source_does_not_infer_language_from_target)
{
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    const Result result =
        engine.Execute("The screenshot area can be moved across monitors.", "auto", "en", deadline, {});
    EXPECT_EQ(result.error, Error::NotApplicable);
    EXPECT_EQ(result.detectedLanguage, "en");
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
    EXPECT_EQ(engine.Execute("12345", "auto", "en", deadline, {}).error, Error::NotApplicable);
}
// 验证主动取消优先于超时和模型缺失，已过期任务不加载。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, cancellation_wins_over_expired_deadline)
{
    Engine engine(this->root);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", Clock::now(), cancel.get_token()).error, Error::Cancelled);
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", Clock::now(), {}).error, Error::TimedOut);
    EXPECT_EQ(engine.Execute("\xff", "en", "zh-CN", Clock::now() + std::chrono::seconds(10), {}).error,
              Error::InvalidInput);
}
// 验证模型缺失或相同长度损坏按本地不可用报告，不返回正文。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, missing_and_changed_models_fail_integrity)
{
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", deadline, {}).error, Error::ModelMissing);
    this->Copy("en-zh");
    const std::filesystem::path path = this->root / "resources/translation/en-zh/config.json";
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.put('!');
    file.close();
    const Result result = engine.Execute("hello", "en", "zh-CN", deadline, {});
    EXPECT_EQ(result.error, Error::ModelIntegrity);
    EXPECT_TRUE(result.text.empty());
}
// 验证真实中英两方向都有完整译文，切换方向只保留当前模型，维护可释放后重新加载。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, real_bilingual_models_and_idle_release)
{
    this->Copy("en-zh");
    this->Copy("zh-en");
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
    const Result english =
        engine.Execute("The screenshot area can be moved across monitors.", "auto", "zh-CN", deadline, {});
    ASSERT_EQ(english.error, Error::None);
    EXPECT_EQ(english.detectedLanguage, "en");
    EXPECT_FALSE(english.text.empty());
    EXPECT_EQ(english.text.find(">>"), std::string::npos);
    const Result chinese = engine.Execute("你好，世界！", "auto", "en", deadline, {});
    ASSERT_EQ(chinese.error, Error::None);
    EXPECT_EQ(chinese.detectedLanguage, "zh-CN");
    EXPECT_NE(chinese.text.find("Hello"), std::string::npos);
    const Clock::time_point maintenance = engine.NextMaintenance();
    EXPECT_NE(maintenance, Clock::time_point::max());
    engine.Maintain(maintenance - std::chrono::seconds(1));
    EXPECT_EQ(engine.NextMaintenance(), maintenance);
    engine.Maintain(maintenance);
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
    EXPECT_EQ(engine.Execute("Hello", "en", "zh-CN", deadline, {}).error, Error::None);
    engine.Release();
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
}
// 验证真实分词后超模型上限不会截断并返回看似成功的片段。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, oversized_tokens_do_not_publish_partial_translation)
{
    this->Copy("en-zh");
    Engine engine(this->root);
    std::string text;
    for (int index = 0; index < 600; ++index)
        text += " translation";
    const Result result = engine.Execute(text, "en", "zh-CN", Clock::now() + std::chrono::seconds(30), {});
    EXPECT_EQ(result.error, Error::InputTooLong);
    EXPECT_TRUE(result.text.empty());
}
// 真实双向模型只翻译内容行，混合文字、原有换行、全角空格与 Tab 缩进逐字节保持。
// 入参：无。返回：断言结果。
TEST_F(LocalTranslationTest, real_models_preserve_original_line_structure)
{
    this->Copy("en-zh");
    this->Copy("zh-en");
    Engine engine(this->root);
    for (const bool english : {true, false})
    {
        const std::string source = english ? "en" : "zh-CN";
        const std::string target = english ? "zh-CN" : "en";
        const std::string first = english ? "Hello 世界!" : "你好，世界！";
        const std::string second = english ? "Good morning." : "请关闭窗口。";
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
        const Result firstResult = engine.Execute(first, source, target, deadline, {});
        const Result secondResult = engine.Execute(second, source, target, deadline, {});
        ASSERT_EQ(firstResult.error, Error::None);
        ASSERT_EQ(secondResult.error, Error::None);
        const std::string text = "\t " + first + "\r\n\r\n　\t" + second + "\r" + first + "\n \t\r\n\n";
        const Result formatted = engine.Execute(text, source, target, deadline, {});
        ASSERT_EQ(formatted.error, Error::None);
        EXPECT_EQ(formatted.text, "\t " + firstResult.text + "\r\n\r\n　\t" + secondResult.text + "\r" +
                                      firstResult.text + "\n \t\r\n\n");
    }
}
// 多行各自很短也必须先通过原有整文 512 token 门禁，不能借逐行排版扩容。
// 入参：无。返回：断言结果。
TEST_F(LocalTranslationTest, short_lines_cannot_bypass_whole_document_token_limit)
{
    this->Copy("en-zh");
    Engine engine(this->root);
    std::string text;
    for (int index = 0; index < 600; ++index)
        text += "translation\n";
    const Result result = engine.Execute(text, "en", "zh-CN", Clock::now() + std::chrono::seconds(30), {});
    EXPECT_EQ(result.error, Error::InputTooLong);
    EXPECT_TRUE(result.text.empty());
}
// 验证热模型解码阶段超时和跨线程取消真正收尾才返回，结果正文不泄漏。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, running_decode_deadline_and_stop_do_not_publish)
{
    this->Copy("en-zh");
    Engine engine(this->root);
    ASSERT_EQ(engine.Execute("Hello", "en", "zh-CN", Clock::now() + std::chrono::seconds(30), {}).error, Error::None);
    std::string text = "Hello\n";
    for (int index = 0; index < 40; ++index)
        text += " Please close the application before installing the update.";
    const Result expired = engine.Execute(text, "en", "zh-CN", Clock::now() + std::chrono::milliseconds(1), {});
    EXPECT_EQ(expired.error, Error::TimedOut);
    EXPECT_TRUE(expired.text.empty());
    std::stop_source stop;
    // 延后取消实际计算，线程由测试范围唯一拥有并回收。
    // 入参：无。
    // 返回：无。
    std::jthread cancel(
        [&stop]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            stop.request_stop();
        });
    const Result cancelled =
        engine.Execute(text, "en", "zh-CN", Clock::now() + std::chrono::seconds(30), stop.get_token());
    EXPECT_EQ(cancelled.error, Error::Cancelled);
    EXPECT_TRUE(cancelled.text.empty());
}
} // namespace
