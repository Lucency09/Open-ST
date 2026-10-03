// 实现所有翻译提供方共享的 UTF-8、JSON 和编码后预算验证，不执行网络。
#include <Windows.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <translation_protocol.h>

namespace open_st::translation_detail
{
// 按 Unicode 标量计数并拒绝过长编码、代理半区和 NUL。
// 入参：text 为原文；count 为输出码点数。
// 返回：合法为 true。
bool CountText(std::string_view text, std::size_t& count) noexcept
{
    count = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        unsigned length = 1;
        std::uint32_t scalar = lead;
        if (lead == 0)
            return false;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            length = 2;
            scalar = lead & 0x1fU;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            length = 3;
            scalar = lead & 0x0fU;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            length = 4;
            scalar = lead & 0x07U;
        }
        else if (lead >= 0x80)
            return false;
        if (length > text.size() - index)
            return false;
        for (unsigned part = 1; part < length; ++part)
        {
            const unsigned char byte = static_cast<unsigned char>(text[index + part]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            scalar = (scalar << 6) | (byte & 0x3fU);
        }
        if ((length == 2 && scalar < 0x80) || (length == 3 && scalar < 0x800) || (length == 4 && scalar < 0x10000) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
        ++count;
        index += length;
    }
    return true;
}
// 严格转换 Windows 请求字段，禁止非法 UTF-8 静默替换。
// 入参：text 为 UTF-8。
// 返回：完整 UTF-16，错误返回空。
std::wstring Wide(std::string_view text)
{
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {};
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), needed) != needed)
        return {};
    return result;
}
// 编码原始字节一次，不把 &、+、% 或换行作为协议语法。
// 入参：value 为未编码的 UTF-8。
// 返回：百分号编码文本。
std::string Encode(std::string_view value)
{
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : value)
    {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~')
            encoded.push_back(static_cast<char>(byte));
        else
        {
            encoded.push_back('%');
            encoded.push_back(HEX[byte >> 4]);
            encoded.push_back(HEX[byte & 15]);
        }
    }
    return encoded;
}
// 遍历配置时限制节点和层级，再序列化检查字节；不先递归复制不可信深树。
// 入参：value 为配置树，maxBytes 为静态序列化上限。
// 返回：有界合法为 true。
bool BoundedJson(const nlohmann::json& value, std::size_t maxBytes)
{
    std::vector<std::pair<const nlohmann::json*, unsigned>> pending{{&value, 0}};
    std::size_t nodes{}, bytes{};
    while (!pending.empty())
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();
        if (++nodes > 65536 || depth > 32)
            return false;
        if (node->is_string())
        {
            const std::string& text = node->get_ref<const std::string&>();
            std::size_t count{};
            if (!CountText(text, count) || count > 4096)
                return false;
            bytes += text.size();
        }
        else if (node->is_object())
        {
            for (const auto& item : node->items())
            {
                std::size_t count{};
                if (!CountText(item.key(), count) || count > 256)
                    return false;
                bytes += item.key().size();
                pending.emplace_back(&item.value(), depth + 1);
            }
        }
        else if (node->is_array())
            for (const nlohmann::json& child : *node)
                pending.emplace_back(&child, depth + 1);
        if (bytes > maxBytes || pending.size() > 65536)
            return false;
    }
    return value.dump().size() <= maxBytes;
}
// 在解析回调中拒绝过深或节点过多的远端 JSON，不发布被筛掉的部分树。
// 入参：text 为有界正文。
// 返回：完整树；非法输入抛异常。
nlohmann::json ParseJson(std::string_view text)
{
    if (text.size() > 1048576)
        throw std::invalid_argument("json budget");
    std::size_t events{};
    // 解析事件只用于拒绝预算，始终接受合法字段，避免改变服务器结构。
    // 入参：depth 为层级；event/value 为解析器当前事件和值。
    // 返回：预算内为 true；否则直接抛异常终止。
    return nlohmann::json::parse(text,
                                 [&events](int depth, nlohmann::json::parse_event_t, nlohmann::json&)
                                 {
                                     if (depth > 32 || ++events > 131072)
                                         throw std::invalid_argument("json complexity");
                                     return true;
                                 });
}
// HTTP 状态只兜底，业务层已识别的配额等优先处理。
// 入参：status 为状态码。
// 返回：稳定分类。
TranslationError HttpStatusError(unsigned status) noexcept
{
    if (status >= 200 && status < 300)
        return TranslationError::None;
    if (status >= 300 && status < 400)
        return TranslationError::Redirect;
    if (status == 401)
        return TranslationError::Authentication;
    if (status == 403)
        return TranslationError::Permission;
    if (status == 407)
        return TranslationError::Proxy;
    if (status == 429)
        return TranslationError::RateLimited;
    if (status == 456)
        return TranslationError::Quota;
    if (status == 408 || status == 504)
        return TranslationError::Timeout;
    if (status == 400 || status == 422)
        return TranslationError::InvalidInput;
    return TranslationError::Service;
}
// 对转换完成的 URL/头做 UTF-8 字节计数，而不是误用 UTF-16 单元。
// 入参：request 为完整请求。
// 返回：预算合法且公共参数合法为 None。
TranslationError RequestBudget(const http::Request& request)
{
    // 只计算现有 UTF-16 参数的严格 UTF-8 字节数，不新复制敏感头。
    // 入参：value 为 UTF-16 字段。
    // 返回：字节数；非法时使用超预算值。
    const auto bytes = [](const std::wstring& value) -> std::size_t
    {
        if (value.empty())
            return 0;
        if (value.size() > static_cast<std::size_t>(INT_MAX))
            return SIZE_MAX;
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        return count > 0 ? static_cast<std::size_t>(count) : SIZE_MAX;
    };
    if (request.body.size() > 65536 || bytes(request.url) > 8192)
        return TranslationError::InputTooLarge;
    std::size_t headerBytes{};
    for (const http::Header& header : request.headers)
    {
        const std::size_t name = bytes(header.name), value = bytes(header.value);
        if (name > 8192 || value > 8192)
            return TranslationError::InputTooLarge;
        headerBytes += name + value + 4;
        if (headerBytes > 8192)
            return TranslationError::InputTooLarge;
    }
    return http::IsValidRequest(request) ? TranslationError::None : TranslationError::InvalidConfiguration;
}
// 发布完整译文前统一校验字节、编码与空白，避免 HTML/结构错误伪装成功。
// 入参：text 为候选文字。
// 返回：合法为 None。
TranslationError ValidateResultText(std::string_view text) noexcept
{
    if (text.size() > 1048576)
        return TranslationError::Truncated;
    std::size_t count{};
    if (!CountText(text, count))
        return TranslationError::UnsupportedResponse;
    if (count == 0 || text.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return TranslationError::EmptyResult;
    return TranslationError::None;
}
} // namespace open_st::translation_detail
