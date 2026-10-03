// 实现统一字段目录、有序 JSON 条目校验及设置快照，不读取业务文件或访问网络。
#include <algorithm>
#include <array>
#include <http_transport.h>
#include <objbase.h>
#include <set>
#include <stdexcept>
#include <translation_client.h>
#include <translation_protocol.h>
#include <windows.h>

namespace open_st
{
namespace
{
// 检查小型单行文本，名称与字段不允许控制字符。
// 入参：value 为 UTF-8，maximum 为码点上限，empty 控制可空。
// 返回：合法为 true。
bool Plain(std::string_view value, std::size_t maximum, bool empty = true)
{
    std::size_t count{};
    return translation_detail::CountText(value, count) && count <= maximum && (empty || count != 0) &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// 检查对象字段是否严格属于领域 schema，不接受未消费的配置。
// 入参：value 为对象；names 为允许字段。
// 返回：没有未知字段为 true。
bool Keys(const nlohmann::json& value, std::initializer_list<std::string_view> names)
{
    if (!value.is_object())
        return false;
    for (const auto& entry : value.items())
        if (std::find(names.begin(), names.end(), entry.key()) == names.end())
            return false;
    return true;
}
// 生成稳定 UUID，防止复制或删除后复用身份。
// 入参：无。
// 返回：ASCII UUID；系统失败抛异常。
std::string NewId()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("profile identity unavailable");
    wchar_t buffer[40]{};
    if (StringFromGUID2(guid, buffer, 40) == 0)
        throw std::runtime_error("profile identity unavailable");
    std::string id;
    for (const wchar_t c : buffer)
        if (c != L'\0' && c != L'{' && c != L'}')
            id.push_back(static_cast<char>(c));
    return id;
}
// 判断 URL 是否符合静态固定目标规则。
// 入参：value 为地址，base 表示 OpenAI 根地址。
// 返回：合法为 true。
bool Url(std::string_view value, bool base)
{
    if (!Plain(value, 2048, false) || value.find('?') != std::string_view::npos ||
        value.find('{') != std::string_view::npos || value.find('}') != std::string_view::npos)
        return false;
    std::string normalized(value);
    while (normalized.ends_with('/'))
        normalized.pop_back();
    if (base && normalized.ends_with("/chat/completions"))
        return false;
    http::Request candidate;
    candidate.url = translation_detail::Wide(normalized);
    candidate.allowHttp = true;
    return http::IsValidRequest(candidate);
}
} // namespace
// 提供所有 top-level 翻译字段，结构列表作为真实 JSON 而非文本存储。
// 入参：无。
// 返回：静态目录。
std::span<const std::string_view> TranslationSettingKeys() noexcept
{
    static constexpr std::array<std::string_view, 5> KEYS{
        "translation.interfaces", "translation.source_language", "translation.target_language",
        "translation.network.proxy_mode", "translation.network.proxy_address"};
    return KEYS;
}
// 发布领域候选以供设置和执行共用。
// 入参：fieldKey 为字段名称。
// 返回：静态选项。
std::span<const std::string_view> TranslationChoices(std::string_view fieldKey) noexcept
{
    static constexpr std::array<std::string_view, 7> KINDS{
        "baidu", "google", "openai", "deepl", "microsoft", "ctranslate2_local", "custom_http"};
    static constexpr std::array<std::string_view, 4> SOURCES{"auto", "zh-CN", "en", "ja"};
    static constexpr std::array<std::string_view, 3> TARGETS{"zh-CN", "en", "ja"};
    static constexpr std::array<std::string_view, 2> SERVICES{"free", "pro"};
    static constexpr std::array<std::string_view, 2> PROXIES{"system", "custom"};
    static constexpr std::array<std::string_view, 4> METHODS{"GET", "POST", "PUT", "PATCH"};
    static constexpr std::array<std::string_view, 4> BODY_MODES{"none", "json", "form", "raw"};
    static constexpr std::array<std::string_view, 2> RESPONSE_MODES{"text", "json"};
    static constexpr std::array<std::string_view, 2> EXTRACTION_MODES{"single", "array"};
    static constexpr std::array<std::string_view, 4> SCALAR_TYPES{"string", "number", "boolean", "null"};
    static constexpr std::array<std::string_view, 7> ERROR_CATEGORIES{
        "authentication", "permission", "quota", "rate_limited", "input", "service", "context_limit"};
    if (fieldKey == "custom.method")
        return METHODS;
    if (fieldKey == "custom.body_mode")
        return BODY_MODES;
    if (fieldKey == "custom.response_mode")
        return RESPONSE_MODES;
    if (fieldKey == "custom.extraction")
        return EXTRACTION_MODES;
    if (fieldKey == "custom.scalar_type")
        return SCALAR_TYPES;
    if (fieldKey == "custom.error_category")
        return ERROR_CATEGORIES;
    if (fieldKey == "translation.kind")
        return KINDS;
    if (fieldKey == "translation.source_language")
        return SOURCES;
    if (fieldKey == "translation.target_language")
        return TARGETS;
    if (fieldKey == "deepl.service")
        return SERVICES;
    if (fieldKey == "translation.network.proxy_mode")
        return PROXIES;
    return {};
}
// 通过领域字段描述生成普通子表单，不让界面复制配置规则。
// 入参：kind 为类型。
// 返回：静态字段描述。
std::span<const TranslationProfileField> TranslationProfileFields(std::string_view kind) noexcept
{
    static constexpr std::array BAIDU{TranslationProfileField{"app_id"}, TranslationProfileField{"api_key", true}};
    static constexpr std::array OPENAI{TranslationProfileField{"base_url"}, TranslationProfileField{"model"},
                                       TranslationProfileField{"api_key", true}};
    static constexpr std::array DEEPL{TranslationProfileField{"service"}, TranslationProfileField{"api_key", true}};
    static constexpr std::array MICROSOFT{TranslationProfileField{"region"}, TranslationProfileField{"api_key", true}};
    if (kind == "baidu")
        return BAIDU;
    if (kind == "openai")
        return OPENAI;
    if (kind == "deepl")
        return DEEPL;
    if (kind == "microsoft")
        return MICROSOFT;
    return {};
}
// 从默认配置资源创建新条目，不复制用户凭据或开启自动外发。
// 入参：kind 为类型；defaultInterfaces 为宿主读取的默认接口数组。
// 返回：新身份、停用、空凭据的配置；模板缺失或非法时抛出参数异常。
nlohmann::json CreateTranslationProfile(std::string_view kind, const nlohmann::json& defaultInterfaces)
{
    nlohmann::json profile;
    if (kind == "custom_http")
    {
        profile = {{"id", NewId()},
                   {"name", kind},
                   {"enabled", false},
                   {"kind", kind},
                   {"configuration", translation_detail::DefaultCustomConfiguration()},
                   {"secrets", {{"api_key", ""}}}};
    }
    else
    {
        if (ValidateTranslationInterfaces(defaultInterfaces) != TranslationError::None)
            throw std::invalid_argument("invalid default translation interfaces");
        for (const nlohmann::json& candidate : defaultInterfaces)
        {
            if (candidate.at("kind").get_ref<const std::string&>() != kind)
                continue;
            if (!profile.is_null())
                throw std::invalid_argument("ambiguous default translation profile");
            profile = candidate;
        }
        if (profile.is_null())
            throw std::invalid_argument("missing default translation profile");
        profile["id"] = NewId();
        profile["enabled"] = false;
        for (nlohmann::json& secret : profile["secrets"])
            secret = "";
    }
    return profile;
}
// 校验唯一字符串设置，代理无值仅在 custom 生效时拒绝。
// 入参：fieldKey 为字段，value 为候选值。
// 返回：静态合法时 None。
TranslationError ValidateTranslationField(std::string_view fieldKey, std::string_view value) noexcept
try
{
    if (!Plain(value, 2048))
        return TranslationError::InvalidConfiguration;
    const std::span<const std::string_view> choices = TranslationChoices(fieldKey);
    if (!choices.empty())
        return std::find(choices.begin(), choices.end(), value) != choices.end()
                   ? TranslationError::None
                   : TranslationError::InvalidConfiguration;
    if (fieldKey != "translation.network.proxy_address")
        return TranslationError::InvalidConfiguration;
    if (value.empty())
        return TranslationError::None;
    http::Request candidate;
    candidate.url = L"https://example.invalid/";
    candidate.proxy = {http::ProxyMode::Custom, translation_detail::Wide(value)};
    return http::IsValidRequest(candidate) ? TranslationError::None : TranslationError::InvalidConfiguration;
}
catch (...)
{
    return TranslationError::InvalidConfiguration;
}
// 校验条目所有字段与真实类型，缺失凭据允许保存，未知字段不能静默忽略。
// 入参：profile 为单个条目。
// 返回：首个结构错误。
TranslationError ValidateTranslationProfile(const nlohmann::json& profile) noexcept
try
{
    if (!translation_detail::BoundedJson(profile, 65536) ||
        !Keys(profile, {"id", "name", "enabled", "kind", "configuration", "secrets"}) || profile.size() != 6 ||
        !profile.at("enabled").is_boolean())
        return TranslationError::InvalidConfiguration;
    const std::string id = profile.at("id").get<std::string>();
    const std::string name = profile.at("name").get<std::string>();
    const std::string kind = profile.at("kind").get<std::string>();
    if (!Plain(id, 64, false) ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        !Plain(name, 80, false))
        return TranslationError::InvalidConfiguration;
    const std::span<const std::string_view> kinds = TranslationChoices("translation.kind");
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
        return TranslationError::InvalidConfiguration;
    const nlohmann::json& config = profile.at("configuration");
    const nlohmann::json& secrets = profile.at("secrets");
    if (!config.is_object() || !secrets.is_object() || secrets.size() > 16)
        return TranslationError::InvalidConfiguration;
    for (const auto& entry : secrets.items())
        if (entry.key().empty() || entry.key().size() > 64 ||
            entry.key().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            !entry.value().is_string() || !Plain(entry.value().get_ref<const std::string&>(), 4096))
            return TranslationError::InvalidConfiguration;
    if (kind == "custom_http")
        return translation_detail::ValidateCustom(profile);
    if (kind == "ctranslate2_local")
        return config == nlohmann::json{{"pack_id", "opus-mt-2020-07-17-int8-v1"}} && secrets.empty()
                   ? TranslationError::None
                   : TranslationError::InvalidConfiguration;
    if (!Keys(secrets, {"api_key"}))
        return TranslationError::InvalidConfiguration;
    std::size_t expected{};
    for (const TranslationProfileField& field : TranslationProfileFields(kind))
    {
        const nlohmann::json& object = field.secret ? secrets : config;
        if (!object.contains(field.key) || !object.at(field.key).is_string() ||
            !Plain(object.at(field.key).get_ref<const std::string&>(), 4096))
            return TranslationError::InvalidConfiguration;
        if (!field.secret)
            ++expected;
    }
    if (config.size() != expected || ((kind == "google" || kind == "ctranslate2_local") && !secrets.empty()))
        return TranslationError::InvalidConfiguration;
    if (kind == "openai" && !Url(config.at("base_url").get<std::string>(), true))
        return TranslationError::InvalidConfiguration;
    if (kind == "deepl" && config.at("service") != "free" && config.at("service") != "pro")
        return TranslationError::InvalidConfiguration;
    return TranslationError::None;
}
catch (const std::bad_alloc&)
{
    return TranslationError::OutOfMemory;
}
catch (...)
{
    return TranslationError::InvalidConfiguration;
}
// 同时校验总预算、单项预算和身份唯一性，重排不改变身份。
// 入参：interfaces 为有序数组。
// 返回：合法为 None。
TranslationError ValidateTranslationInterfaces(const nlohmann::json& interfaces) noexcept
try
{
    if (!interfaces.is_array() || interfaces.size() > 16 || !translation_detail::BoundedJson(interfaces, 1048576))
        return TranslationError::InvalidConfiguration;
    std::set<std::string> ids;
    for (const nlohmann::json& profile : interfaces)
    {
        const TranslationError error = ValidateTranslationProfile(profile);
        if (error != TranslationError::None)
            return error;
        if (!ids.insert(profile.at("id").get<std::string>()).second)
            return TranslationError::InvalidConfiguration;
    }
    return TranslationError::None;
}
catch (const std::bad_alloc&)
{
    return TranslationError::OutOfMemory;
}
catch (...)
{
    return TranslationError::InvalidConfiguration;
}
// 只解释宿主已经合并默认资源的完整快照，不保留另一套代码默认值。
// 入参：read 为同一快照的窄 JSON 查询函数。
// 返回：独立配置；缺失、错误类型或非法值均抛出参数异常。
TranslationSettings ReadTranslationSettings(const std::function<std::optional<nlohmann::json>(std::string_view)>& read)
{
    if (!read)
        throw std::invalid_argument("translation settings reader unavailable");
    TranslationSettings settings;
    const std::optional<nlohmann::json> interfaces = read("translation.interfaces");
    if (!interfaces)
        throw std::invalid_argument("translation interfaces missing");
    settings.configuration.interfaces = *interfaces;
    const std::array<std::pair<std::string_view, std::string*>, 4> fields{
        {{"translation.source_language", &settings.options.sourceLanguage},
         {"translation.target_language", &settings.options.targetLanguage},
         {"translation.network.proxy_mode", &settings.configuration.proxyMode},
         {"translation.network.proxy_address", &settings.configuration.proxyAddress}}};
    for (const auto& [key, destination] : fields)
    {
        const std::optional<nlohmann::json> value = read(key);
        if (!value || !value->is_string())
            throw std::invalid_argument("translation settings field missing or invalid");
        *destination = value->get<std::string>();
    }
    if (ValidateTranslationConfiguration(settings.configuration, settings.options) != TranslationError::None)
        throw std::invalid_argument("invalid translation settings");
    return settings;
}
// 运行前检查全局配置；单条凭据不足在顺序调度内处理。
// 入参：configuration 为配置，options 为语言。
// 返回：合法为 None。
TranslationError ValidateTranslationConfiguration(const TranslationConfiguration& configuration,
                                                  const TranslationOptions& options) noexcept
{
    if (ValidateTranslationField("translation.source_language", options.sourceLanguage) != TranslationError::None ||
        ValidateTranslationField("translation.target_language", options.targetLanguage) != TranslationError::None ||
        ValidateTranslationField("translation.network.proxy_mode", configuration.proxyMode) != TranslationError::None ||
        ValidateTranslationField("translation.network.proxy_address", configuration.proxyAddress) !=
            TranslationError::None ||
        (configuration.proxyMode == "custom" && configuration.proxyAddress.empty()))
        return TranslationError::InvalidConfiguration;
    return ValidateTranslationInterfaces(configuration.interfaces);
}
// 仅以保存的明确协议返回不安全传输标志，不自行修改目标。
// 入参：profile 为条目。
// 返回：使用 HTTP 为 true。
bool TranslationProfileUsesInsecureHttp(const nlohmann::json& profile) noexcept
try
{
    const std::string kind = profile.at("kind").get<std::string>();
    if (kind != "openai" && kind != "custom_http")
        return false;
    return profile.at("configuration")
        .at(kind == "openai" ? "base_url" : "url")
        .get<std::string>()
        .starts_with("http://");
}
catch (...)
{
    return false;
}
// 返回固定错误键，任何原始响应和凭据都不会进入 UI 诊断。
// 入参：error 为领域分类。
// 返回：静态本地化资源键。
std::string_view TranslationErrorTextKey(TranslationError error) noexcept
{
    switch (error)
    {
    case TranslationError::None:
        return "translation.error.none";
    case TranslationError::Busy:
        return "translation.error.busy";
    case TranslationError::InvalidConfiguration:
        return "translation.error.configuration";
    case TranslationError::MissingCredentials:
        return "translation.error.credentials";
    case TranslationError::InvalidInput:
        return "translation.error.input";
    case TranslationError::InputTooLarge:
        return "translation.error.limit";
    case TranslationError::SameLanguage:
        return "translation.error.same_language";
    case TranslationError::Authentication:
        return "translation.error.authentication";
    case TranslationError::Permission:
        return "translation.error.permission";
    case TranslationError::Quota:
        return "translation.error.quota";
    case TranslationError::RateLimited:
        return "translation.error.rate";
    case TranslationError::Proxy:
        return "translation.error.proxy";
    case TranslationError::Tls:
        return "translation.error.tls";
    case TranslationError::Network:
        return "translation.error.network";
    case TranslationError::Timeout:
        return "translation.error.timeout";
    case TranslationError::Cancelled:
        return "translation.error.cancelled";
    case TranslationError::Service:
        return "translation.error.service";
    case TranslationError::UnsupportedResponse:
        return "translation.error.response";
    case TranslationError::EmptyResult:
        return "translation.error.empty";
    case TranslationError::Truncated:
        return "translation.error.truncated";
    case TranslationError::ContextLimit:
        return "translation.error.context";
    case TranslationError::Redirect:
        return "translation.error.redirect";
    case TranslationError::NotApplicable:
        return "translation.error.not_applicable";
    case TranslationError::OutOfMemory:
        return "translation.error.memory";
    case TranslationError::NoInterfaces:
        return "translation.error.no_interfaces";
    case TranslationError::BudgetExhausted:
        return "translation.error.budget";
    case TranslationError::ModelMissing:
        return "translation.error.model_missing";
    case TranslationError::ModelIntegrity:
        return "translation.error.model_integrity";
    default:
        return "translation.error.unavailable";
    }
}
} // namespace open_st
