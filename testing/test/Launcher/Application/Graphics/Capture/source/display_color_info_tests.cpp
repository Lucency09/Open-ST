#include <gtest/gtest.h>

#include "display_color_info.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace
{
std::vector<DISPLAYCONFIG_PATH_INFO> testPaths;
LONG topologyResult = ERROR_SUCCESS;
LONG whiteLevelResult = ERROR_SUCCESS;
ULONG rawWhiteLevel = 2500U;
unsigned int retryCount{};
unsigned int whiteQueries{};
LUID queriedTargetAdapter{};
UINT32 queriedTargetId{};

// 构造源适配器和目标适配器不同的活动路径，防止查询误用源编号。
DISPLAYCONFIG_PATH_INFO MakePath(ULONG sourceAdapter, UINT32 sourceId, ULONG targetAdapter, UINT32 targetId)
{
    DISPLAYCONFIG_PATH_INFO path{};
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.adapterId = LUID{sourceAdapter, 3};
    path.sourceInfo.id = sourceId;
    path.targetInfo.adapterId = LUID{targetAdapter, 4};
    path.targetInfo.id = targetId;
    return path;
}

// 返回伪造活动路径的缓冲区尺寸，并支持模拟系统拓扑查询失败。
LONG WINAPI FakeBufferSizes(UINT32 flags, UINT32* pathCount, UINT32* modeCount)
{
    EXPECT_EQ(flags, QDC_ONLY_ACTIVE_PATHS);
    *pathCount = static_cast<UINT32>(testPaths.size());
    *modeCount = 1U;
    return topologyResult;
}

// 返回可注入一次尺寸竞态的拓扑快照，验证重试确实重新读取当前路径。
LONG WINAPI FakeQueryPaths(UINT32 flags, UINT32* pathCount, DISPLAYCONFIG_PATH_INFO* paths,
                           UINT32* modeCount, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID* topology)
{
    EXPECT_EQ(flags, QDC_ONLY_ACTIVE_PATHS);
    EXPECT_EQ(topology, nullptr);
    if (retryCount != 0U)
    {
        --retryCount;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    std::copy(testPaths.begin(), testPaths.end(), paths);
    *pathCount = static_cast<UINT32>(testPaths.size());
    *modeCount = 0U;
    return ERROR_SUCCESS;
}

// 按源编号返回显示名，记录白亮度查询使用的目标适配器和目标编号。
LONG WINAPI FakeDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* header)
{
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME* source = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(header);
        const wchar_t* name = header->id == 7U ? L"\\\\.\\DISPLAY1" : L"\\\\.\\DISPLAY2";
        (void)wcscpy_s(source->viewGdiDeviceName, name);
        return ERROR_SUCCESS;
    }
    EXPECT_EQ(header->type, DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL);
    ++whiteQueries;
    queriedTargetAdapter = header->adapterId;
    queriedTargetId = header->id;
    DISPLAYCONFIG_SDR_WHITE_LEVEL* white = reinterpret_cast<DISPLAYCONFIG_SDR_WHITE_LEVEL*>(header);
    white->SDRWhiteLevel = rawWhiteLevel;
    return whiteLevelResult;
}

const open_st::DisplayConfigApi TEST_API{&FakeBufferSizes, &FakeQueryPaths, &FakeDeviceInfo};

// 为每个测试建立独立系统边界状态，避免错误注入泄漏至其他用例。
class DisplayColorInfoTest : public testing::Test
{
  protected:
    // 恢复活动路径、查询返回值和目标记录为可预测初始状态。
    void SetUp() override
    {
        testPaths = {MakePath(99U, 7U, 99U, 99U), MakePath(11U, 8U, 22U, 88U),
                     MakePath(11U, 7U, 22U, 42U)};
        topologyResult = ERROR_SUCCESS;
        whiteLevelResult = ERROR_SUCCESS;
        rawWhiteLevel = 2500U;
        retryCount = 0U;
        whiteQueries = 0U;
        queriedTargetAdapter = {};
        queriedTargetId = 0U;
    }
};
} // namespace

