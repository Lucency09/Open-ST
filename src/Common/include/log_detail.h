// 声明日志宏编译必需的级别、格式化模板和内部写入桥接，不公开日志服务状态。

#pragma once

#include <cstdint>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace open_st::log_detail
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
// 入参：level：日志级别；message：调用期间借用的正文；sourceFile：可为空的源码路径；sourceLine：源码行号，无位置时为
// 0。
// 返回：无返回值；日志失败不传播到业务调用。
void WriteText(LogLevel level, const std::string& message, std::string_view sourceFile,
               std::uint_least32_t sourceLine) noexcept;

// 按参数顺序拼接正文后写入指定级别日志。
// 入参：level：级别；sourceFile：可为空的源码路径；sourceLine：源码行号；arguments：正文参数；Arguments：支持流插入的类型包。
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

// 拼接正文并写入不含源码位置的日志。
// 入参：level：日志级别；arguments：按顺序插入流的正文参数；Arguments：支持流插入的类型包。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
template <typename... Arguments> void Write(LogLevel level, Arguments&&... arguments) noexcept
{
    log_detail::FormatAndWrite(level, {}, 0, std::forward<Arguments>(arguments)...);
}

// 拼接正文并写入含调用位置的日志。
// 入参：level：日志级别；location：源码文件及行号；arguments：正文参数；Arguments：支持流插入的类型包。
// 返回：无返回值；日志失败或格式化异常不传播到业务调用。
template <typename... Arguments>
void WriteAt(LogLevel level, const std::source_location& location, Arguments&&... arguments) noexcept
{
    log_detail::FormatAndWrite(level, location.file_name(), location.line(), std::forward<Arguments>(arguments)...);
}
} // namespace open_st::log_detail
