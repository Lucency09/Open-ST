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

// 逐字段比较适配器 LUID，避免仅按目标编号匹配不同显卡上的显示器。
bool EqualAdapterId(LUID first, LUID second) noexcept
{
    return first.LowPart == second.LowPart && first.HighPart == second.HighPart;
}

// 对活动拓扑快照进行有界重试，处理尺寸查询与实际查询之间发生的显示器变化。
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
// 严格绑定活动路径的源和目标；克隆源对应多个目标时不任意挑选一个白亮度。
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

// 保留 DXGI 显示源名称；只有成功取得所属适配器时才执行精确的 DisplayConfig 查询。
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

// 仅比较具有明确来源的参考白，避免把未知 SDR 信息误报为会话失效。
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

// 正常绘制前按缓存目标读取一次系统参考白，不建立额外线程或定时轮询。
bool IsCapturedOutputColorStateCurrent(const OutputColorMetadata& metadata) noexcept
{
    return IsCapturedOutputColorStateCurrentWithApi(metadata, DisplayConfigApi{});
}
} // namespace open_st
