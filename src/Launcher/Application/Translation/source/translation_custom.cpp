// 实现静态 HTTP 模板和响应字段映射，不执行脚本、认证跳转或任何自动重试。
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <translation_protocol.h>

namespace open_st::translation_detail
{
namespace
{
// 校验有限单行值，Unicode 码点数独立于 UTF-8 字节。
// 入参：value 为字符串，maximum 为码点上限。
// 返回：无控制字符且有界时 true。
bool Plain(std::string_view value, std::size_t maximum)
{
    std::size_t count{};
    return CountText(value, count) && count <= maximum &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// 验证精确对象形状，不接受不会被消费的配置字段。
// 入参：object 为候选，keys 为允许字段。
// 返回：每个字段都合法为 true。
bool Keys(const nlohmann::json& object, std::initializer_list<std::string_view> keys)
{
    if (!object.is_object())
        return false;
    for (const auto& entry : object.items())
        if (std::find(keys.begin(), keys.end(), entry.key()) == keys.end())
            return false;
    return true;
}
// 校验 JSON Pointer 长度与语法，不要求响应此时存在。
// 入参：pointer 为路径字符串。
// 返回：语法合法为 true。
bool Pointer(const nlohmann::json& pointer)
{
    if (!pointer.is_string() || pointer.get_ref<const std::string&>().size() > 256)
        return false;
    const nlohmann::json::json_pointer parsed(pointer.get<std::string>());
    return parsed.to_string() == pointer.get<std::string>();
}
// 将稳定错误名称映射为领域分类，防止上游错误文本进入界面。
// 入参：name 为映射配置值。
// 返回：未知时 None，用于校验拒绝。
TranslationError MappedError(std::string_view name)
{
    if (name == "authentication")
        return TranslationError::Authentication;
    if (name == "permission")
        return TranslationError::Permission;
    if (name == "quota")
        return TranslationError::Quota;
    if (name == "rate_limited")
        return TranslationError::RateLimited;
    if (name == "input")
        return TranslationError::InvalidInput;
    if (name == "service")
        return TranslationError::Service;
    if (name == "context_limit")
        return TranslationError::ContextLimit;
    return TranslationError::None;
}
// 扫描并可选展开明确占位符，不让原文改变主机、字段名或结构。
// 入参：text 为模板，profile 为秘密名目录，variables 收集用过的变量，values 为运行期值或空。
// 返回：静态验证或完整展开值；非法占位符抛异常。
std::string Expand(std::string_view text, const nlohmann::json& profile, std::set<std::string>& variables,
                   const nlohmann::json* values = nullptr)
{
    std::string result;
    std::size_t cursor{};
    while (cursor < text.size())
    {
        const std::size_t begin = text.find("{{", cursor);
        const std::size_t stray = text.find("}}", cursor);
        if (stray != std::string_view::npos && (begin == std::string_view::npos || stray < begin))
            throw std::invalid_argument("template delimiter");
        if (begin == std::string_view::npos)
        {
            result += text.substr(cursor);
            break;
        }
        result += text.substr(cursor, begin - cursor);
        const std::size_t end = text.find("}}", begin + 2);
        if (end == std::string_view::npos)
            throw std::invalid_argument("template delimiter");
        const std::string name(text.substr(begin + 2, end - begin - 2));
        if (name != "text" && name != "source" && name != "target" &&
            !(name.starts_with("secret.") && profile.at("secrets").contains(name.substr(7))))
            throw std::invalid_argument("template variable");
        variables.insert(name);
        if (values)
        {
            if (name.starts_with("secret."))
                result += profile.at("secrets").at(name.substr(7)).get<std::string>();
            else
                result += values->at(name).get<std::string>();
        }
        else
            result += 'x';
        if (result.size() > 65536)
            throw std::length_error("template expansion");
        cursor = end + 2;
    }
    return result;
}
// 遍历 JSON 字符串值，禁止对象键中藏入模板。
// 入参：node 为有界树，profile 为条目，variables 为变量集合，values 为运行值。
// 返回：展开后的同形树。
nlohmann::json ExpandJson(const nlohmann::json& node, const nlohmann::json& profile, std::set<std::string>& variables,
                          const nlohmann::json* values = nullptr, std::size_t* used = nullptr)
{
    std::size_t localUsed{};
    if (!used)
        used = &localUsed;
    if (node.is_string())
    {
        std::string expanded = Expand(node.get_ref<const std::string&>(), profile, variables, values);
        *used += expanded.size();
        if (*used > 65536)
            throw std::length_error("json template expansion");
        return expanded;
    }
    nlohmann::json result = node;
    if (node.is_array())
        for (std::size_t index = 0; index < node.size(); ++index)
            result[index] = ExpandJson(node[index], profile, variables, values, used);
    else if (node.is_object())
        for (const auto& entry : node.items())
        {
            if (entry.key().find("{{") != std::string::npos || entry.key().find("}}") != std::string::npos)
                throw std::invalid_argument("template key");
            result[entry.key()] = ExpandJson(entry.value(), profile, variables, values, used);
        }
    return result;
}
// 校验查询或头部的键值表，并收集模板实际引用。
// 入参：object 为键值表，profile 为条目，variables 为引用集合，header 表示请求头。
// 返回：有效时 true。
bool KeyValues(const nlohmann::json& object, const nlohmann::json& profile, std::set<std::string>& variables,
               bool header)
{
    if (!object.is_object() || object.size() > 32)
        return false;
    http::Request request;
    request.url = L"https://example.invalid/";
    std::set<std::wstring> headerNames;
    for (const auto& entry : object.items())
    {
        if (entry.key().empty() || !Plain(entry.key(), 256) || entry.key().find('{') != std::string::npos ||
            entry.key().find('}') != std::string::npos || !entry.value().is_string())
            return false;
        const std::string expanded = Expand(entry.value().get_ref<const std::string&>(), profile, variables);
        if (header)
        {
            std::wstring name = Wide(entry.key());
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (!headerNames.insert(name).second)
                return false;
            request.headers.push_back({Wide(entry.key()), Wide(expanded)});
        }
    }
    return !header || http::IsValidRequest(request);
}
// 校验声明的语种映射，不允许目标 auto 或未知语种。
// 入参：mapping 为语言字典，source 区分源/目标。
// 返回：有效且非空时 true。
bool Languages(const nlohmann::json& mapping, bool source)
{
    if (!mapping.is_object() || mapping.empty() || mapping.size() > 4)
        return false;
    const std::span<const std::string_view> choices =
        TranslationChoices(source ? "translation.source_language" : "translation.target_language");
    for (const auto& entry : mapping.items())
        if (std::find(choices.begin(), choices.end(), entry.key()) == choices.end() || !entry.value().is_string() ||
            entry.value().get_ref<const std::string&>().empty() ||
            !Plain(entry.value().get_ref<const std::string&>(), 64))
            return false;
    return true;
}
// 按 JSON Pointer 定位，缺失使用 nullptr，不借助表达式执行。
// 入参：object 为响应树，pointer 为已验证路径。
// 返回：借用字段或空指针。
const nlohmann::json* At(const nlohmann::json& object, std::string_view pointer)
{
    const nlohmann::json::json_pointer path{std::string(pointer)};
    return object.contains(path) ? &object.at(path) : nullptr;
}
// 保守拒绝 HTML 内容，即使被配置成纯文本响应也不发布网页。
// 入参：type 为响应 Content-Type；body 为原文。
// 返回：看似 HTML 时 true。
bool Html(std::wstring type, std::string_view body)
{
    std::transform(type.begin(), type.end(), type.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (type.find(L"text/html") != std::wstring::npos || type.find(L"application/xhtml+xml") != std::wstring::npos)
        return true;
    std::string prefix(body.substr(0, 256));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::size_t begin = prefix.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if (begin == std::string::npos)
        return false;
    const std::string_view trimmed(prefix.data() + begin, prefix.size() - begin);
    return trimmed.starts_with("<!doctype html") || trimmed.starts_with("<html") || trimmed.starts_with("<head") ||
           trimmed.starts_with("<body") || trimmed.starts_with("<?xml");
}
} // namespace
// 提供能表达所有语言参数的 JSON 模板示例，域名不会真实调用。
// 入参：无。
// 返回：默认自定义配置。
nlohmann::json DefaultCustomConfiguration()
{
    return {{"method", "POST"},
            {"url", "https://example.invalid/translate"},
            {"headers", {{"Authorization", "Bearer {{secret.api_key}}"}}},
            {"query", nlohmann::json::object()},
            {"source_languages", {{"auto", "auto"}, {"zh-CN", "zh"}, {"en", "en"}, {"ja", "ja"}}},
            {"target_languages", {{"zh-CN", "zh"}, {"en", "en"}, {"ja", "ja"}}},
            {"body_mode", "json"},
            {"body", {{"text", "{{text}}"}, {"source", "{{source}}"}, {"target", "{{target}}"}}},
            {"response", {{"mode", "json"}, {"text_pointer", "/translation"}}}};
}
// 校验完整静态模板及响应映射，未填秘密允许保存但不得未知引用。
// 入参：profile 为完整条目。
// 返回：合法为 None。
TranslationError ValidateCustom(const nlohmann::json& profile)
{
    const nlohmann::json& config = profile.at("configuration");
    if (!Keys(config, {"method", "url", "headers", "query", "source_languages", "target_languages", "body_mode", "body",
                       "response"}) ||
        config.size() != 9)
        return TranslationError::InvalidConfiguration;
    const std::string method = config.at("method").get<std::string>();
    if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH")
        return TranslationError::InvalidConfiguration;
    const std::string url = config.at("url").get<std::string>();
    http::Request request;
    request.url = Wide(url);
    request.allowHttp = true;
    if (!Plain(url, 2048) || url.find_first_of("{}?") != std::string::npos || !http::IsValidRequest(request))
        return TranslationError::InvalidConfiguration;
    if (!Languages(config.at("source_languages"), true) || !Languages(config.at("target_languages"), false))
        return TranslationError::InvalidConfiguration;
    std::set<std::string> variables;
    if (!KeyValues(config.at("headers"), profile, variables, true) ||
        !KeyValues(config.at("query"), profile, variables, false))
        return TranslationError::InvalidConfiguration;
    const std::string mode = config.at("body_mode").get<std::string>();
    const nlohmann::json& body = config.at("body");
    if (method == "GET" && mode != "none")
        return TranslationError::InvalidConfiguration;
    if (mode == "none")
    {
        if (!body.is_null())
            return TranslationError::InvalidConfiguration;
    }
    else if (mode == "json")
    {
        if (!body.is_object() && !body.is_array())
            return TranslationError::InvalidConfiguration;
        ExpandJson(body, profile, variables);
    }
    else if (mode == "form")
    {
        if (!KeyValues(body, profile, variables, false))
            return TranslationError::InvalidConfiguration;
    }
    else if (mode == "raw")
    {
        Expand(body.get<std::string>(), profile, variables);
    }
    else
        return TranslationError::InvalidConfiguration;
    if (!variables.contains("text") || (config.at("source_languages").size() > 1 && !variables.contains("source")) ||
        (config.at("target_languages").size() > 1 && !variables.contains("target")))
        return TranslationError::InvalidConfiguration;
    const nlohmann::json& response = config.at("response");
    if (!Keys(response, {"mode", "text_pointer", "array_pointer", "item_pointer", "separator", "success",
                         "error_pointer", "error_map"}))
        return TranslationError::InvalidConfiguration;
    const std::string responseMode = response.at("mode").get<std::string>();
    if (responseMode == "text")
        return response.size() == 1 ? TranslationError::None : TranslationError::InvalidConfiguration;
    if (responseMode != "json")
        return TranslationError::InvalidConfiguration;
    if (response.contains("array_pointer"))
    {
        if (response.contains("text_pointer") || !Pointer(response.at("array_pointer")) ||
            !Pointer(response.at("item_pointer")))
            return TranslationError::InvalidConfiguration;
        std::size_t count{};
        if (!CountText(response.at("separator").get<std::string>(), count) || count > 16)
            return TranslationError::InvalidConfiguration;
    }
    else if (!Pointer(response.at("text_pointer")) || response.contains("item_pointer") ||
             response.contains("separator"))
        return TranslationError::InvalidConfiguration;
    if (response.contains("success"))
    {
        const nlohmann::json& success = response.at("success");
        if (!Keys(success, {"pointer", "equals"}) || success.size() != 2 || !Pointer(success.at("pointer")) ||
            !success.at("equals").is_primitive())
            return TranslationError::InvalidConfiguration;
    }
    if (response.contains("error_pointer") != response.contains("error_map"))
        return TranslationError::InvalidConfiguration;
    if (response.contains("error_pointer"))
    {
        if (!Pointer(response.at("error_pointer")) || !response.at("error_map").is_object() ||
            response.at("error_map").size() > 64)
            return TranslationError::InvalidConfiguration;
        for (const auto& entry : response.at("error_map").items())
            if (!entry.value().is_string() || MappedError(entry.value().get<std::string>()) == TranslationError::None)
                return TranslationError::InvalidConfiguration;
    }
    return TranslationError::None;
}
// 运行期替换变量后分别编码表单/查询及 JSON，再执行最终预算检查。
// 入参：input 为整轮输入，profile 为单项，output 接收请求。
// 返回：成功为 None，缺少能力或秘密值时不访问网络。
TranslationError BuildCustomRequest(const TranslationRequest& input, const nlohmann::json& profile,
                                    http::Request& output)
try
{
    const nlohmann::json& config = profile.at("configuration");
    if (!config.at("source_languages").contains(input.options.sourceLanguage) ||
        !config.at("target_languages").contains(input.options.targetLanguage))
        return TranslationError::NotApplicable;
    const nlohmann::json values{{"text", input.text},
                                {"source", config.at("source_languages").at(input.options.sourceLanguage)},
                                {"target", config.at("target_languages").at(input.options.targetLanguage)}};
    std::set<std::string> variables;
    http::Request request;
    const std::string method = config.at("method").get<std::string>();
    request.method = method == "GET"     ? http::Method::Get
                     : method == "PUT"   ? http::Method::Put
                     : method == "PATCH" ? http::Method::Patch
                                         : http::Method::Post;
    std::string url = config.at("url").get<std::string>();
    for (const auto& entry : config.at("query").items())
    {
        url += url.find('?') == std::string::npos ? '?' : '&';
        url +=
            Encode(entry.key()) + '=' + Encode(Expand(entry.value().get<std::string>(), profile, variables, &values));
        if (url.size() > 8192)
            return TranslationError::InputTooLarge;
    }
    request.url = Wide(url);
    request.allowHttp = url.starts_with("http://");
    bool contentType{};
    for (const auto& entry : config.at("headers").items())
    {
        const std::string value = Expand(entry.value().get<std::string>(), profile, variables, &values);
        if (value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos)
            return TranslationError::NotApplicable;
        if (value.size() > 8192)
            return TranslationError::InputTooLarge;
        request.headers.push_back({Wide(entry.key()), Wide(value)});
        contentType = contentType || _wcsicmp(request.headers.back().name.c_str(), L"Content-Type") == 0;
    }
    const std::string mode = config.at("body_mode").get<std::string>();
    std::wstring type;
    if (mode == "json")
    {
        request.body = ExpandJson(config.at("body"), profile, variables, &values).dump();
        type = L"application/json; charset=utf-8";
    }
    else if (mode == "form")
    {
        for (const auto& entry : config.at("body").items())
        {
            if (!request.body.empty())
                request.body += '&';
            request.body += Encode(entry.key()) + '=' +
                            Encode(Expand(entry.value().get<std::string>(), profile, variables, &values));
            if (request.body.size() > 65536)
                return TranslationError::InputTooLarge;
        }
        type = L"application/x-www-form-urlencoded; charset=utf-8";
    }
    else if (mode == "raw")
    {
        request.body = Expand(config.at("body").get<std::string>(), profile, variables, &values);
        type = L"text/plain; charset=utf-8";
    }
    for (const std::string& variable : variables)
        if (variable.starts_with("secret.") && profile.at("secrets").at(variable.substr(7)).get<std::string>().empty())
            return TranslationError::MissingCredentials;
    if (!contentType && !type.empty())
        request.headers.push_back({L"Content-Type", std::move(type)});
    request.proxy = {input.configuration.proxyMode == "custom" ? http::ProxyMode::Custom : http::ProxyMode::System,
                     Wide(input.configuration.proxyAddress)};
    request.maxResponseBytes = 1048576;
    request.maxErrorBytes = 16384;
    const TranslationError budget = RequestBudget(request);
    if (budget != TranslationError::None)
        return budget;
    output = std::move(request);
    return TranslationError::None;
}
catch (const std::length_error&)
{
    return TranslationError::InputTooLarge;
}
// 响应先处理显式业务错误，再处理 HTTP，最后严格提取完整文字。
// 入参：profile 为配置，response 为 HTTP 元数据，body 为正文，output 为结果。
// 返回：合法成功为 None，不产生部分译文。
TranslationError ParseCustomResponse(const nlohmann::json& profile, const http::Result& response, std::string_view body,
                                     TranslationResult& output)
try
{
    const TranslationError status = HttpStatusError(response.status);
    if (body.size() > (status == TranslationError::None ? 1048576U : 16384U))
        return TranslationError::UnsupportedResponse;
    const nlohmann::json& mapping = profile.at("configuration").at("response");
    if (mapping.at("mode") == "text")
    {
        if (status != TranslationError::None)
            return status;
        if (Html(response.contentType, body))
            return TranslationError::UnsupportedResponse;
        const TranslationError valid = ValidateResultText(body);
        if (valid != TranslationError::None)
            return valid;
        output.text = body;
        return TranslationError::None;
    }
    const nlohmann::json document = ParseJson(body);
    bool success = true;
    if (mapping.contains("success"))
    {
        const nlohmann::json& condition = mapping.at("success");
        const nlohmann::json* actual = At(document, condition.at("pointer").get<std::string>());
        success = actual && *actual == condition.at("equals");
    }
    bool unknownError{};
    if (mapping.contains("error_pointer"))
    {
        const nlohmann::json* code = At(document, mapping.at("error_pointer").get<std::string>());
        if (code && !code->is_null() && code->is_primitive())
        {
            const std::string value = code->is_string() ? code->get<std::string>() : code->dump();
            const nlohmann::json& errors = mapping.at("error_map");
            if (errors.contains(value))
                return MappedError(errors.at(value).get<std::string>());
            unknownError = !mapping.contains("success") || !success;
        }
        else if (code && !code->is_null())
            unknownError = true;
    }
    if (status != TranslationError::None)
        return status;
    if (!success || unknownError)
        return TranslationError::Service;
    std::string text;
    if (mapping.contains("array_pointer"))
    {
        const nlohmann::json* array = At(document, mapping.at("array_pointer").get<std::string>());
        if (!array || !array->is_array() || array->size() > 1024)
            return TranslationError::UnsupportedResponse;
        const std::string separator = mapping.at("separator").get<std::string>();
        bool first = true;
        for (const nlohmann::json& item : *array)
        {
            const nlohmann::json* value = At(item, mapping.at("item_pointer").get<std::string>());
            if (!value || !value->is_string())
                return TranslationError::UnsupportedResponse;
            if (!first)
                text += separator;
            first = false;
            text += value->get_ref<const std::string&>();
            if (text.size() > 1048576)
                return TranslationError::Truncated;
        }
    }
    else
    {
        const nlohmann::json* value = At(document, mapping.at("text_pointer").get<std::string>());
        if (!value || !value->is_string())
            return TranslationError::UnsupportedResponse;
        text = value->get<std::string>();
    }
    const TranslationError valid = ValidateResultText(text);
    if (valid != TranslationError::None)
        return valid;
    output.text = std::move(text);
    return TranslationError::None;
}
catch (const std::bad_alloc&)
{
    throw;
}
catch (...)
{
    const TranslationError status = HttpStatusError(response.status);
    return status == TranslationError::None ? TranslationError::UnsupportedResponse : status;
}
} // namespace open_st::translation_detail
