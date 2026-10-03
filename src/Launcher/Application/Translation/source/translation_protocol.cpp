// 实现五种单次翻译协议，仅接受和生成有界文本，不拥有重试或供应商切换策略。
#include <Windows.h>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <sha256.h>
#include <translation_protocol.h>

namespace open_st::translation_detail
{
namespace
{
// 将规范语种映射至供应商契约，不将 UI 语言码直接发给不同协议。
// 入参：provider 为接口，language 为领域语种，target 区分源与目标。
// 返回：协议语种值。
std::string Language(std::string_view provider, std::string_view language, bool target)
{
    if (language == "auto")
        return "auto";
    if (provider == "baidu")
        return language == "zh-CN" ? "zh" : language == "ja" ? "jp" : "en";
    if (provider == "deepl")
        return language == "zh-CN" ? (target ? "ZH-HANS" : "ZH") : language == "ja" ? "JA" : "EN";
    if (provider == "microsoft" && language == "zh-CN")
        return "zh-Hans";
    return std::string(language);
}
// 只展示领域支持的检测语种，未知或缺失保持空值。
// 入参：language 为服务器检测语种。
// 返回：规范语种或空。
std::string Detected(std::string_view language)
{
    if (language == "zh" || language == "ZH" || language == "zh-CN" || language == "zh-Hans" || language == "ZH-HANS")
        return "zh-CN";
    if (language == "en" || language == "EN")
        return "en";
    if (language == "ja" || language == "JA" || language == "jp")
        return "ja";
    return {};
}
// 对百度编码前的原文签名，摘要固定小写，不将原密钥发送至服务端。
// 入参：input 为请求，salt 为盐。
// 返回：摘要字符串；系统失败为空。
std::string BaiduSign(const TranslationRequest& input, const nlohmann::json& profile, std::string_view salt)
{
    const std::string plain = profile.at("configuration").at("app_id").get<std::string>() + input.text +
                              std::string(salt) + profile.at("secrets").at("api_key").get<std::string>();
    std::array<std::byte, 16> digest{};
    if (!ComputeMd5(std::as_bytes(std::span(plain.data(), plain.size())), digest))
        return {};
    constexpr char HEX[] = "0123456789abcdef";
    std::string sign;
    for (const std::byte byte : digest)
    {
        const unsigned value = std::to_integer<unsigned>(byte);
        sign.push_back(HEX[value >> 4]);
        sign.push_back(HEX[value & 15]);
    }
    return sign;
}
// 从 JSON 中读取字符串字段，不对任意值做隐式转换。
// 入参：value 为对象，key 为字段。
// 返回：字符串值或空串。
std::string String(const nlohmann::json& value, std::string_view key)
{
    if (!value.is_object())
        return {};
    const auto found = value.find(std::string(key));
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
// 将百度 HTTP 200 中的业务错误映射为稳定类别，不展示回显正文。
// 入参：code 为协议错误号。
// 返回：分类错误。
TranslationError BaiduError(std::string_view code)
{
    if (code == "52003" || code == "54001")
        return TranslationError::Authentication;
    if (code == "54003" || code == "54005")
        return TranslationError::RateLimited;
    if (code == "54004")
        return TranslationError::Quota;
    if (code == "58000" || code == "58002" || code == "90107")
        return TranslationError::Permission;
    if (code == "52001")
        return TranslationError::Timeout;
    if (code == "54000" || code == "58001")
        return TranslationError::InvalidInput;
    return TranslationError::Service;
}
} // namespace
// 在单次请求边界落实语种、码点、字节和头部预算，不执行网络。
// 入参：input 为完整接口参数，salt 为随机盐，output 接收构造结果。
// 返回：成功为 None，失败不改变 output。
TranslationError BuildRequest(const TranslationRequest& input, const nlohmann::json& profile, std::string_view salt,
                              http::Request& output)
{
    const TranslationError valid = ValidateTranslationProfile(profile);
    if (valid != TranslationError::None)
        return valid;
    std::size_t count = 0;
    if (!CountText(input.text, count) || count == 0)
        return TranslationError::InvalidInput;
    const std::string provider = profile.at("kind").get<std::string>();
    if (provider == "custom_http")
        return BuildCustomRequest(input, profile, output);
    if (provider == "ctranslate2_local")
        return TranslationError::NotApplicable;
    const nlohmann::json& parameters = profile.at("configuration");
    const nlohmann::json& secrets = profile.at("secrets");
    const std::string key = secrets.value("api_key", std::string{});
    if ((provider != "google" && key.empty()) ||
        (provider == "baidu" && parameters.at("app_id").get<std::string>().empty()) ||
        (provider == "openai" && parameters.at("model").get<std::string>().empty()))
        return TranslationError::MissingCredentials;
    const std::size_t maximum = provider == "baidu" ? 1000U : provider == "google" ? 2000U : 10000U;
    if (count > maximum)
        return TranslationError::InputTooLarge;
    const std::string source = Language(provider, input.options.sourceLanguage, false);
    const std::string target = Language(provider, input.options.targetLanguage, true);
    const TranslationConfiguration& config = input.configuration;
    http::Request request;
    request.method = http::Method::Post;
    request.maxResponseBytes = 1024U * 1024U;
    request.maxErrorBytes = 16U * 1024U;
    request.proxy.mode = config.proxyMode == "custom" ? http::ProxyMode::Custom : http::ProxyMode::System;
    if (request.proxy.mode == http::ProxyMode::Custom)
        request.proxy.address = Wide(config.proxyAddress);
    request.headers.push_back({L"Content-Type", L"application/json; charset=utf-8"});
    nlohmann::json body;
    if (provider == "baidu")
    {
        const std::string sign = BaiduSign(input, profile, salt);
        if (salt.empty() || sign.empty())
            return TranslationError::Unavailable;
        request.url = L"https://fanyi-api.baidu.com/api/trans/vip/translate";
        request.headers[0].value = L"application/x-www-form-urlencoded; charset=utf-8";
        request.body = "q=" + Encode(input.text) + "&from=" + source + "&to=" + target +
                       "&appid=" + Encode(parameters.at("app_id").get<std::string>()) + "&salt=" + Encode(salt) +
                       "&sign=" + sign;
    }
    else if (provider == "google")
    {
        request.url =
            Wide("https://translate.googleapis.com/translate_a/single?client=gtx&dt=t&sl=" + source + "&tl=" + target);
        request.headers[0].value = L"application/x-www-form-urlencoded; charset=utf-8";
        request.body = "q=" + Encode(input.text);
    }
    else if (provider == "openai")
    {
        std::string root = parameters.at("base_url").get<std::string>();
        while (root.ends_with('/'))
            root.pop_back();
        request.url = Wide(root + "/chat/completions");
        request.allowHttp = root.starts_with("http://");
        request.headers.push_back({L"Authorization", Wide("Bearer " + key)});
        const std::string instruction = "Translate the user text " +
                                        (source == "auto" ? std::string{} : "from " + source + " ") + "into " + target +
                                        ". Preserve line breaks. Return only the translation.";
        body = {{"model", parameters.at("model").get<std::string>()},
                {"stream", false},
                {"messages", nlohmann::json::array({{{"role", "system"}, {"content", instruction}},
                                                    {{"role", "user"}, {"content", input.text}}})}};
    }
    else if (provider == "deepl")
    {
        request.url = parameters.at("service").get<std::string>() == "free" ? L"https://api-free.deepl.com/v2/translate"
                                                                            : L"https://api.deepl.com/v2/translate";
        request.headers.push_back({L"Authorization", Wide("DeepL-Auth-Key " + key)});
        body = {{"text", nlohmann::json::array({input.text})}, {"target_lang", target}};
        if (source != "auto")
            body["source_lang"] = source;
    }
    else
    {
        request.url = Wide("https://api.cognitive.microsofttranslator.com/translate?api-version=3.0&to=" + target +
                           (source == "auto" ? std::string{} : "&from=" + source));
        request.headers.push_back({L"Ocp-Apim-Subscription-Key", Wide(key)});
        if (!parameters.at("region").get<std::string>().empty())
            request.headers.push_back(
                {L"Ocp-Apim-Subscription-Region", Wide(parameters.at("region").get<std::string>())});
        body = nlohmann::json::array({{{"Text", input.text}}});
    }
    if (!body.is_null())
        request.body = body.dump();
    const TranslationError budget = RequestBudget(request);
    if (budget != TranslationError::None)
        return budget;
    output = std::move(request);
    return TranslationError::None;
}
// 对各供应商结构严格取出纯文本，任何错误与截断都不发布部分译文。
// 入参：provider 为接口，status 为 HTTP 状态，body 为有界 JSON，output 接收成功结果。
// 返回：成功为 None。
TranslationError ParseResponse(std::string_view provider, unsigned status, std::string_view body,
                               TranslationResult& output)
{
    if (body.size() > (status >= 200 && status < 300 ? 1024U * 1024U : 16U * 1024U))
        return TranslationError::UnsupportedResponse;
    try
    {
        const nlohmann::json json = ParseJson(body);
        if (provider == "baidu" && json.contains("error_code"))
        {
            const nlohmann::json& code = json.at("error_code");
            if (!code.is_string() && !code.is_number_integer())
                return TranslationError::UnsupportedResponse;
            const std::string value = code.is_string() ? code.get<std::string>() : code.dump();
            if (value != "52000")
                return BaiduError(value);
        }
        if (provider == "openai" && json.contains("error"))
        {
            const std::string code = String(json.at("error"), "code");
            if (code == "context_length_exceeded")
                return TranslationError::ContextLimit;
            if (code == "insufficient_quota")
                return TranslationError::Quota;
            if (code == "invalid_api_key" || code == "invalid_authentication")
                return TranslationError::Authentication;
            if (code == "rate_limit_exceeded")
                return TranslationError::RateLimited;
            if (code == "permission_denied")
                return TranslationError::Permission;
            if (code == "invalid_request_error")
                return TranslationError::InvalidInput;
            const TranslationError statusError = HttpStatusError(status);
            return statusError == TranslationError::None ? TranslationError::Service : statusError;
        }
        if (provider == "microsoft" && json.contains("error"))
        {
            const nlohmann::json& error = json.at("error");
            const std::string code =
                error.contains("code")
                    ? (error.at("code").is_string() ? error.at("code").get<std::string>() : error.at("code").dump())
                    : std::string{};
            if (code.starts_with("401"))
                return TranslationError::Authentication;
            if (code.starts_with("403"))
                return TranslationError::Quota;
            if (code.starts_with("429"))
                return TranslationError::RateLimited;
            if (code == "400050" || code == "400077")
                return TranslationError::ContextLimit;
            if (code.starts_with("400"))
                return TranslationError::InvalidInput;
            if (code.starts_with("500"))
                return TranslationError::Service;
        }
        const TranslationError statusError = HttpStatusError(status);
        if (statusError != TranslationError::None)
            return statusError;
        TranslationResult candidate;
        if (provider == "baidu")
        {
            const nlohmann::json& lines = json.at("trans_result");
            if (!lines.is_array() || lines.size() > 1024)
                return TranslationError::UnsupportedResponse;
            for (const nlohmann::json& line : lines)
            {
                if (!line.at("dst").is_string())
                    return TranslationError::UnsupportedResponse;
                if (!candidate.text.empty())
                    candidate.text += '\n';
                candidate.text += line.at("dst").get<std::string>();
            }
            candidate.detectedLanguage = Detected(String(json, "from"));
        }
        else if (provider == "google")
        {
            if (!json.is_array() || json.empty() || !json.at(0).is_array() || json.at(0).size() > 1024)
                return TranslationError::UnsupportedResponse;
            for (const nlohmann::json& segment : json.at(0))
            {
                if (!segment.is_array() || segment.empty() || !segment.at(0).is_string())
                    return TranslationError::UnsupportedResponse;
                candidate.text += segment.at(0).get<std::string>();
            }
            if (json.size() > 2 && json.at(2).is_string())
                candidate.detectedLanguage = Detected(json.at(2).get<std::string>());
        }
        else if (provider == "openai")
        {
            const nlohmann::json& choices = json.at("choices");
            if (!choices.is_array() || choices.empty())
                return TranslationError::UnsupportedResponse;
            const nlohmann::json& choice = choices.at(0);
            const std::string finish = String(choice, "finish_reason");
            if (finish == "length")
                return TranslationError::Truncated;
            if (finish != "stop")
                return TranslationError::UnsupportedResponse;
            const nlohmann::json& message = choice.at("message");
            if (!String(message, "refusal").empty())
                return TranslationError::Permission;
            if (!message.at("content").is_string())
                return TranslationError::UnsupportedResponse;
            candidate.text = message.at("content").get<std::string>();
        }
        else if (provider == "deepl")
        {
            const nlohmann::json& translations = json.at("translations");
            if (!translations.is_array() || translations.size() != 1)
                return TranslationError::UnsupportedResponse;
            const nlohmann::json& result = translations.at(0);
            candidate.text = result.at("text").get<std::string>();
            candidate.detectedLanguage = Detected(String(result, "detected_source_language"));
        }
        else if (provider == "microsoft")
        {
            if (!json.is_array() || json.size() != 1)
                return TranslationError::UnsupportedResponse;
            const nlohmann::json& result = json.at(0);
            const nlohmann::json& translations = result.at("translations");
            if (!translations.is_array() || translations.size() != 1)
                return TranslationError::UnsupportedResponse;
            candidate.text = translations.at(0).at("text").get<std::string>();
            if (result.contains("detectedLanguage"))
                candidate.detectedLanguage = Detected(String(result.at("detectedLanguage"), "language"));
        }
        else
            return TranslationError::InvalidConfiguration;
        const TranslationError valid = ValidateResultText(candidate.text);
        if (valid != TranslationError::None)
            return valid;
        output.text = std::move(candidate.text);
        output.detectedLanguage = std::move(candidate.detectedLanguage);
        return TranslationError::None;
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        const TranslationError statusError = HttpStatusError(status);
        return statusError == TranslationError::None ? TranslationError::UnsupportedResponse : statusError;
    }
}
} // namespace open_st::translation_detail
