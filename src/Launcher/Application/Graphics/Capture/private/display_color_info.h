// 文件职责：声明可注入的 DisplayConfig 查询边界，以及显示目标关联和颜色状态读取接口。

#pragma once

#include <frozen_desktop_frame.h>

#include <dxgi.h>
#include <windows.h>

namespace open_st
{
// 注入 Windows 显示配置边界，允许单元测试覆盖拓扑变化、错误和目标关联。
struct DisplayConfigApi final
{
    // 查询当前显示路径和模式数组所需容量，便于测试模拟拓扑变化。
    decltype(&GetDisplayConfigBufferSizes) getBufferSizes{&GetDisplayConfigBufferSizes};
    // 填充活动显示路径与模式信息，保持 Windows 查询结果和错误码语义。
    decltype(&QueryDisplayConfig) queryPaths{&QueryDisplayConfig};
    // 按显示目标请求设备信息，供源名称关联及 SDR 参考白读取。
    decltype(&DisplayConfigGetDeviceInfo) getDeviceInfo{&DisplayConfigGetDeviceInfo};
};

// 按源适配器和显示名定位唯一活动目标，补充 SDR 参考白信息。
// 入参：sourceAdapterId：源适配器 LUID；deviceName：源显示设备名；metadata：输入输出参数，保留已有颜色能力并更新目标身份和参考白字段；api：可注入的 DisplayConfig
// 查询接口。
// 返回：无返回值；成功写入目标身份和可用参考白，未找到唯一目标时身份无效，参考白查询失败时其有效标记为 false。
void ReadDisplayConfigColorInfo(LUID sourceAdapterId, const wchar_t* deviceName, OutputColorMetadata& metadata,
                                const DisplayConfigApi& api = {});
// 从 DXGI 输出补充源名称，并通过所属适配器查询显示目标颜色信息。
// 入参：output：借用的 DXGI 显示输出；description：该输出的 DXGI 描述；metadata：输入输出参数，补充源名称、目标身份和参考白。
// 返回：无返回值；源名称被保存，成功时补充精确目标信息；输出或适配器查询失败时提前返回。
void ReadDisplayColorInfo(IDXGIOutput* output, const DXGI_OUTPUT_DESC& description, OutputColorMetadata& metadata);
// 通过可注入系统接口检查冻结输出的参考白是否过期。
// 入参：metadata：冻结时保存的目标身份与参考白；api：用于回查目标白亮度的显示配置接口。
// 返回：未缓存参考白或当前查询值与缓存一致时为 true；身份无效、查询失败或参考白变化时为 false。
[[nodiscard]] bool IsCapturedOutputColorStateCurrentWithApi(const OutputColorMetadata& metadata,
                                                            const DisplayConfigApi& api) noexcept;
} // namespace open_st
