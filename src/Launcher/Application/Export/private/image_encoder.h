// 声明 SDR 输入校验及内存编码接口，隔离编码与文件系统副作用。

#pragma once

#include "image_file_writer.h"
#include <vector>

namespace open_st
{
// 检查 SDR 图像视图能否被 Windows 图像接口安全读取。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：尺寸、行跨度及缓冲区覆盖范围有效时为 true；否则为 false，允许最后一行不带行尾填充。
[[nodiscard]] bool ValidateSdrImage(const SdrImageView& image, std::wstring& error);
// 将 SDR 图像编码为含 sRGB 元数据的 PNG 或 JPEG 内存字节，调用线程须已初始化 COM。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；format：PNG 或 JPEG
// 编码格式；encoded：输出参数，成功时接收自有编码字节；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：完整编码成功时为 true；输入、格式、WIC 调用或分配失败时为 false，encoded 保持原值。
[[nodiscard]] bool EncodeSdrImage(const SdrImageView& image, ImageFileFormat format, std::vector<std::uint8_t>& encoded,
                                  std::wstring& error);
} // namespace open_st
