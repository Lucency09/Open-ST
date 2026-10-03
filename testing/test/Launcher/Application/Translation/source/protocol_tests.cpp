// 用合成协议覆盖多语言、编码、预算与供应商错误，不访问任何线上服务。
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sha256.h>
#include <translation_protocol.h>

namespace open_st
{
namespace
{
// 创建完整假配置，值不对应任何真实服务账号。
// 入参：provider 为协议名称。
// 返回：可用于纯请求构造的配置。
TranslationRequest Input(std::string provider)
{
    TranslationRequest input;
    input.text = "hello";
    nlohmann::json profile = CreateTranslationProfile(provider);
    if (provider == "baidu")
        profile["configuration"]["app_id"] = "2015063000000001";
    if (provider == "openai")
        profile["configuration"]["model"] = "fixture-model";
    if (profile["secrets"].contains("api_key"))
        profile["secrets"]["api_key"] = provider == "baidu" ? "12345678" : "fixture-key";
    input.configuration.interfaces = nlohmann::json::array({profile});
    return input;
}
// 将用例单项交给真实新版协议边界，不使用旧单供应商结构。
// 入参：input 为用例，salt 为签名盐，output 为请求。
// 返回：领域错误。
TranslationError Build(const TranslationRequest& input, std::string_view salt, http::Request& output)
{
    return translation_detail::BuildRequest(input, input.configuration.interfaces.at(0), salt, output);
}
} // namespace
// 验证公开演示签名基于编码前原文，且密钥不出现在请求正文。
// 入参：无。
// 返回：无，通过断言报告协议结果。
TEST(TranslationProtocol, BaiduUsesRawTextSignature)
{
    http::Request request;
    EXPECT_EQ(Build(Input("baidu"), "1435660288", request), TranslationError::None);
    EXPECT_NE(request.body.find("sign=2f7b6bfb034a64a978707bd303d20cce"), std::string::npos);
    EXPECT_EQ(request.body.find("12345678"), std::string::npos);
    EXPECT_EQ(request.url, L"https://fanyi-api.baidu.com/api/trans/vip/translate");
}
// 验证 Unicode 与表单特殊字符只出现在编码后的 q 中，防止参数注入。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, EncodesUnicodeAndFormDelimiters)
{
    TranslationRequest input = Input("baidu");
    input.text = "你好&+%\n";
    http::Request request;
    ASSERT_EQ(Build(input, "1", request), TranslationError::None);
    EXPECT_NE(request.body.find("q=%E4%BD%A0%E5%A5%BD%26%2B%25%0A&"), std::string::npos);
}
// 验证码点计数不按 UTF-8 字节数计费，并拒绝代理半区和过长编码。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, StrictUnicodeCodepoints)
{
    std::size_t count = 0;
    ASSERT_TRUE(translation_detail::CountText("你好😀", count));
    EXPECT_EQ(count, 3U);
    EXPECT_FALSE(translation_detail::CountText("\xed\xa0\x80", count));
    EXPECT_FALSE(translation_detail::CountText("\xc0\xaf", count));
    EXPECT_FALSE(translation_detail::CountText(std::string("a\0b", 3), count));
}
// 验证每个供应商本地码点上限，不通过分段隐式增加请求次数。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, EnforcesProviderBudgets)
{
    for (const std::string provider : {"baidu", "google", "openai", "deepl", "microsoft"})
    {
        TranslationRequest input = Input(provider);
        const std::size_t limit = provider == "baidu" ? 1000U : provider == "google" ? 2000U : 10000U;
        input.text.assign(limit, 'a');
        http::Request request;
        EXPECT_EQ(Build(input, "1", request), TranslationError::None);
        input.text += 'a';
        EXPECT_EQ(Build(input, "1", request), TranslationError::InputTooLarge);
    }
}
// 验证 JSON 转义后的完整请求仍受 64 KiB 限制。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, EnforcesSerializedBudget)
{
    TranslationRequest input = Input("openai");
    input.text.assign(10000, '\x01');
    input.configuration.interfaces[0]["configuration"]["model"] = std::string(2048, '\\');
    http::Request request;
    EXPECT_EQ(Build(input, "1", request), TranslationError::None);
    EXPECT_LE(request.body.size(), 65536U);
}
// 验证缺失账号不会发出请求，控制字符与完整资源路径不能充当 API 根地址。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, RejectsInvalidConfiguration)
{
    TranslationRequest input = Input("baidu");
    input.configuration.interfaces[0]["secrets"]["api_key"] = "";
    http::Request request;
    EXPECT_EQ(Build(input, "1", request), TranslationError::MissingCredentials);
    nlohmann::json profile = CreateTranslationProfile("openai");
    profile["secrets"]["api_key"] = "key\r\nOther: injected";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    profile["secrets"]["api_key"] = "";
    for (const std::string url : {"https://example.com/v1/chat/completions/", "https://user:pass@example.com"})
    {
        profile["configuration"]["base_url"] = url;
        EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    }
}
// 验证各请求路径、认证及自动语种省略规则，不额外发送检测请求。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, BuildsSingleProviderRequests)
{
    http::Request request;
    ASSERT_EQ(Build(Input("openai"), "1", request), TranslationError::None);
    EXPECT_EQ(request.url, L"https://api.openai.com/v1/chat/completions");
    const nlohmann::json openAi = nlohmann::json::parse(request.body);
    EXPECT_EQ(openAi.at("messages").size(), 2U);
    EXPECT_EQ(openAi.at("messages").at(1).at("content"), "hello");
    ASSERT_EQ(Build(Input("deepl"), "1", request), TranslationError::None);
    EXPECT_EQ(request.url, L"https://api-free.deepl.com/v2/translate");
    const nlohmann::json deepL = nlohmann::json::parse(request.body);
    EXPECT_FALSE(deepL.contains("source_lang"));
    EXPECT_EQ(deepL.at("target_lang"), "ZH-HANS");
    ASSERT_EQ(Build(Input("microsoft"), "1", request), TranslationError::None);
    EXPECT_EQ(request.url.find(L"&from="), std::wstring::npos);
    EXPECT_NE(request.url.find(L"to=zh-Hans"), std::wstring::npos);
    ASSERT_EQ(Build(Input("google"), "1", request), TranslationError::None);
    EXPECT_EQ(request.body, "q=hello");
    EXPECT_EQ(request.method, http::Method::Post);
}
// 验证五家合成成功样例生成纯文本与规范检测语种。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, ParsesFiveProviderFixtures)
{
    TranslationResult result;
    EXPECT_EQ(translation_detail::ParseResponse(
                  "baidu", 200, R"({"from":"en","trans_result":[{"dst":"你好"},{"dst":"世界"}]})", result),
              TranslationError::None);
    EXPECT_EQ(result.text, "你好\n世界");
    EXPECT_EQ(result.detectedLanguage, "en");
    EXPECT_EQ(
        translation_detail::ParseResponse("google", 200, R"([[["你好","hello"],["世界","world"]],null,"en"])", result),
        TranslationError::None);
    EXPECT_EQ(result.text, "你好世界");
    EXPECT_EQ(translation_detail::ParseResponse(
                  "openai", 200, R"({"choices":[{"finish_reason":"stop","message":{"content":"你好"}}]})", result),
              TranslationError::None);
    EXPECT_EQ(result.text, "你好");
    EXPECT_EQ(translation_detail::ParseResponse(
                  "deepl", 200, R"({"translations":[{"text":"你好","detected_source_language":"EN"}]})", result),
              TranslationError::None);
    EXPECT_EQ(translation_detail::ParseResponse(
                  "microsoft", 200,
                  R"([{"detectedLanguage":{"language":"en"},"translations":[{"text":"你好","to":"zh-Hans"}]}])",
                  result),
              TranslationError::None);
    EXPECT_EQ(result.detectedLanguage, "en");
}
// 验证百度 HTTP 200 业务错误、HTTP 状态及截断不会冒充成功。
// 入参：无。
// 返回：无。
TEST(TranslationProtocol, CategorizesErrorsWithoutPublishingPartialText)
{
    TranslationResult result;
    result.text = "previous";
    EXPECT_EQ(translation_detail::ParseResponse("baidu", 200, R"({"error_code":"52003"})", result),
              TranslationError::Authentication);
    EXPECT_EQ(translation_detail::ParseResponse("baidu", 200, R"({"error_code":54003})", result),
              TranslationError::RateLimited);
    EXPECT_EQ(translation_detail::ParseResponse("baidu", 200, R"({"error_code":"54004"})", result),
              TranslationError::Quota);
    EXPECT_EQ(translation_detail::ParseResponse("baidu", 200, R"({"error_code":"58002"})", result),
              TranslationError::Permission);
    EXPECT_EQ(translation_detail::ParseResponse("openai", 401, "", result), TranslationError::Authentication);
    EXPECT_EQ(translation_detail::ParseResponse("deepl", 456, "", result), TranslationError::Quota);
    EXPECT_EQ(translation_detail::ParseResponse("google", 429, "<html>limited</html>", result),
              TranslationError::RateLimited);
    EXPECT_EQ(translation_detail::ParseResponse("google", 302, "", result), TranslationError::Redirect);
    EXPECT_EQ(translation_detail::ParseResponse("google", 503, "", result), TranslationError::Service);
    EXPECT_EQ(translation_detail::ParseResponse(
                  "openai", 200, R"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})", result),
              TranslationError::Truncated);
    EXPECT_EQ(
        translation_detail::ParseResponse("openai", 400, R"({"error":{"code":"context_length_exceeded"}})", result),
        TranslationError::ContextLimit);
    EXPECT_EQ(translation_detail::ParseResponse("google", 200, "<html>captcha</html>", result),
              TranslationError::UnsupportedResponse);
    EXPECT_EQ(result.text, "previous");
}

// 验证界面发布的方法、正文模式和错误类别确实被现有协议校验接受。
// 入参：无；仅构造合成配置，不创建网络请求。
// 返回：断言结果，避免表单下拉提供无法保存的选项。
TEST(TranslationProtocol, custom_editor_choices_match_supported_configuration)
{
    // 创建带完整变量引用的独立草稿，避免正文模式影响语言能力校验。
    // 入参：无。返回：只含合成参数的配置。
    const auto baseline = []()
    {
        nlohmann::json profile = CreateTranslationProfile("custom_http");
        profile["configuration"]["query"] = {{"text", "{{text}}"}, {"source", "{{source}}"}, {"target", "{{target}}"}};
        return profile;
    };
    ASSERT_FALSE(TranslationChoices("custom.method").empty());
    ASSERT_FALSE(TranslationChoices("custom.body_mode").empty());
    ASSERT_FALSE(TranslationChoices("custom.error_category").empty());
    EXPECT_TRUE(TranslationProfileFields("custom_http").empty());
    for (const std::string_view method : TranslationChoices("custom.method"))
    {
        nlohmann::json profile = baseline();
        profile["configuration"]["method"] = method;
        profile["configuration"]["body_mode"] = "none";
        profile["configuration"]["body"] = nullptr;
        EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::None) << method;
    }
    for (const std::string_view mode : TranslationChoices("custom.body_mode"))
    {
        nlohmann::json profile = baseline();
        profile["configuration"]["body_mode"] = mode;
        if (mode == "none")
            profile["configuration"]["body"] = nullptr;
        else if (mode == "raw")
            profile["configuration"]["body"] = "{{text}}";
        else
            profile["configuration"]["body"] = {{"text", "{{text}}"}};
        EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::None) << mode;
    }
    for (const std::string_view category : TranslationChoices("custom.error_category"))
    {
        nlohmann::json profile = baseline();
        profile["configuration"]["response"]["error_pointer"] = "/error";
        profile["configuration"]["response"]["error_map"] = {{"fixture", category}};
        EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::None) << category;
    }
}
} // namespace open_st
