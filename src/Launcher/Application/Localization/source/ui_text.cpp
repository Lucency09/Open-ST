#include <ui_text.h>

#include "ui_text_internal.h"

#include <json_file.h>
#include <log.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr int RESOURCE_SCHEMA_VERSION = 1;
constexpr std::string_view UI_TEXT_CARD_NAME = "localization.ui_text";
constexpr std::string_view DEFAULT_LANGUAGE_CODE = "en-US";

// 语言下拉框直接显示代码，因此只接受可安全显示的 ASCII BCP-47 风格字符。
bool IsLanguageCode(std::string_view languageCode) noexcept
{
    if (languageCode.empty())
    {
        return false;
    }
    for (const char character : languageCode)
    {
        const bool letter = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '-')
        {
            return false;
        }
    }
    return true;
}

// 把 JSON 中的 UTF-8 文本转换为 Win32 W 接口所需的 UTF-16 宽字符串。
std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    const int inputLength = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), inputLength, nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring converted(static_cast<std::size_t>(length), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), inputLength, converted.data(), length);
    return written == length ? converted : std::wstring{};
}

// 只校验本地化资源的固定外层协议；文本 key 和语言代码全部由 JSON 动态提供。
bool IsUiTextDocument(const nlohmann::json& document) noexcept
{
    try
    {
        if (!document.is_object())
        {
            return false;
        }
        const nlohmann::json::const_iterator schemaIterator = document.find("schemaVersion");
        const nlohmann::json::const_iterator textsIterator = document.find("texts");
        if (schemaIterator == document.end() || !schemaIterator->is_number_integer() ||
            schemaIterator->get<int>() != RESOURCE_SCHEMA_VERSION || textsIterator == document.end() ||
            !textsIterator->is_object() || textsIterator->empty())
        {
            return false;
        }
        for (nlohmann::json::const_iterator textIterator = textsIterator->begin();
             textIterator != textsIterator->end(); ++textIterator)
        {
            if (textIterator.key().empty() || !textIterator->is_object())
            {
                return false;
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 判断一份已通过外层协议校验的资源是否仍声明指定动态语言。
bool ContainsLanguage(const nlohmann::json& document, std::string_view languageCode) noexcept
{
    try
    {
        const nlohmann::json& texts = document.at("texts");
        for (nlohmann::json::const_iterator textIterator = texts.begin(); textIterator != texts.end(); ++textIterator)
        {
            if (textIterator->is_object())
            {
                const nlohmann::json::const_iterator languageIterator = textIterator->find(languageCode);
                if (languageIterator != textIterator->end() && languageIterator->is_string())
                {
                    return true;
                }
            }
        }
    }
    catch (...)
    {
    }
    return false;
}

// 获取程序目录，作为正式运行时 resources/ 的路径基准。
std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> pathBuffer{};
    const DWORD length = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length >= static_cast<DWORD>(pathBuffer.size()))
    {
        return {};
    }
    return std::filesystem::path(std::wstring_view(pathBuffer.data(), length)).parent_path();
}

class UiTextState final
{
  public:
    // 只取得具名 JSON 句柄并记录资源位置，不在初始化阶段读取文件。
    bool Initialize(const std::filesystem::path& applicationDirectory) noexcept
    {
        this->Shutdown();
        try
        {
            const std::filesystem::path resourcePath = applicationDirectory / L"resources" / L"ui_text.json";
            open_st::JsonFileHandle file =
                open_st::JsonFileManager::Instance().GetFile(UI_TEXT_CARD_NAME, resourcePath);
            if (!file.IsValid())
            {
                return false;
            }

            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->file_ = std::move(file);
            this->languageCode_ = DEFAULT_LANGUAGE_CODE;
            this->initialized_ = true;
            return true;
        }
        catch (...)
        {
            this->Shutdown();
            return false;
        }
    }

    // 释放模块句柄和已生效业务文本，不干预 Common 的缓存或名称绑定。
    void Shutdown() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        this->file_ = {};
        this->validatedDocument_.reset();
        this->rejectedStructure_ = false;
        this->readFailed_ = false;
        this->readWarningPending_ = false;
        this->languageCode_ = DEFAULT_LANGUAGE_CODE;
        this->initialized_ = false;
    }

    // 消费一次业务读取告警，保留持续故障标记以免 UI 连续弹窗。
    bool ConsumeReadWarning() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return std::exchange(this->readWarningPending_, false);
    }

    // 从当前资源动态判断语言是否存在；不再维护固定语言代码列表。
    bool SetLanguage(std::string_view languageCode) noexcept
    {
        try
        {
            const std::vector<std::string> languages = this->Languages();
            bool supported = false;
            for (const std::string& language : languages)
            {
                if (language == languageCode)
                {
                    supported = true;
                    break;
                }
            }

            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->languageCode_ = supported ? std::string(languageCode) : std::string(DEFAULT_LANGUAGE_CODE);
            return supported;
        }
        catch (...)
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->languageCode_ = DEFAULT_LANGUAGE_CODE;
            return false;
        }
    }

    // 在线程同步保护下返回当前语言代码的独立副本。
    std::string LanguageCode() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return this->languageCode_;
    }

    // 发起一次真实读取并确认当前或最后有效的本地化业务快照可用。
    bool Available() noexcept
    {
        return this->Document() != nullptr;
    }

    // 扫描所有文本对象的属性名，以稳定排序后的动态语言代码列表返回给设置窗口。
    std::vector<std::string> Languages() noexcept
    {
        try
        {
            const std::shared_ptr<const nlohmann::json> document = this->Document();
            std::set<std::string> languageSet;
            languageSet.emplace(DEFAULT_LANGUAGE_CODE);
            if (document != nullptr)
            {
                const nlohmann::json& texts = document->at("texts");
                for (nlohmann::json::const_iterator textIterator = texts.begin(); textIterator != texts.end();
                     ++textIterator)
                {
                    if (!textIterator->is_object())
                    {
                        continue;
                    }
                    for (nlohmann::json::const_iterator languageIterator = textIterator->begin();
                         languageIterator != textIterator->end(); ++languageIterator)
                    {
                        if (IsLanguageCode(languageIterator.key()) && languageIterator->is_string())
                        {
                            languageSet.emplace(languageIterator.key());
                        }
                    }
                }
            }
            return std::vector<std::string>(languageSet.begin(), languageSet.end());
        }
        catch (...)
        {
            return {std::string(DEFAULT_LANGUAGE_CODE)};
        }
    }

    // 每次查询先让 Common 句柄检查磁盘版本，再按当前语言读取并执行英文回退。
    std::wstring Text(std::string_view key)
    {
        const std::shared_ptr<const nlohmann::json> document = this->Document();
        if (document == nullptr)
        {
            return L"?";
        }

        const std::string languageCode = this->LanguageCode();
        std::string selectedText;
        bool usedEnglishFallback = false;
        try
        {
            const nlohmann::json& texts = document->at("texts");
            const nlohmann::json::const_iterator textIterator = texts.find(key);
            if (textIterator != texts.end() && textIterator->is_object())
            {
                const nlohmann::json::const_iterator selectedIterator = textIterator->find(languageCode);
                if (selectedIterator != textIterator->end() && selectedIterator->is_string())
                {
                    selectedText = selectedIterator->get<std::string>();
                }
                if (selectedText.empty() && languageCode != DEFAULT_LANGUAGE_CODE)
                {
                    const nlohmann::json::const_iterator englishIterator = textIterator->find(DEFAULT_LANGUAGE_CODE);
                    if (englishIterator != textIterator->end() && englishIterator->is_string())
                    {
                        selectedText = englishIterator->get<std::string>();
                        usedEnglishFallback = !selectedText.empty();
                    }
                }
            }
        }
        catch (...)
        {
            selectedText.clear();
        }

        if (usedEnglishFallback)
        {
            OPEN_ST_LOG_WARNING("A localized value is missing; using en-US. key=", key,
                                " language=", languageCode);
        }
        const std::wstring converted = Utf8ToWide(selectedText);
        if (converted.empty())
        {
            OPEN_ST_LOG_ERROR("A UI text key could not be resolved. key=", key, " language=", languageCode);
            return L"?";
        }
        return converted;
    }

  private:
    // 串行读取、校验并接受业务文本；失败时只保留已生效文本，不接触 Common 缓存状态。
    std::shared_ptr<const nlohmann::json> Document() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        if (!this->initialized_)
        {
            return nullptr;
        }
        try
        {
            nlohmann::json document;
            if (!this->file_.Read(document))
            {
                if (!this->readFailed_)
                {
                    this->readWarningPending_ = true;
                }
                this->readFailed_ = true;
                return this->validatedDocument_;
            }
            this->readFailed_ = false;
            if (IsUiTextDocument(document))
            {
                if (this->validatedDocument_ == nullptr || *this->validatedDocument_ != document)
                {
                    this->validatedDocument_ = std::make_shared<const nlohmann::json>(std::move(document));
                }
                this->rejectedStructure_ = false;
                if (this->languageCode_ != DEFAULT_LANGUAGE_CODE &&
                    !ContainsLanguage(*this->validatedDocument_, this->languageCode_))
                {
                    this->languageCode_ = DEFAULT_LANGUAGE_CODE;
                }
            }
            else if (!this->rejectedStructure_)
            {
                this->rejectedStructure_ = true;
                this->readWarningPending_ = true;
                OPEN_ST_LOG_ERROR("The UI text JSON structure is invalid; keeping the last valid resource.");
            }
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("The UI text resource could not be accepted; keeping the last valid resource.");
        }
        return this->validatedDocument_;
    }

    std::mutex mutex_;
    open_st::JsonFileHandle file_;
    std::shared_ptr<const nlohmann::json> validatedDocument_;
    std::string languageCode_{DEFAULT_LANGUAGE_CODE};
    bool rejectedStructure_{};
    bool readFailed_{};
    bool readWarningPending_{};
    bool initialized_{};
};

