#pragma once

#include "sdr_image_view.h"
#include <filesystem>
#include <string>

namespace open_st
{
enum class ImageFileFormat
{
    Jpeg,
    Png
};

// 先内存编码再写目标，调用线程须已初始化 COM；覆盖确认归调用方，覆盖写入不保证原子性。
[[nodiscard]] bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format,
                                  std::wstring& error);
} // namespace open_st
