// 声明 SDR 图像复制入口，由系统剪贴板接收成功发布的数据所有权。

#pragma once

#include "sdr_image_view.h"
#include <Windows.h>
#include <string>

namespace open_st
{
// 将 SDR 图像复制到系统剪贴板，供其他应用按不透明 CF_DIB 粘贴。
// 入参：owner：有效的剪贴板所有者窗口；image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：成功发布时为 true，失败时为 false；成功后图像内存归系统，发布阶段失败可能已清空旧剪贴板。
[[nodiscard]] bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring& error);
} // namespace open_st
