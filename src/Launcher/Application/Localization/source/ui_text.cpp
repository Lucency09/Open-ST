// 加载并验证多语言资源，维护生效语言并实现英文回退和文本参数替换。

#include <ui_text.h>

#include "ui_text_internal.h"

#include <json_file.h>
#include <log.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <windows_util.h>

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

// 验证语言代码能否安全显示在语言选择列表中。
// 入参：languageCode：待校验的语言代码视图。
// 返回：非空且仅含 ASCII 字母、数字、连字符时 true；否则 false，不执行完整 BCP-47 语法校验。
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

// 把资源中的 UTF-8 文本转换为 Win32 使用的宽字符串。
// 入参：text：借用的 UTF-8 文本。
// 返回：成功返回 UTF-16 文本；空输入、长度过大、无效编码或系统转换失败返回空字符串。
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

// 验证本地化文档外层协议及动态语言和文本键结构。
// 入参：document：只读候选 JSON 文档。
// 返回：schemaVersion、languages、texts 结构合法且声明 en-US 时 true；非法或异常时 false。
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
        const nlohmann::json::const_iterator languagesIterator = document.find("languages");
        if (schemaIterator == document.end() || !schemaIterator->is_number_integer() ||
            *schemaIterator != RESOURCE_SCHEMA_VERSION || textsIterator == document.end() ||
            !textsIterator->is_object() || textsIterator->empty() || languagesIterator == document.end() ||
            !languagesIterator->is_array() || languagesIterator->empty())
        {
            return false;
        }
        std::set<std::string> languageSet;
        for (const nlohmann::json& language : *languagesIterator)
        {
            if (!language.is_string())
            {
                return false;
            }
            const std::string code = language.get<std::string>();
            if (!IsLanguageCode(code) || !languageSet.insert(code).second)
            {
                return false;
            }
        }
        if (!languageSet.contains(std::string(DEFAULT_LANGUAGE_CODE)))
        {
            return false;
        }
        for (nlohmann::json::const_iterator textIterator = textsIterator->begin(); textIterator != textsIterator->end();
             ++textIterator)
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

// 检查资源是否声明指定的运行语言。
// 入参：document：已通过外层校验的资源文档；languageCode：待查语言代码。
// 返回：languages 中存在精确匹配项时 true；不存在或访问异常时 false。
bool ContainsLanguage(const nlohmann::json& document, std::string_view languageCode) noexcept
{
    try
    {
        for (const nlohmann::json& language : document.at("languages"))
        {
            if (language.get_ref<const std::string&>() == languageCode)
            {
                return true;
            }
        }
    }
    catch (...)
    {
    }
    return false;
}

class UiTextState final
{
  public:
    // 建立本地化资源的懒加载句柄并重置运行语言。
    // 入参：applicationDirectory：包含 resources/ui_text.json 的应用目录。
    // 返回：取得有效 JSON 句柄时 true，不表示资源内容已读入；句柄获取或初始化异常时 false。
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

    // 释放本地化句柄和已接受的业务文本，重置语言与告警状态。
    // 入参：无。
    // 返回：无返回值；恢复默认 en-US，不清理 Common 管理器的缓存或名称绑定。
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

