#pragma once

#include "sdr_image_view.h"
#include <Windows.h>
#include <string>

namespace open_st
{
// 将图像发布为不透明 CF_DIB；owner 必须是有效窗口，失败返回诊断文本，不进行无限重试。
[[nodiscard]] bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring& error);
} // namespace open_st
