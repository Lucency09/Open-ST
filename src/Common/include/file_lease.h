// 提供稳定文件身份上的跨进程立即尝试租约，不解释业务路径或创建窗口。

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace open_st
{
enum class FileLeaseMode
{
    Shared,
    Exclusive
};
enum class FileLeaseErrorCode
{
    None,
    Busy,
    AccessDenied,
    Io
};
struct FileLeaseError
{
    FileLeaseErrorCode code{FileLeaseErrorCode::None};
    std::uint32_t systemCode{};
};

class FileLease final
{
  public:
    // 创建空租约。
    // 入参：无。
    // 返回：无返回值。
    FileLease() noexcept;
    // 释放租约和路径锚定句柄，不删除稳定锁文件。
    // 入参：无。
    // 返回：无返回值。
    ~FileLease();
    // 转移租约所有权。
    // 入参：other：原租约。
    // 返回：无返回值。
    FileLease(FileLease&& other) noexcept;
    // 转移租约所有权并释放原租约。
    // 入参：other：原租约。
    // 返回：当前对象。
    FileLease& operator=(FileLease&& other) noexcept;
    // 禁止复制系统租约所有权。
    // 入参：源租约。
    // 返回：不可调用。
    FileLease(const FileLease&) = delete;
    // 禁止复制系统租约所有权。
    // 入参：源租约。
    // 返回：不可调用。
    FileLease& operator=(const FileLease&) = delete;
    // 对稳定文件的第零字节尝试一次加锁，拒绝重解析点和硬链接。
    // 入参：path：稳定锁文件；mode：共享或独占；error：可选错误输出；createParents：锚定后逐级创建缺失目录。
    // 返回：成功 true；失败 false，不等待、不重试，不替换当前已持有租约。
    [[nodiscard]] bool TryAcquire(const std::filesystem::path& path, FileLeaseMode mode,
                                  FileLeaseError* error = nullptr, bool createParents = false) noexcept;
    // 释放持有的租约。
    // 入参：无。
    // 返回：无返回值。
    void Reset() noexcept;
    // 查询是否持有租约。
    // 入参：无。
    // 返回：持有租约为 true。
    [[nodiscard]] bool IsHeld() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
