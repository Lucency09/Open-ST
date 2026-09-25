// 封装 Windows CNG 增量 SHA-256，统一 OCR 与更新的散列资源、失败及完成语义。
#include <Windows.h>
#include <algorithm>
#include <bcrypt.h>
#include <limits>
#include <sha256.h>

namespace open_st
{
struct Sha256::Impl
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    bool valid{};
    // 同时处理部分初始化与正常销毁，先销毁依赖算法提供者的散列。
    // 入参：无。
    // 返回：无。
    ~Impl()
    {
        if (this->hash)
            BCryptDestroyHash(this->hash);
        if (this->algorithm)
            BCryptCloseAlgorithmProvider(this->algorithm, 0);
    }
};

// 获取系统 SHA-256 实现，不自建散列算法；所有失败经 IsValid 表达。
// 入参：无。
// 返回：无。
Sha256::Sha256() noexcept
{
    try
    {
        this->impl_ = std::make_unique<Impl>();
        if (BCryptOpenAlgorithmProvider(&this->impl_->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return;
        this->impl_->valid =
            BCryptCreateHash(this->impl_->algorithm, &this->impl_->hash, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    catch (...)
    {
        this->impl_.reset();
    }
}
// 销毁唯一状态及系统资源。
// 入参：无。
// 返回：无。
Sha256::~Sha256() = default;

// 仅接受尚未完成且从未出现失败的状态。
// 入参：无。
// 返回：可追加或完成为 true。
bool Sha256::IsValid() const noexcept
{
    return this->impl_ && this->impl_->valid;
}

// 以 ULONG 可表示的块长度交给 BCrypt，避免大缓冲区长度截断。
// 入参：bytes 为同步借用数据。
// 返回：全部块成功为 true，任一失败使整个状态失效。
bool Sha256::Append(std::span<const std::byte> bytes) noexcept
{
    if (!this->IsValid())
        return false;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const ULONG count =
            static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<ULONG>::max()));
        if (BCryptHashData(this->impl_->hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + offset)),
                           count, 0) < 0)
        {
            this->impl_->valid = false;
            return false;
        }
        offset += count;
    }
    return true;
}

// 只在系统成功完成时替换输出，同时阻止完成后的误追加与二次完成。
// 入参：digest 为调用方结果数组。
// 返回：完整摘要已写入时 true，失败保持原结果。
bool Sha256::Finish(std::array<std::byte, 32>& digest) noexcept
{
    if (!this->IsValid())
        return false;
    this->impl_->valid = false;
    std::array<std::byte, 32> candidate{};
    if (BCryptFinishHash(this->impl_->hash, reinterpret_cast<PUCHAR>(candidate.data()),
                         static_cast<ULONG>(candidate.size()), 0) < 0)
        return false;
    digest = candidate;
    return true;
}
} // namespace open_st
