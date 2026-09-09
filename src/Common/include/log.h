// 提供分级日志宏，并按构建配置控制调试日志及调用位置采集。

#pragma once

#include "../private/logger.h"

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
