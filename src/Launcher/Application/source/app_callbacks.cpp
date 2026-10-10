// 集中组装 App 注入子模块的回调，连接本地化、业务命令与跨线程截图门禁。

#include "capture_storage_options.h"
#include "capture_visual_session.h"
#include "capture_text_session.h"
#include "diagnostic_text.h"
#include "app_translation_state.h"
#include <app.h>
#include <algorithm>
#include <shellapi.h>
#include <array>
#include <stdexcept>
#include <hotkeys.h>
#include <pin_window_manager.h>
#include <settings.h>
#include <settings_window.h>
#include <ui_text.h>
#include <vector>
#include <windows_util.h>
#ifdef OPEN_ST_HAS_OCR
#include <ocr_client.h>
#endif
#ifdef OPEN_ST_HAS_TRANSLATION
#include <translation_client.h>
#include <http_transport.h>
#endif

namespace open_st
{
#ifdef OPEN_ST_HAS_TRANSLATION
namespace
{
// 复用HTTP参数校验限制网页协议；完整地址允许文档锚点，但拒绝控制符和浏览器路径混淆。
// 入参：url为配置中的UTF-8网址。返回：适合交给默认浏览器的HTTPS地址。
bool ValidTranslationResource(std::string_view url)
{
    if (url.empty() || url.size() > 32768 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }))
        return false;
    std::string lower(url);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                   { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c); });
    if (lower.find("%00") != std::string::npos || lower.find("%0d") != std::string::npos ||
        lower.find("%0a") != std::string::npos || lower.find("%5c") != std::string::npos)
        return false;
    const std::wstring page = Utf8ToWide(url);
    if (page.empty())
        return false;
    http::Request request;
    request.url = page.substr(0, page.find(L'#'));
    return http::IsValidRequest(request);
}
// 只本地化程序生成的诊断标记；远端同名字面文本保持原样。
// 入参：parts为已经脱敏和限长的片段。返回：当前界面语言的可复制文字。
std::wstring DiagnosticDisplay(const TranslationDiagnosticText& parts)
{
    std::wstring result;
    for (const TranslationDiagnosticPart& part : parts)
    {
        switch (part.kind)
        {
        case TranslationDiagnosticPartKind::Text:
            result += Utf8ToWide(part.text);
            break;
        case TranslationDiagnosticPartKind::Redacted:
            result += GetUiText("translation.diagnostic.redacted");
            break;
        case TranslationDiagnosticPartKind::Omitted:
            result += GetUiText("translation.diagnostic.omitted");
            break;
        case TranslationDiagnosticPartKind::TooLong:
            result += GetUiText("translation.diagnostic.too_long");
            break;
        }
    }
    return result;
}
} // namespace
#endif
// 组装贴图与宿主之间的窄回调，保持兄弟模块互不依赖。
// 入参：无。
// 返回：文本同步查询，复制保存仅提交异步请求。
PinWindowCallbacks App::MakePinCallbacks()
{
    PinWindowCallbacks callbacks;
    // 查询贴图标题和菜单所需的当前语言文字。
    // 入参：key 为资源键。
    // 返回：本地化宽字符串。
    callbacks.text = [](std::string_view key) { return GetUiText(key); };
    // 将贴图输出意图投递到 App 消息队列。
    // 入参：id 为稳定图像标识；command 为复制或保存。
    // 返回：成功入队 true，不代表输出已经成功。
    callbacks.command = [this](PinId id, PinCommand command) { return this->PostPinCommand(id, command); };
    // 让主消息循环在 Manager 最外层调用退出后重新检查延迟退出条件。
    // 入参：无。
    // 返回：无返回值；不在管理器的同步回调内销毁 App。
    callbacks.stopped = [this]()
    {
        if (IsWindow(this->messageWindow_))
            PostMessageW(this->messageWindow_, WM_APP + 6, 0, 0);
    };
    return callbacks;
}