// 返回进程内唯一的本地化状态对象。
UiTextState& GetUiTextState()
{
    static UiTextState state;
    return state;
}
} // namespace

namespace open_st
{
// 以可执行文件目录为基准建立正式运行时的本地化 JSON 句柄。
bool InitializeUiText() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = ExecutableDirectory();
        return !applicationDirectory.empty() && InitializeUiText(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

// 以调用方提供的目录建立句柄，供隔离测试使用；此调用本身不读取资源文件。
bool InitializeUiText(const std::filesystem::path& applicationDirectory) noexcept
{
    return GetUiTextState().Initialize(applicationDirectory);
}

// 释放本地化句柄和模块状态。
void ShutdownUiText() noexcept
{
    GetUiTextState().Shutdown();
}

// 显式触发首次懒加载并报告本地化资源是否可用。
bool IsUiTextAvailable() noexcept
{
    return GetUiTextState().Available();
}

// 向 Application 提供一次性的读取故障提示，不向 UI 暴露 Common 状态。
bool ConsumeUiTextReadWarning() noexcept
{
    return GetUiTextState().ConsumeReadWarning();
}

// 切换语言；有效语言代码来自当前 ui_text.json 的动态属性集合。
bool SetUiLanguage(std::string_view languageCode) noexcept
{
    return GetUiTextState().SetLanguage(languageCode);
}

// 返回当前语言代码的副本。
std::string CurrentUiLanguageCode() noexcept
{
    return GetUiTextState().LanguageCode();
}

// 返回当前资源中动态发现的语言代码，供设置窗口生成下拉选项。
std::vector<std::string> GetAvailableUiLanguages() noexcept
{
    return GetUiTextState().Languages();
}

// 按动态 JSON key 取得宽字符串，并执行调用方明确提供的命名占位符替换。
std::wstring GetUiText(std::string_view key, std::initializer_list<UiTextArgument> arguments)
{
    std::wstring result = GetUiTextState().Text(key);
    for (const UiTextArgument& argument : arguments)
    {
        if (argument.name.empty())
        {
            continue;
        }

        std::wstring token = L"{";
        token.append(argument.name);
        token.push_back(L'}');
        std::wstring::size_type position = 0;
        while ((position = result.find(token, position)) != std::wstring::npos)
        {
            result.replace(position, token.size(), argument.value);
            position += argument.value.size();
        }
    }
    return result;
}
} // namespace open_st
