// 验证有序配置、静态 HTTP 模板与有界响应映射；完全不访问线上服务。
#include <algorithm>
#include <fstream>
#include <limits>
#include "translation_test_defaults.h"
#include <gtest/gtest.h>
#include <translation_protocol.h>

namespace open_st
{
namespace
{
// 创建带非真实密钥的自定义请求样例。
// 入参：无。
// 返回：独立配置。
nlohmann::json Custom()
{
    nlohmann::json profile = TestTranslationProfile("custom_http");
    profile["secrets"]["api_key"] = "fixture";
    return profile;
}
// 创建中英自动源语种样例，不读取本机设置。
// 入参：无。
// 返回：有界原文。
TranslationRequest Text()
{
    TranslationRequest input = TestTranslationRequest();
    input.text = "你好\"\n&+%😀";
    return input;
}
} // namespace
// 验证默认六项唯一身份、仅百度启用，本地不需要秘密字段。
// 入参：无。
// 返回：断言结果。
TEST(TranslationSchema, DefaultsAndNewIdentity)
{
    const nlohmann::json defaults = TestDefaultSettings().at("translation.interfaces");
    EXPECT_EQ(defaults.size(), 6U);
    EXPECT_EQ(ValidateTranslationInterfaces(defaults), TranslationError::None);
    EXPECT_TRUE(defaults[0]["enabled"].get<bool>());
    for (std::size_t i = 1; i < defaults.size(); ++i)
        EXPECT_FALSE(defaults[i]["enabled"].get<bool>());
    EXPECT_NE(TestTranslationProfile("google")["id"], TestTranslationProfile("google")["id"]);
    EXPECT_EQ(TestDefaultSettings().at("translation.interfaces"), defaults);
    EXPECT_EQ(ValidateTranslationProfile(TestTranslationProfile("custom_http")), TranslationError::None);
    std::ifstream stream(std::filesystem::path(OPEN_ST_TEST_SOURCE_ROOT) / "resources/default_settings.json");
    ASSERT_TRUE(stream.good());
    const nlohmann::json resource = nlohmann::json::parse(stream);
    EXPECT_EQ(resource.at("settings").at("translation.interfaces"), defaults);
}
// 验证同类型多实例合法而重复 ID、未知类型与超项数拒绝。
// 入参：无。
// 返回：断言结果。
TEST(TranslationSchema, RejectsDuplicatesAndUnsupportedStructure)
{
    nlohmann::json profiles =
        nlohmann::json::array({TestTranslationProfile("google"), TestTranslationProfile("google")});
    EXPECT_EQ(ValidateTranslationInterfaces(profiles), TranslationError::None);
    profiles[1]["id"] = profiles[0]["id"];
    EXPECT_EQ(ValidateTranslationInterfaces(profiles), TranslationError::InvalidConfiguration);
    profiles = nlohmann::json::array();
    for (int i = 0; i < 17; ++i)
        profiles.push_back(TestTranslationProfile("google"));
    EXPECT_EQ(ValidateTranslationInterfaces(profiles), TranslationError::InvalidConfiguration);
    profiles = TestDefaultSettings().at("translation.interfaces");
    profiles[0]["unknown"] = "unused";
    EXPECT_EQ(ValidateTranslationInterfaces(profiles), TranslationError::InvalidConfiguration);
}
// 验证配置必须来自完整快照，缺失和非法值不得触发代码默认回退。
// 入参：无。返回：断言结果。
TEST(TranslationSchema, SettingsCaptureRejectsMissingAndInvalidFields)
{
    EXPECT_THROW(ReadTranslationSettings({}), std::invalid_argument);
    const nlohmann::json defaults = TestDefaultSettings();
    for (const std::string_view key : TranslationSettingKeys())
    {
        for (int mode = 0; mode < 2; ++mode)
        {
            nlohmann::json values = defaults;
            if (mode == 0)
                values.erase(std::string(key));
            else
                values[key] = 42;
            EXPECT_THROW(ReadTranslationSettings(
                             [&values](std::string_view field) -> std::optional<nlohmann::json>
                             {
                                 const auto found = values.find(field);
                                 return found == values.end() ? std::nullopt : std::optional<nlohmann::json>(*found);
                             }),
                         std::invalid_argument);
        }
    }
    EXPECT_EQ(TranslationSettingKeys().size(), 6U);
}
// 验证资源默认值决定新条目及语言代理，新增条目不复用身份或秘密。
// 入参：无。返回：断言结果。
TEST(TranslationSchema, ConfiguredDefaultsAreTheOnlySource)
{
    nlohmann::json values = TestDefaultSettings();
    for (nlohmann::json& profile : values["translation.interfaces"])
    {
        if (profile["kind"] != "openai")
            continue;
        profile["configuration"]["base_url"] = "https://fixture.invalid/v2";
        profile["configuration"]["model"] = "configured-model";
        profile["name"] = "configured-name";
        profile["secrets"]["api_key"] = "must-not-copy";
        profile["enabled"] = true;
        const nlohmann::json created = CreateTranslationProfile("openai", values["translation.interfaces"]);
        EXPECT_EQ(created["configuration"], profile["configuration"]);
        EXPECT_EQ(created["name"], profile["name"]);
        EXPECT_NE(created["id"], profile["id"]);
        EXPECT_FALSE(created["enabled"].get<bool>());
        EXPECT_TRUE(created["secrets"]["api_key"].get<std::string>().empty());
    }
    values["translation.source_language"] = "ja";
    values["translation.target_language"] = "en";
    values["translation.network.proxy_mode"] = "custom";
    values["translation.network.proxy_address"] = "localhost:3456";
    const TranslationSettings settings = ReadTranslationSettings(
        [&values](std::string_view key) -> std::optional<nlohmann::json> { return values.at(key); });
    EXPECT_EQ(settings.options.sourceLanguage, "ja");
    EXPECT_EQ(settings.options.targetLanguage, "en");
    EXPECT_EQ(settings.configuration.proxyMode, "custom");
    EXPECT_EQ(settings.configuration.proxyAddress, "localhost:3456");
    EXPECT_THROW(CreateTranslationProfile("openai", nlohmann::json::array()), std::invalid_argument);
    EXPECT_THROW(CreateTranslationProfile("unknown", values["translation.interfaces"]), std::invalid_argument);
}
// 验证秘密空值可保存，而控制字符和恶意代理地址不可保存。
// 入参：无。
// 返回：断言结果。
TEST(TranslationSchema, AllowsEmptyCredentialsButRejectsMalformedValues)
{
    EXPECT_EQ(ValidateTranslationProfile(TestTranslationProfile("baidu")), TranslationError::None);
    EXPECT_EQ(ValidateTranslationField("translation.network.proxy_address", "http://u:p@localhost:1"),
              TranslationError::InvalidConfiguration);
    EXPECT_EQ(ValidateTranslationField("translation.network.proxy_address", "localhost:7897"), TranslationError::None);
    EXPECT_EQ(ValidateTranslationField("translation.network.proxy_mode", "direct"),
              TranslationError::InvalidConfiguration);
    nlohmann::json local = TestTranslationProfile("ctranslate2_local");
    local["configuration"]["pack_id"] = "elsewhere";
    EXPECT_EQ(ValidateTranslationProfile(local), TranslationError::InvalidConfiguration);
}
// 验证档位数值与显示顺序均来自配置，而不是代码默认映射。
// 入参：无。返回：断言结果。
TEST(TranslationSchema, LocalQualityUsesConfiguredCatalog)
{
    nlohmann::json presets = TestDefaultSettings().at("translation.local_quality_presets");
    nlohmann::json profile = TestTranslationProfile("ctranslate2_local");
    ASSERT_EQ(ValidateLocalQualityPresets(presets), TranslationError::None);
    std::reverse(presets.begin(), presets.end());
    for (nlohmann::json& preset : presets)
        preset["beam_size"] = 7;
    const std::vector<std::string> choices = LocalQualityChoices(presets);
    ASSERT_EQ(choices.size(), presets.size());
    for (std::size_t i = 0; i < presets.size(); ++i)
    {
        EXPECT_EQ(choices[i], presets[i]["id"].get<std::string>());
        profile["configuration"]["quality"] = choices[i];
        std::size_t beam{};
        EXPECT_EQ(ResolveLocalQuality(presets, profile, beam), TranslationError::None);
        EXPECT_EQ(beam, 7U);
    }
    nlohmann::json settings = TestDefaultSettings();
    settings["translation.local_quality_presets"] = presets;
    const TranslationSettings snapshot = ReadTranslationSettings(
        [&settings](std::string_view key) -> std::optional<nlohmann::json> { return settings.at(key); });
    settings["translation.local_quality_presets"][0]["beam_size"] = 1;
    EXPECT_EQ(snapshot.configuration.localQualityPresets, presets);
}
// 验证完整目录在任何条目非法时整体拒绝，不能使用部分可解析的档位。
// 入参：无。返回：断言结果。
TEST(TranslationSchema, LocalQualityRejectsMalformedCatalog)
{
    const nlohmann::json defaults = TestDefaultSettings().at("translation.local_quality_presets");
    std::vector<nlohmann::json> invalid{nullptr, nlohmann::json::array(), nlohmann::json::object()};
    for (const nlohmann::json& value :
         {nlohmann::json(0), nlohmann::json(-1), nlohmann::json(9), nlohmann::json(1.5), nlohmann::json(true),
          nlohmann::json("2"), nlohmann::json(std::numeric_limits<std::uint64_t>::max())})
    {
        nlohmann::json presets = defaults;
        presets[0]["beam_size"] = value;
        invalid.push_back(presets);
    }
    nlohmann::json presets = defaults;
    presets[1]["id"] = presets[0]["id"];
    invalid.push_back(presets);
    presets = defaults;
    presets[0]["id"] = "unknown";
    invalid.push_back(presets);
    presets = defaults;
    presets[0]["extra"] = 1;
    invalid.push_back(presets);
    presets = defaults;
    presets[0].erase("beam_size");
    invalid.push_back(presets);
    const nlohmann::json profile = TestTranslationProfile("ctranslate2_local");
    for (const nlohmann::json& bad : invalid)
    {
        std::size_t beam = 42;
        EXPECT_EQ(ValidateLocalQualityPresets(bad), TranslationError::InvalidConfiguration);
        EXPECT_THROW(LocalQualityChoices(bad), std::invalid_argument);
        EXPECT_EQ(ResolveLocalQuality(bad, profile, beam), TranslationError::InvalidConfiguration);
        EXPECT_EQ(beam, 42U);
    }
}
// 验证旧配置须先由设置迁移；执行层不猜测缺失或未知档位。
// 入参：无。返回：断言结果。
TEST(TranslationSchema, LocalQualityRequiresExplicitValidSelection)
{
    const nlohmann::json presets = TestDefaultSettings().at("translation.local_quality_presets");
    const nlohmann::json original = TestTranslationProfile("ctranslate2_local");
    for (const nlohmann::json& value :
         {nlohmann::json("unknown"), nlohmann::json(""), nlohmann::json(2), nlohmann::json(nullptr)})
    {
        nlohmann::json profile = original;
        profile["configuration"]["quality"] = value;
        std::size_t beam = 42;
        EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
        EXPECT_EQ(ResolveLocalQuality(presets, profile, beam), TranslationError::InvalidConfiguration);
        EXPECT_EQ(beam, 42U);
    }
    nlohmann::json missing = original;
    missing["configuration"].erase("quality");
    EXPECT_EQ(ValidateTranslationProfile(missing), TranslationError::InvalidConfiguration);
    std::size_t beam{};
    EXPECT_EQ(ResolveLocalQuality(presets, TestTranslationProfile("google"), beam),
              TranslationError::InvalidConfiguration);
}
// 验证 JSON 变量只替换字符串值，Unicode/引号换行保持完整数据。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, SerializesJsonValuesWithoutInjection)
{
    nlohmann::json profile = Custom();
    http::Request request;
    const TranslationRequest input = Text();
    ASSERT_EQ(translation_detail::BuildRequest(input, profile, "salt", request), TranslationError::None);
    const nlohmann::json body = nlohmann::json::parse(request.body);
    EXPECT_EQ(body["text"], input.text);
    EXPECT_EQ(body["source"], "auto");
    EXPECT_EQ(body["target"], "zh");
    EXPECT_EQ(request.method, http::Method::Post);
}
// 验证四种方法与 query/form 的一次百分号编码，GET 不能携带正文。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, MethodsAndEncoding)
{
    nlohmann::json profile = Custom();
    nlohmann::json& config = profile["configuration"];
    config["method"] = "GET";
    config["body_mode"] = "none";
    config["body"] = nullptr;
    config["query"] = {{"q", "{{text}}"}, {"from", "{{source}}"}, {"to", "{{target}}"}};
    http::Request request;
    ASSERT_EQ(translation_detail::BuildRequest(Text(), profile, "salt", request), TranslationError::None);
    EXPECT_EQ(request.method, http::Method::Get);
    EXPECT_TRUE(request.body.empty());
    EXPECT_NE(request.url.find(L"%26%2B%25"), std::wstring::npos);
    config["body_mode"] = "raw";
    config["body"] = "{{text}}";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    config["query"] = nlohmann::json::object();
    config["body_mode"] = "form";
    config["body"] = {{"q", "{{text}}"}, {"from", "{{source}}"}, {"to", "{{target}}"}};
    for (const std::string method : {"POST", "PUT", "PATCH"})
    {
        config["method"] = method;
        ASSERT_EQ(translation_detail::BuildRequest(Text(), profile, "salt", request), TranslationError::None);
        EXPECT_NE(request.body.find("%26%2B%25"), std::string::npos);
        EXPECT_EQ(request.method, method == "PUT"     ? http::Method::Put
                                  : method == "PATCH" ? http::Method::Patch
                                                      : http::Method::Post);
    }
}
// 验证模板中的未知变量、隐藏主机替换、虚假语言能力声明都被拒绝。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, RejectsUndeclaredVariablesAndFalseLanguageCapabilities)
{
    nlohmann::json profile = Custom();
    profile["configuration"]["body"]["text"] = "{{secret.missing}}";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    profile = Custom();
    profile["configuration"]["url"] = "https://{{text}}/";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    profile = Custom();
    profile["configuration"]["body"]["target"] = "zh";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    profile = Custom();
    profile["configuration"]["headers"]["Host"] = "evil.invalid";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
}
// 验证凭据缺失和语言能力不足在发送前跳过，不把合法原文当作全局错误。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, SkipsMissingSecretsAndUnsupportedLanguages)
{
    nlohmann::json profile = TestTranslationProfile("custom_http");
    http::Request request;
    EXPECT_EQ(translation_detail::BuildRequest(Text(), profile, "salt", request), TranslationError::MissingCredentials);
    profile = Custom();
    profile["configuration"]["source_languages"] = {{"en", "en"}};
    EXPECT_EQ(translation_detail::BuildRequest(Text(), profile, "salt", request), TranslationError::NotApplicable);
}
// 验证变量展开后的 URL/头/正文预算，不截断或发送部分请求。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, AppliesBudgetsAfterExpansionAndEncoding)
{
    nlohmann::json profile = Custom();
    TranslationRequest input = Text();
    input.text.assign(9000, '&');
    profile["configuration"]["query"] = {{"q", "{{text}}"}};
    http::Request request;
    EXPECT_EQ(translation_detail::BuildRequest(input, profile, "salt", request), TranslationError::InputTooLarge);
    profile = Custom();
    profile["configuration"]["body"]["copies"] = nlohmann::json::array(
        {"{{text}}", "{{text}}", "{{text}}", "{{text}}", "{{text}}", "{{text}}", "{{text}}", "{{text}}"});
    EXPECT_EQ(translation_detail::BuildRequest(input, profile, "salt", request), TranslationError::InputTooLarge);
    profile = Custom();
    profile["configuration"]["headers"]["X-Text"] = "{{text}}";
    input.text = "a\r\nb";
    EXPECT_EQ(translation_detail::BuildRequest(input, profile, "salt", request), TranslationError::NotApplicable);
}
// 验证深度、静态值长度和路径语法都有界。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, RejectsStaticComplexityAndInvalidPointers)
{
    nlohmann::json profile = Custom();
    profile["configuration"]["response"]["text_pointer"] = "/bad~escape";
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    profile = Custom();
    profile["configuration"]["body"]["huge"] = std::string(4097, 'a');
    EXPECT_EQ(ValidateTranslationProfile(profile), TranslationError::InvalidConfiguration);
    EXPECT_THROW(translation_detail::ParseJson(std::string(40, '[') + "0" + std::string(40, ']')), std::exception);
}
// 验证数组字段按顺序整段组合，任何成员错误时保留已有译文。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, MapsArraysAtomically)
{
    nlohmann::json profile = Custom();
    profile["configuration"]["response"] = {
        {"mode", "json"}, {"array_pointer", "/items"}, {"item_pointer", "/text"}, {"separator", "\n"}};
    http::Result response;
    response.status = 200;
    TranslationResult output;
    ASSERT_EQ(translation_detail::ParseCustomResponse(profile, response, R"({"items":[{"text":"一"},{"text":"二"}]})",
                                                      output),
              TranslationError::None);
    EXPECT_EQ(output.text, "一\n二");
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response,
                                                      R"({"items":[{"text":"partial"},{"text":42}]})", output),
              TranslationError::UnsupportedResponse);
    EXPECT_EQ(output.text, "一\n二");
}
// 验证显式业务错误先于 HTTP400 分类，未知400则停止，200失败不能冒充成功。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, ErrorMappingPrecedesHttpFallback)
{
    nlohmann::json profile = Custom();
    nlohmann::json& mapping = profile["configuration"]["response"];
    mapping["success"] = {{"pointer", "/ok"}, {"equals", true}};
    mapping["error_pointer"] = "/code";
    mapping["error_map"] = {{"quota", "quota"}, {"input", "input"}};
    http::Result response;
    response.status = 400;
    TranslationResult result;
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, R"({"code":"quota"})", result),
              TranslationError::Quota);
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, R"({"code":"input"})", result),
              TranslationError::InvalidInput);
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "{}", result), TranslationError::InvalidInput);
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, R"({"code":"unrecognized"})", result),
              TranslationError::InvalidInput);
    response.status = 200;
    EXPECT_EQ(
        translation_detail::ParseCustomResponse(profile, response, R"({"ok":false,"translation":"fake"})", result),
        TranslationError::Service);
    response.status = 500;
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, R"({"ok":true,"translation":"fake"})", result),
              TranslationError::Service);
}
// 验证未识别业务错误不能伪装成功；共用状态字段必须明确声明成功值。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, UnknownBusinessErrorsCannotPublishSuccessText)
{
    nlohmann::json profile = Custom();
    nlohmann::json& mapping = profile["configuration"]["response"];
    mapping["error_pointer"] = "/code";
    mapping["error_map"] = {{"quota", "quota"}};
    http::Result response;
    response.status = 200;
    TranslationResult result;
    result.text = "previous";
    EXPECT_EQ(translation_detail::ParseCustomResponse(
                  profile, response, R"({"code":"unrecognized","translation":"server error"})", result),
              TranslationError::Service);
    EXPECT_EQ(translation_detail::ParseCustomResponse(
                  profile, response, R"({"code":{"message":"unexpected"},"translation":"server error"})", result),
              TranslationError::Service);
    EXPECT_EQ(result.text, "previous");
    EXPECT_EQ(
        translation_detail::ParseCustomResponse(profile, response, R"({"code":null,"translation":"complete"})", result),
        TranslationError::None);
    mapping["success"] = {{"pointer", "/code"}, {"equals", 0}};
    EXPECT_EQ(
        translation_detail::ParseCustomResponse(profile, response, R"({"code":0,"translation":"complete"})", result),
        TranslationError::None);
    EXPECT_EQ(result.text, "complete");
}
// 验证纯文本拒绝 HTML、空白、非法编码且不渲染内容。
// 入参：无。
// 返回：断言结果。
TEST(TranslationCustom, TextRejectsHtmlAndInvalidEncoding)
{
    nlohmann::json profile = Custom();
    profile["configuration"]["response"] = {{"mode", "text"}};
    http::Result response;
    response.status = 200;
    TranslationResult result;
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "\n<!DOCTYPE html><html>error</html>", result),
              TranslationError::UnsupportedResponse);
    response.contentType = L"text/html; charset=utf-8";
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "error", result),
              TranslationError::UnsupportedResponse);
    response.contentType = L"text/plain; charset=utf-8";
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "\xc0\xaf", result),
              TranslationError::UnsupportedResponse);
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "  \n", result),
              TranslationError::EmptyResult);
    EXPECT_EQ(translation_detail::ParseCustomResponse(profile, response, "你好", result), TranslationError::None);
}
} // namespace open_st
