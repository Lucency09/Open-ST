// 声明截图与贴图共用的保存目录设置适配，不参与输出或窗口生命周期。
#pragma once

#include <filesystem>

namespace open_st
{
// 读取并解码上次保存目录。
// 入参：无。
// 返回：已配置目录或空路径；读取和路径转换异常交由调用方归类为保存失败。
std::filesystem::path LastSaveDirectory();
// 记录成功输出文件的父目录，隔离输出成功后的次要故障。
// 入参：path 为已成功保存的文件路径。
// 返回：记录成功 true；设置写入、路径转换或分配失败 false，不撤销图片。
bool RememberSaveDirectory(const std::filesystem::path& path) noexcept;
} // namespace open_st
