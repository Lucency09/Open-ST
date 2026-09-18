// 实现带线程同步、按日期和容量轮转及受限文件清理的本地日志服务。

#include <log.h>
#include <windows_util.h>

#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view FILE_PREFIX = "Open-ST-";
constexpr std::string_view FILE_EXTENSION = ".log";
constexpr std::size_t DATE_LENGTH = 10;
constexpr std::size_t FILE_TIMESTAMP_LENGTH = 23;
constexpr std::size_t MAX_MESSAGE_BYTES = 64U * 1024U;
constexpr std::uintmax_t MIN_FILE_BYTES = 128;
constexpr std::uint64_t MAX_FILENAME_COLLISIONS = 100000;

struct LogFileInfo
{
    std::filesystem::path path;
    std::string date;
    std::string sortTimestamp;
    std::uint64_t collisionIndex{};
    bool timestamped{};
};

struct FileTimestamp
{
    std::string date;
    std::string value;
};

struct MaintenanceHandleDeleter
{
    // 释放维护期间持有的文件或目录句柄。
    // 入参：handle：可为空或无效的 Win32 句柄。
    // 返回：无返回值；有效句柄恰好关闭一次。
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using MaintenanceHandle = std::unique_ptr<void, MaintenanceHandleDeleter>;

// 读取路径自身身份，允许正常日志轮转删除旧文件，但不跟随重解析点。
// 入参：path：完整文件路径；information：输出文件身份和属性。
// 返回：已验证普通文件的句柄；失败返回空句柄。
MaintenanceHandle ReadLogIdentity(const std::filesystem::path& path, BY_HANDLE_FILE_INFORMATION& information)
{
    MaintenanceHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.get(), &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return {};
    return handle;
}

// 比较文件实际身份，防止路径别名绕过当前日志保护。
// 入参：left、right：已读取成功的文件身份。
// 返回：卷序号及文件索引均相同时为 true。
bool SameLogIdentity(const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right) noexcept
{
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}

// 逐级固定日志目录，拒绝重解析点并阻止维护期间重命名或替换路径组件。
// 入参：directory：绝对日志目录；handles：接收需要全程保留的目录句柄；stop：取消标志。
// 返回：所有组件都是普通目录时为 true；目录不可用或已取消时为 false。
bool AnchorLogDirectory(const std::filesystem::path& directory, std::vector<MaintenanceHandle>& handles,
                        std::stop_token stop)
{
    std::filesystem::path current = directory.root_path();
    std::vector<std::filesystem::path> paths{current};
    for (const std::filesystem::path& component : directory.relative_path())
    {
        current /= component;
        paths.push_back(current);
    }
    for (const std::filesystem::path& path : paths)
    {
        if (stop.stop_requested())
            return false;
        MaintenanceHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (handle.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return false;
        handles.push_back(std::move(handle));
    }
    return true;
}

// 识别日志文件名中的日期片段格式。
// 入参：text：待检查的 YYYY-MM-DD 片段。
// 返回：数字和连字符位置符合格式时 true；否则 false，不校验日期实际存在。
bool IsDateText(std::string_view text) noexcept
{
    if (text.size() != DATE_LENGTH || text[4] != '-' || text[7] != '-')
    {
        return false;
    }
    for (std::size_t position = 0; position < text.size(); ++position)
    {
        if (position == 4 || position == 7)
        {
            continue;
        }
        if (text[position] < '0' || text[position] > '9')
        {
            return false;
        }
    }
    return true;
}

// 识别日志文件名的毫秒时间戳格式。
// 入参：text：待检查的 YYYY-MM-DD-HH-MM-SS-mmm 片段。
// 返回：长度和数字、连字符位置全部合法时 true；否则 false，不校验时间取值范围。
bool IsFileTimestampText(std::string_view text) noexcept
{
    if (text.size() != FILE_TIMESTAMP_LENGTH)
    {
        return false;
    }
    for (std::size_t position = 0; position < text.size(); ++position)
    {
        const bool separator =
            position == 4 || position == 7 || position == 10 || position == 13 || position == 16 || position == 19;
        if (separator)
        {
            if (text[position] != '-')
            {
                return false;
            }
        }
        else if (text[position] < '0' || text[position] > '9')
        {
            return false;
        }
    }
    return true;
}

// 解析日志文件重名时附加的正整数序号。
// 入参：text：待解析的十进制文本；value：输出解析数值。
// 返回：完整解析为非零 uint64_t 时 true；否则 false，失败时 value 可能已被部分解析结果修改。
bool ParsePositiveIndex(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0;
}

// 识别当前及旧版日志文件名并提取排序依据。
// 入参：path：候选日志路径；result：输出文件日期、时间戳及冲突序号，调用前应使用新记录。
// 返回：名称符合日志规则时 true；不匹配或异常时 false，失败可能已部分修改 result。
bool ParseLogFile(const std::filesystem::path& path, LogFileInfo& result) noexcept
{
    try
    {
        const std::string name = path.filename().string();
        if (name.size() < FILE_PREFIX.size() + FILE_EXTENSION.size() || !name.starts_with(FILE_PREFIX) ||
            !name.ends_with(FILE_EXTENSION))
        {
            return false;
        }

        const std::string_view body{name.data() + FILE_PREFIX.size(),
                                    name.size() - FILE_PREFIX.size() - FILE_EXTENSION.size()};
        if (body.size() < DATE_LENGTH || !IsDateText(body.substr(0, DATE_LENGTH)))
        {
            return false;
        }

        result.path = path;
        result.date.assign(body.substr(0, DATE_LENGTH));

        if (body.size() == FILE_TIMESTAMP_LENGTH && IsFileTimestampText(body))
        {
            result.sortTimestamp.assign(body);
            result.timestamped = true;
            return true;
        }

        if (body.size() > FILE_TIMESTAMP_LENGTH + 1 && body[FILE_TIMESTAMP_LENGTH] == '-' &&
            IsFileTimestampText(body.substr(0, FILE_TIMESTAMP_LENGTH)) &&
            ParsePositiveIndex(body.substr(FILE_TIMESTAMP_LENGTH + 1), result.collisionIndex))
        {
            result.sortTimestamp.assign(body.substr(0, FILE_TIMESTAMP_LENGTH));
            result.timestamped = true;
            return true;
        }

        // 兼容旧版 Open-ST-YYYY-MM-DD[-NNN].log，使历史文件仍参与保留数量清理。
        if (body.size() == DATE_LENGTH)
        {
            result.sortTimestamp = result.date + "-00-00-00-000";
            return true;
        }
        if (body.size() > DATE_LENGTH + 1 && body[DATE_LENGTH] == '-' &&
            ParsePositiveIndex(body.substr(DATE_LENGTH + 1), result.collisionIndex))
        {
            result.sortTimestamp = result.date + "-00-00-00-000";
            return true;
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

// 把系统时间点转换为日志使用的本地日历时间。
// 入参：time：system_clock 时间点。
// 返回：localtime_s 填充的 tm 结构；本函数不单独报告时间转换错误。
std::tm LocalTime(std::chrono::system_clock::time_point time) noexcept
{
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    (void)localtime_s(&local, &rawTime);
    return local;
}

// 生成检测日志跨日轮转所需的本地日期。
// 入参：无。
// 返回：当前本地时间对应的 YYYY-MM-DD 字符串。
std::string CurrentDate()
{
    const std::tm local = LocalTime(std::chrono::system_clock::now());
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d");
    return stream.str();
}

// 缩短日志中的源码位置并统一路径分隔符。
// 入参：sourceFile：source_location 提供的源码路径视图。
// 返回：优先截取 src/ 或 testing/ 开始的路径；未找到标记时返回统一为斜杠的完整输入路径。
std::string RelativeSourcePath(std::string_view sourceFile)
{
    std::string path(sourceFile);
    std::replace(path.begin(), path.end(), '\\', '/');
    constexpr std::array<std::string_view, 2> ROOT_MARKERS{"/src/", "/testing/"};
    for (std::string_view marker : ROOT_MARKERS)
    {
        const std::size_t position = path.rfind(marker);
        if (position != std::string::npos)
        {
            return path.substr(position + 1);
        }
    }
    return path;
}

// 为新日志文件生成一致的日期及毫秒时间戳。
// 入参：无。
// 返回：同一当前时刻产生的 FileTimestamp，date 为日期，value 为文件名时间戳。
FileTimestamp CurrentFileTimestamp()
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::tm local = LocalTime(now);
    const std::chrono::milliseconds milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream date;
    date << std::put_time(&local, "%Y-%m-%d");
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << '-' << std::setfill('0') << std::setw(3)
          << milliseconds.count();
    return FileTimestamp{date.str(), value.str()};
}

// 为日志记录选择可读的级别名称。
// 入参：level：日志级别枚举。
// 返回：静态字符串指针，未知值为 UNKNOWN，调用方不负责释放。
const char* LevelName(open_st::log_detail::LogLevel level) noexcept
{
    switch (level)
    {
    case open_st::log_detail::LogLevel::Debug:
        return "DEBUG";
    case open_st::log_detail::LogLevel::Info:
        return "INFO";
    case open_st::log_detail::LogLevel::Warning:
        return "WARNING";
    case open_st::log_detail::LogLevel::Error:
        return "ERROR";
    case open_st::log_detail::LogLevel::Fatal:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

// 按构建配置判断是否记录指定级别日志。
// 入参：level：待判断的日志级别。
// 返回：Debug 构建接受 Debug 及以上，Release 接受 Info 及以上；达到阈值时 true。
bool IsEnabled(open_st::log_detail::LogLevel level) noexcept
{
#if defined(OPEN_ST_DEBUG_LOGS)
    return level >= open_st::log_detail::LogLevel::Debug;
#else
    return level >= open_st::log_detail::LogLevel::Info;
#endif
}

class LoggerState final
{
  public:
    // 按应用目录与轮转配置启动进程日志输出。
    // 入参：applicationDirectory：应用基准目录，日志位于其 data/logs；options：最大行数、文件字节数和保留数量。
    // 返回：配置有效且目录和输出文件准备成功时 true；失败或异常时 false 并重置日志状态。
    bool Initialize(const std::filesystem::path& applicationDirectory, const open_st::LogOptions& options) noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->ResetLocked();
            this->lastFailure_.reset();
            this->pendingFailure_.reset();
            if (applicationDirectory.empty() || options.maxLines == 0 || options.maxFileBytes < MIN_FILE_BYTES ||
                options.retainedFiles == 0)
            {
                return false;
            }

            this->options_ = options;
            this->logDirectory_ = std::filesystem::absolute(applicationDirectory / "data" / "logs").lexically_normal();
            open_st::FileLease coordination;
            if (!this->AcquireDirectoryLocked(coordination, nullptr, true, true))
            {
                this->ResetLocked();
                return false;
            }
            const std::string date = CurrentDate();
            if (!this->OpenLatestLocked(date))
            {
                this->ResetLocked();
                return false;
            }
            this->initialized_ = true;
            this->CleanupLocked();
            return true;
        }
        catch (...)
        {
            this->Shutdown();
            return false;
        }
    }

    // 刷新并停止日志输出，保留磁盘日志文件。
    // 入参：无。
    // 返回：无返回值；关闭输出流并重置进程日志状态，不删除历史文件。
    void Shutdown() noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->ResetLocked();
        }
        catch (...)
        {
        }
    }

    // 消费尚未发送给 UI 的故障，读取不产生新的日志。
    // 入参：无。
    // 返回：一次性故障结果或空值。
    std::optional<open_st::FileLeaseError> ConsumeFailure() noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            return std::exchange(this->pendingFailure_, std::nullopt);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    // 提供当前运行日志服务的实际目录，不改变任何状态。
    // 入参：无。
    // 返回：运行时目录副本；未初始化或异常时为空。
    std::optional<std::filesystem::path> Directory() noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (this->initialized_)
                return this->logDirectory_;
        }
        catch (...)
        {
        }
        return std::nullopt;
    }

