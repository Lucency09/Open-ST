// 声明可注入的剪贴板系统边界和 DIB 构造入口，供隔离副作用测试使用。

#pragma once

#include "clipboard_writer.h"
#include <vector>

namespace open_st
{
// 显式注入系统边界供测试替身使用，产品默认绑定 Win32，不修改全局函数表。
struct ClipboardApi
{
    decltype(&::IsWindow) isWindow{&::IsWindow};
    decltype(&::GlobalAlloc) allocate{&::GlobalAlloc};
    decltype(&::GlobalLock) lock{&::GlobalLock};
    decltype(&::GlobalUnlock) unlock{&::GlobalUnlock};
    decltype(&::GlobalFree) free{&::GlobalFree};
    decltype(&::OpenClipboard) open{&::OpenClipboard};
    decltype(&::EmptyClipboard) empty{&::EmptyClipboard};
    decltype(&::SetClipboardData) set{&::SetClipboardData};
    decltype(&::CloseClipboard) close{&::CloseClipboard};
};
// 把 SDR 图像转换为可发布到剪贴板的底向上 32 位 BI_RGB DIB。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；dib：输出参数，成功时接收位图头及紧凑像素字节；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：构建完成时为 true；校验或分配失败时为 false 且不修改 dib；DIB 保留 RGB 并把保留字节清零。
[[nodiscard]] bool BuildClipboardDib(const SdrImageView& image, std::vector<std::uint8_t>& dib, std::wstring& error);
// 通过注入的系统接口将完整 SDR 图像发布为 CF_DIB，并转交全局内存所有权。
// 入参：owner：有效的剪贴板所有者窗口；image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；api：调用期间有效的剪贴板系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：成功发布时为 true；准备或系统调用失败时为 false，并释放尚未移交的本地内存；清空剪贴板之后的失败无法恢复旧内容。
[[nodiscard]] bool CopyImageWithApi(HWND owner, const SdrImageView& image, const ClipboardApi& api,
                                    std::wstring& error);
} // namespace open_st