// 组装供设置和欢迎窗口调用的本地化及系统集成回调。
// 入参：无。
// 返回：包含文本、语言、启动项和忙状态通知的回调集合；其中捕获的 App 须存活到回调解除。
SettingsWindowCallbacks App::MakeSettingsCallbacks()
{
    SettingsWindowCallbacks callbacks;
    // 子模态接入主循环同一分派，截图状态收尾不能留在被阻塞的外层循环。
    // 入参：message 为原生消息。返回：完整处理后为真。
    callbacks.processThreadMessage = [this](MSG& message) { return this->ProcessApplicationMessage(message); };
#ifdef OPEN_ST_HAS_TRANSLATION
    callbacks.translationAvailable = true;
    // 资源入口以整个配置字段为一次快照；网址不来自源码或接口密钥。
    // 入参：无。返回：有效入口及不阻止编辑的资源错误提示。
    callbacks.translationResources = []()
    {
        SettingsTranslationResources result;
        const auto invalid = [&]() { result.error = GetUiText("translation.resources.invalid"); };
        try
        {
            const std::array<std::string_view, 1> keys{"translation.provider_resources"};
            const std::optional<nlohmann::json> snapshot = ReadSettingsSnapshot(keys);
            if (!snapshot || !snapshot->at(keys[0]).is_object() || snapshot->at(keys[0]).size() > 16)
            {
                invalid();
                return result;
            }
            for (const auto& provider : snapshot->at(keys[0]).items())
            {
                if (!provider.value().is_array() || provider.value().size() > 8)
                {
                    invalid();
                    continue;
                }
                for (const nlohmann::json& entry : provider.value())
                {
                    if (!entry.is_object() || !entry.contains("label_key") || !entry.at("label_key").is_string() ||
                        !entry.contains("url") || !entry.at("url").is_string())
                    {
                        invalid();
                        continue;
                    }
                    const std::string label = entry.at("label_key").get<std::string>();
                    const std::string url = entry.at("url").get<std::string>();
                    if (label.empty() || label.size() > 128 || !ValidTranslationResource(url))
                    {
                        invalid();
                        continue;
                    }
                    result.links.push_back({provider.key(), label, url});
                }
            }
        }
        catch (...)
        {
            invalid();
        }
        return result;
    };
    // 用户明确点击才打开配置地址；再次校验，绝不附加API Key或其他请求参数。
    // 入参：url为编辑窗口快照中的完整链接。返回：系统接受启动为真。
    callbacks.openTranslationResource = [this](std::string_view url)
    {
        try
        {
            if (!ValidTranslationResource(url))
                return false;
            const std::wstring page = Utf8ToWide(url);
            return reinterpret_cast<INT_PTR>(
                       ShellExecuteW(this->DialogOwner(), L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        }
        catch (...)
        {
            return false;
        }
    };
    // 读取一个完整样本对象，显示和实际请求共享此快照，不重复解释默认值。
    // 入参：无。返回：完整且合法的样本；读取失败或字段非法为空。
    callbacks.translationTestSample = []() -> std::optional<SettingsTranslationTestSample>
    {
        const std::array<std::string_view, 1> keys{"translation.test_sample"};
        const std::optional<nlohmann::json> snapshot = ReadSettingsSnapshot(keys);
        if (!snapshot)
            return std::nullopt;
        try
        {
            const nlohmann::json& value = snapshot->at(keys[0]);
            SettingsTranslationTestSample sample;
            sample.text = value.at("text").get<std::string>();
            sample.sourceLanguage = value.at("source_language").get<std::string>();
            sample.targetLanguage = value.at("target_language").get<std::string>();
            const std::wstring text = Utf8ToWide(sample.text);
            if (text.empty() || sample.text.find('\0') != std::string::npos ||
                ValidateTranslationField("translation.source_language", sample.sourceLanguage) !=
                    TranslationError::None ||
                ValidateTranslationField("translation.target_language", sample.targetLanguage) !=
                    TranslationError::None ||
                sample.sourceLanguage == sample.targetLanguage)
                return std::nullopt;
            sample.description = GetUiText("translation.test.sample",
                                           {{L"text", text},
                                            {L"source", GetUiText("translation.choice." + sample.sourceLanguage)},
                                            {L"target", GetUiText("translation.choice." + sample.targetLanguage)}});
            return sample;
        }
        catch (...)
        {
            return std::nullopt;
        }
    };
    // 接口测试捕获未保存的单项和代理草稿，临时启用只存在于本次不可变请求。
    // 入参：profile/proxyMode/proxyAddress 为编辑草稿。返回：身份或安全拒绝信息。
    callbacks.submitTranslationTest = [this](const nlohmann::json& profile, std::string_view proxyMode,
                                             std::string_view proxyAddress, const SettingsTranslationTestSample& sample,
                                             const nlohmann::json& localQualityPresets)
    {
        SettingsTranslationTestStatus status;
        try
        {
            if (this->shuttingDown_)
            {
                status.text = GetUiText("translation.error.unavailable");
                return status;
            }
            TranslationClient* client = this->EnsureTranslationClient();
            if (!client)
            {
                status.text = GetUiText("translation.error.unavailable");
                return status;
            }
            if (client->Snapshot().busy)
            {
                status.text = GetUiText(TranslationErrorTextKey(TranslationError::Busy));
                return status;
            }
            TranslationRequest request;
            request.text = sample.text;
            request.options = {sample.sourceLanguage, sample.targetLanguage};
            nlohmann::json candidate = profile;
            candidate["enabled"] = true;
            request.configuration.interfaces = nlohmann::json::array({std::move(candidate)});
            request.configuration.localQualityPresets = localQualityPresets;
            request.configuration.proxyMode = proxyMode;
            request.configuration.proxyAddress = proxyAddress;
            request.diagnostic = true;
            // 先建立独立补收计时器，避免正常文字会话空闲时移除自身timer导致测试无刷新。
            if (!SetTimer(this->messageWindow_, TranslationPollTimer, 100, nullptr))
            {
                status.text = GetUiText(TranslationErrorTextKey(TranslationError::Unavailable));
                return status;
            }
            TranslationError error{};
            if (!client->Submit(request, status.requestId, error))
            {
                KillTimer(this->messageWindow_, TranslationPollTimer);
                status.text = GetUiText(TranslationErrorTextKey(error));
                return status;
            }
            this->translation_->probeRequest = status.requestId;
            status.running = true;
            status.text = GetUiText("translation.test.started");
        }
        catch (...)
        {
            status.text = GetUiText(TranslationErrorTextKey(TranslationError::InvalidConfiguration));
        }
        return status;
    };
    // 只展示当前编辑请求的安全快照；不转存响应、不记录日志、不读取用户配置。
    // 入参：id 为编辑器借用的请求身份。返回：匹配请求的可复制文本。
    callbacks.translationTestStatus = [this](std::uint64_t id)
    {
        SettingsTranslationTestStatus status;
        status.requestId = id;
        status.text = GetUiText("translation.test.cancelled");
        try
        {
            if (!id || !this->translation_ || this->translation_->probeRequest != id || !this->translation_->client)
                return status;
            const TranslationSnapshot snapshot = this->translation_->client->Snapshot();
            if (snapshot.requestId != id)
                return status;
            status.running = snapshot.busy;
            status.text =
                GetUiText(status.running                                  ? "translation.test.started"
                          : snapshot.phase == TranslationPhase::Succeeded ? "translation.test.completed"
                          : snapshot.phase == TranslationPhase::Cancelled ? "translation.test.cancelled"
                                                                          : TranslationErrorTextKey(snapshot.error));
            for (const TranslationAttempt& attempt : snapshot.attempts)
            {
                if (attempt.error != TranslationError::None)
                    status.text += L"\r\n" + GetUiText(TranslationErrorTextKey(attempt.error));
                if (!attempt.diagnostic)
                    continue;
                const TranslationDiagnostic& diagnostic = *attempt.diagnostic;
                status.text += L"\r\n" + GetUiText("translation.test.elapsed") + L": " +
                               std::to_wstring(diagnostic.elapsedMilliseconds) + L" ms";
                if (diagnostic.httpStatus)
                    status.text +=
                        L"\r\n" + GetUiText("translation.test.http") + L": " + std::to_wstring(diagnostic.httpStatus);
                if (diagnostic.systemError)
                    status.text += L"\r\n" + GetUiText("translation.test.system") + L": " +
                                   std::to_wstring(diagnostic.systemError);
                if (!diagnostic.providerCode.empty())
                    status.text += L"\r\n" + GetUiText("translation.test.provider_code") + L": " +
                                   DiagnosticDisplay(diagnostic.providerCode);
                if (!diagnostic.providerMessage.empty())
                    status.text += L"\r\n" + GetUiText("translation.test.provider_message") + L": " +
                                   DiagnosticDisplay(diagnostic.providerMessage);
                if (!diagnostic.response.empty())
                    status.text += L"\r\n" + GetUiText("translation.test.response") + L":\r\n" +
                                   DiagnosticDisplay(diagnostic.response);
            }
            if (snapshot.phase == TranslationPhase::Succeeded && snapshot.result)
            {
                std::wstring translated = Utf8ToWide(snapshot.result->text);
                if (!snapshot.attempts.empty() && snapshot.attempts.back().diagnostic &&
                    !snapshot.attempts.back().diagnostic->translatedText.empty())
                    translated = DiagnosticDisplay(snapshot.attempts.back().diagnostic->translatedText);
                status.text += L"\r\n" + GetUiText("translation.test.result") + L":\r\n" + translated;
            }
            if (!status.running && !this->shuttingDown_)
                KillTimer(this->messageWindow_, TranslationPollTimer);
        }
        catch (...)
        {
            status.text = GetUiText(TranslationErrorTextKey(TranslationError::Unavailable));
        }
        return status;
    };
    // 关闭或取消编辑器只撤销它拥有的请求，不影响另一文字窗口后续提交的任务。
    // 入参：id 为原测试身份。返回：无。
    callbacks.cancelTranslationTest = [this](std::uint64_t id)
    {
        if (!id || !this->translation_ || id != this->translation_->probeRequest || !this->translation_->client)
            return;
        this->translation_->probeRequest = 0;
        if (this->translation_->client->Snapshot().requestId == id)
            this->translation_->client->Cancel();
        if (!this->shuttingDown_)
            KillTimer(this->messageWindow_, TranslationPollTimer);
    };
    // 按领域目录提供本地化候选，不在Settings复制合法值。
    // 入参：key为领域字段。返回：窗口自有选项。
    callbacks.translationChoices = [](std::string_view key)
    {
        std::vector<SettingsOption> result;
        for (const std::string_view value : TranslationChoices(key))
            result.push_back({std::string(value), GetUiText("translation.choice." + std::string(value))});
        return result;
    };
    // 新条目由领域创建独立稳定身份。入参：kind为类型。返回：草稿条目。
    callbacks.createTranslationProfile = [](std::string_view kind)
    {
        const std::optional<nlohmann::json> defaults = GetDefaultJsonSetting("translation.interfaces");
        if (!defaults)
            throw std::runtime_error("Translation default configuration is unavailable");
        return CreateTranslationProfile(kind, *defaults);
    };
    // 将领域字段适配到普通子表单描述。入参：kind为类型。返回：无业务依赖的字段。
    callbacks.translationProfileFields = [](std::string_view kind, const nlohmann::json& localQualityPresets)
    {
        std::vector<SettingsTranslationField> result;
        for (const TranslationProfileField& field : TranslationProfileFields(kind))
        {
            SettingsTranslationField item{std::string(field.key), field.secret, field.multiline, {}};
            if (kind == "ctranslate2_local" && field.key == "quality")
            {
                for (const std::string& value : LocalQualityChoices(localQualityPresets))
                    item.options.push_back({value, GetUiText("translation.choice." + value)});
            }
            else
                for (const std::string_view value :
                     TranslationChoices(std::string(kind) + "." + std::string(field.key)))
                    item.options.push_back({std::string(value), GetUiText("translation.choice." + std::string(value))});
            result.push_back(std::move(item));
        }
        return result;
    };
    // 列表验证只返回固定分类，凭据不进入诊断。入参：value为列表。返回：安全错误或空。
    callbacks.validateTranslationInterfaces = [](const nlohmann::json& value)
    {
        const TranslationError error = ValidateTranslationInterfaces(value);
        return error == TranslationError::None ? std::wstring{} : GetUiText(TranslationErrorTextKey(error));
    };
    // 子表单复用领域结构验证。入参：value为条目。返回：安全错误或空。
    callbacks.validateTranslationProfile = [](const nlohmann::json& value)
    {
        const TranslationError error = ValidateTranslationProfile(value);
        return error == TranslationError::None ? std::wstring{} : GetUiText(TranslationErrorTextKey(error));
    };
    // 简单字段与执行共享规则。入参：key/value为字段及草稿。返回：安全错误或空。
    callbacks.validateTranslationField = [](std::string_view key, std::string_view value)
    {
        const TranslationError error = ValidateTranslationField(key, value);
        return error == TranslationError::None ? std::wstring{} : GetUiText(TranslationErrorTextKey(error));
    };
    // 校验一次原子提交的组合，不重读磁盘或发起请求。
    // 入参：values为全部设置候选。返回：安全错误或空。
    callbacks.validateTranslationSettings = [](const nlohmann::json& values)
    {
        try
        {
            const TranslationSettings settings = ReadTranslationSettings(
                // 仅借用本次候选值。入参：key为领域字段。返回：独立值或缺失。
                [&values](std::string_view key) -> std::optional<nlohmann::json>
                {
                    const auto found = values.find(key);
                    return found == values.end() ? std::nullopt : std::optional<nlohmann::json>(*found);
                });
            const TranslationError error = ValidateTranslationConfiguration(settings.configuration, settings.options);
            return error == TranslationError::None ? std::wstring{} : GetUiText(TranslationErrorTextKey(error));
        }
        catch (...)
        {
            return GetUiText(TranslationErrorTextKey(TranslationError::InvalidConfiguration));
        }
    };
    // 提示本地边界或显式HTTP风险，不在UI线程加载模型。
    // 入参：profile为条目。返回：安全说明。
    callbacks.translationProfileStatus = [](const nlohmann::json& profile)
    {
        if (profile.value("kind", "") == "ctranslate2_local")
        {
            const TranslationError status = QueryLocalTranslationModelFiles(GetExecutableDirectory());
            return GetUiText("translation.local_help") + L"\n" +
                   GetUiText(status == TranslationError::None ? "translation.models_present"
                                                              : TranslationErrorTextKey(status));
        }
        return GetUiText(TranslationProfileUsesInsecureHttp(profile) ? "translation.http_warning"
                                                                     : "translation.online_help");
    };
    // 设置成功应用后撤销旧任务，下一次明确操作才重新发送。入参：无。返回：无。
    callbacks.translationSettingsApplied = [this]() noexcept
    {
        if (this->textSession_)
            this->textSession_->ConfigurationChanged();
    };
#endif
#ifdef OPEN_ST_HAS_OCR
    callbacks.ocrAvailable = true;
    // 从领域模块取得唯一模型档位，界面只负责本地化。
    // 入参：无。返回：模型选项。
    callbacks.ocrModels = []()
    {
        std::vector<SettingsOption> result;
        for (auto value : OcrModelChoices())
            result.push_back({std::string(value), GetUiText("ocr.model." + std::string(value))});
        return result;
    };
    // 从领域模块取得语言组合，避免窗口复制合法值列表。
    // 入参：无。返回：语言选项。
    callbacks.ocrLanguages = []()
    {
        std::vector<SettingsOption> result;
        for (auto value : OcrLanguageChoices())
            result.push_back({std::string(value), GetUiText("ocr.language." + std::string(value))});
        return result;
    };
    // 复用领域校验，Settings不链接OCR。
    // 入参：model/language为草稿。返回：组合合法为true。
    callbacks.validOcrOptions = [](std::string_view model, std::string_view language)
    { return AreOcrOptionsValid({std::string(model), std::string(language)}); };
#endif
    // 打开实际日志目录，不应用设置草稿。
    // 入参：无。
    // 返回：系统接受目录打开请求时为 true。
    callbacks.openLogDirectory = [this]() { return this->OpenLogDirectory(); };
    // 在确认后请求一次历史清理，工作线程由 App 持有。
    // 入参：无。
    // 返回：成功启动时为 true。
    callbacks.clearHistoricalLogs = [this]() { return this->StartHistoricalLogCleanup(); };
    // 查询清理状态，关闭或重开设置窗口不会丢失结果。
    // 入参：无。
    // 返回：忙状态和当前语言文字。
    callbacks.maintenanceStatus = [this]() { return this->QueryLogMaintenanceStatus(); };
    // 为设置页共享颜色规范化规则，仅显式提交使用规范值。
    // 入参：value 为原始颜色文本。
    // 返回：合法规范值或空值。
    callbacks.normalizeBorderColor = [](std::string_view value) { return NormalizeSelectionBorderColor(value); };
    // 校验文件格式 token，不依赖下拉框排列。
    // 入参：value 为格式文本。
    // 返回：产品支持时为 true。
    callbacks.validImageFormat = [](std::string_view value) { return IsImageFormatSetting(value); };
    // 与输出链共享质量约束。
    // 入参：value 为 JPEG 质量。
    // 返回：合法为 true。
    callbacks.validJpegQuality = [](std::int64_t value) { return IsJpegQualitySetting(value); };
    // 通过唯一领域校验入口适配截图默认参数。
    // 入参：key为设置键，value为整数。返回：合法为true。
    callbacks.validCaptureVisualSetting = [](std::string_view key, std::int64_t value)
    { return IsCaptureVisualSetting(key, value); };
    // 为子窗口提供指定键的当前语言文本。
    // 入参：key：布局或工具栏请求的本地化文本键。
    // 返回：GetUiText 返回的本地化宽字符串。
    callbacks.text = [](std::string_view key)
    {
        if (key == "welcome.intro")
        {
            std::optional<std::string> value = GetStringSetting("capture.hotkey");
            HotkeyChord chord;
            if (!value || !ParseHotkey(*value, chord))
                value = GetDefaultStringSetting("capture.hotkey");
            const std::wstring display = value && ParseHotkey(*value, chord)
                                             ? std::wstring(value->begin(), value->end())
                                             : GetUiText("hotkey.unregistered");
            return GetUiText(key, {{L"hotkey", display}});
        }
        return GetUiText(key);
    };
    // 向设置窗口提供当前生效的语言代码。
    // 入参：无。
    // 返回：当前运行时语言代码，用于设置缺失时的默认选择。
    callbacks.currentLanguage = []() { return CurrentUiLanguageCode(); };
    // 向设置窗口提供当前可用的语言列表。
    // 入参：无。
    // 返回：文本资源可用时返回语言代码列表；不可用时返回空列表。
    callbacks.availableLanguages = []()
    { return IsUiTextAvailable() ? GetAvailableUiLanguages() : std::vector<std::string>{}; };
    // 在设置保存后切换运行语言并刷新已有界面。
    // 入参：language：已提交的语言代码。
    // 返回：SetUiLanguage 的应用结果；切换成功 true，失败 false，并执行界面刷新。
    callbacks.languageApplied = [this](std::string_view language)
    {
        const bool applied = SetUiLanguage(language);
        this->RefreshLocalizedUi();
        return applied;
    };
    callbacks.largeIcon = this->largeIcon_;
    callbacks.smallIcon = this->smallIcon_;
    // 把已保存的启动项开关应用到系统。
    // 入参：enabled：用户已提交的启动项启用状态。
    // 返回：ApplyStartup 的执行结果，成功 true，失败 false。
    callbacks.startupApplied = [this](bool enabled) { return this->ApplyStartup(enabled); };
    // 为设置页面查询当前启动项状态文字。
    // 入参：无显式入参；捕获 App 查询系统集成状态。
    // 返回：当前语言的启动状态宽字符串。
    callbacks.startupStatus = [this]() { return this->StartupStatusText(); };
    // 同步设置窗口忙状态以控制截图请求准入。
    // 入参：busy：设置窗口是否正在执行需阻止截图的操作。
    // 返回：无返回值；更新 settingsBusy_ 并刷新截图门禁。
    callbacks.busyChanged = [this](bool busy)
    {
        this->settingsBusy_ = busy;
        this->UpdateCaptureGate();
    };
    // 将配置解析为 Settings 自有数值类型。
    // 入参：value 为组合 token。
    // 返回：有效组合或空值。
    callbacks.hotkeyDecode = [](std::string_view value) -> std::optional<SettingsHotkeyChord>
    {
        HotkeyChord chord;
        if (!ParseHotkey(value, chord))
            return std::nullopt;
        return SettingsHotkeyChord{chord.modifiers, chord.key};
    };
    // 将录入值验证并序列化为稳定配置。
    // 入参：value 为 Settings 录入组合。
    // 返回：规范 token；非法组合为空。
    callbacks.hotkeyEncode = [](SettingsHotkeyChord value) -> std::optional<std::string>
    {
        const HotkeyChord chord{value.modifiers, value.key};
        if (!IsSupportedHotkey(chord))
            return std::nullopt;
        return SerializeHotkey(chord);
    };
    // 格式化组合预览，尚未完成的组合显示本地化录入提示。
    // 入参：value 为当前预览值。
    // 返回：可显示组合或录入提示。
    callbacks.hotkeyFormat = [](SettingsHotkeyChord value)
    {
        const std::string token = SerializeHotkey({value.modifiers, value.key});
        return token.empty() ? GetUiText("settings.hotkey.record") : std::wstring(token.begin(), token.end());
    };
    // 在保存前保留旧注册并准备新组合。
    // 入参：value 为目标 token。
    // 返回：候选准备成功为 true。
    callbacks.hotkeyPrepare = [this](std::string_view value) { return this->PrepareHotkey(value); };
    // 将实际注册与目标组合比较，决定无草稿变动时是否仍允许应用。
    // 入参：value 为当前草稿或保存目标。
    // 返回：没有活动注册、组合不同或释放待重试时为 true。
    callbacks.hotkeyNeedsApply = [this](std::string_view value)
    {
        HotkeyChord chord;
        if (!this->hotkeys_ || this->hotkeyCleanupPending_ || !ParseHotkey(value, chord))
            return true;
        return this->hotkeys_->Active() != chord;
    };
    // 完成无异常注册切换，不执行界面刷新。
    // 入参：commit 为配置是否已保存。
    // 返回：清理成功为 true。
    callbacks.hotkeyFinish = [this](bool commit) noexcept { return this->FinishHotkey(commit); };
    // 查询实际活动与清理状态。
    // 入参：无。
    // 返回：当前语言状态。
    callbacks.hotkeyStatus = [this]() { return this->HotkeyStatusText(); };
    // 同步录入暂停及前后排队消息边界。
    // 入参：recording 为是否开始录入。
    // 返回：无。
    callbacks.hotkeyRecording = [this](bool recording)
    {
        this->hotkeyRecording_ = recording;
        this->UpdateCaptureGate();
    };
    // 在事务后刷新托盘和已有窗口文字。
    // 入参：无。
    // 返回：无。
    callbacks.hotkeyRefresh = [this]() { this->RefreshLocalizedUi(); };
    return callbacks;
}

// 创建供截图工具栏同步查询本地化文本的回调。
// 入参：无。
// 返回：文本查询函数；在 UI 线程调用，本地化服务须在调用期间可用。
std::function<std::wstring(std::string_view)> App::MakeToolbarTextResolver()
{
    // 查询工具栏指定文本键对应的当前语言文字。
    // 入参：key：工具栏请求的本地化文本键。
    // 返回：GetUiText 返回的本地化宽字符串。
    return [](std::string_view key) { return GetUiText(key); };
}

// 创建供截图工具栏投递业务命令的回调。
// 入参：无。
// 返回：UI 线程命令接收函数；捕获的 App 须存活到工具栏解除回调。
std::function<bool(CaptureToolbarCommand, std::uint64_t)> App::MakeToolbarCommandHandler()
{
    // 把工具栏命令转交 App 消息队列执行。
    // 入参：command：按钮稳定命令 ID；token：当前选区代次；捕获 App。
    // 返回：预订并投递成功 true；状态不允许或投递失败 false，不代表业务已执行成功。
    return [this](CaptureToolbarCommand command, std::uint64_t token)
    { return this->PostToolbarCommand(command, token); };
}

// 创建供单实例接收线程检查截图准入状态的回调。
// 入参：无。
// 返回：仅读取原子门禁的查询函数；App 须先停止并等待监听线程，再释放自身资源。
std::function<std::optional<LPARAM>()> App::MakeCaptureGateCallback()
{
    // 向实例接收线程提供当前截图请求是否可进入的判断。
    // 入参：无显式入参；捕获 this，原子读取应用截图门禁。
    // 返回：暂停时 nullopt；允许时返回当前代次，作为排队请求的校验令牌。
    return [this]() -> std::optional<LPARAM>
    {
        const std::uint64_t gate = this->captureGate_.load(std::memory_order_acquire);
        return (gate & 1) != 0 ? std::nullopt : std::optional<LPARAM>(static_cast<LPARAM>(gate));
    };
}
} // namespace open_st
