// 实现正式发行与受信请求的纯值校验，不读取配置或创建窗口。
#include "update_model.h"
#include <charconv>
#include <nlohmann/json.hpp>

namespace open_st::update_detail
{
// 完整匹配数字三元组，防止按字符串排序导致 0.10 小于 0.9。
// 入参：text 为完整版本。返回：数值版本或空。
std::optional<UpdateVersion> ParseVersion(std::string_view text) noexcept
{
    if (text.starts_with('v'))
        text.remove_prefix(1);
    UpdateVersion version{};
    for (std::size_t index = 0; index < version.size(); ++index)
    {
        const std::size_t end = text.find('.');
        const std::string_view part = text.substr(0, end);
        if (part.empty() || (part.size() > 1 && part.front() == '0'))
            return {};
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), version[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
            return {};
        if (index + 1 == version.size())
        {
            if (end != std::string_view::npos)
                return {};
        }
        else
        {
            if (end == std::string_view::npos)
                return {};
            text.remove_prefix(end + 1);
        }
    }
    return version;
}
// 哈希只接受完整十六进制，不悄悄裁剪或补齐。
// 入参：text 为原摘要。返回：规范摘要或空。
std::optional<std::string> NormalizeHash(std::string_view text)
{
    if (text.size() != 64)
        return {};
    std::string value(text);
    for (char& digit : value)
    {
        if (digit >= 'A' && digit <= 'F')
            digit = static_cast<char>(digit - 'A' + 'a');
        else if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
            return {};
    }
    return value;
}
// 仅解析当前产品已规定的字段，任何异常均拒绝整份候选。
// 入参：json 为响应，release 为结果。返回：合法正式 Release 为 true。
bool ParseRelease(std::string_view json, UpdateRelease& release) noexcept
try
{
    if (json.size() > MAX_METADATA_BYTES)
        return false;
    const nlohmann::json document = nlohmann::json::parse(json);
    if (!document.is_object() || !document.contains("draft") || !document["draft"].is_boolean() ||
        document["draft"].get<bool>() || !document.contains("prerelease") || !document["prerelease"].is_boolean() ||
        document["prerelease"].get<bool>())
        return false;
    UpdateRelease candidate;
    if (!document.at("id").is_number_unsigned())
        return false;
    candidate.id = document.at("id").get<std::uint64_t>();
    if (!candidate.id)
        return false;
    candidate.tag = document.at("tag_name").get<std::string>();
    const auto version = ParseVersion(candidate.tag);
    if (!version)
        return false;
    candidate.version = *version;
    candidate.versionText = candidate.tag.starts_with('v') ? candidate.tag.substr(1) : candidate.tag;
    const std::string page = "https://github.com/Lucency09/Open-ST/releases/tag/" + candidate.tag;
    if (document.at("html_url").get<std::string>() != page)
        return false;
    candidate.page.assign(page.begin(), page.end());
    const auto& assets = document.at("assets");
    if (!assets.is_array() || assets.size() > 128)
        return false;
    const std::string installer = "Open-ST-" + candidate.versionText + "-win-x64-Setup.exe";
    const std::string portable = "Open-ST-" + candidate.versionText + "-win-x64.zip";
    for (const auto& item : assets)
    {
        const std::string name = item.at("name").get<std::string>();
        if (name != installer && name != portable && name != "SHA256SUMS.txt")
            continue;
        if (item.at("state").get<std::string>() != "uploaded" || !item.at("id").is_number_unsigned() ||
            !item.at("size").is_number_unsigned())
            return false;
        UpdateAsset asset{item.at("id").get<std::uint64_t>(), item.at("size").get<std::uint64_t>(), name, {}};
        if (!asset.id || !asset.size || asset.size > MAX_INSTALLER_BYTES)
            return false;
        if (item.contains("digest") && !item["digest"].is_null())
        {
            const std::string digest = item["digest"].get<std::string>();
            if (!digest.starts_with("sha256:"))
                return false;
            const auto hash = NormalizeHash(std::string_view(digest).substr(7));
            if (!hash)
                return false;
            asset.sha256 = *hash;
        }
        auto& slot = name == installer  ? candidate.installer
                     : name == portable ? candidate.portable
                                        : candidate.checksums;
        if (slot)
            return false;
        slot = std::move(asset);
    }
    release = std::move(candidate);
    return true;
}
catch (...)
{
    return false;
}
// 固定初始 API 与可信资产域，不接受凭据、异常端口或 HTTP 降级。
// 入参：url 为候选；asset 标记二进制链路。返回：协议与主机通过时 true。
bool TrustedUrl(std::wstring_view url, bool asset) noexcept
{
    if (!url.starts_with(L"https://") || url.size() > 16384)
        return false;
    for (wchar_t ch : url)
        if (ch <= 32 || ch == 127 || ch == L'\\' || ch == L'#')
            return false;
    const std::size_t split = url.find(L'/', 8);
    if (split == std::wstring_view::npos)
        return false;
    const std::wstring_view host = url.substr(8, split - 8), path = url.substr(split);
    if (host == L"api.github.com")
        return asset ? path.starts_with(L"/repos/Lucency09/Open-ST/releases/assets/")
                     : path == L"/repos/Lucency09/Open-ST/releases/latest";
    if (!asset)
        return false;
    return host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com";
}
// 清单中必须恰有一个目标，拒绝重复摘要和相似文件名。
// 入参：text 为完整清单，name 为期望文件名。返回：目标摘要或空。
std::optional<std::string> ChecksumFor(std::string_view text, std::string_view name)
{
    std::optional<std::string> result;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.size() > 66 && line.substr(66) == name && line[64] == ' ' && (line[65] == ' ' || line[65] == '*'))
        {
            if (result)
                return {};
            result = NormalizeHash(line.substr(0, 64));
            if (!result)
                return {};
        }
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return result;
}
} // namespace open_st::update_detail