    // 删除本次快照中的历史日志，保持当前流和正常轮转不受维护操作重置。
    // 入参：stop：枚举及各次删除前检查的取消标志。
    // 返回：结构化结果；开始及删除时的当前文件都保留，异常不向上层传播。
    open_st::LogCleanupResult ClearHistorical(std::stop_token stop) noexcept
    {
        open_st::LogCleanupResult result;
        try
        {
            std::filesystem::path directory;
            std::filesystem::path initialPath;
            std::uint64_t generation{};
            BY_HANDLE_FILE_INFORMATION initialIdentity{};
            {
                const std::scoped_lock<std::mutex> lock(this->mutex_);
                if (stop.stop_requested())
                {
                    result.status = open_st::LogCleanupStatus::Cancelled;
                    return result;
                }
                if (!this->initialized_)
                    return result;
                open_st::FileLease coordination;
                if (!this->AcquireDirectoryLocked(coordination, &result.coordinationError, false))
                {
                    result.status = result.coordinationError.code == open_st::FileLeaseErrorCode::Busy
                                        ? open_st::LogCleanupStatus::Busy
                                        : open_st::LogCleanupStatus::Unavailable;
                    ++result.failed;
                    return result;
                }
                directory = this->logDirectory_;
                initialPath = this->currentPath_;
                generation = this->generation_;
                const MaintenanceHandle initialFile = ReadLogIdentity(this->currentPath_, initialIdentity);
                if (!initialFile)
                {
                    ++result.failed;
                    return result;
                }
                // 只保留身份值；立即关闭查询句柄，避免正常轮转删除旧文件后留下待删除对象。
            }
            std::vector<MaintenanceHandle> directories;
            if (!AnchorLogDirectory(directory, directories, stop))
            {
                if (stop.stop_requested())
                    result.status = open_st::LogCleanupStatus::Cancelled;
                else
                    ++result.failed;
                return result;
            }
            std::vector<std::filesystem::path> candidates;
            std::error_code error;
            std::filesystem::directory_iterator iterator(directory, error);
            const std::filesystem::directory_iterator end;
            while (!error && iterator != end)
            {
                if (stop.stop_requested())
                {
                    result.status = open_st::LogCleanupStatus::Cancelled;
                    return result;
                }
                LogFileInfo info;
                if (ParseLogFile(iterator->path(), info))
                    candidates.push_back(std::move(info.path));
                iterator.increment(error);
            }
            if (error)
                ++result.failed;
            result.status = open_st::LogCleanupStatus::Completed;
            for (const std::filesystem::path& candidate : candidates)
            {
                const std::scoped_lock<std::mutex> lock(this->mutex_);
                if (stop.stop_requested())
                {
                    result.status = open_st::LogCleanupStatus::Cancelled;
                    return result;
                }
                if (!this->initialized_ || this->generation_ != generation)
                {
                    ++result.failed;
                    result.status = result.deleted != 0 ? open_st::LogCleanupStatus::PartialFailure
                                                        : open_st::LogCleanupStatus::Unavailable;
                    return result;
                }
                open_st::FileLease coordination;
                if (!this->AcquireDirectoryLocked(coordination, &result.coordinationError, false))
                {
                    result.status = result.coordinationError.code == open_st::FileLeaseErrorCode::Busy
                                        ? open_st::LogCleanupStatus::Busy
                                        : open_st::LogCleanupStatus::PartialFailure;
                    ++result.failed;
                    return result;
                }
                if (candidate == initialPath || candidate == this->currentPath_)
                {
                    ++result.retained;
                    continue;
                }
                this->DeleteHistoricalLocked(candidate, initialIdentity, result);
            }
            if (stop.stop_requested())
                result.status = open_st::LogCleanupStatus::Cancelled;
            else if (result.failed != 0)
                result.status = open_st::LogCleanupStatus::PartialFailure;
        }
        catch (...)
        {
            ++result.failed;
            if (result.status == open_st::LogCleanupStatus::Completed)
                result.status = open_st::LogCleanupStatus::PartialFailure;
        }
        return result;
    }

