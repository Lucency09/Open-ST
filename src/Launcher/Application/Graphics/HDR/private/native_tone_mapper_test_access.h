// 文件职责：仅向 HDR 定向测试开放未初始化转换器的 WARP 驱动选择。
#pragma once
#include <native_tone_mapper.h>
namespace open_st
{
class NativeToneMapperTestAccess final
{
  public:
    // 在首次建图形设备前强制 WARP，完整运行同一生产效果链。
    // 入参：mapper 为未初始化转换器。返回：未初始化时设置成功，否则失败。
    static bool ForceWarp(NativeToneMapper& mapper) noexcept;
};
} // namespace open_st
