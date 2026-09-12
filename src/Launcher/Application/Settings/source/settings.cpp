// 实现默认值与用户设置合并、动态键读写及持久化状态告警。

#include <settings.h>

#include "settings_internal.h"

#include <json_file.h>
#include <log.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <windows_util.h>

#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace open_st
{
// 校验设置文档固定外层协议；settings 内部 key 不设枚举或白名单。
// 入参：document 为待校验的 JSON 文档。
// 返回：版本为 1 且 settings 成员为对象时为 true；其他结构或异常为 false。
bool IsSettingsDocument(const nlohmann::json& document) noexcept
{
    try
    {
        if (!document.is_object())
        {
            return false;
        }
        const nlohmann::json::const_iterator schemaIterator = document.find("schemaVersion");
        const nlohmann::json::const_iterator settingsIterator = document.find("settings");
        return schemaIterator != document.end() && schemaIterator->is_number_integer() && *schemaIterator == 1 &&
               settingsIterator != document.end() && settingsIterator->is_object();
    }
    catch (...)
    {
        return false;
    }
}
} // namespace open_st

namespace
{
using open_st::IsSettingsDocument;

constexpr std::string_view USER_SETTINGS_CARD_NAME = "settings.user";
constexpr std::string_view DEFAULT_SETTINGS_CARD_NAME = "settings.default";

class SettingsState final
{
  public:
    // 取得两个具名 JSON 句柄，并且只在用户文件确实不存在时从默认文档创建它。
    // 入参：applicationDirectory 为应用根目录，其 resources 和 data 子目录分别保存资源与用户设置。
    // 返回：默认设置可用且业务状态初始化成功时为 true；失败为 false。用户文件不可持久化通过独立状态查询报告。
    bool Initialize(const std::filesystem::path& applicationDirectory) noexcept
    {
        this->Shutdown();
        try
        {
            open_st::JsonFileHandle defaultFile = open_st::JsonFileManager::Instance().GetFile(
                DEFAULT_SETTINGS_CARD_NAME, applicationDirectory / L"resources" / L"default_settings.json");
            open_st::JsonFileHandle userFile = open_st::JsonFileManager::Instance().GetFile(
                USER_SETTINGS_CARD_NAME, applicationDirectory / L"data" / L"settings.json");
            if (!defaultFile.IsValid() || !userFile.IsValid())
            {
                return false;
            }

            nlohmann::json defaultDocument;
            if (!defaultFile.Read(defaultDocument) || !IsSettingsDocument(defaultDocument))
            {
                OPEN_ST_LOG_ERROR("The default settings resource is unavailable or invalid.");
                return false;
            }

            const bool persistenceAvailable = userFile.Write(
                // 仅缺失时填入默认设置；已有文档不改写，但仍由 Common 检查安全写入前提。
                // 入参：document 为 Common 锁内候选用户文档，缺失时填入捕获的默认文档。
                // 返回：填充或保留后的文档结构有效为 true；结构无效为 false，取消提交。
                [&defaultDocument](std::optional<nlohmann::json>& document)
                {
                    if (!document.has_value())
                    {
                        document = defaultDocument;
                    }
                    if (!IsSettingsDocument(*document))
                    {
                        OPEN_ST_LOG_WARNING("The user settings structure is invalid; preserving the file.");
                        return false;
                    }
                    return true;
                });

            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->defaultFile_ = std::move(defaultFile);
            this->userFile_ = std::move(userFile);
            this->resourcesDirectory_ = applicationDirectory / L"resources";
            this->persistenceAvailable_ = persistenceAvailable;
            this->initialized_ = true;
            return true;
        }
        catch (...)
        {
            this->Shutdown();
            return false;
        }
    }

    // 仅释放设置模块持有的句柄，缓存与名称绑定生命周期由 Common 管理。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Shutdown() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        this->userFile_ = {};
        this->defaultFile_ = {};
        this->layoutFile_ = {};
        this->resourcesDirectory_.clear();
        this->initialized_ = false;
        this->persistenceAvailable_ = false;
        this->userReadFailed_ = false;
        this->defaultReadFailed_ = false;
        this->readWarningPending_ = false;
    }

