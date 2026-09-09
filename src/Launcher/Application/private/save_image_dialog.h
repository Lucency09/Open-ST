// 声明图片保存目标和系统对话框入口，区分确认、取消及失败。

#pragma once
#include "capture_completion.h"
#include <filesystem>
#include <image_file_writer.h>
#include <string>
#include <windows.h>

namespace open_st
{
struct SaveImageTarget final
{
    std::filesystem::path path;
    ImageFileFormat format{ImageFileFormat::Png};
};
// 让用户选择截图保存路径及 PNG 或 JPEG 编码格式。
// 入参：owner：借用的所属窗口；lastDirectory：上次目录，空或失效时使用系统默认；target：成功时输出目标；errorMessage：失败诊断输出；调用线程须初始化 STA COM。
// 返回：确认返回 Accepted；取消返回 Cancelled 且保留 target；系统失败返回 Failed 并写诊断。
[[nodiscard]] SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path& lastDirectory,
                                             SaveImageTarget& target, std::wstring& errorMessage);
} // namespace open_st
