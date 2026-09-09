// 定义日志内部格式化入口、轮转配置及进程日志服务接口。

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
// 初始化进程日志输出并应用文件轮转限制。
// 入参：无。
// 返回：初始化成功时为 true；路径、配置或文件访问失败时为 false，不向调用方传播异常。
bool InitializeLogging() noexcept;
// 刷新并关闭进程日志输出，结束本次日志会话。
// 入参：无。
// 返回：无返回值；保留已写日志，允许重复关闭。
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

// 把已格式化日志正文及可选源码位置交给进程日志服务。
// 入参：level：日志级别；message：调用期间借用的已格式化正文；sourceFile：可为空的源码路径；sourceLine：源码行号，无位置时为 0。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
void WriteText(LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept;

// 按参数顺序拼接正文后写入指定级别日志。
// 入参：level：日志级别；sourceFile：可为空的源码路径；sourceLine：源码行号，无位置时为 0；arguments：按顺序插入输出流的正文参数；模板参数 Arguments：支持流插入的参数类型包。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
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

// 按参数顺序拼接正文后写入指定级别日志。
// 入参：level：日志级别；arguments：按顺序插入输出流的正文参数；模板参数 Arguments：支持流插入的参数类型包。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
template <typename... Arguments> void Write(LogLevel level, Arguments&&... arguments) noexcept
{
    log_detail::FormatAndWrite(level, {}, 0, std::forward<Arguments>(arguments)...);
}

// 按参数顺序拼接正文后写入指定级别日志。
// 入参：level：日志级别；location：调用处的源码文件及行号；arguments：按顺序插入输出流的正文参数；模板参数 Arguments：支持流插入的参数类型包。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
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
    // 禁止构造没有独立状态的日志门面，统一使用进程日志服务。
    // 入参：无。
    // 返回：无；构造函数已删除，创建实例会导致编译错误。
    Logger() = delete;

    // 初始化进程日志输出并应用文件轮转限制。
    // 入参：无。
    // 返回：初始化成功时为 true；路径、配置或文件访问失败时为 false，不向调用方传播异常。
    static bool Initialize() noexcept;
    // 初始化进程日志输出并应用文件轮转限制。
    // 入参：applicationDirectory：应用根目录，日志写入其日志子目录；options：单文件行数、字节上限和保留文件数。
    // 返回：初始化成功时为 true；路径、配置或文件访问失败时为 false，不向调用方传播异常。
    static bool Initialize(const std::filesystem::path& applicationDirectory,
                           const LogOptions& options = LogOptions{}) noexcept;
    // 刷新并关闭进程日志输出，结束本次日志会话。
    // 入参：无。
    // 返回：无返回值；保留已写日志，允许重复关闭。
    static void Shutdown() noexcept;
    // 停止日志服务并删除已识别的本程序日志文件。
    // 入参：无。
    // 返回：关闭及清理成功时为 true；清理失败时为 false，可再次重试，非本程序文件不删除。
    static bool ShutdownAndClear() noexcept;
    // 把已格式化日志正文及可选源码位置交给进程日志服务。
    // 入参：level：日志级别；message：调用期间借用的已格式化正文；sourceFile：可为空的源码路径；sourceLine：源码行号，无位置时为 0。
    // 返回：无返回值；日志失败或格式化异常不传播到业务调用。
    static void WriteText(log_detail::LogLevel level, const std::string& message, std::string_view sourceFile,
                          std::uint_least32_t sourceLine) noexcept;
};
} // namespace open_st
