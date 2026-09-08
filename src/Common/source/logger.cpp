#include <log.h>

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

bool ParsePositiveIndex(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0;
}

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

std::tm LocalTime(std::chrono::system_clock::time_point time) noexcept
{
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    (void)localtime_s(&local, &rawTime);
    return local;
}

std::string CurrentDate()
{
    const std::tm local = LocalTime(std::chrono::system_clock::now());
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d");
    return stream.str();
}

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

bool IsEnabled(open_st::log_detail::LogLevel level) noexcept
{
#if defined(OPEN_ST_DEBUG_LOGS)
    return level >= open_st::log_detail::LogLevel::Debug;
#else
    return level >= open_st::log_detail::LogLevel::Info;
#endif
}

std::filesystem::path ExecutableDirectory()
{
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < buffer.size() - 1)
        {
            return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

class LoggerState final
{
  public:
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

    // 在同一把锁内停写并清理；目录与父目录句柄禁止改名替换，文件按句柄删除。
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
                // 统一释放成功或失败的 Win32 句柄。
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
            prefix << '[' << RelativeSourcePath(sourceFile) << ':' << sourceLine << "] ";
        }
        const std::string prefixText = prefix.str();
        std::string record = prefixText;
        const std::size_t availableByFile = static_cast<std::size_t>(this->options_.maxFileBytes - record.size() - 1);
        const std::size_t messageLimit = std::min(MAX_MESSAGE_BYTES, availableByFile);

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

LoggerState& GetLoggerState()
{
    static LoggerState state;
    return state;
}
} // namespace

namespace open_st
{
bool Logger::Initialize() noexcept
{
    try
    {
        const std::filesystem::path applicationDirectory = ExecutableDirectory();
        return !applicationDirectory.empty() && Logger::Initialize(applicationDirectory);
    }
    catch (...)
    {
        return false;
    }
}

bool Logger::Initialize(const std::filesystem::path& applicationDirectory, const LogOptions& options) noexcept
{
    return GetLoggerState().Initialize(applicationDirectory, options);
}

void Logger::Shutdown() noexcept
{
    GetLoggerState().Shutdown();
}

// 清理入口保留结果供退出窗口决定重试或保留日志退出。
bool Logger::ShutdownAndClear() noexcept
{
    return GetLoggerState().ShutdownAndClear();
}

void Logger::WriteText(log_detail::LogLevel level, const std::string& message, std::string_view sourceFile,
                       std::uint_least32_t sourceLine) noexcept
{
    GetLoggerState().Write(level, message, sourceFile, sourceLine);
}

bool InitializeLogging() noexcept
{
    return Logger::Initialize();
}

void ShutdownLogging() noexcept
{
    Logger::Shutdown();
}

namespace log_detail
{
void WriteText(LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept
{
    Logger::WriteText(level, message, sourceFile, sourceLine);
}
} // namespace log_detail
} // namespace open_st
