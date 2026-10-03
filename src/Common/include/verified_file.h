// 提供固定长度和 SHA-256 的同句柄只读校验，不解释资源目录或模型业务。
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace open_st
{
enum class VerifiedFileError
{
    None,
    Missing,
    Integrity,
    Cancelled,
    Unavailable,
    OutOfMemory
};
// 从拒绝并发写入/删除的句柄读取固定字节，校验成功才交付相同内存。
// 入参：path 为路径；size/sha256 为可信预期；cancel 为可空停止谓词；bytes 为输出。
// 返回：分类错误；任何失败清空输出，分配失败单独报告，不重新按路径加载。
VerifiedFileError ReadVerifiedFile(const std::filesystem::path& path, std::uint64_t size, std::string_view sha256,
                                   const std::function<bool()>& cancel, std::string& bytes) noexcept;
} // namespace open_st
