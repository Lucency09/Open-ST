#include <settings.h>

#include "settings_internal.h"

#include <json_file.h>
#include <log.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
constexpr int SETTINGS_SCHEMA_VERSION = 1;
constexpr std::string_view USER_SETTINGS_CARD_NAME = "settings.user";
constexpr std::string_view DEFAULT_SETTINGS_CARD_NAME = "settings.default";

// 获取程序目录，作为正式运行时 resources/ 与 data/ 的共同路径基准。
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

// 校验设置文档固定外层协议；settings 内部 key 不设枚举或白名单。
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
        return schemaIterator != document.end() && schemaIterator->is_number_integer() &&
               schemaIterator->get<int>() == SETTINGS_SCHEMA_VERSION && settingsIterator != document.end() &&
               settingsIterator->is_object();
    }
    catch (...)
    {
        return false;
    }
}

class SettingsState final
{
  public:
    // 取得两个具名 JSON 句柄，并且只在用户文件确实不存在时从默认文档创建它。
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

            // 仅缺失时填入默认设置；已有文档不改写，但仍由 Common 检查安全写入前提。
            const bool persistenceAvailable = userFile.Write(
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
    bool ConsumeReadWarning() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return std::exchange(this->readWarningPending_, false);
    }

    // 返回初始化时用户设置是否可安全读写；失败时业务仍可读取默认配置。
    bool PersistenceAvailable() noexcept
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        return this->initialized_ && this->persistenceAvailable_;
    }

    // 按动态 key 读取字符串，用户文档没有合法值时查询默认文档。
    std::optional<std::string> String(std::string_view key) noexcept
    {
        return this->ReadValue<std::string>(key, [](const nlohmann::json& value)
                                            { return value.is_string(); });
    }

    // 按动态 key 读取布尔值，用户文档没有合法值时查询默认文档。
    std::optional<bool> Boolean(std::string_view key) noexcept
    {
        return this->ReadValue<bool>(key, [](const nlohmann::json& value)
                                     { return value.is_boolean(); });
    }

    // 按动态 key 读取有符号整数，用户文档没有合法值时查询默认文档。
    std::optional<std::int64_t> Integer(std::string_view key) noexcept
    {
        return this->ReadValue<std::int64_t>(key, [](const nlohmann::json& value)
                                             { return value.is_number_integer(); });
    }

    // 更新动态字符串 key；编辑写入会先检查外部变化并保留未知字段。
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
    bool SetBoolean(std::string_view key, bool value) noexcept
    {
        return this->SetValue(key, value);
    }

    // 更新动态整数 key。
    bool SetInteger(std::string_view key, std::int64_t value) noexcept
    {
        return this->SetValue(key, value);
    }

  private:
    // 在调用方持有业务锁时读取并验证文档，对同一文件的持续故障只请求一次告警。
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

    // 每次通过句柄读取内容；用户文件不可用时由设置业务显式读取默认文档。
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

    // 从业务读取内容中提取指定类型的动态 key，并验证设置外层协议。
    template <typename Value, typename Predicate>
    static std::optional<Value> SettingsValue(const nlohmann::json& document,
                                             std::string_view key, Predicate predicate)
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

    // 对最新且语义有效的用户文档执行一次短小字段修改。
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
            // 只修改已有且业务结构合法的文档，不在字段修改时重建缺失配置。
            const bool updated = this->userFile_.Write(
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

// 返回进程内唯一的设置业务状态对象。
SettingsState& GetSettingsState()
{
    static SettingsState state;
    return state;
}
} // namespace

namespace open_st
{
// 为私有编辑会话复制已初始化的文件入口。
bool GetSettingsEditFiles(JsonFileHandle& userFile, JsonFileHandle& defaultFile) noexcept
{
    return GetSettingsState().EditFiles(userFile, defaultFile);
}

// 由 Settings 提供布局内容，Renderer 不接触路径和文件管理器。
bool ReadSettingsLayout(nlohmann::json& document) noexcept
{
    return GetSettingsState().ReadLayout(document);
}

// 以可执行文件目录为基准初始化正式运行时设置。
bool InitializeSettings() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = ExecutableDirectory();
        return !applicationDirectory.empty() && InitializeSettings(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

// 以调用者提供的目录初始化设置，供隔离测试使用。
bool InitializeSettings(const std::filesystem::path& applicationDirectory) noexcept
{
    return GetSettingsState().Initialize(applicationDirectory);
}

// 关闭设置状态并释放 JSON 句柄。
void ShutdownSettings() noexcept
{
    GetSettingsState().Shutdown();
}

// 返回用户设置文件在初始化时是否成功加载或创建。
bool IsSettingsPersistenceAvailable() noexcept
{
    return GetSettingsState().PersistenceAvailable();
}

// 消费业务层待显示的粗略读取告警，不重置持续故障的去重状态。
bool ConsumeSettingsReadWarning() noexcept
{
    return GetSettingsState().ConsumeReadWarning();
}

// 通过动态 key 读取字符串设置。
std::optional<std::string> GetStringSetting(std::string_view key) noexcept
{
    return GetSettingsState().String(key);
}

// 通过动态 key 读取布尔设置。
std::optional<bool> GetBoolSetting(std::string_view key) noexcept
{
    return GetSettingsState().Boolean(key);
}

// 通过动态 key 读取整数设置。
std::optional<std::int64_t> GetIntegerSetting(std::string_view key) noexcept
{
    return GetSettingsState().Integer(key);
}

// 通过动态 key 写入字符串设置。
bool SetStringSetting(std::string_view key, std::string_view value) noexcept
{
    return GetSettingsState().SetString(key, value);
}

// 通过动态 key 写入布尔设置。
bool SetBoolSetting(std::string_view key, bool value) noexcept
{
    return GetSettingsState().SetBoolean(key, value);
}

// 通过动态 key 写入整数设置。
bool SetIntegerSetting(std::string_view key, std::int64_t value) noexcept
{
    return GetSettingsState().SetInteger(key, value);
}
} // namespace open_st