// 验证同名源必须同时匹配适配器，并使用该路径的目标身份查询 200 nit SDR 参考白。
TEST_F(DisplayColorInfoTest, binds_source_adapter_and_name_to_exact_target)
{
    open_st::OutputColorMetadata metadata{};
    metadata.maximumLuminance = 1000.0F;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_TRUE(metadata.hasDisplayConfigIdentity);
    EXPECT_TRUE(metadata.hasSdrWhiteLevel);
    EXPECT_FLOAT_EQ(metadata.sdrWhiteLevelNits, 200.0F);
    EXPECT_FLOAT_EQ(metadata.maximumLuminance, 1000.0F);
    EXPECT_EQ(metadata.adapterLowPart, 22U);
    EXPECT_EQ(metadata.adapterHighPart, 4);
    EXPECT_EQ(metadata.targetId, 42U);
    EXPECT_EQ(queriedTargetAdapter.LowPart, 22U);
    EXPECT_EQ(queriedTargetAdapter.HighPart, 4);
    EXPECT_EQ(queriedTargetId, 42U);
    EXPECT_EQ(whiteQueries, 1U);
}

// 验证显示器变化导致缓冲区过小时重新查询；持续变化达到上限后保持元数据未知。
TEST_F(DisplayColorInfoTest, retries_topology_race_with_a_finite_limit)
{
    open_st::OutputColorMetadata metadata{};
    retryCount = 1U;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_TRUE(metadata.hasSdrWhiteLevel);
    retryCount = 10U;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_FALSE(metadata.hasDisplayConfigIdentity);
    EXPECT_FALSE(metadata.hasSdrWhiteLevel);
    EXPECT_FLOAT_EQ(metadata.sdrWhiteLevelNits, 0.0F);
}

// 验证权限错误、零白亮度和目标不存在均不生成猜测值，且不拿峰值亮度替代参考白。
TEST_F(DisplayColorInfoTest, leaves_failed_or_invalid_white_level_explicitly_unknown)
{
    open_st::OutputColorMetadata metadata{};
    metadata.maximumLuminance = 1000.0F;
    whiteLevelResult = ERROR_ACCESS_DENIED;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_TRUE(metadata.hasDisplayConfigIdentity);
    EXPECT_FALSE(metadata.hasSdrWhiteLevel);
    whiteLevelResult = ERROR_SUCCESS;
    rawWhiteLevel = 0U;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_FALSE(metadata.hasSdrWhiteLevel);
    EXPECT_FLOAT_EQ(metadata.sdrWhiteLevelNits, 0.0F);
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"missing", metadata, TEST_API);
    EXPECT_FALSE(metadata.hasDisplayConfigIdentity);
    topologyResult = ERROR_ACCESS_DENIED;
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_FALSE(metadata.hasDisplayConfigIdentity);
    EXPECT_FLOAT_EQ(metadata.maximumLuminance, 1000.0F);
}

// 验证同一显示源映射到不同克隆目标时拒绝任意挑选参考白。
TEST_F(DisplayColorInfoTest, rejects_ambiguous_clone_targets)
{
    testPaths.push_back(MakePath(11U, 7U, 22U, 43U));
    open_st::OutputColorMetadata metadata{};
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_FALSE(metadata.hasDisplayConfigIdentity);
    EXPECT_FALSE(metadata.hasSdrWhiteLevel);
    EXPECT_EQ(whiteQueries, 0U);
}

// 验证已知参考白变化或系统查询失败会使会话失效，并始终使用捕获时绑定的目标。
TEST_F(DisplayColorInfoTest, detects_changed_or_unqueryable_captured_reference_white)
{
    open_st::OutputColorMetadata metadata{};
    open_st::ReadDisplayConfigColorInfo(LUID{11U, 3}, L"\\\\.\\DISPLAY1", metadata, TEST_API);
    EXPECT_TRUE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
    EXPECT_EQ(queriedTargetAdapter.LowPart, 22U);
    EXPECT_EQ(queriedTargetAdapter.HighPart, 4);
    EXPECT_EQ(queriedTargetId, 42U);
    rawWhiteLevel = 2600U;
    EXPECT_FALSE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
    rawWhiteLevel = 2500U;
    whiteLevelResult = ERROR_ACCESS_DENIED;
    EXPECT_FALSE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
    whiteLevelResult = ERROR_SUCCESS;
    rawWhiteLevel = 0U;
    EXPECT_FALSE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
}

// 验证无参考白缓存时不猜测状态，而自述已知参考白却缺失目标身份时必须拒绝。
TEST_F(DisplayColorInfoTest, allows_unknown_white_without_accepting_inconsistent_known_metadata)
{
    open_st::OutputColorMetadata metadata{};
    EXPECT_TRUE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
    EXPECT_EQ(whiteQueries, 0U);
    metadata.hasSdrWhiteLevel = true;
    metadata.sdrWhiteLevelNits = 200.0F;
    EXPECT_FALSE(open_st::IsCapturedOutputColorStateCurrentWithApi(metadata, TEST_API));
    EXPECT_EQ(whiteQueries, 0U);
}
