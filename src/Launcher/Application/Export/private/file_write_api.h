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
// 写入已编码字节；失败只标记本次新建文件删除，已有文件不删除，覆盖失败不承诺回滚。
[[nodiscard]] bool WriteEncodedFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                    const FileWriteApi& api, std::wstring& error);
// 编码全部成功后才进入可注入的文件边界，供测试验证失败不会创建或截断目标。
[[nodiscard]] bool WriteImageWithApi(const SdrImageView& image, const std::filesystem::path& path,
                                     ImageFileFormat format, const FileWriteApi& api, std::wstring& error);
} // namespace open_st
