// 将在线和本地引擎适配为统一单次提供方；取消、缓存维护不产生第二套调度。
#include <algorithm>
#include <array>
// CNG 头要求 Windows 基础类型已定义；独立分组以保留包含顺序。
#include <windows.h>

#include <bcrypt.h>
#include <translation_local.h>
#include <translation_protocol.h>
#include <translation_provider.h>
#include <winhttp.h>

namespace open_st::translation_detail
{
// 提供方状态由稳定错误分类派生，调度不识别具体引擎。
// 入参：error 为领域错误。
// 返回：统一结果类别。
ProviderStatus StatusOf(TranslationError error) noexcept
{
    if (error == TranslationError::None)
        return ProviderStatus::Success;
    if (error == TranslationError::Cancelled)
        return ProviderStatus::Cancelled;
    if (error == TranslationError::Timeout)
        return ProviderStatus::TimedOut;
    if (error == TranslationError::NotApplicable || error == TranslationError::InputTooLarge ||
        error == TranslationError::ContextLimit)
        return ProviderStatus::NotApplicable;
    if (error == TranslationError::MissingCredentials || error == TranslationError::ModelMissing ||
        error == TranslationError::ModelIntegrity || error == TranslationError::Unavailable)
        return ProviderStatus::Unavailable;
    return ProviderStatus::Failed;
}
namespace
{
// 所有适配器使用同一全局语种能力检查，不以目标反推自动源语言。
// 入参：input 为请求，capabilities 为真实能力。
// 返回：可执行为 None。
TranslationError Fits(const TranslationRequest& input, const ProviderCapabilities& capabilities)
{
    if (input.options.sourceLanguage == "auto" && !capabilities.supportsAuto)
        return TranslationError::NotApplicable;
    const auto pair = std::make_pair(input.options.sourceLanguage, input.options.targetLanguage);
    if (std::find(capabilities.languagePairs.begin(), capabilities.languagePairs.end(), pair) ==
        capabilities.languagePairs.end())
        return TranslationError::NotApplicable;
    std::size_t count{};
    if (!CountText(input.text, count) || count == 0)
        return TranslationError::InvalidInput;
    return count > capabilities.maxInputCodepoints ? TranslationError::InputTooLarge : TranslationError::None;
}
// 只把公共传输错误转换为领域分类，不展示系统正文。
// 入参：result 为传输结果。
// 返回：成功传输为 None。
TranslationError NetworkError(const http::Result& result)
{
    if (result.error == http::Error::None)
        return TranslationError::None;
    if (result.error == http::Error::Cancelled)
        return TranslationError::Cancelled;
    if (result.error == http::Error::Timeout)
        return TranslationError::Timeout;
    if (result.error == http::Error::InvalidRequest)
        return TranslationError::InvalidConfiguration;
    if (result.error == http::Error::ResponseTooLarge)
        return TranslationError::Truncated;
    if (result.systemError == ERROR_NOT_ENOUGH_MEMORY || result.systemError == ERROR_OUTOFMEMORY)
        return TranslationError::OutOfMemory;
    if (result.systemError == ERROR_WINHTTP_SECURE_FAILURE ||
        result.systemError == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID ||
        result.systemError == ERROR_WINHTTP_SECURE_INVALID_CA ||
        result.systemError == ERROR_WINHTTP_SECURE_CERT_CN_INVALID)
        return TranslationError::Tls;
    if (result.systemError == ERROR_WINHTTP_AUTODETECTION_FAILED ||
        result.systemError == ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT ||
        result.systemError == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT)
        return TranslationError::Proxy;
    return TranslationError::Network;
}
// 规范占位仅用于内部字节预算和客户端成功校验，界面消费结构化片段。
// 入参：parts 为已脱敏片段。返回：不含秘密的规范文本，不作为本地化展示来源。
std::string DiagnosticBudgetText(const TranslationDiagnosticText& parts)
{
    std::string result;
    for (const TranslationDiagnosticPart& part : parts)
        switch (part.kind)
        {
        case TranslationDiagnosticPartKind::Text:
            result += part.text;
            break;
        case TranslationDiagnosticPartKind::Redacted:
            result += "[redacted]";
            break;
        case TranslationDiagnosticPartKind::Omitted:
            result += "[omitted]";
            break;
        case TranslationDiagnosticPartKind::TooLong:
            result += "[omitted: too long]";
            break;
        }
    return result;
}
// 诊断的服务消息和译文共用脱敏器，普通翻译不经过此路径。
// 入参：profile/request 为固定请求；value 为候选展示文本。
// 返回：脱敏且限长的 UTF-8 文本，无法安全展示时省略。
TranslationDiagnosticText SafeDiagnosticText(const nlohmann::json& profile, const http::Request& request,
                                             std::string value)
{
    std::vector<std::string> hidden;
    // 遍历已有有界配置的字符串值，防止错误消息回显静态配置或凭据。
    // 入参：self 为递归函数，value 为有界配置。返回：无。
    const auto collect = [&hidden](const auto& self, const nlohmann::json& node) -> void
    {
        if (node.is_string())
        {
            const std::string text = node.get<std::string>();
            if (!text.empty())
            {
                hidden.push_back(text);
                hidden.push_back(Encode(text));
            }
        }
        else if (node.is_structured())
            for (const nlohmann::json& child : node)
                self(self, child);
    };
    collect(collect, profile.at("secrets"));
    collect(collect, profile.at("configuration"));
    for (const http::Header& header : request.headers)
    {
        if (std::any_of(header.value.begin(), header.value.end(), [](wchar_t character) { return character > 127; }))
            return {{TranslationDiagnosticPartKind::Omitted, {}}};
        std::string headerValue;
        for (const wchar_t character : header.value)
            headerValue.push_back(static_cast<char>(character));
        hidden.push_back(headerValue);
        const std::size_t space = headerValue.find(' ');
        if (space != std::string::npos)
            hidden.push_back(headerValue.substr(space + 1));
    }
    if (profile.at("kind") == "custom_http")
    {
        try
        {
            collect(collect, ParseJson(request.body));
        }
        catch (...)
        {
            // 非 JSON 表单由下面的键值扫描覆盖，原始正文不公开。
        }
    }
    // 百度签名及随机盐是动态表单值，同样禁止通过错误正文回显。
    if (profile.at("kind") == "baidu" || profile.at("kind") == "custom_http")
    {
        std::size_t offset{};
        while (offset < request.body.size())
        {
            const std::size_t end = request.body.find('&', offset);
            const std::string item = request.body.substr(offset, end - offset);
            const std::size_t equal = item.find('=');
            if (equal != std::string::npos)
                hidden.push_back(item.substr(equal + 1));
            if (end == std::string::npos)
                break;
            offset = end + 1;
        }
    }
    // 在原始字节上标记所有秘密并合并交叠区间，程序占位从不参加后续匹配。
    std::vector<bool> masked(value.size(), false);
    for (const std::string& secret : hidden)
    {
        if (secret.empty())
            continue;
        std::size_t position{};
        while ((position = value.find(secret, position)) != std::string::npos)
        {
            std::fill(masked.begin() + static_cast<std::ptrdiff_t>(position),
                      masked.begin() + static_cast<std::ptrdiff_t>(position + secret.size()), true);
            ++position;
        }
    }
    TranslationDiagnosticText parts;
    for (std::size_t begin = 0; begin < value.size();)
    {
        std::size_t end = begin + 1;
        while (end < value.size() && masked[end] == masked[begin])
            ++end;
        parts.push_back({masked[begin] ? TranslationDiagnosticPartKind::Redacted : TranslationDiagnosticPartKind::Text,
                         masked[begin] ? std::string{} : value.substr(begin, end - begin)});
        begin = end;
    }
    const std::string safe = DiagnosticBudgetText(parts);
    std::size_t count{};
    if (!CountText(safe, count) ||
        std::any_of(safe.begin(), safe.end(),
                    [](unsigned char character)
                    {
                        return (character < 32 && character != '\r' && character != '\n' && character != '\t') ||
                               character == 127;
                    }))
        return {{TranslationDiagnosticPartKind::Omitted, {}}};
    if (safe.size() > 1024)
        return {{TranslationDiagnosticPartKind::TooLong, {}}};
    return parts;
}
// 仅选取已知协议错误字段，并删除凭据及请求派生值；未知正文不尝试通用脱敏。
// 入参：profile/request 为本次固定配置，response/body 为当前响应。
// 返回：有界元数据与安全错误摘要，自定义响应仅保留传输状态。
TranslationDiagnostic Diagnostic(const nlohmann::json& profile, const http::Request& request,
                                 const http::Result& response, std::string_view body)
{
    TranslationDiagnostic result;
    result.httpStatus = response.status;
    result.systemError = response.systemError;
    result.response = {{TranslationDiagnosticPartKind::Omitted, {}}};
    if (profile.at("kind") == "custom_http" || body.size() > 16384)
        return result;
    try
    {
        const nlohmann::json json = ParseJson(body);
        if (!json.is_object())
            return result;
        const std::string kind = profile.at("kind").get<std::string>();
        const nlohmann::json* error = &json;
        if ((kind == "openai" || kind == "microsoft") && json.contains("error"))
            error = &json.at("error");
        if (!error->is_object())
            return result;
        // 只接受标量字段，避免将服务任意对象或请求镜像带入结果。
        // 入参：key 为固定协议字段。返回：有界文本。
        const auto field = [error](std::string_view key) -> std::string
        {
            const auto item = error->find(std::string(key));
            if (item == error->end())
                return {};
            if (item->is_string())
                return item->get<std::string>();
            return item->is_number_integer() ? item->dump() : std::string{};
        };
        std::string code = field(kind == "baidu" ? "error_code" : "code");
        std::string message = field(kind == "baidu" ? "error_msg" : "message");
        result.providerCode = SafeDiagnosticText(profile, request, std::move(code));
        result.providerMessage = SafeDiagnosticText(profile, request, std::move(message));
        if (!result.providerCode.empty() || !result.providerMessage.empty())
        {
            result.response = {{TranslationDiagnosticPartKind::Text, "{\"code\":\""}};
            // 转义服务原文为 JSON 字符串内容，程序标记仍保持独立类型供界面本地化。
            // 入参：parts 为字段安全片段。返回：无，追加摘要片段。
            const auto append = [&result](const TranslationDiagnosticText& parts)
            {
                for (const TranslationDiagnosticPart& part : parts)
                {
                    if (part.kind == TranslationDiagnosticPartKind::Text)
                    {
                        const std::string quoted = nlohmann::json(part.text).dump();
                        result.response.push_back({part.kind, quoted.substr(1, quoted.size() - 2)});
                    }
                    else
                        result.response.push_back(part);
                }
            };
            append(result.providerCode);
            result.response.push_back({TranslationDiagnosticPartKind::Text, "\",\"message\":\""});
            append(result.providerMessage);
            result.response.push_back({TranslationDiagnosticPartKind::Text, "\"}"});
        }
        if (DiagnosticBudgetText(result.response).size() > 4096)
            result.response = {{TranslationDiagnosticPartKind::TooLong, {}}};
    }
    catch (...)
    {
        // 诊断失败不影响原请求分类，也绝不退回显示原始正文。
    }
    return result;
}
class OnlineProvider final : public ITranslationProvider
{
  public:
    // 保存协议类型和单次传输，不读取配置文件。
    // 入参：kind 为类型，transport 为独占传输。
    // 返回：未执行实例。
    OnlineProvider(std::string kind, std::unique_ptr<http::Transport> transport)
        : kind_(std::move(kind)), transport_(std::move(transport))
    {
    }
    // 在线能力来自统一语种或自定义声明，查询不访问网络。
    // 入参：profile 为单项。
    // 返回：语言对和输入保护。
    ProviderCapabilities Capabilities(const nlohmann::json& profile) const override
    {
        ProviderCapabilities capabilities;
        capabilities.network = true;
        capabilities.supportsAuto = true;
        capabilities.maxInputCodepoints = this->kind_ == "baidu" ? 1000U : this->kind_ == "google" ? 2000U : 10000U;
        capabilities.fields = TranslationProfileFields(this->kind_);
        if (this->kind_ == "custom_http")
        {
            const nlohmann::json& config = profile.at("configuration");
            capabilities.supportsAuto = config.at("source_languages").contains("auto");
            for (const auto& source : config.at("source_languages").items())
                for (const auto& target : config.at("target_languages").items())
                    if (source.key() != target.key())
                        capabilities.languagePairs.emplace_back(source.key(), target.key());
        }
        else
            for (const std::string_view source : TranslationChoices("translation.source_language"))
                for (const std::string_view target : TranslationChoices("translation.target_language"))
                    if (source != target)
                        capabilities.languagePairs.emplace_back(source, target);
        return capabilities;
    }
    // 纯请求构造执行参数和编码后预算校验，未配置项直接跳过。
    // 入参：input 为整轮输入，profile 为单项。
    // 返回：可执行为 None。
    TranslationError Validate(const TranslationRequest& input, const nlohmann::json& profile) const override
    {
        const TranslationError fits = Fits(input, this->Capabilities(profile));
        if (fits != TranslationError::None)
            return fits;
        http::Request request;
        return BuildRequest(input, profile, "00000000000000000000000000000000", request);
    }
    // 一次构造、一次发送、一次解析；不请求重试或选择其他供应商。
    // 入参：input/profile 为固定快照，deadline 为期限，stop 为整轮取消。
    // 返回：完整统一结果。
    ProviderResult Execute(const TranslationRequest& input, const nlohmann::json& profile, Clock::time_point deadline,
                           std::stop_token stop) override
    {
        ProviderResult result;
        if (stop.stop_requested())
            result.error = TranslationError::Cancelled;
        else if (Clock::now() >= deadline)
            result.error = TranslationError::Timeout;
        else
        {
            std::array<unsigned char, 16> saltBytes{};
            if (BCryptGenRandom(nullptr, saltBytes.data(), static_cast<ULONG>(saltBytes.size()),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                result.error = TranslationError::Unavailable;
            else
            {
                constexpr char HEX[] = "0123456789abcdef";
                std::string salt;
                for (const unsigned char byte : saltBytes)
                {
                    salt += HEX[byte >> 4];
                    salt += HEX[byte & 15];
                }
                http::Request request;
                result.error = BuildRequest(input, profile, salt, request);
                if (result.error == TranslationError::None)
                {
                    request.deadline = deadline;
                    std::string body;
                    // 传输已限制总字节，此处只保留当前一家的有界响应。
                    // 入参：bytes 为借用正文块。
                    // 返回：有界时 true；超预算立即终止传输。
                    const http::Result response = this->transport_->Perform(
                        request, stop,
                        [&body](std::span<const std::byte> bytes)
                        {
                            if (bytes.size() > 1048576 - body.size())
                                return false;
                            body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                            return true;
                        });
                    if (input.diagnostic)
                        result.diagnostic = Diagnostic(profile, request, response, body);
                    result.error = NetworkError(response);
                    if (result.error == TranslationError::None)
                    {
                        TranslationResult parsed;
                        result.error = this->kind_ == "custom_http"
                                           ? ParseCustomResponse(profile, response, body, parsed)
                                           : ParseResponse(this->kind_, response.status, body, parsed);
                        if (result.error == TranslationError::None)
                        {
                            if (input.diagnostic)
                            {
                                result.diagnostic->translatedText =
                                    SafeDiagnosticText(profile, request, std::move(parsed.text));
                                result.text = DiagnosticBudgetText(result.diagnostic->translatedText);
                            }
                            else
                                result.text = std::move(parsed.text);
                            result.detectedLanguage = std::move(parsed.detectedLanguage);
                        }
                    }
                }
            }
        }
        if (stop.stop_requested())
        {
            result.error = TranslationError::Cancelled;
            result.text.clear();
        }
        else if (Clock::now() >= deadline && result.error != TranslationError::OutOfMemory &&
                 result.error != TranslationError::InvalidInput &&
                 result.error != TranslationError::InvalidConfiguration)
        {
            result.error = TranslationError::Timeout;
            result.text.clear();
        }
        result.status = StatusOf(result.error);
        return result;
    }
    // 在线提供方没有模型缓存维护。
    // 入参：无。
    // 返回：永不触发空闲维护。
    Clock::time_point NextMaintenance() const noexcept override
    {
        return Clock::time_point::max();
    }
    // 保持统一生命周期契约，在线适配器没有待释放缓存。
    // 入参：now 为当前时刻。
    // 返回：无。
    void Maintain(Clock::time_point) noexcept override {}

  private:
    std::string kind_;
    std::unique_ptr<http::Transport> transport_;
};
class LocalCTranslateProvider final : public ITranslationProvider
{
  public:
    // 构建本地引擎外壳，真正校验/加载只在工作线程首次使用时发生。
    // 入参：root 为程序目录。
    // 返回：未加载实例。
    explicit LocalCTranslateProvider(const std::filesystem::path& root) : engine_(root) {}
    // 声明中英双向及本机自动识别能力，不联网检测。
    // 入参：profile 为当前条目。
    // 返回：显式中英与自动源语言的两个目标方向。
    ProviderCapabilities Capabilities(const nlohmann::json&) const override
    {
        return {false,
                true,
                10000,
                {{"zh-CN", "en"}, {"en", "zh-CN"}, {"auto", "en"}, {"auto", "zh-CN"}},
                TranslationProfileFields("ctranslate2_local")};
    }
    // 只做能力验证，不加载模型或分词。
    // 入参：input 为输入，profile 为单项。
    // 返回：支持方向时 None。
    TranslationError Validate(const TranslationRequest& input, const nlohmann::json& profile) const override
    {
        return Fits(input, this->Capabilities(profile));
    }
    // 调用独立子模块，同步实际收尾后才向唯一调度器返回。
    // 入参：input 为文字与语言，profile 为已验证单项，deadline 为期限，stop 为取消。
    // 返回：统一分类结果。
    ProviderResult Execute(const TranslationRequest& input, const nlohmann::json&, Clock::time_point deadline,
                           std::stop_token stop) override
    {
        translation_local::Result local = this->engine_.Execute(input.text, input.options.sourceLanguage,
                                                                input.options.targetLanguage, deadline, stop);
        TranslationError error = TranslationError::Unavailable;
        switch (local.error)
        {
        case translation_local::Error::None:
            error = TranslationError::None;
            break;
        case translation_local::Error::NotApplicable:
            error = TranslationError::NotApplicable;
            break;
        case translation_local::Error::InvalidInput:
            error = TranslationError::InvalidInput;
            break;
        case translation_local::Error::InputTooLong:
            error = TranslationError::InputTooLarge;
            break;
        case translation_local::Error::ModelMissing:
            error = TranslationError::ModelMissing;
            break;
        case translation_local::Error::ModelIntegrity:
            error = TranslationError::ModelIntegrity;
            break;
        case translation_local::Error::TimedOut:
            error = TranslationError::Timeout;
            break;
        case translation_local::Error::Cancelled:
            error = TranslationError::Cancelled;
            break;
        case translation_local::Error::Truncated:
            error = TranslationError::Truncated;
            break;
        case translation_local::Error::OutOfMemory:
            error = TranslationError::OutOfMemory;
            break;
        case translation_local::Error::Inference:
            error = TranslationError::Service;
            break;
        default:
            break;
        }
        if (error == TranslationError::None)
            error = ValidateResultText(local.text);
        return {StatusOf(error),
                error,
                error == TranslationError::None ? std::move(local.text) : std::string{},
                std::move(local.detectedLanguage),
                {}};
    }
    // 转交共享当前方向缓存的维护截止时间。
    // 入参：无。
    // 返回：缓存空闲到期时刻。
    Clock::time_point NextMaintenance() const noexcept override
    {
        return this->engine_.NextMaintenance();
    }
    // 在唯一工作线程中释放到期资源。
    // 入参：now 为当前时刻。
    // 返回：无。
    void Maintain(Clock::time_point now) noexcept override
    {
        this->engine_.Maintain(now);
    }

  private:
    translation_local::Engine engine_;
};
} // namespace
// 构造可注入的真实在线适配器，状态机测试不会访问付费网络。
// 入参：kind 为协议，transport 为独占传输。
// 返回：统一提供方。
std::unique_ptr<ITranslationProvider> CreateOnlineProvider(std::string_view kind,
                                                           std::unique_ptr<http::Transport> transport)
{
    return std::make_unique<OnlineProvider>(std::string(kind), std::move(transport));
}
// 类型分支仅限工厂，运行调度始终消费同一接口。
// 入参：kind 为提供方类型；root 为资源目录。
// 返回：提供方，未知类型为空。
std::unique_ptr<ITranslationProvider> CreateProvider(std::string_view kind, const std::filesystem::path& root)
{
    if (kind == "ctranslate2_local")
        return std::make_unique<LocalCTranslateProvider>(root);
    const std::span<const std::string_view> kinds = TranslationChoices("translation.kind");
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
        return {};
    return CreateOnlineProvider(kind, http::MakeWinHttpTransport());
}
} // namespace open_st::translation_detail

namespace open_st
{
// 将子模块的元数据查询转换为公共领域分类，不创建本地引擎或在线传输。
// 入参：root 为应用绝对资源根。
// 返回：None 仅为文件齐全且尺寸匹配，完整验证仍由实际翻译执行。
TranslationError QueryLocalTranslationModelFiles(const std::filesystem::path& root) noexcept
{
    switch (translation_local::QueryModelFiles(root))
    {
    case translation_local::Error::None:
        return TranslationError::None;
    case translation_local::Error::ModelMissing:
        return TranslationError::ModelMissing;
    case translation_local::Error::ModelIntegrity:
        return TranslationError::ModelIntegrity;
    default:
        return TranslationError::Unavailable;
    }
}
} // namespace open_st