    // 停止日志输出并删除可识别的程序日志，供退出清理重试。
    // 入参：无；使用初始化时记录的日志目录。
    // 返回：目录不存在或全部清理成功 true；路径含重解析点、访问失败或删除失败 false，保留目录供重试。
    bool ShutdownAndClear(open_st::FileLeaseError* outputError, std::size_t* retained) noexcept
    {
        if (outputError != nullptr)
            *outputError = {open_st::FileLeaseErrorCode::Io, ERROR_GEN_FAILURE};
        if (retained != nullptr)
            *retained = 0;
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->ResetLocked(false);
            if (this->logDirectory_.empty())
            {
                if (outputError != nullptr)
                    *outputError = {};
                return true;
            }
            std::error_code existenceError;
            if (!std::filesystem::exists(this->logDirectory_, existenceError) && !existenceError)
            {
                this->logDirectory_.clear();
                if (outputError != nullptr)
                    *outputError = {};
                return true;
            }
            open_st::FileLease coordination;
            open_st::FileLeaseError coordinationError;
            if (!this->AcquireDirectoryLocked(coordination, &coordinationError, false))
            {
                if (outputError != nullptr)
                    *outputError = coordinationError;
                return false;
            }
            struct CloseHandleDeleter
            {
                // 释放退出清理过程中打开的 Win32 句柄。
                // 入参：handle：待释放的文件或目录句柄，可为 nullptr 或 INVALID_HANDLE_VALUE。
                // 返回：无返回值；有效句柄调用 CloseHandle，无效值不操作。
                void operator()(void* handle) const noexcept
                {
                    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
                        CloseHandle(handle);
                }
            };
            using OwnedHandle = std::unique_ptr<void, CloseHandleDeleter>;
            std::vector<OwnedHandle> directories;
            std::filesystem::path current = this->logDirectory_.root_path();
            for (const std::filesystem::path& component : this->logDirectory_.relative_path())
            {
                current /= component;
                OwnedHandle handle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (handle.get() == INVALID_HANDLE_VALUE)
                {
                    const DWORD error = GetLastError();
                    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    {
                        this->logDirectory_.clear();
                        return true;
                    }
                    return false;
                }
                BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(handle.get(), &information) ||
                    (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                    (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    return false;
                directories.push_back(std::move(handle));
            }
            std::error_code error;
            std::filesystem::directory_iterator iterator(this->logDirectory_, error);
            const std::filesystem::directory_iterator end;
            bool success = !error;
            while (!error && iterator != end)
            {
                LogFileInfo info;
                if (ParseLogFile(iterator->path(), info))
                {
                    open_st::FileLease active;
                    open_st::FileLeaseError leaseError;
                    if (!active.TryAcquire(this->ActiveLeasePath(info.path), open_st::FileLeaseMode::Exclusive,
                                           &leaseError, true))
                    {
                        if (leaseError.code != open_st::FileLeaseErrorCode::Busy)
                            success = false;
                        else if (retained != nullptr)
                            ++*retained;
                        iterator.increment(error);
                        continue;
                    }
                    OwnedHandle file(CreateFileW(info.path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                    BY_HANDLE_FILE_INFORMATION information{};
                    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &information) ||
                        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                        success = false;
                    else if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        FILE_DISPOSITION_INFO disposition{TRUE};
                        if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                                        sizeof(disposition)))
                            success = false;
                    }
                }
                iterator.increment(error);
            }
            if (error)
                success = false;
            if (success)
            {
                this->logDirectory_.clear();
                if (outputError != nullptr)
                    *outputError = {};
            }
            return success;
        }
        catch (...)
        {
            return false;
        }
    }

    // 向进程日志服务提交文本并按日期、行数或容量轮转。
    // 入参：level：记录级别；message：UTF-8
    // 日志正文；sourceFile：可选源码路径；sourceLine：源码行号，零表示不输出位置。
    // 返回：无返回值；未初始化或级别未启用时忽略，写入失败停止本次输出，Error/Fatal 立即刷新。
    void Write(open_st::log_detail::LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept
    {
        if (!IsEnabled(level))
        {
            return;
        }
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (!this->initialized_)
            {
                return;
            }

            const std::string date = CurrentDate();
            const bool dateChanged = date != this->currentDate_;
            open_st::FileLease coordination;
            if (dateChanged && !this->AcquireDirectoryLocked(coordination))
            {
                this->ResetLocked();
                return;
            }
            if (dateChanged && !this->OpenNewLocked())
            {
                this->ResetLocked();
                return;
            }
            if (dateChanged)
            {
                this->CleanupLocked();
            }

            const std::string record = this->BuildRecordLocked(level, message, sourceFile, sourceLine);
            const std::uintmax_t recordBytes = static_cast<std::uintmax_t>(record.size());
            const std::uintmax_t separatorBytes = this->needsSeparator_ ? 1U : 0U;
            const bool exceedsSize = recordBytes > this->options_.maxFileBytes ||
                                     separatorBytes > this->options_.maxFileBytes - recordBytes ||
                                     this->fileBytes_ > this->options_.maxFileBytes - recordBytes - separatorBytes;
            if (this->lineCount_ >= this->options_.maxLines || exceedsSize)
            {
                if (!coordination.IsHeld() && !this->AcquireDirectoryLocked(coordination))
                {
                    this->ResetLocked();
                    return;
                }
                if (!this->OpenNewLocked())
                {
                    this->ResetLocked();
                    return;
                }
                this->CleanupLocked();
            }

            if (this->needsSeparator_)
            {
                this->file_.put('\n');
                ++this->fileBytes_;
                this->needsSeparator_ = false;
            }
            this->file_.write(record.data(), static_cast<std::streamsize>(record.size()));
            if (!this->file_)
            {
                this->ReportFailureLocked({open_st::FileLeaseErrorCode::Io, ERROR_WRITE_FAULT});
                this->ResetLocked();
                return;
            }
            ++this->lineCount_;
            this->fileBytes_ += recordBytes;

            if (level == open_st::log_detail::LogLevel::Error || level == open_st::log_detail::LogLevel::Fatal)
            {
                this->file_.flush();
                if (!this->file_)
                {
                    this->ReportFailureLocked({open_st::FileLeaseErrorCode::Io, ERROR_WRITE_FAULT});
                    this->ResetLocked();
                }
            }
        }
        catch (...)
        {
        }
    }

  private:
    // 保存去重后的日志基础设施错误，禁止递归调用日志宏。
    // 入参：error：当前故障，调用方持有线程互斥。
    // 返回：无返回值。
    void ReportFailureLocked(open_st::FileLeaseError error) noexcept
    {
        if (!this->lastFailure_.has_value() || this->lastFailure_->code != error.code ||
            this->lastFailure_->systemCode != error.systemCode)
        {
            this->lastFailure_ = error;
            this->pendingFailure_ = error;
        }
    }
    // 尝试一次目录协调锁，普通日志行不调用此接口。
    // 入参：lease：输出租约；output：可选错误；notify：是否发布后台通知。
    // 返回：成功 true，失败立即 false。
    bool AcquireDirectoryLocked(open_st::FileLease& lease, open_st::FileLeaseError* output = nullptr,
                                bool notify = true, bool createParents = false)
    {
        open_st::FileLeaseError error;
        const bool acquired = lease.TryAcquire(this->logDirectory_ / ".coordination.lock",
                                               open_st::FileLeaseMode::Exclusive, &error, createParents);
        if (output != nullptr)
            *output = error;
        if (!acquired && notify)
            this->ReportFailureLocked(error);
        return acquired;
    }
    // 生成日志的稳定活跃租约名，租约文件始终保留。
    // 入参：path：日志路径。
    // 返回：本日志对应的协调路径。
    std::filesystem::path ActiveLeasePath(const std::filesystem::path& path) const
    {
        std::filesystem::path lease = this->logDirectory_ / ".leases" / path.filename();
        lease += L".lock";
        return lease;
    }

    // 在短写锁临界区验证候选身份并按句柄删除，拒绝链接、别名和无法确认的目标。
    // 入参：candidate：命名已识别路径；initialIdentity：操作开始时的当前文件身份；result：累计结果。
    // 返回：无返回值；每个候选计入删除、失败、保留或已消失之一，调用方须持有 mutex_。
    void DeleteHistoricalLocked(const std::filesystem::path& candidate,
                                const BY_HANDLE_FILE_INFORMATION& initialIdentity, open_st::LogCleanupResult& result)
    {
        open_st::FileLease active;
        open_st::FileLeaseError leaseError;
        if (!active.TryAcquire(this->ActiveLeasePath(candidate), open_st::FileLeaseMode::Exclusive, &leaseError, true))
        {
            if (leaseError.code == open_st::FileLeaseErrorCode::Busy)
                ++result.retained;
            else
                ++result.failed;
            return;
        }
        MaintenanceHandle file(CreateFileW(candidate.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                ++result.alreadyMissing;
            else
                ++result.failed;
            return;
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(file.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            ++result.failed;
            return;
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            SameLogIdentity(information, initialIdentity))
        {
            ++result.retained;
            return;
        }
        BY_HANDLE_FILE_INFORMATION currentIdentity{};
        const MaintenanceHandle currentFile = ReadLogIdentity(this->currentPath_, currentIdentity);
        if (!currentFile || information.nNumberOfLinks != 1)
        {
            ++result.failed;
            return;
        }
        if (SameLogIdentity(information, currentIdentity))
        {
            ++result.retained;
            return;
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            ++result.deleted;
        else
            ++result.failed;
    }

    // 在持锁状态下关闭当前文件并复位日志运行状态。
    // 入参：clearDirectory：是否同时清除日志目录，false 保留目录以便清理重试；调用方须持有 mutex_。
    // 返回：无返回值；刷新关闭输出，清空路径及计数并取消初始化状态。
    void ResetLocked(bool clearDirectory = true) noexcept
    {
        ++this->generation_;
        if (this->file_.is_open())
        {
            this->file_.flush();
            this->file_.close();
        }
        this->file_.clear();
        this->activeFile_.reset();
        this->activeLease_.Reset();
        if (clearDirectory)
            this->logDirectory_.clear();
        this->currentPath_.clear();
        this->currentDate_.clear();
        this->lineCount_ = 0;
        this->fileBytes_ = 0;
        this->needsSeparator_ = false;
        this->initialized_ = false;
    }

    // 枚举程序日志文件并按从旧到新排序以支持续写和保留清理。
    // 入参：无显式入参；调用方须持有 mutex_。
    // 返回：符合日志命名规则的普通文件信息列表；按日期、时间戳及冲突序号排序，枚举失败可能仅返回已收集项。
    std::vector<LogFileInfo> ListFilesLocked() const
    {
        std::vector<LogFileInfo> files;
        std::error_code error;
        std::filesystem::directory_iterator iterator(this->logDirectory_, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end)
        {
            LogFileInfo info;
            if (iterator->is_regular_file(error) && !error && ParseLogFile(iterator->path(), info))
            {
                files.push_back(std::move(info));
            }
            iterator.increment(error);
        }
        std::sort(files.begin(), files.end(),
                  // 比较两个日志文件的时间先后，供保留策略排序。
                  // 入参：left、right：只读日志文件信息。
                  // 返回：左项日期、时间戳或冲突序号按字典顺序更小时 true，否则 false。
                  [](const LogFileInfo& left, const LogFileInfo& right)
                  {
                      if (left.date != right.date)
                      {
                          return left.date < right.date;
                      }
                      if (left.sortTimestamp != right.sortTimestamp)
                      {
                          return left.sortTimestamp < right.sortTimestamp;
                      }
                      return left.collisionIndex < right.collisionIndex;
                  });
        return files;
    }

    // 续写指定日期的最新时间戳日志，必要时新建文件。
    // 入参：date：用于筛选已有日志的本地日期；调用方须持有 mutex_。
    // 返回：成功打开可写日志时 true；打开失败 false，日志缺失或已满时转为创建新日志。
    bool OpenLatestLocked(const std::string& date)
    {
        const std::vector<LogFileInfo> files = this->ListFilesLocked();
        const LogFileInfo* latest = nullptr;
        for (const LogFileInfo& file : files)
        {
            if (file.date == date && file.timestamped)
            {
                latest = &file;
            }
        }

        if (latest == nullptr)
        {
            return this->OpenNewLocked();
        }
        open_st::FileLeaseError error;
        if (!this->OpenPathLocked(latest->path, latest->date, false, &error))
        {
            if (error.code == open_st::FileLeaseErrorCode::Busy)
                return this->OpenNewLocked();
            this->ReportFailureLocked(error);
            return false;
        }
        if (this->lineCount_ >= this->options_.maxLines || this->fileBytes_ >= this->options_.maxFileBytes)
        {
            return this->OpenNewLocked();
        }
        return true;
    }

    // 使用当前毫秒时间戳创建新的轮转日志文件。
    // 入参：无显式入参；调用方须持有 mutex_。
    // 返回：找到未使用名称并打开文件时 true；文件查询、打开失败或重名序号耗尽时 false。
    bool OpenNewLocked()
    {
        const FileTimestamp timestamp = CurrentFileTimestamp();
        for (std::uint64_t collision = 0; collision < MAX_FILENAME_COLLISIONS; ++collision)
        {
            std::ostringstream name;
            name << FILE_PREFIX << timestamp.value;
            if (collision > 0)
            {
                name << '-' << std::setfill('0') << std::setw(3) << collision;
            }
            name << FILE_EXTENSION;

            const std::filesystem::path path = this->logDirectory_ / name.str();
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
            {
                this->ReportFailureLocked({open_st::FileLeaseErrorCode::Io, static_cast<DWORD>(error.value())});
                return false;
            }
            if (!exists)
            {
                return this->OpenPathLocked(path, timestamp.date, true);
            }
        }
        return false;
    }

    // 切换到指定日志文件并恢复追加写入所需的计数。
    // 入参：path：目标日志路径；date：该文件对应日期；调用方须持有 mutex_。
    // 返回：文件成功以追加方式打开时 true；读取长度、行数或打开失败时 false，旧输出流已关闭。
    bool OpenPathLocked(const std::filesystem::path& path, const std::string& date, bool createNew,
                        open_st::FileLeaseError* outputError = nullptr)
    {
        if (outputError != nullptr)
            *outputError = {open_st::FileLeaseErrorCode::Io, ERROR_OPEN_FAILED};
        open_st::FileLease active;
        open_st::FileLeaseError error;
        if (!active.TryAcquire(this->ActiveLeasePath(path), open_st::FileLeaseMode::Exclusive, &error, true))
        {
            if (outputError != nullptr)
                *outputError = error;
            else
                this->ReportFailureLocked(error);
            return false;
        }
        MaintenanceHandle reserved(
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        createNew ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        // 统一报告打开阶段故障，业务显式调用取结果，后台轮转发布一次通知。
        // 入参：code：确定的 Win32 故障码。
        // 返回：恒为 false。
        const auto fail = [this, outputError](DWORD code)
        {
            const open_st::FileLeaseError failure{code == ERROR_ACCESS_DENIED
                                                      ? open_st::FileLeaseErrorCode::AccessDenied
                                                      : open_st::FileLeaseErrorCode::Io,
                                                  code};
            if (outputError != nullptr)
                *outputError = failure;
            else
                this->ReportFailureLocked(failure);
            return false;
        };
        BY_HANDLE_FILE_INFORMATION identity{};
        if (reserved.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(reserved.get(), &identity))
            return fail(GetLastError());
        if ((identity.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            identity.nNumberOfLinks != 1)
            return fail(ERROR_ACCESS_DENIED);
        if (this->file_.is_open())
        {
            this->file_.flush();
            this->file_.close();
        }
        this->file_.clear();
        this->activeFile_.reset();
        this->activeLease_.Reset();

        std::uintmax_t fileBytes = 0;
        std::size_t lineCount = 0;
        bool endsWithNewline = true;
        std::error_code fileError;
        if (std::filesystem::exists(path, fileError) && !fileError)
        {
            fileBytes = std::filesystem::file_size(path, fileError);
            if (fileError || !this->CountLinesLocked(path, lineCount, endsWithNewline))
                return fail(fileError ? static_cast<DWORD>(fileError.value()) : ERROR_READ_FAULT);
        }

        this->file_.open(path, std::ios::binary | std::ios::app);
        if (!this->file_.is_open())
            return fail(ERROR_OPEN_FAILED);
        this->activeLease_ = std::move(active);
        this->activeFile_ = std::move(reserved);
        this->currentPath_ = path;
        this->currentDate_ = date;
        this->lineCount_ = lineCount;
        this->fileBytes_ = fileBytes;
        this->needsSeparator_ = fileBytes > 0 && !endsWithNewline;
        if (outputError != nullptr)
            *outputError = {};
        return true;
    }

    // 读取现有日志行数及结尾状态，避免追加时拼接到旧末行。
    // 入参：path：日志文件路径；count：输出行数；endsWithNewline：输出是否为空或以换行结束；调用方须持锁。
    // 返回：扫描至文件末尾时 true；打开或读取失败 false，count 可能已有部分计数。
    bool CountLinesLocked(const std::filesystem::path& path, std::size_t& count, bool& endsWithNewline) const
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            return false;
        }

        std::array<char, 4096> buffer{};
        count = 0;
        bool hasBytes = false;
        char last = '\0';
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read = input.gcount();
            for (std::streamsize position = 0; position < read; ++position)
            {
                hasBytes = true;
                last = buffer[static_cast<std::size_t>(position)];
                if (last == '\n')
                {
                    ++count;
                }
            }
        }
        if (!input.eof())
        {
            return false;
        }
        if (hasBytes && last != '\n')
        {
            ++count;
        }
        endsWithNewline = !hasBytes || last == '\n';
        return true;
    }

    // 生成带时间、级别及可选位置的单行日志记录。
    // 入参：level：记录级别；message：UTF-8
    // 日志正文；sourceFile：可选源码路径；sourceLine：源码行号，零表示不输出位置；调用方须持有 mutex_。
    // 返回：带末尾换行的日志字节串；移除回车、转义正文换行，按消息及文件字节预算截断正文，可能截断多字节字符。
    std::string BuildRecordLocked(open_st::log_detail::LogLevel level, const std::string& message,
                                  std::string_view sourceFile, std::uint_least32_t sourceLine) const
    {
        const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        const std::tm local = LocalTime(now);
        const std::chrono::milliseconds milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream prefix;
        prefix << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
               << milliseconds.count() << " [" << LevelName(level) << "] ";
        if (!sourceFile.empty() && sourceLine > 0)
        {
            std::string source = RelativeSourcePath(sourceFile);
            const std::string suffix = ':' + std::to_string(sourceLine) + "] ";
            const std::uintmax_t fixedBytes = prefix.str().size() + 1 + suffix.size() + 1;
            const std::uintmax_t sourceBudget =
                this->options_.maxFileBytes > fixedBytes ? this->options_.maxFileBytes - fixedBytes : 0;
            if (source.size() > sourceBudget)
            {
                // 最小文件预算足以保留时间、级别、行号及省略标记，超长路径保留尾部。
                const std::size_t tailBytes = static_cast<std::size_t>(sourceBudget > 3 ? sourceBudget - 3 : 0);
                source = sourceBudget >= 3 ? "..." + source.substr(source.size() - tailBytes)
                                           : source.substr(0, static_cast<std::size_t>(sourceBudget));
            }
            prefix << '[' << source << suffix;
        }
        const std::string prefixText = prefix.str();
        std::string record = prefixText;
        const std::uintmax_t usedBytes = record.size() + 1;
        const std::uintmax_t availableByFile =
            this->options_.maxFileBytes > usedBytes ? this->options_.maxFileBytes - usedBytes : 0;
        const std::size_t messageLimit =
            static_cast<std::size_t>(std::min<std::uintmax_t>(MAX_MESSAGE_BYTES, availableByFile));

        for (char character : message)
        {
            if (record.size() - prefixText.size() >= messageLimit)
            {
                break;
            }
            if (character == '\r')
            {
                continue;
            }
            if (character == '\n')
            {
                if (record.size() - prefixText.size() + 2 > messageLimit)
                {
                    break;
                }
                record.append("\\n");
            }
            else
            {
                record.push_back(character);
            }
        }
        record.push_back('\n');
        return record;
    }

    // 按保留数量清理最旧日志并保留当前输出文件。
    // 入参：无显式入参；调用方须持有 mutex_。
    // 返回：无返回值；达到保留数量即停止，删除错误或异常时提前结束，不删除当前输出文件。
    void CleanupLocked() noexcept
    {
        try
        {
            std::vector<LogFileInfo> files = this->ListFilesLocked();
            std::size_t remaining = files.size();
            BY_HANDLE_FILE_INFORMATION current{};
            const MaintenanceHandle currentHandle = ReadLogIdentity(this->currentPath_, current);
            if (!currentHandle)
                return;
            for (const LogFileInfo& candidate : files)
            {
                if (remaining <= this->options_.retainedFiles)
                    break;
                if (candidate.path == this->currentPath_)
                    continue;
                open_st::LogCleanupResult result;
                this->DeleteHistoricalLocked(candidate.path, current, result);
                remaining -= result.deleted + result.alreadyMissing;
                if (result.failed != 0)
                {
                    this->ReportFailureLocked({open_st::FileLeaseErrorCode::Io, ERROR_WRITE_FAULT});
                    return;
                }
            }
        }
        catch (...)
        {
        }
    }

    std::mutex mutex_;
    std::ofstream file_;
    open_st::FileLease activeLease_;
    MaintenanceHandle activeFile_;
    std::optional<open_st::FileLeaseError> pendingFailure_;
    std::optional<open_st::FileLeaseError> lastFailure_;
    std::filesystem::path logDirectory_;
    std::filesystem::path currentPath_;
    std::string currentDate_;
    std::size_t lineCount_{};
    std::uintmax_t fileBytes_{};
    std::uint64_t generation_{};
    open_st::LogOptions options_{};
    bool needsSeparator_{};
    bool initialized_{};
};

// 获取进程唯一的日志状态对象。
// 入参：无。
// 返回：静态日志状态引用，由进程生命周期管理，调用方不释放。
LoggerState& GetLoggerState()
{
    static LoggerState state;
    return state;
}
} // namespace

