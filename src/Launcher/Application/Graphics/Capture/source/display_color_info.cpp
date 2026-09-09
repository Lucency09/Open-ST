// 文件职责：实现 DXGI 与 DisplayConfig 显示目标关联，读取并校验 SDR 参考白元数据。

#include "display_color_info.h"

#include <display_color_state.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr unsigned int MAX_TOPOLOGY_QUERY_ATTEMPTS = 3U;

// 比较完整适配器 LUID，避免把不同显卡的同编号显示目标混为一体。
// 入参：first、second：待比较的 Windows 适配器 LUID。
// 返回：高低字段均相等时为 true，否则为 false。
bool EqualAdapterId(LUID first, LUID second) noexcept
{
    return first.LowPart == second.LowPart && first.HighPart == second.HighPart;
}

// 读取活动显示路径，并对查询期间拓扑变化进行有限重试。
// 入参：api：可注入的 Windows 显示配置查询接口；paths：输出参数，成功时接收活动显示路径。
// 返回：取得完整快照时为 true；无路径、查询错误或重试耗尽为 false，失败时 paths 不承诺为可用快照。
bool QueryActivePaths(const open_st::DisplayConfigApi& api, std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
{
    for (unsigned int attempt = 0U; attempt < MAX_TOPOLOGY_QUERY_ATTEMPTS; ++attempt)
    {
        UINT32 pathCount{};
        UINT32 modeCount{};
        if (api.getBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0U)
        {
            return false;
        }
        paths.resize(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(std::max(modeCount, 1U));
        const LONG result = api.queryPaths(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                           modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (result != ERROR_SUCCESS)
        {
            return false;
        }
        paths.resize(pathCount);
        return true;
    }
    return false;
}
} // namespace

namespace open_st
{
// 按源适配器和显示名定位唯一活动目标，补充 SDR 参考白信息。
// 入参：sourceAdapterId：源适配器 LUID；deviceName：源显示设备名；metadata：输入输出参数，保留已有颜色能力并更新目标身份和参考白字段；api：可注入的 DisplayConfig
// 查询接口。
// 返回：无返回值；成功写入目标身份和可用参考白，未找到唯一目标时身份无效，参考白查询失败时其有效标记为 false。
void ReadDisplayConfigColorInfo(LUID sourceAdapterId, const wchar_t* deviceName, OutputColorMetadata& metadata,
                                const DisplayConfigApi& api)
{
    metadata.hasDisplayConfigIdentity = false;
    metadata.hasSdrWhiteLevel = false;
    metadata.adapterLowPart = 0U;
    metadata.adapterHighPart = 0;
    metadata.targetId = 0U;
    metadata.sdrWhiteLevelNits = 0.0F;
    if (deviceName == nullptr || deviceName[0] == L'\0')
    {
        return;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!QueryActivePaths(api, paths))
    {
        return;
    }
    const DISPLAYCONFIG_PATH_INFO* matchingPath = nullptr;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0U ||
            !EqualAdapterId(path.sourceInfo.adapterId, sourceAdapterId))
        {
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (api.getDeviceInfo(&source.header) != ERROR_SUCCESS)
        {
            return;
        }
        if (std::wcscmp(source.viewGdiDeviceName, deviceName) != 0)
        {
            continue;
        }
        if (matchingPath != nullptr &&
            (!EqualAdapterId(matchingPath->targetInfo.adapterId, path.targetInfo.adapterId) ||
             matchingPath->targetInfo.id != path.targetInfo.id))
        {
            return;
        }
        matchingPath = &path;
    }
    if (matchingPath == nullptr)
    {
        return;
    }

    metadata.adapterLowPart = matchingPath->targetInfo.adapterId.LowPart;
    metadata.adapterHighPart = matchingPath->targetInfo.adapterId.HighPart;
    metadata.targetId = matchingPath->targetInfo.id;
    metadata.hasDisplayConfigIdentity = true;
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = matchingPath->targetInfo.adapterId;
    whiteLevel.header.id = matchingPath->targetInfo.id;
    if (api.getDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS && whiteLevel.SDRWhiteLevel != 0U)
    {
        // Windows 的值是 80 nit 倍数乘以 1000，不能与显示器最大亮度混用。
        metadata.sdrWhiteLevelNits = static_cast<float>(whiteLevel.SDRWhiteLevel) * 80.0F / 1000.0F;
        metadata.hasSdrWhiteLevel = true;
    }
}

// 从 DXGI 输出补充源名称，并通过所属适配器查询显示目标颜色信息。
// 入参：output：借用的 DXGI 显示输出；description：该输出的 DXGI 描述；metadata：输入输出参数，补充源名称、目标身份和参考白。
// 返回：无返回值；源名称被保存，成功时补充精确目标信息；输出或适配器查询失败时提前返回。
void ReadDisplayColorInfo(IDXGIOutput* output, const DXGI_OUTPUT_DESC& description, OutputColorMetadata& metadata)
{
    std::copy_n(description.DeviceName, metadata.deviceName.size(), metadata.deviceName.begin());
    metadata.deviceName.back() = L'\0';
    ComPtr<IDXGIAdapter> adapter;
    if (output == nullptr || FAILED(output->GetParent(IID_PPV_ARGS(adapter.GetAddressOf()))))
    {
        return;
    }
    DXGI_ADAPTER_DESC adapterDescription{};
    if (FAILED(adapter->GetDesc(&adapterDescription)))
    {
        return;
    }
    ReadDisplayConfigColorInfo(adapterDescription.AdapterLuid, metadata.deviceName.data(), metadata);
}

// 通过可注入系统接口检查冻结输出的参考白是否过期。
// 入参：metadata：冻结时保存的目标身份与参考白；api：用于回查目标白亮度的显示配置接口。
// 返回：未缓存参考白或当前查询值与缓存一致时为 true；身份无效、查询失败或参考白变化时为 false。
bool IsCapturedOutputColorStateCurrentWithApi(const OutputColorMetadata& metadata,
                                             const DisplayConfigApi& api) noexcept
{
    if (!metadata.hasSdrWhiteLevel)
    {
        return true;
    }
    if (!metadata.hasDisplayConfigIdentity || metadata.sdrWhiteLevelNits <= 0.0F)
    {
        return false;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = LUID{metadata.adapterLowPart, metadata.adapterHighPart};
    whiteLevel.header.id = metadata.targetId;
    if (api.getDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS || whiteLevel.SDRWhiteLevel == 0U)
    {
        return false;
    }
    const float currentNits = static_cast<float>(whiteLevel.SDRWhiteLevel) * 80.0F / 1000.0F;
    return currentNits == metadata.sdrWhiteLevelNits;
}

// 检查冻结时保存的 SDR 参考白是否仍与系统一致。
// 入参：metadata：冻结 plane 保存的目标身份、参考白值及有效标记。
// 返回：未缓存参考白时为 true；已缓存且查询值一致时为 true，元数据矛盾、查询失败或亮度变化时为 false。
bool IsCapturedOutputColorStateCurrent(const OutputColorMetadata& metadata) noexcept
{
    return IsCapturedOutputColorStateCurrentWithApi(metadata, DisplayConfigApi{});
}
} // namespace open_st
