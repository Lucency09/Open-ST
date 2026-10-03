// 保留输入已有的硬换行与 Unicode 行首空白，各行共享调用者的唯一任务预算。
#include <array>
#include <translation_lines.h>

namespace open_st::translation_local::detail
{
namespace
{
constexpr std::size_t MAX_OUTPUT_BYTES = 1024 * 1024;
// 按 Unicode White_Space 集合识别行首空白，不受系统区域或代码页影响。
// 入参：text 为已验证 UTF-8 行。返回：原始 UTF-8 前缀字节数。
std::size_t Indentation(std::string_view text) noexcept
{
    static constexpr std::array<std::string_view, 18> UNICODE_SPACES{
        "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82",
        "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88",
        "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f"};
    std::size_t count{};
    while (count < text.size())
    {
        const std::string_view remaining = text.substr(count);
        const char first = remaining.front();
        if (first == ' ' || first == '\t' || first == '\v' || first == '\f')
        {
            ++count;
            continue;
        }
        std::size_t width = remaining.starts_with("\xe3\x80\x80") ? 3U : 0U;
        for (const std::string_view space : UNICODE_SPACES)
            if (remaining.starts_with(space))
            {
                width = space.size();
                break;
            }
        if (width == 0)
            break;
        count += width;
    }
    return count;
}
// 在追加之前计费，防止格式或多行输出绕过整项响应上限。
// 入参：output 为尚未发布草稿，part 为完整片段。返回：预算内时追加并返回 true。
bool Append(std::string& output, std::string_view part)
{
    if (part.size() > MAX_OUTPUT_BYTES - output.size())
        return false;
    output.append(part);
    return true;
}
} // namespace
// 取消优先于期限耗尽，不能因超时分类而触发用户取消后的在线后备。
// 入参：同一项 deadline 和整轮 stop。返回：当前停止原因。
Error StopReason(std::chrono::steady_clock::time_point deadline, std::stop_token stop) noexcept
{
    if (stop.stop_requested())
        return Error::Cancelled;
    return std::chrono::steady_clock::now() >= deadline ? Error::TimedOut : Error::None;
}
// 保留全部格式，只在整项成功后返回译文；不创建新线程、分段任务或逐行后备。
// 入参：已预算校验的整文、统一 deadline/stop 和同步翻译回调。返回：完整结果。
Result TranslateLines(std::string_view text, std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                      const std::function<Result(std::string_view)>& translate)
{
    Result result;
    std::size_t cursor{};
    while (cursor < text.size())
    {
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        const std::size_t found = text.find_first_of("\r\n", cursor);
        const std::size_t end = found == std::string_view::npos ? text.size() : found;
        const std::string_view line = text.substr(cursor, end - cursor);
        const std::size_t indent = Indentation(line);
        if (indent == line.size())
        {
            if (!Append(result.text, line))
                return {Error::Truncated};
        }
        else
        {
            Result translated = translate(line.substr(indent));
            const Error stopped = StopReason(deadline, stop);
            if (stopped == Error::Cancelled)
                return {stopped};
            if (translated.error == Error::OutOfMemory || translated.error == Error::InvalidInput)
                return {translated.error};
            if (stopped != Error::None)
                return {stopped};
            if (translated.error != Error::None)
                return {translated.error};
            const std::string_view content = std::string_view(translated.text).substr(Indentation(translated.text));
            if (content.empty() || content.find_first_of("\r\n") != std::string_view::npos)
                return {Error::Inference};
            if (!Append(result.text, line.substr(0, indent)) || !Append(result.text, content))
                return {Error::Truncated};
            result.inferenceMilliseconds += translated.inferenceMilliseconds;
        }
        if (end == text.size())
            break;
        const std::size_t separator = text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2U : 1U;
        if (!Append(result.text, text.substr(end, separator)))
            return {Error::Truncated};
        cursor = end + separator;
    }
    if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
        return {stopped};
    return result;
}
} // namespace open_st::translation_local::detail
