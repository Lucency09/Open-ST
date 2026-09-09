// 提供仅测试可见的 JSON 绑定释放入口，支持隔离文件状态。

#pragma once

#include <json_file.h>

namespace open_st
{
// 仅测试目标可见；产品模块不得包含本头文件。
struct JsonFileTestAccess final
{
    // 为隔离测试释放指定业务文件的管理器绑定。
    // 入参：cardName：要解除的业务文件标识。
    // 返回：无绑定或成功移除时为 true；仍有外部句柄持有状态或内部失败时为 false。
    [[nodiscard]] static bool ReleaseFile(std::string_view cardName) noexcept
    {
        return JsonFileManager::Instance().ReleaseFile(cardName);
    }
};
} // namespace open_st