namespace open_st
{
// 读取后台日志故障的一次性通知。
// 入参：无。
// 返回：尚未消费的故障，没有则为空。
std::optional<FileLeaseError> ConsumeLoggingFailure() noexcept
{
    return GetLoggerState().ConsumeFailure();
}

// 查询已初始化日志服务的目录，不启动或重置日志。
// 入参：无。
// 返回：当前目录副本；不可用时为空。
std::optional<std::filesystem::path> GetLoggingDirectory() noexcept
{
    return GetLoggerState().Directory();
}

// 在保持当前日志连续写入的前提下清理历史文件。
// 入参：stop：操作取消标志。
// 返回：结构化清理状态及各类计数。
LogCleanupResult ClearHistoricalLogs(std::stop_token stop) noexcept
{
    return GetLoggerState().ClearHistorical(stop);
}

// 使用当前程序目录和默认轮转配置启动日志服务。
// 入参：无。
// 返回：默认日志初始化成功时 true；程序路径查询或日志准备失败时 false。
bool Logger::Initialize() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = GetExecutableDirectory();
        return !applicationDirectory.empty() && Logger::Initialize(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

// 按应用目录与轮转配置启动进程日志输出。
// 入参：applicationDirectory：应用基准目录，日志位于其 data/logs；options：最大行数、文件字节数和保留数量。
// 返回：配置有效且目录和输出文件准备成功时 true；失败或异常时 false 并重置日志状态。
bool Logger::Initialize(const std::filesystem::path& applicationDirectory, const LogOptions& options) noexcept
{
    return GetLoggerState().Initialize(applicationDirectory, options);
}

// 刷新并停止日志输出，保留磁盘日志文件。
// 入参：无。
// 返回：无返回值；关闭输出流并重置进程日志状态，不删除历史文件。
void Logger::Shutdown() noexcept
{
    GetLoggerState().Shutdown();
}

// 停止日志输出并删除可识别的程序日志，供退出清理重试。
// 入参：无；使用初始化时记录的日志目录。
// 返回：目录不存在或全部清理成功 true；路径含重解析点、访问失败或删除失败 false，保留目录供重试。
bool Logger::ShutdownAndClear(FileLeaseError* error, std::size_t* retained) noexcept
{
    return GetLoggerState().ShutdownAndClear(error, retained);
}

// 向进程日志服务提交文本并按日期、行数或容量轮转。
// 入参：level：记录级别；message：UTF-8 日志正文；sourceFile：可选源码路径；sourceLine：源码行号，零表示不输出位置。
// 返回：无返回值；未初始化或级别未启用时忽略，写入失败停止本次输出，Error/Fatal 立即刷新。
void Logger::WriteText(log_detail::LogLevel level, const std::string& message, std::string_view sourceFile,
                       std::uint_least32_t sourceLine) noexcept
{
    GetLoggerState().Write(level, message, sourceFile, sourceLine);
}

// 使用当前程序目录和默认轮转配置启动日志服务。
// 入参：无。
// 返回：默认日志初始化成功时 true；程序路径查询或日志准备失败时 false。
bool InitializeLogging() noexcept
{
    return Logger::Initialize();
}

// 刷新并停止日志输出，保留磁盘日志文件。
// 入参：无。
// 返回：无返回值；关闭输出流并重置进程日志状态，不删除历史文件。
void ShutdownLogging() noexcept
{
    Logger::Shutdown();
}

// 转发既有退出日志清理行为，使调用方无需访问私有 Logger。
// 入参：无。
// 返回：日志关闭及清理成功为 true；失败为 false，允许用户重试。
bool ShutdownAndClearLogging(FileLeaseError* error, std::size_t* retained) noexcept
{
    return Logger::ShutdownAndClear(error, retained);
}

namespace log_detail
{
// 向进程日志服务提交文本并按日期、行数或容量轮转。
// 入参：level：记录级别；message：UTF-8 日志正文；sourceFile：可选源码路径；sourceLine：源码行号，零表示不输出位置。
// 返回：无返回值；未初始化或级别未启用时忽略，写入失败停止本次输出，Error/Fatal 立即刷新。
void WriteText(LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept
{
    Logger::WriteText(level, message, sourceFile, sourceLine);
}
} // namespace log_detail
} // namespace open_st
