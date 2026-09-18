// 文件职责：声明更新下载的私有文件、完整性校验和启动交接前的路径保护。

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace open_st::update_detail
{
enum class UpdateDownloadError
{
    None,
    Cancelled,
    InvalidPath,
    AccessDenied,
    Busy,
    Io,
    SizeMismatch,
    HashMismatch
};

class ProtectedDownload final
{
    struct Impl;
    struct ConstructionKey
    {
    };

  public:
    // 建立当前用户私有请求目录和不可执行临时文件，保护每一级已解析路径。
    // 入参：cacheRoot 为本地绝对缓存目录；expectedSize 为非零长度；expectedSHA256 为 64 位十六进制；error 输出错误。
    // 返回：独占下载文件或空；任何失败仅清理本次创建的对象，不遍历他人缓存。
    static std::unique_ptr<ProtectedDownload> Create(const std::filesystem::path& cacheRoot, std::uint64_t expectedSize,
                                                     std::string_view expectedSHA256, UpdateDownloadError& error);
    // 在用户显式检查更新时清理可核实的本账户旧请求，活跃、外来或含未知内容的目录保留。
    // 入参：cacheRoot 为缓存根目录。
    // 返回：无；单次最多检查 128 项，不等待占用，不跟随链接，也不后台重试。
    static void CleanupOwned(const std::filesystem::path& cacheRoot) noexcept;
    // 释放保护句柄；未明确 Preserve 时删除本次文件及空的私有请求目录。
    // 入参：无。
    // 返回：无；不删除父缓存目录，不传播异常。
    ~ProtectedDownload();
    // 禁止复制文件、路径保护和增量散列的唯一所有权。
    // 入参：未命名引用为源对象。
    // 返回：无；已删除。
    ProtectedDownload(const ProtectedDownload&) = delete;
    // 禁止通过赋值共享可写下载事务。
    // 入参：未命名引用为源对象。
    // 返回：无；已删除。
    ProtectedDownload& operator=(const ProtectedDownload&) = delete;
    // 追加响应字节并进行 SHA-256 累计，拒绝超过已确认的发行长度。
    // 入参：bytes 为同步借用数据；error 输出错误。
    // 返回：全部写入并散列成功时 true；失败后事务不可继续完成。
    bool Append(std::span<const std::byte> bytes, UpdateDownloadError& error);
    // 校验精确长度和 SHA-256，转换为可执行文件及拒绝改写、删除的只读保护句柄。
    // 入参：error 输出错误。
    // 返回：完整文件已安全就绪时 true；失败不会产生可启动资格。
    bool Complete(UpdateDownloadError& error);
    // 查询本次持有的文件路径，只有 Complete 成功后才能用于启动交接。
    // 入参：无。
    // 返回：对象存活期间有效的只读路径引用；未完成时是 .part 文件。
    const std::filesystem::path& Path() const noexcept;
    // 在系统成功启动之后保留缓存，不因关于窗口或应用退出而删除已交接安装包。
    // 入参：无。
    // 返回：无；只有已完成文件可被保留，保护句柄仍持有至对象析构。
    void Preserve() noexcept;

    // 仅允许类内以不可外部构造的令牌通过 make_unique 创建已初始化事务。
    // 入参：ConstructionKey 为类内令牌；impl 为独占状态，转移所有权。
    // 返回：构造函数无返回值。
    ProtectedDownload(ConstructionKey, std::unique_ptr<Impl> impl) noexcept;

  private:
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st::update_detail
