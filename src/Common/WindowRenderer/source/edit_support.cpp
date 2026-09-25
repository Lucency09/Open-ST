// 公共编辑输入辅助实现。
#include "edit_support.h"
#include <commctrl.h>
#include <stdexcept>
namespace open_st::renderer_detail
{
namespace
{
// 保留系统编辑与撤销语义，仅补齐全选和输入法生命周期。
// 入参：window/message/wParam/lParam 为原生消息；id 为子类编号；data 指向借用状态。
// 返回：已处理全选返回零，其余返回系统编辑过程结果。
LRESULT CALLBACK FormEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto& state = *reinterpret_cast<FormEditState*>(data);
    if (message == WM_IME_STARTCOMPOSITION)
        state.composing = true;
    else if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS)
        state.composing = false;
    else if (message == WM_CHAR && wParam == 1 && !state.composing)
    {
        SendMessageW(window, EM_SETSEL, 0, -1);
        return 0;
    }
    else if (message == WM_NCDESTROY)
    {
        state.composing = false;
        RemoveWindowSubclass(window, FormEditProc, id);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
} // namespace
// 安装表单编辑框的输入法和全选处理，其他编辑能力由系统提供。
// 入参：window 为编辑框；state 为控件存活期间保持有效的状态。
// 返回：安装成功为 true；失败不接管 HWND。
bool AttachFormEditSupport(HWND window, FormEditState& state) noexcept
{
    state.composing = false;
    return window != nullptr &&
           SetWindowSubclass(window, FormEditProc, 1, reinterpret_cast<DWORD_PTR>(&state)) != FALSE;
}
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
// 入参：value 为原始 UTF-8 字节；multiline 为 true 时将换行统一为 CRLF。
// 返回：宽字符串；非空非法编码抛出异常，空值保持为空。
std::wstring EditWide(std::string_view value, bool multiline)
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
    if (multiline)
    {
        std::wstring normalized;
        normalized.reserve(result.size());
        for (std::size_t index = 0; index < result.size(); ++index)
        {
            if (result[index] == L'\r' || result[index] == L'\n')
            {
                normalized.append(L"\r\n");
                if (result[index] == L'\r' && index + 1 < result.size() && result[index + 1] == L'\n')
                    ++index;
            }
            else
                normalized.push_back(result[index]);
        }
        return normalized;
    }
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
