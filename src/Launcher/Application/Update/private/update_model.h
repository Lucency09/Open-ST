// 定义更新模块内部的正式版本、资产及纯响应校验。
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <update_client.h>

namespace open_st::update_detail
{
using UpdateVersion = std::array<std::uint32_t, 3>;
struct UpdateAsset
{
    std::uint64_t id{}, size{};
    std::string name, sha256;
};
struct UpdateRelease
{
    std::uint64_t id{};
    UpdateVersion version{};
    std::string tag, versionText;
    std::wstring page;
    std::optional<UpdateAsset> installer, portable, checksums;
};
inline constexpr std::uint64_t MAX_INSTALLER_BYTES = 512U * 1024U * 1024U;
inline constexpr std::size_t MAX_METADATA_BYTES = 1024U * 1024U;
// 严格解析三段数值版本，允许一个小写 v 前缀。
// 入参：text 为完整字符串。返回：合法值或空。
std::optional<UpdateVersion> ParseVersion(std::string_view text) noexcept;
// 校验固定仓库、正式版及版本匹配的发行资产。
// 入参：json 为内存响应；release 成功接收候选。返回：校验结果。
bool ParseRelease(std::string_view json, UpdateRelease& release) noexcept;
// 查询请求及重定向只能进入固定 GitHub API／资产 HTTPS 域。
// 入参：url 为完整地址；asset 区分资产和元数据请求。返回：允许为 true。
bool TrustedUrl(std::wstring_view url, bool asset) noexcept;
// 读取校验清单中唯一的精确文件条目，不接受部分匹配。
// 入参：text 为清单；name 为已核验文件名。返回：小写 SHA256 或空。
std::optional<std::string> ChecksumFor(std::string_view text, std::string_view name);
// 将 SHA256 表示规范化为严格的 64 位小写十六进制。
// 入参：text 为摘要内容，不含算法前缀。返回：规范值或空。
std::optional<std::string> NormalizeHash(std::string_view text);
} // namespace open_st::update_detail
