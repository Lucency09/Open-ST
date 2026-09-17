// 公共编辑输入辅助实现。
#include "edit_support.h"
#include <stdexcept>
namespace open_st::renderer_detail
{
// 读取编辑框原始文本，保留空字符串和用户尚未完成的输入。
// 入参：window 为借用的原生编辑框。
// 返回：文本副本；分配失败抛出异常，不改写控件。
std::wstring ReadEditText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(copied));
    return value;
}
// 将宿主 UTF-8 草稿转换为 Win32 显示字符串。
// 入参：value 为原始 UTF-8 字节。
// 返回：宽字符串；非空非法编码抛出异常，空值保持为空。
std::wstring EditWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count == 0)
        throw std::runtime_error("Invalid edit UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        count);
    return result;
}
// 将原生文本转换为宿主字符串草稿使用的 UTF-8。
// 入参：value 为完整原始编辑内容。
// 返回：UTF-8 副本；非法 UTF-16 抛出异常，空值保持为空。
std::string EditUtf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count == 0)
        throw std::runtime_error("Invalid edit UTF-16");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        count, nullptr, nullptr);
    return result;
}
// 创建字体供两种编辑宿主使用。入参：value 为字体描述。返回：自有字体或空。
HFONT CreateEditFont(const LOGFONTW& value) noexcept
{
    return CreateFontIndirectW(&value);
}
} // namespace open_st::renderer_detail
