// 声明可注入的文件写入边界，支持验证短写、失败及新文件清理行为。

#pragma once

#include "image_file_writer.h"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace open_st
{
// 文件边界以实例注入，便于模拟短写和磁盘失败，不影响其他线程。
struct FileWriteApi
{
    decltype(&::CreateFileW) create{&::CreateFileW};
    decltype(&::WriteFile) write{&::WriteFile};
    decltype(&::FlushFileBuffers) flush{&::FlushFileBuffers};
    decltype(&::CloseHandle) close{&::CloseHandle};
    decltype(&::SetFileInformationByHandle) setInformation{&::SetFileInformationByHandle};
};
// 将已经编码的图像字节完整写入目标文件并刷新文件缓冲区。
// 入参：path：目标文件路径，已有文件会截断覆盖；bytes：调用期间借用的非空编码字节；api：调用期间有效的文件系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：全部字节写入且刷新成功时为 true；失败时为 false，尝试删除本次新建的不完整文件，覆盖失败不回滚旧文件。
[[nodiscard]] bool WriteEncodedFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                    const FileWriteApi& api, std::wstring& error);
// 先在内存完成 SDR 图像编码，再经注入接口写入目标文件，调用线程须已初始化 COM。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；path：目标路径，覆盖确认由调用方完成；format：PNG 或 JPEG
// 格式；api：调用期间有效的文件系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：编码、写入及刷新全部成功时为 true；否则为 false，编码失败不触碰目标，覆盖写入失败不保证回滚。
[[nodiscard]] bool WriteImageWithApi(const SdrImageView& image, const std::filesystem::path& path,
                                     ImageFileFormat format, const FileWriteApi& api, std::wstring& error);
} // namespace open_st