    // 取得业务初始化的文件入口，不向公开 Settings API 暴露 JSON 快照。
    // 入参：userFile 输出用户设置文件句柄；defaultFile 输出默认资源句柄；二者共享 Common 管理的文件状态。
    // 返回：业务已初始化并复制两个句柄时为 true；未初始化时为 false 且输出保持原值。
    bool EditFiles(open_st::JsonFileHandle& userFile, open_st::JsonFileHandle& defaultFile) noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        if (!this->initialized_)
        {
            return false;
        }
        userFile = this->userFile_;
        defaultFile = this->defaultFile_;
        return true;
    }

    // 窗口首次打开时注册布局句柄，之后每次打开仍由 Common 检查磁盘变化。
    // 入参：document 输出读取成功的窗口布局 JSON。
    // 返回：布局读取成功为 true；未初始化、文件或解析失败为 false，失败不改变 document。
    bool ReadLayout(nlohmann::json& document) noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (!this->initialized_)
            {
                return false;
            }
            if (!this->layoutFile_.IsValid())
            {
                this->layoutFile_ = open_st::JsonFileManager::Instance().GetFile(
                    "settings.layout", this->resourcesDirectory_ / L"setting_windows.json");
            }
            return this->layoutFile_.Read(document);
        }
        catch (...)
        {
            return false;
        }
    }

    // 提供给 UI 一次性的读取故障提示，不暴露底层错误原因或缓存状态。
    // 入参：无显式入参。
    // 返回：本次取走待显示读取告警时为 true；没有待显示告警为 false。
    bool ConsumeReadWarning() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return std::exchange(this->readWarningPending_, false);
    }

    // 查询设置业务当前记录的持久化可用性。
    // 入参：无显式入参。
    // 返回：最近一次初始化或成功写入记录为可持久化时为 true；未初始化或记录失败时为 false，不保证下一次写入成功。
    bool PersistenceAvailable() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return this->initialized_ && this->persistenceAvailable_;
    }

    // 按动态属性名读取字符串设置，用户值不可用时回退默认资源。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：用户配置或默认资源中的有效字符串值；均不可用或类型错误时为 std::nullopt。
    std::optional<std::string> String(std::string_view key) noexcept
    {
        // 判断当前设置值是否符合字符串读取接口要求的原始类型。
        // 入参：value 为待判型的原始 JSON 设置值。
        // 返回：value 是 JSON 字符串类型时为 true，否则为 false。
        return this->ReadValue<std::string>(key, [](const nlohmann::json& value) { return value.is_string(); });
    }

    // 按动态属性名读取布尔设置，用户值不可用时回退默认资源。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：用户配置或默认资源中的有效布尔值；均不可用或类型错误时为 std::nullopt。
    std::optional<bool> Boolean(std::string_view key) noexcept
    {
        // 判断当前设置值是否符合布尔值读取接口要求的原始类型。
        // 入参：value 为待判型的原始 JSON 设置值。
        // 返回：value 是 JSON 布尔值类型时为 true，否则为 false。
        return this->ReadValue<bool>(key, [](const nlohmann::json& value) { return value.is_boolean(); });
    }

    // 按动态属性名读取有符号整数设置，用户值不可用时回退默认资源。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：用户配置或默认资源中可由 int64_t 表示的整数；均缺失、类型错误或超范围时为 std::nullopt。
    std::optional<std::int64_t> Integer(std::string_view key) noexcept
    {
        return this->ReadValue<std::int64_t>(
            key,
            // 先验证无符号数范围，防止转为 int64_t 时溢出并阻断默认值回退。
            // 入参：value 为待校验的原始 JSON 设置值。
            // 返回：整数可由 int64_t 表示时为 true；类型错误或超出范围时为 false。
            [](const nlohmann::json& value)
            {
                return value.is_number_integer() &&
                       (!value.is_number_unsigned() ||
                        value.get<std::uint64_t>() <=
                            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
            });
    }

    // 更新动态字符串 key；编辑写入会先检查外部变化并保留未知字段。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的字符串值。
    // 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
    bool SetString(std::string_view key, std::string_view value) noexcept
    {
        try
        {
            return this->SetValue(key, std::string(value));
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("The string setting could not be prepared for writing.");
            return false;
        }
    }

    // 更新动态布尔 key。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的布尔值。
    // 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
    bool SetBoolean(std::string_view key, bool value) noexcept
    {
        return this->SetValue(key, value);
    }

    // 更新动态整数 key。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的有符号整数值。
    // 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
    bool SetInteger(std::string_view key, std::int64_t value) noexcept
    {
        return this->SetValue(key, value);
    }

  private:
    // 读取并验证设置文档，同时管理该文件连续故障的告警去重状态。
    // 入参：file 为借用的设置文件入口；document 输出读取的 JSON；failed 为此文件持续读取故障标记，在读取后更新。
    // 返回：读取成功且设置文档结构合法时为 true；否则为 false 并按故障状态请求一次告警。
    bool ReadDocument(const open_st::JsonFileHandle& file, nlohmann::json& document, bool& failed) noexcept
    {
        const bool read = file.Read(document);
        const bool valid = read && IsSettingsDocument(document);
        if (!valid && !failed)
        {
            this->readWarningPending_ = true;
            if (read)
            {
                OPEN_ST_LOG_WARNING("The settings JSON structure is invalid; using business fallback.");
            }
        }
        failed = !valid;
        return valid;
    }

    // 按用户优先、默认回退顺序读取指定类型的动态设置值。
    // 入参：key 为 settings 对象中的动态设置属性名。predicate 为原始 JSON 类型判定器；模板 Value
    // 为返回值类型，Predicate 为判定器类型。
    // 返回：用户或默认文档中符合判定的 Value
    // 副本；未初始化、读取失败或无有效字段时为 std::nullopt。
    template <typename Value, typename Predicate>
    std::optional<Value> ReadValue(std::string_view key, Predicate predicate) noexcept
    {
        try
        {
            if (key.empty())
            {
                return std::nullopt;
            }
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (!this->initialized_)
            {
                return std::nullopt;
            }

            nlohmann::json userDocument;
            if (this->ReadDocument(this->userFile_, userDocument, this->userReadFailed_))
            {
                const std::optional<Value> userValue = SettingsValue<Value>(userDocument, key, predicate);
                if (userValue.has_value())
                {
                    return userValue;
                }
            }

            nlohmann::json defaultDocument;
            return this->ReadDocument(this->defaultFile_, defaultDocument, this->defaultReadFailed_)
                       ? SettingsValue<Value>(defaultDocument, key, predicate)
                       : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    // 从单份设置文档提取指定类型字段，供用户值和默认值读取共用。
    // 入参：document 为候选设置文档；key 为动态字段名；predicate 判断原始类型；模板 Value 为转换目标类型，Predicate
    // 为判定器类型。
    // 返回：协议和字段类型匹配时返回 Value 副本；缺失或不匹配为
    // std::nullopt，转换异常由外层读取边界处理。
    template <typename Value, typename Predicate>
    static std::optional<Value> SettingsValue(const nlohmann::json& document, std::string_view key, Predicate predicate)
    {
        if (!IsSettingsDocument(document))
        {
            return std::nullopt;
        }
        const nlohmann::json& settings = document.at("settings");
        const nlohmann::json::const_iterator valueIterator = settings.find(key);
        if (valueIterator == settings.end() || !predicate(*valueIterator))
        {
            return std::nullopt;
        }
        return valueIterator->get<Value>();
    }

    // 在 Common 编辑锁保护下修改单个动态设置并更新持久化状态。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为新设置值；模板 Value 为该值的类型。
    // 返回：最新有效文档中的字段写入成功为 true；参数、状态、读取、结构或写入失败为 false。
    template <typename Value> bool SetValue(std::string_view key, Value value) noexcept
    {
        try
        {
            if (key.empty())
            {
                return false;
            }
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (!this->initialized_)
            {
                return false;
            }
            const std::string ownedKey(key);
            const bool updated = this->userFile_.Write(
                // 只修改已有且业务结构合法的文档，不在字段修改时重建缺失配置。
                // 入参：document 为 Common 提供的锁内候选文档；字段名和新值由回调捕获持有。
                // 返回：文档存在且协议合法时修改字段并返回 true；否则返回 false，取消本次提交。
                [ownedKey, value = std::move(value)](std::optional<nlohmann::json>& document)
                {
                    if (!document.has_value() || !IsSettingsDocument(*document))
                    {
                        return false;
                    }
                    (*document)["settings"][ownedKey] = value;
                    return true;
                });
            if (updated)
            {
                this->persistenceAvailable_ = true;
            }
            return updated;
        }
        catch (...)
        {
            return false;
        }
    }

    std::mutex mutex_;
    open_st::JsonFileHandle userFile_;
    open_st::JsonFileHandle defaultFile_;
    open_st::JsonFileHandle layoutFile_;
    std::filesystem::path resourcesDirectory_;
    bool initialized_{};
    bool persistenceAvailable_{};
    bool userReadFailed_{};
    bool defaultReadFailed_{};
    bool readWarningPending_{};
};

// 取得保存设置句柄、读写锁及告警状态的进程级业务对象。
// 入参：无显式入参。
// 返回：进程期唯一 SettingsState 对象的借用引用，调用方不得释放。
SettingsState& GetSettingsState()
{
    static SettingsState state;
    return state;
}
} // namespace

namespace open_st
{
// 为私有编辑会话复制已初始化的文件入口。
// 入参：userFile 输出用户设置文件句柄；defaultFile 输出默认资源句柄；二者共享 Common 管理的文件状态。
// 返回：业务已初始化并复制两个句柄时为 true；未初始化时为 false 且输出保持原值。
bool GetSettingsEditFiles(JsonFileHandle& userFile, JsonFileHandle& defaultFile) noexcept
{
    return GetSettingsState().EditFiles(userFile, defaultFile);
}

// 由 Settings 提供布局内容，Renderer 不接触路径和文件管理器。
// 入参：document 输出读取成功的窗口布局 JSON。
// 返回：布局读取成功为 true；未初始化、文件或解析失败为 false，失败不改变 document。
bool ReadSettingsLayout(nlohmann::json& document) noexcept
{
    return GetSettingsState().ReadLayout(document);
}

// 以可执行文件目录为基准初始化正式运行时设置。
// 入参：无显式入参。
// 返回：默认设置可用且业务状态初始化成功时为 true；失败为 false。用户文件不可持久化通过独立状态查询报告。
bool InitializeSettings() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = GetExecutableDirectory();
        return !applicationDirectory.empty() && InitializeSettings(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

// 以调用者提供的目录初始化设置，供隔离测试使用。
// 入参：applicationDirectory 为应用根目录，其 resources 和 data 子目录分别保存资源与用户设置。
// 返回：默认设置可用且业务状态初始化成功时为 true；失败为 false。用户文件不可持久化通过独立状态查询报告。
bool InitializeSettings(const std::filesystem::path& applicationDirectory) noexcept
{
    return GetSettingsState().Initialize(applicationDirectory);
}

// 关闭设置状态并释放 JSON 句柄。
// 入参：无显式入参。
// 返回：无返回值。
void ShutdownSettings() noexcept
{
    GetSettingsState().Shutdown();
}

// 查询设置业务当前记录的持久化可用性。
// 入参：无显式入参。
// 返回：最近一次初始化或成功写入记录为可持久化时为 true；未初始化或记录失败时为 false，不保证下一次写入成功。
bool IsSettingsPersistenceAvailable() noexcept
{
    return GetSettingsState().PersistenceAvailable();
}

// 消费业务层待显示的粗略读取告警，不重置持续故障的去重状态。
// 入参：无显式入参。
// 返回：本次取走待显示读取告警时为 true；没有待显示告警为 false。
bool ConsumeSettingsReadWarning() noexcept
{
    return GetSettingsState().ConsumeReadWarning();
}

// 按动态属性名读取字符串设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中的有效字符串值；均不可用或类型错误时为 std::nullopt。
std::optional<std::string> GetStringSetting(std::string_view key) noexcept
{
    return GetSettingsState().String(key);
}

// 按动态属性名读取布尔设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中的有效布尔值；均不可用或类型错误时为 std::nullopt。
std::optional<bool> GetBoolSetting(std::string_view key) noexcept
{
    return GetSettingsState().Boolean(key);
}

// 按动态属性名读取有符号整数设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中可由 int64_t 表示的整数；均缺失、类型错误或超范围时为 std::nullopt。
std::optional<std::int64_t> GetIntegerSetting(std::string_view key) noexcept
{
    return GetSettingsState().Integer(key);
}

// 通过动态 key 写入字符串设置。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的字符串值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
bool SetStringSetting(std::string_view key, std::string_view value) noexcept
{
    return GetSettingsState().SetString(key, value);
}

// 通过动态 key 写入布尔设置。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的布尔值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
bool SetBoolSetting(std::string_view key, bool value) noexcept
{
    return GetSettingsState().SetBoolean(key, value);
}

// 通过动态 key 写入整数设置。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的有符号整数值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
bool SetIntegerSetting(std::string_view key, std::int64_t value) noexcept
{
    return GetSettingsState().SetInteger(key, value);
}
// 启动前只读用户语言，用户值无效时回退资源默认值。
// 入参：applicationDirectory 为应用根目录，其 resources 和 data 子目录分别保存资源与用户设置。
// 返回：有效的启动语言代码；用户值无效时回退默认资源，两者均不可用时为 std::nullopt。
std::optional<std::string> ReadStartupLanguage(const std::filesystem::path& applicationDirectory) noexcept
{
    try
    {
        if (applicationDirectory.empty())
            return std::nullopt;
        for (const std::filesystem::path& path : {applicationDirectory / L"data" / L"settings.json",
                                                  applicationDirectory / L"resources" / L"default_settings.json"})
        {
            JsonFileHandle file = JsonFileManager::Instance().GetFile(
                path.filename() == L"settings.json" ? USER_SETTINGS_CARD_NAME : DEFAULT_SETTINGS_CARD_NAME, path);
            nlohmann::json document;
            if (!file.Read(document) || !IsSettingsDocument(document))
                continue;
            const nlohmann::json& settings = document.at("settings");
            const auto value = settings.find("ui.language");
            if (value != settings.end() && value->is_string() && !value->get_ref<const std::string&>().empty())
                return value->get<std::string>();
        }
    }
    catch (...)
    {
    }
    return std::nullopt;
}

// 在完整设置初始化前只读启动语言，不创建用户配置。
// 入参：无显式入参。
// 返回：有效的启动语言代码；用户值无效时回退默认资源，两者均不可用时为 std::nullopt。
std::optional<std::string> ReadStartupLanguage() noexcept
{
    try
    {
        return ReadStartupLanguage(GetExecutableDirectory());
    }
    catch (...)
    {
        return std::nullopt;
    }
}
} // namespace open_st
