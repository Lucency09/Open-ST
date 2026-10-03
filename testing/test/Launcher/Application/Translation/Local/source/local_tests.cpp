// 验证本地真实权重、双向标记、长文本拒绝、缓存卸载及取消边界，测试不创建网络请求。
#include <ctranslate2/models/model.h>
#include <ctranslate2/models/sequence_to_sequence.h>
#include <ctranslate2/utils.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <translation_local.h>
#include <vector>
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
    EXPECT_EQ(engine.Execute("hello", "ja", "en", Options{1}, deadline, {}).error, Error::NotApplicable);
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
}
// 自动识别独立于目标；相同源目标不反推另一种源语言，不触碰缺失模型。
// 入参：无。返回：断言结果。
TEST_F(LocalTranslationTest, auto_source_does_not_infer_language_from_target)
{
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    const Result result =
        engine.Execute("The screenshot area can be moved across monitors.", "auto", "en", Options{1}, deadline, {});
    EXPECT_EQ(result.error, Error::NotApplicable);
    EXPECT_EQ(result.detectedLanguage, "en");
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
    EXPECT_EQ(engine.Execute("12345", "auto", "en", Options{1}, deadline, {}).error, Error::NotApplicable);
}
// 验证主动取消优先于超时和模型缺失，已过期任务不加载。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, cancellation_wins_over_expired_deadline)
{
    Engine engine(this->root);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", Options{1}, Clock::now(), cancel.get_token()).error,
              Error::Cancelled);
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", Options{1}, Clock::now(), {}).error, Error::TimedOut);
    EXPECT_EQ(engine.Execute("\xff", "en", "zh-CN", Options{1}, Clock::now() + std::chrono::seconds(10), {}).error,
              Error::InvalidInput);
}
// 验证模型缺失或相同长度损坏按本地不可用报告，不返回正文。
// 入参：无。
// 返回：无。
TEST_F(LocalTranslationTest, missing_and_changed_models_fail_integrity)
{
    Engine engine(this->root);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
    EXPECT_EQ(engine.Execute("hello", "en", "zh-CN", Options{1}, deadline, {}).error, Error::ModelMissing);
    this->Copy("en-zh");
    const std::filesystem::path path = this->root / "resources/translation/en-zh/config.json";
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.put('!');
    file.close();
    const Result result = engine.Execute("hello", "en", "zh-CN", Options{1}, deadline, {});
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
        engine.Execute("The screenshot area can be moved across monitors.", "auto", "zh-CN", Options{1}, deadline, {});
    ASSERT_EQ(english.error, Error::None);
    EXPECT_EQ(english.detectedLanguage, "en");
    EXPECT_FALSE(english.text.empty());
    EXPECT_EQ(english.text.find(">>"), std::string::npos);
    const Result chinese = engine.Execute("你好，世界！", "auto", "en", Options{1}, deadline, {});
    ASSERT_EQ(chinese.error, Error::None);
    EXPECT_EQ(chinese.detectedLanguage, "zh-CN");
    EXPECT_NE(chinese.text.find("Hello"), std::string::npos);
    const Clock::time_point maintenance = engine.NextMaintenance();
    EXPECT_NE(maintenance, Clock::time_point::max());
    engine.Maintain(maintenance - std::chrono::seconds(1));
    EXPECT_EQ(engine.NextMaintenance(), maintenance);
    engine.Maintain(maintenance);
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
    EXPECT_EQ(engine.Execute("Hello", "en", "zh-CN", Options{1}, deadline, {}).error, Error::None);
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
    const Result result = engine.Execute(text, "en", "zh-CN", Options{1}, Clock::now() + std::chrono::seconds(30), {});
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
        const Result firstResult = engine.Execute(first, source, target, Options{1}, deadline, {});
        const Result secondResult = engine.Execute(second, source, target, Options{1}, deadline, {});
        ASSERT_EQ(firstResult.error, Error::None);
        ASSERT_EQ(secondResult.error, Error::None);
        const std::string text = "\t " + first + "\r\n\r\n　\t" + second + "\r" + first + "\n \t\r\n\n";
        const Result formatted = engine.Execute(text, source, target, Options{1}, deadline, {});
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
    const Result result = engine.Execute(text, "en", "zh-CN", Options{1}, Clock::now() + std::chrono::seconds(30), {});
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
    ASSERT_EQ(engine.Execute("Hello", "en", "zh-CN", Options{1}, Clock::now() + std::chrono::seconds(30), {}).error,
              Error::None);
    std::string text = "Hello\n";
    for (int index = 0; index < 40; ++index)
        text += " Please close the application before installing the update.";
    const Result expired =
        engine.Execute(text, "en", "zh-CN", Options{1}, Clock::now() + std::chrono::milliseconds(1), {});
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
        engine.Execute(text, "en", "zh-CN", Options{1}, Clock::now() + std::chrono::seconds(30), stop.get_token());
    EXPECT_EQ(cancelled.error, Error::Cancelled);
    EXPECT_TRUE(cancelled.text.empty());
}
// 零束宽在读取模型前拒绝；档位映射仍由上层配置负责。
// 入参：无。返回：断言结果。
TEST_F(LocalTranslationTest, zero_beam_is_rejected_before_loading)
{
    Engine engine(this->root);
    const Result result =
        engine.Execute("Hello", "en", "zh-CN", Options{0}, Clock::now() + std::chrono::seconds(30), {});
    EXPECT_EQ(result.error, Error::InvalidInput);
    EXPECT_TRUE(result.text.empty());
    EXPECT_EQ(engine.NextMaintenance(), Clock::time_point::max());
}
class LocalBeamTranslationTest : public LocalTranslationTest, public testing::WithParamInterface<std::size_t>
{
};
// 三种配置束宽都使用相同真实权重，代表样本与混合行结束符保持逐行对应。
// 入参：测试参数为显式束宽。返回：断言结果；不把非空译文当成语义质量验收。
TEST_P(LocalBeamTranslationTest, representative_samples_preserve_original_layout)
{
    this->Copy("en-zh");
    this->Copy("zh-en");
    Engine engine(this->root);
    const Options options{GetParam()};
    for (const bool english : {true, false})
    {
        const std::string source = english ? "en" : "zh-CN";
        const std::string target = english ? "zh-CN" : "en";
        const std::string first = english ? "Hello, world!" : "你好，世界！";
        const std::string second = english ? "Please close the window." : "请关闭窗口。";
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
        const Result firstResult = engine.Execute(first, source, target, options, deadline, {});
        const Result secondResult = engine.Execute(second, source, target, options, deadline, {});
        ASSERT_EQ(firstResult.error, Error::None);
        ASSERT_EQ(secondResult.error, Error::None);
        EXPECT_FALSE(firstResult.text.empty());
        EXPECT_FALSE(secondResult.text.empty());
        EXPECT_EQ(firstResult.text.find(">>"), std::string::npos);
        EXPECT_EQ(secondResult.text.find(">>"), std::string::npos);
        const std::string text = "\t " + first + "\r\n\r\n　\t" + second + "\r \t\n";
        const Result formatted = engine.Execute(text, source, target, options, deadline, {});
        ASSERT_EQ(formatted.error, Error::None);
        EXPECT_EQ(formatted.text, "\t " + firstResult.text + "\r\n\r\n　\t" + secondResult.text + "\r \t\n");
    }
}
// 热模型在每种束宽下都能停止整项任务、不返回部分行，并能继续完成下一次请求。
// 入参：测试参数为显式束宽。返回：断言结果；不与其他引擎并行执行或下载模型。
TEST_P(LocalBeamTranslationTest, running_stop_and_deadline_allow_next_request)
{
    this->Copy("en-zh");
    Engine engine(this->root);
    const Options options{GetParam()};
    const std::string sample = "Please close the window.";
    ASSERT_EQ(engine.Execute(sample, "en", "zh-CN", options, Clock::now() + std::chrono::seconds(30), {}).error,
              Error::None);
    std::string text = "Hello\n";
    for (int index = 0; index < 40; ++index)
        text += " Please close the application before installing the update.";
    const Result expired =
        engine.Execute(text, "en", "zh-CN", options, Clock::now() + std::chrono::milliseconds(20), {});
    EXPECT_EQ(expired.error, Error::TimedOut);
    EXPECT_TRUE(expired.text.empty());
    const Result afterTimeout =
        engine.Execute(sample, "en", "zh-CN", options, Clock::now() + std::chrono::seconds(30), {});
    ASSERT_EQ(afterTimeout.error, Error::None);
    ASSERT_FALSE(afterTimeout.text.empty());
    std::stop_source stop;
    // 等待真实请求进入计算后取消，测试结束前回收唯一辅助线程。
    // 入参：无。返回：无。
    std::jthread cancel(
        [&stop]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            stop.request_stop();
        });
    const Result cancelled =
        engine.Execute(text, "en", "zh-CN", options, Clock::now() + std::chrono::seconds(30), stop.get_token());
    cancel.join();
    EXPECT_EQ(cancelled.error, Error::Cancelled);
    EXPECT_TRUE(cancelled.text.empty());
    const Result afterCancel =
        engine.Execute(sample, "en", "zh-CN", options, Clock::now() + std::chrono::seconds(30), {});
    ASSERT_EQ(afterCancel.error, Error::None);
    EXPECT_EQ(afterCancel.text, afterTimeout.text);
}
// 直接验证补丁的解码步检查；第八次回调发生在编码完成及两轮解码之后，不能仅由编码边界满足。
// 入参：参数为显式束宽。返回：停止必抛异常且不交付部分结果，同一副本随后仍可完整推理。
TEST_P(LocalBeamTranslationTest, native_stop_reaches_decode_loop_and_recovers)
{
    ctranslate2::set_num_threads(2);
    const std::filesystem::path path = std::filesystem::path(OPEN_ST_TRANSLATION_CACHE) / "en-zh";
    const std::shared_ptr<const ctranslate2::models::Model> model =
        ctranslate2::models::Model::load(path.string(), ctranslate2::Device::CPU, 0, ctranslate2::ComputeType::INT8);
    std::unique_ptr<ctranslate2::models::SequenceToSequenceReplica> replica = model->as_sequence_to_sequence();
    // 固定词表已核对的公共句子分词，测试只考察原生停止协议，不引入另一套产品分词实现。
    const std::vector<std::vector<std::string>> source{{">>cmn_Hans<<", "▁Please", "▁close", "▁the", "▁application",
                                                        "▁before", "▁installing", "▁the", "▁update", "."}};
    ctranslate2::TranslationOptions options;
    options.beam_size = GetParam();
    options.max_input_length = 0;
    options.min_decoding_length = 8;
    options.max_decoding_length = 64;
    options.return_end_token = true;
    options.end_token = std::string("</s>");
    std::size_t checks = 0;
    // 三次编码／解码入口检查后，第四至七次覆盖两轮解码，第八次在第三轮开始停止。
    // 入参：无。返回：仅到达确定的解码检查点时停止。
    options.should_stop = [&checks] { return ++checks == 8; };
    std::vector<ctranslate2::TranslationResult> result;
    EXPECT_THROW(result = replica->translate(source, {}, options), std::runtime_error);
    EXPECT_EQ(checks, 8U);
    EXPECT_TRUE(result.empty());
    options.should_stop = nullptr;
    ASSERT_NO_THROW(result = replica->translate(source, {}, options));
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result.front().hypotheses.size(), 1U);
    ASSERT_FALSE(result.front().hypotheses.front().empty());
    EXPECT_EQ(result.front().hypotheses.front().back(), "</s>");
}
INSTANTIATE_TEST_SUITE_P(ExplicitBeamOptions, LocalBeamTranslationTest,
                         testing::Values(std::size_t{1}, std::size_t{2}, std::size_t{4}));
} // namespace
