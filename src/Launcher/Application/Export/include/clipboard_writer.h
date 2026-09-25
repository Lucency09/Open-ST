// 声明 SDR 图像复制入口，由系统剪贴板接收成功发布的数据所有权。

#pragma once

#include "sdr_image_view.h"
#include <Windows.h>
#include <string>
#include <string_view>

namespace open_st
{
// 将 SDR 图像复制到系统剪贴板，供其他应用按不透明 CF_DIB 粘贴。
// 入参：owner：有效的剪贴板所有者窗口；image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：成功发布时为 true，失败时为 false；成功后图像内存归系统，发布阶段失败可能已清空旧剪贴板。
[[nodiscard]] bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring& error);
// 将 Unicode 文本发布为 CF_UNICODETEXT，统一换行为 CRLF 并保留终止空字符。
// 入参：owner 为有效窗口；text 为借用的完整 UTF-16 文本；error 接收不含正文的诊断。
// 返回：成功为 true；非法编码、嵌入空字符或系统失败为 false，发布阶段失败可能已清空旧内容。
[[nodiscard]] bool CopyTextToClipboard(HWND owner, std::wstring_view text, std::wstring& error);
} // namespace open_st
