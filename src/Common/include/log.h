#pragma once

#include "../private/logger.h"

// Release 中预处理器会完整移除 Debug 调用，传入表达式不会进入生成代码，也不会求值。
#if defined(OPEN_ST_DEBUG_LOGS)
#define OPEN_ST_LOG_LOCATION std::source_location::current()
#define OPEN_ST_LOG_IMPL(level, ...) (::open_st::log_detail::WriteAt(level, OPEN_ST_LOG_LOCATION, __VA_ARGS__))
#else
#define OPEN_ST_LOG_IMPL(level, ...) (::open_st::log_detail::Write(level, __VA_ARGS__))
#endif

#if defined(OPEN_ST_DEBUG_LOGS)
#define OPEN_ST_LOG_DEBUG(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Debug, __VA_ARGS__)
#else
#define OPEN_ST_LOG_DEBUG(...) ((void)0)
#endif

#define OPEN_ST_LOG_INFO(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Info, __VA_ARGS__)
#define OPEN_ST_LOG_WARNING(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Warning, __VA_ARGS__)
#define OPEN_ST_LOG_ERROR(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Error, __VA_ARGS__)
#define OPEN_ST_LOG_FATAL(...) OPEN_ST_LOG_IMPL(::open_st::log_detail::LogLevel::Fatal, __VA_ARGS__)
