#pragma once

#include <json_file.h>

namespace open_st
{
// 仅测试目标可见；产品模块不得包含本头文件。
struct JsonFileTestAccess final
{
    // 清理指定绑定以模拟冷启动；还有外部句柄时拒绝清理。
    [[nodiscard]] static bool ReleaseFile(std::string_view cardName) noexcept
    {
        return JsonFileManager::Instance().ReleaseFile(cardName);
    }
};
} // namespace open_st
