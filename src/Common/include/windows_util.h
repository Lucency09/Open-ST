// 声明不依赖业务目录与图形模块的 Windows 路径查询和 HRESULT 诊断辅助。

#pragma once

#include <filesystem>
#include <string>
#include <windows.h>

namespace open_st
{
// 查询当前可执行文件的父目录，不决定资源或可写状态的业务子目录。
// 入参：无。
// 返回：绝对父目录；系统查询失败或路径达到 32768 字符缓冲区上限时为空，分配异常交由调用方处理。
[[nodiscard]] std::filesystem::path GetExecutableDirectory();

// 组合失败操作名称和 HRESULT，保持现有图形与捕获诊断格式。
// 入参：operation：非空指针，指向失败操作的宽字符名称；result：该操作返回的 HRESULT。
// 返回：操作名称及大写十六进制 HRESULT 字符串；分配或流异常交由调用方处理。
[[nodiscard]] std::wstring FormatHResult(const wchar_t* operation, HRESULT result);
} // namespace open_st
