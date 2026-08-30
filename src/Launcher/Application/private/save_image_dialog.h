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
// 在调用线程已初始化 STA COM 的前提下显示系统保存对话框；取消不改目标。
[[nodiscard]] SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path& lastDirectory,
                                             SaveImageTarget& target, std::wstring& errorMessage);
} // namespace open_st
