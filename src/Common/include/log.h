// 提供分级日志宏，并按构建配置控制调试日志及调用位置采集。

#pragma once

#include <file_lease.h>
#include <log_detail.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>

namespace open_st
{
enum class LogCleanupStatus
{
    Completed,
    PartialFailure,
    Unavailable,
    Cancelled,
    Busy
};

struct LogCleanupResult
{
    LogCleanupStatus status{LogCleanupStatus::Unavailable};
    std::size_t deleted{};
    std::size_t failed{};
    std::size_t retained{};
    std::size_t alreadyMissing{};
    FileLeaseError coordinationError{};
};

// 查询当前初始化日志服务所用目录，不创建目录或启动服务。
// 入参：无。
// 返回：正在运行时返回实际目录副本；未初始化或查询失败时为空。
std::optional<std::filesystem::path> GetLoggingDirectory() noexcept;
// 清理可识别的历史日志，保持当前输出流继续写入。
// 入参：stop：取消请求，在枚举和逐项删除前检查。
// 返回：结构化状态和删除、失败、保留、已消失计数；异常不越过模块边界。
LogCleanupResult ClearHistoricalLogs(std::stop_token stop = {}) noexcept;

// 初始化进程日志输出并应用文件轮转限制。
// 入参：无。
// 返回：初始化成功时为 true；路径、配置或文件访问失败时为 false，不向调用方传播异常。
bool InitializeLogging() noexcept;
// 刷新并关闭进程日志输出，结束本次日志会话。
// 入参：无。
// 返回：无返回值；保留已写日志，允许重复关闭。
void ShutdownLogging() noexcept;
// 停止日志输出并清理已识别的本程序日志，供退出清理流程调用。
// 入参：error：可选错误输出，Busy 不应进入自动重试。
// 返回：关闭和清理成功为 true；失败为 false，不删除其他文件或其他进程的活跃日志。
bool ShutdownAndClearLogging(FileLeaseError* error = nullptr, std::size_t* retained = nullptr) noexcept;

// 消费后台日志故障的去重通知，不触发日志写入或窗口。
// 入参：无。
// 返回：尚未消费的故障；无新故障为空。
std::optional<FileLeaseError> ConsumeLoggingFailure() noexcept;
} // namespace open_st

// Release 中预处理器会完整移除 Debug 调用，传入表达式不会进入生成代码，也不会求值。
#if defined(OPEN_ST_DEBUG_LOGS)
// 捕获日志宏调用处的源码位置。
// 入参：无；展开位置由调用处决定。
// 返回：调用位置的 std::source_location 值。
#define OPEN_ST_LOG_LOCATION std::source_location::current()
// 把统一日志宏路由到对应的格式化写入入口。
// 入参：level：日志级别；可变参数：支持流插入的正文片段。
// 返回：无返回值；按当前构建配置选择是否携带源码位置。
#define OPEN_ST_LOG_IMPL(level, ...) (::open_st::log_detail::WriteAt(level, OPEN_ST_LOG_LOCATION, __VA_ARGS__))
#else
// 把统一日志宏路由到对应的格式化写入入口。
// 入参：level：日志级别；可变参数：支持流插入的正文片段。
// 返回：无返回值；按当前构建配置选择是否携带源码位置。
#define OPEN_ST_LOG_IMPL(level, ...) (::open_st::log_detail::Write(level, __VA_ARGS__))
#endif

#if defined(OPEN_ST_DEBUG_LOGS)
// 记录调试级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；日志写入失败不向业务传播异常。
#define OPEN_ST_LOG_DEBUG(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Debug, __VA_ARGS__)
#else
// 记录调试级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；本构建禁用调试日志，参数表达式不会求值。
#define OPEN_ST_LOG_DEBUG(...) ((void)0)
#endif

// 记录信息级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；日志写入失败不向业务传播异常。
#define OPEN_ST_LOG_INFO(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Info, __VA_ARGS__)
// 记录警告级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；日志写入失败不向业务传播异常。
#define OPEN_ST_LOG_WARNING(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Warning, __VA_ARGS__)
// 记录错误级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；日志写入失败不向业务传播异常。
#define OPEN_ST_LOG_ERROR(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Error, __VA_ARGS__)
// 记录致命错误级别日志。
// 入参：可变参数：按顺序拼接的日志正文片段，不应承担业务副作用。
// 返回：无返回值；日志写入失败不向业务传播异常，记录后不会自动终止进程。
#define OPEN_ST_LOG_FATAL(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Fatal, __VA_ARGS__)
