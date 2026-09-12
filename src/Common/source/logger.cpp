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
            if (applicationDirectory.empty() || options.maxLines == 0 || options.maxFileBytes < MIN_FILE_BYTES ||
                options.retainedFiles == 0)
            {
                return false;
            }

            this->options_ = options;
            this->logDirectory_ = std::filesystem::absolute(applicationDirectory / "data" / "logs").lexically_normal();
            std::error_code error;
            std::filesystem::create_directories(this->logDirectory_, error);
            if (error)
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

    // 停止日志输出并删除可识别的程序日志，供退出清理重试。
    // 入参：无；使用初始化时记录的日志目录。
    // 返回：目录不存在或全部清理成功 true；路径含重解析点、访问失败或删除失败 false，保留目录供重试。
    bool ShutdownAndClear() noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            this->ResetLocked(false);
            if (this->logDirectory_.empty())
                return true;
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
                OwnedHandle handle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
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
                this->logDirectory_.clear();
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
                    this->ResetLocked();
                }
            }
        }
        catch (...)
        {
        }
    }

  private:
    // 在持锁状态下关闭当前文件并复位日志运行状态。
    // 入参：clearDirectory：是否同时清除日志目录，false 保留目录以便清理重试；调用方须持有 mutex_。
    // 返回：无返回值；刷新关闭输出，清空路径及计数并取消初始化状态。
    void ResetLocked(bool clearDirectory = true) noexcept
    {
        if (this->file_.is_open())
        {
            this->file_.flush();
            this->file_.close();
        }
        this->file_.clear();
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
        if (!this->OpenPathLocked(latest->path, latest->date))
        {
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
                return false;
            }
            if (!exists)
            {
                return this->OpenPathLocked(path, timestamp.date);
            }
        }
        return false;
    }

    // 切换到指定日志文件并恢复追加写入所需的计数。
    // 入参：path：目标日志路径；date：该文件对应日期；调用方须持有 mutex_。
    // 返回：文件成功以追加方式打开时 true；读取长度、行数或打开失败时 false，旧输出流已关闭。
    bool OpenPathLocked(const std::filesystem::path& path, const std::string& date)
    {
        if (this->file_.is_open())
        {
            this->file_.flush();
            this->file_.close();
        }
        this->file_.clear();

        std::uintmax_t fileBytes = 0;
        std::size_t lineCount = 0;
        bool endsWithNewline = true;
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error)
        {
            fileBytes = std::filesystem::file_size(path, error);
            if (error || !this->CountLinesLocked(path, lineCount, endsWithNewline))
            {
                return false;
            }
        }

        this->file_.open(path, std::ios::binary | std::ios::app);
        if (!this->file_.is_open())
        {
            return false;
        }
        this->currentPath_ = path;
        this->currentDate_ = date;
        this->lineCount_ = lineCount;
        this->fileBytes_ = fileBytes;
        this->needsSeparator_ = fileBytes > 0 && !endsWithNewline;
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
            while (files.size() > this->options_.retainedFiles)
            {
                std::vector<LogFileInfo>::iterator candidate = files.begin();
                while (candidate != files.end() && candidate->path == this->currentPath_)
                {
                    ++candidate;
                }
                if (candidate == files.end())
                {
                    return;
                }
                std::error_code error;
                std::filesystem::remove(candidate->path, error);
                if (error)
                {
                    return;
                }
                files.erase(candidate);
            }
        }
        catch (...)
        {
        }
    }

    std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path logDirectory_;
    std::filesystem::path currentPath_;
    std::string currentDate_;
    std::size_t lineCount_{};
    std::uintmax_t fileBytes_{};
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
bool Logger::ShutdownAndClear() noexcept
{
    return GetLoggerState().ShutdownAndClear();
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
bool ShutdownAndClearLogging() noexcept
{
    return Logger::ShutdownAndClear();
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
