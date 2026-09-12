// 实现保存目录与 Settings 字符串之间的转换，统一截图与贴图的目录记录边界。
#include "save_directory.h"
#include <settings.h>

namespace open_st
{
// 读取并解码上次保存目录，维持保存对话框的初始路径规则。
// 入参：无。
// 返回：已配置目录或空路径；异常由保存编排处理。
std::filesystem::path LastSaveDirectory()
{
    const std::optional<std::string> directory = GetStringSetting("capture.last_save_directory");
    return directory ? std::filesystem::path(
                           std::u8string_view(reinterpret_cast<const char8_t*>(directory->data()), directory->size()))
                     : std::filesystem::path{};
}

// 隔离保存完成后记录目录的失败，避免把成功输出误报为保存失败。
// 入参：path 为已成功保存的文件路径。
// 返回：目录写入成功 true；转换、分配或写入失败 false。
bool RememberSaveDirectory(const std::filesystem::path& path) noexcept
{
    try
    {
        const std::u8string directory = path.parent_path().u8string();
        return SetStringSetting("capture.last_save_directory",
                                std::string_view(reinterpret_cast<const char*>(directory.data()), directory.size()));
    }
    catch (...)
    {
        return false;
    }
}
} // namespace open_st
