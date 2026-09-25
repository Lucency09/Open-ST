// 公共原生编辑输入的编码、文本和字体辅助。
#pragma once
#include <string>
#include <string_view>
#include <windows.h>
namespace open_st::renderer_detail
{
// 表单编辑框独占的输入法状态，生命周期覆盖原生控件。
struct FormEditState
{
    bool composing{};
};
// 安装表单编辑框的输入法和全选处理，其他编辑能力由系统提供。
// 入参：window 为编辑框；state 为控件存活期间保持有效的状态。
// 返回：安装成功为 true；失败不接管 HWND。
bool AttachFormEditSupport(HWND window, FormEditState& state) noexcept;
// 读取原生编辑全文。入参：window。返回：文本，分配失败抛异常。
std::wstring ReadEditText(HWND window);
// 转换宿主 UTF-8 文本为原生编辑框文本。
// 入参：value 为 UTF-8 文本；multiline 为 true 时将换行统一为 CRLF。
// 返回：UTF-16 文本，非法编码抛异常。
std::wstring EditWide(std::string_view value, bool multiline = false);
// 转UTF8。入参：UTF16文本。返回：文本，非法抛异常。
std::string EditUtf8(std::wstring_view value);
// 创建自有字体。入参：系统或业务LOGFONT。返回：须DeleteObject的字体，可空。
HFONT CreateEditFont(const LOGFONTW& value) noexcept;
} // namespace open_st::renderer_detail
