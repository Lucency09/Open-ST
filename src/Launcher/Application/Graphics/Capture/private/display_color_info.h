#pragma once

#include <frozen_desktop_frame.h>

#include <dxgi.h>
#include <windows.h>

namespace open_st
{
// 注入 Windows 显示配置边界，允许单元测试覆盖拓扑变化、错误和目标关联。
struct DisplayConfigApi final
{
    decltype(&GetDisplayConfigBufferSizes) getBufferSizes{&GetDisplayConfigBufferSizes};
    decltype(&QueryDisplayConfig) queryPaths{&QueryDisplayConfig};
    decltype(&DisplayConfigGetDeviceInfo) getDeviceInfo{&DisplayConfigGetDeviceInfo};
};

// 按源适配器和 GDI 名称查找唯一活动目标，再读取其 SDR 白亮度；失败保持有效性标记为 false。
void ReadDisplayConfigColorInfo(LUID sourceAdapterId, const wchar_t* deviceName, OutputColorMetadata& metadata,
                                const DisplayConfigApi& api = {});
// 从 DXGI 输出取得准确的适配器身份并补充显示目标信息，不改写已有 Output6 颜色能力。
void ReadDisplayColorInfo(IDXGIOutput* output, const DXGI_OUTPUT_DESC& description, OutputColorMetadata& metadata);
// 通过可注入系统边界验证已缓存参考白，使亮度变化和查询失败无需真实 HDR 硬件即可测试。
[[nodiscard]] bool IsCapturedOutputColorStateCurrentWithApi(const OutputColorMetadata& metadata,
                                                            const DisplayConfigApi& api) noexcept;
} // namespace open_st
