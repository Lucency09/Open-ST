// 公共原生编辑输入的编码、文本和字体辅助。
#pragma once
#include <string>
#include <string_view>
#include <windows.h>
namespace open_st::renderer_detail
{
// 读取原生编辑全文。入参：window。返回：文本，分配失败抛异常。
std::wstring ReadEditText(HWND window);
// 转UTF16。入参：UTF8文本。返回：文本，非法抛异常。
std::wstring EditWide(std::string_view value);
// 转UTF8。入参：UTF16文本。返回：文本，非法抛异常。
std::string EditUtf8(std::wstring_view value);
// 创建自有字体。入参：系统或业务LOGFONT。返回：须DeleteObject的字体，可空。
HFONT CreateEditFont(const LOGFONTW& value) noexcept;
} // namespace open_st::renderer_detail
