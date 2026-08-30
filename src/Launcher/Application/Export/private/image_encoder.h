#pragma once

#include "image_file_writer.h"
#include <vector>

namespace open_st
{
// 校验尺寸、行跨度和完整缓冲区长度，并限制 Windows 图像接口的整数范围。
[[nodiscard]] bool ValidateSdrImage(const SdrImageView& image, std::wstring& error);
// 将不透明 RGB 完整编码到内存，失败不修改输出；调用线程须已初始化 COM。
[[nodiscard]] bool EncodeSdrImage(const SdrImageView& image, ImageFileFormat format, std::vector<std::uint8_t>& encoded,
                                  std::wstring& error);
} // namespace open_st
