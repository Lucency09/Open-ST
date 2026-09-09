// 声明 PNG/JPEG 图片文件输出接口及文件格式类型。

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

// 将 SDR 图像保存为 PNG 或 JPEG 文件，调用线程须已初始化 COM。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；path：目标路径，已有文件的覆盖确认由调用方负责；format：PNG 或
// JPEG 格式；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：完整编码、写入和刷新成功时为 true；否则为 false，编码失败不触碰目标，覆盖过程不保证原子性。
[[nodiscard]] bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format,
                                  std::wstring& error);
} // namespace open_st