    // 消费一次需要由应用显示的资源读取或结构错误告警。
    // 入参：无。
    // 返回：原本有待提示告警时 true，否则 false；仅清除待提示标记，持续故障仍保持去重。
    bool ConsumeReadWarning() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return std::exchange(this->readWarningPending_, false);
    }

    // 将运行界面语言切换为当前资源声明的语言。
    // 入参：languageCode：拟生效的语言代码。
    // 返回：支持该代码时 true 并切换；不支持或异常时 false 并回退 en-US。
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

    // 查询当前生效的运行语言。
    // 入参：无。
    // 返回：当前语言代码的独立字符串副本，查询本身不重新读取资源。
    std::string LanguageCode() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return this->languageCode_;
    }

    // 尝试读取当前资源并判断是否存在可显示的业务文本。
    // 入参：无。
    // 返回：存在当前或先前已接受的有效文档时 true；没有有效文档时 false，并非本次磁盘读取成功标志。
    bool Available() noexcept
    {
        return this->Document() != nullptr;
    }

    // 获取供设置窗口使用的动态语言选项。
    // 入参：无。
    // 返回：按已接受文档 languages 顺序返回代码列表；没有有效文档或提取异常时返回空列表。
    std::vector<std::string> Languages() noexcept
    {
        try
        {
            const std::shared_ptr<const nlohmann::json> document = this->Document();
            if (document != nullptr)
            {
                return document->at("languages").get<std::vector<std::string>>();
            }
        }
        catch (...)
        {
        }
        return {};
    }

    // 按当前运行语言查询界面文本并应用英文回退。
    // 入参：key：JSON texts 中的动态文本键。
    // 返回：当前语言非空文本；缺失时尝试 en-US；无有效文本或无法转换时返回问号宽字符串。
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
            OPEN_ST_LOG_WARNING("A localized value is missing; using en-US. key=", key, " language=", languageCode);
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
    // 读取并验证本地化业务文档，在读取故障时保留已生效文本。
    // 入参：无。
    // 返回：共享的不可变有效文档；读取或结构失败保留先前文档，尚无有效文档或未初始化时 nullptr。
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
            // JSON 数值相等会把 1 与 1.0 视为相同，复用校验前仍需确认协议版本是整数。
            const nlohmann::json::const_iterator schemaIterator = document.find("schemaVersion");
            const bool unchanged = this->validatedDocument_ != nullptr && schemaIterator != document.end() &&
                                   schemaIterator->is_number_integer() && *this->validatedDocument_ == document;
            if (unchanged || IsUiTextDocument(document))
            {
                if (!unchanged)
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

// 取得进程唯一的本地化状态对象。
// 入参：无。
// 返回：静态状态对象引用，生命周期持续到进程退出，调用方不负责释放。
UiTextState& GetUiTextState()
{
    static UiTextState state;
    return state;
}
} // namespace

namespace open_st
{
// 以当前可执行文件目录为基准建立本地化懒加载句柄。
// 入参：无。
// 返回：目录查询及句柄初始化成功时 true；失败或异常时 false，初始化不读取内容，后续查询或可用性检查时读取。
bool InitializeUiText() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = GetExecutableDirectory();
        return !applicationDirectory.empty() && InitializeUiText(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

// 建立本地化资源的懒加载句柄并重置运行语言。
// 入参：applicationDirectory：包含 resources/ui_text.json 的应用目录。
// 返回：取得有效 JSON 句柄时 true，不表示资源内容已读入；句柄获取或初始化异常时 false。
bool InitializeUiText(const std::filesystem::path& applicationDirectory) noexcept
{
    return GetUiTextState().Initialize(applicationDirectory);
}

// 释放本地化句柄和已接受的业务文本，重置语言与告警状态。
// 入参：无。
// 返回：无返回值；恢复默认 en-US，不清理 Common 管理器的缓存或名称绑定。
void ShutdownUiText() noexcept
{
    GetUiTextState().Shutdown();
}

// 尝试读取当前资源并判断是否存在可显示的业务文本。
// 入参：无。
// 返回：存在当前或先前已接受的有效文档时 true；没有有效文档时 false，并非本次磁盘读取成功标志。
bool IsUiTextAvailable() noexcept
{
    return GetUiTextState().Available();
}

// 消费一次需要由应用显示的资源读取或结构错误告警。
// 入参：无。
// 返回：原本有待提示告警时 true，否则 false；仅清除待提示标记，持续故障仍保持去重。
bool ConsumeUiTextReadWarning() noexcept
{
    return GetUiTextState().ConsumeReadWarning();
}

// 将运行界面语言切换为当前资源声明的语言。
// 入参：languageCode：拟生效的语言代码。
// 返回：支持该代码时 true 并切换；不支持或异常时 false 并回退 en-US。
bool SetUiLanguage(std::string_view languageCode) noexcept
{
    return GetUiTextState().SetLanguage(languageCode);
}

// 查询当前生效的运行语言。
// 入参：无。
// 返回：当前语言代码的独立字符串副本，查询本身不重新读取资源。
std::string CurrentUiLanguageCode() noexcept
{
    return GetUiTextState().LanguageCode();
}

// 获取供设置窗口使用的动态语言选项。
// 入参：无。
// 返回：按已接受文档 languages 顺序返回代码列表；没有有效文档或提取异常时返回空列表。
std::vector<std::string> GetAvailableUiLanguages() noexcept
{
    return GetUiTextState().Languages();
}

// 取得本地化界面文本并替换调用方指定的命名占位符。
// 入参：key：动态文本键；arguments：占位符 name 和替换 value，仅在本次调用借用，空名称忽略。
// 返回：按参数顺序完成 {name} 替换的宽字符串；文本查找失败保留问号结果，未指定占位符不替换。
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
