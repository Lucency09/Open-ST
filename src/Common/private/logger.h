#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace open_st
{
bool InitializeLogging() noexcept;
void ShutdownLogging() noexcept;

namespace log_detail
{
enum class LogLevel
{
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

void WriteText(LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept;

template <typename... Arguments>
void FormatAndWrite(LogLevel level, std::string_view sourceFile, std::uint_least32_t sourceLine,
                    Arguments&&... arguments) noexcept
{
    try
    {
        std::ostringstream stream;
        (stream << ... << std::forward<Arguments>(arguments));
        log_detail::WriteText(level, stream.str(), sourceFile, sourceLine);
    }
    catch (...)
    {
        // 日志不得向业务传播格式化、分配或流操作异常。
    }
}

template <typename... Arguments> void Write(LogLevel level, Arguments&&... arguments) noexcept
{
    log_detail::FormatAndWrite(level, {}, 0, std::forward<Arguments>(arguments)...);
}

template <typename... Arguments>
void WriteAt(LogLevel level, const std::source_location& location, Arguments&&... arguments) noexcept
{
    log_detail::FormatAndWrite(level, location.file_name(), location.line(), std::forward<Arguments>(arguments)...);
}
} // namespace log_detail

struct LogOptions
{
    std::size_t maxLines{10000};
    std::uintmax_t maxFileBytes{2U * 1024U * 1024U};
    std::size_t retainedFiles{3};
};

class Logger final
{
  public:
    Logger() = delete;

    static bool Initialize() noexcept;
    static bool Initialize(const std::filesystem::path& applicationDirectory,
                           const LogOptions& options = LogOptions{}) noexcept;
    static void Shutdown() noexcept;
    // 停止日志写入并仅删除已识别的本程序日志；失败保留目录以便重试。
    static bool ShutdownAndClear() noexcept;
    static void WriteText(log_detail::LogLevel level, const std::string& message, std::string_view sourceFile,
                          std::uint_least32_t sourceLine) noexcept;
};
} // namespace open_st
