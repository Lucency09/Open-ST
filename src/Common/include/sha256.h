// 提供通用增量 SHA-256；拥有系统散列资源，不包含下载、模型或文件路径业务。
#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace open_st
{
class Sha256 final
{
  public:
    // 创建 SHA-256 系统算法和唯一散列状态，分配失败不抛异常。
    // 入参：无。
    // 返回：构造函数无返回值，调用 IsValid 检查是否可用。
    Sha256() noexcept;
    // 按散列对象、算法提供者顺序释放资源，不输出未完成摘要。
    // 入参：无。
    // 返回：无。
    ~Sha256();
    // 禁止复制增量散列状态，避免不明确的摘要分叉。
    // 入参：未命名源对象。
    // 返回：不可调用。
    Sha256(const Sha256&) = delete;
    // 禁止复制赋值散列资源。
    // 入参：未命名源对象。
    // 返回：不可调用。
    Sha256& operator=(const Sha256&) = delete;
    // 查询状态是否能够继续接受数据或完成摘要。
    // 入参：无。
    // 返回：初始化成功且未失败、未完成时 true。
    [[nodiscard]] bool IsValid() const noexcept;
    // 将借用字节追加至摘要，内部按系统长度上限分块，空片段合法。
    // 入参：bytes 为只在本次调用期间借用的数据。
    // 返回：全部追加成功时 true；失败后对象不可继续使用。
    bool Append(std::span<const std::byte> bytes) noexcept;
    // 完成一次摘要并封闭当前状态，失败不修改输出。
    // 入参：digest 输出标准 32 字节 SHA-256。
    // 返回：首次完整完成为 true；重复完成或此前失败为 false。
    bool Finish(std::array<std::byte, 32>& digest) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
