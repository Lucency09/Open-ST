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
// 构建正高度、底向上、不透明32位 BI_RGB DIB，失败不修改输出。
[[nodiscard]] bool BuildClipboardDib(const SdrImageView& image, std::vector<std::uint8_t>& dib, std::wstring& error);
// 通过可注入系统边界发布图像，失败释放本地所有权，成功将全局内存转给系统。
[[nodiscard]] bool CopyImageWithApi(HWND owner, const SdrImageView& image, const ClipboardApi& api,
                                    std::wstring& error);
} // namespace open_st
