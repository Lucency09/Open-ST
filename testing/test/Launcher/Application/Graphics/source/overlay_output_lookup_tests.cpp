// 文件职责：用固定显示拓扑验证覆盖窗口的适配器选择与布局失效诊断。

#include "overlay_output_lookup.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
// 枚举替身由测试用例持有，借出的 COM 引用无需释放测试数据。
struct BorrowedObject
{
    // 接受 ComPtr 对借用对象的释放。
    // 入参：无。
    // 返回：固定为 1，测试拓扑的存储仍然有效。
    ULONG Release() noexcept { return 1; }
};

struct TestOutput : BorrowedObject
{
    DXGI_OUTPUT_DESC desc{};
    HRESULT status{S_OK};

    // 返回测试指定的名称、位置和连接状态。
    // 入参：output 接收 DXGI 描述。
    // 返回：测试指定的查询结果。
    HRESULT GetDesc(DXGI_OUTPUT_DESC* output) const
    {
        *output = desc;
        return status;
    }
};

struct TestAdapter : BorrowedObject
{
    std::vector<TestOutput> outputs;

    // 按此适配器独立的编号枚举输出。
    // 入参：index 为屏序号，output 接收借用输出。
    // 返回：有效序号返回 S_OK，末尾返回 DXGI_ERROR_NOT_FOUND。
    HRESULT EnumOutputs(UINT index, TestOutput** output)
    {
        if (index >= outputs.size())
            return DXGI_ERROR_NOT_FOUND;
        *output = &outputs[index];
        return S_OK;
    }
};

struct TestFactory
{
    std::vector<TestAdapter> adapters;

    // 按固定顺序枚举测试适配器，索引零代表默认卡。
    // 入参：index 为适配器序号，adapter 接收借用适配器。
    // 返回：有效序号返回 S_OK，末尾返回 DXGI_ERROR_NOT_FOUND。
    HRESULT EnumAdapters1(UINT index, TestAdapter** adapter)
    {
        if (index >= adapters.size())
            return DXGI_ERROR_NOT_FOUND;
        *adapter = &adapters[index];
        return S_OK;
    }
};

// 构造测试用物理显示输出。
// 入参：name 为受控测试设备名，bounds 为虚拟桌面边界，attached 指示是否连接。
// 返回：包含完整身份的输出描述替身。
TestOutput MakeOutput(const wchar_t* name, RECT bounds, BOOL attached = TRUE)
{
    TestOutput output;
    wcscpy_s(output.desc.DeviceName, name);
    output.desc.DesktopCoordinates = bounds;
    output.desc.AttachedToDesktop = attached;
    return output;
}

// 构造位于主屏左侧的第二块屏的冻结身份。
// 入参：无。
// 返回：只含显示身份及物理边界的预览，查找过程无需像素。
open_st::OutputPreviewFrame LeftScreen()
{
    open_st::OutputPreviewFrame frame;
    wcscpy_s(frame.colorMetadata.deviceName.data(), frame.colorMetadata.deviceName.size(), L"DISPLAY2");
    frame.bounds = {-1920, 0, 0, 1080};
    return frame;
}
} // namespace

// 验证默认卡后的第二张有输出的适配器可以接管该屏，断开输出和无输出适配器均跳过。
// 入参：无。
// 返回：断言返回值及被选中的实际适配器对象。
TEST(OverlayOutputLookupTest, selects_owner_across_adapters)
{
    TestFactory factory;
    factory.adapters.resize(3);
    factory.adapters[0].outputs = {MakeOutput(L"DISPLAY1", {0, 0, 1920, 1080}),
                                   MakeOutput(L"DISPLAY2", {-1920, 0, 0, 1080}, FALSE)};
    factory.adapters[2].outputs = {MakeOutput(L"DISPLAY2", {-1920, 0, 0, 1080})};
    Microsoft::WRL::ComPtr<TestAdapter> selected;
    std::wstring error;
    ASSERT_TRUE(open_st::detail::FindOverlayAdapter<TestOutput>(factory, LeftScreen(), selected, error)) << error;
    EXPECT_EQ(selected.Get(), &factory.adapters[2]);
}

// 验证同卡的第二个活动输出可以匹配，即使首个输出属于另一块屏。
// 入参：无。
// 返回：断言命中同卡第二输出所属的适配器。
TEST(OverlayOutputLookupTest, selects_second_output_on_same_adapter)
{
    TestFactory factory;
    factory.adapters.resize(1);
    factory.adapters[0].outputs = {MakeOutput(L"DISPLAY1", {0, 0, 1920, 1080}),
                                   MakeOutput(L"DISPLAY2", {-1920, 0, 0, 1080})};
    Microsoft::WRL::ComPtr<TestAdapter> selected;
    std::wstring error;
    ASSERT_TRUE(open_st::detail::FindOverlayAdapter<TestOutput>(factory, LeftScreen(), selected, error)) << error;
    EXPECT_EQ(selected.Get(), &factory.adapters[0]);
}

// 验证设备同名但坐标改变时拒绝，并区分查询失败与真实布局变化。
// 入参：无。
// 返回：分别断言坐标诊断和底层 GetDesc 失败码。
TEST(OverlayOutputLookupTest, distinguishes_changed_bounds_from_query_failure)
{
    TestFactory factory;
    factory.adapters.resize(1);
    factory.adapters[0].outputs = {MakeOutput(L"DISPLAY2", {0, 0, 1920, 1080})};
    Microsoft::WRL::ComPtr<TestAdapter> selected;
    std::wstring error;
    ASSERT_FALSE(open_st::detail::FindOverlayAdapter<TestOutput>(factory, LeftScreen(), selected, error));
    EXPECT_EQ(selected.Get(), nullptr);
    EXPECT_NE(error.find(L"目标=DISPLAY2 [-1920,0,0,1080]"), std::wstring::npos);
    EXPECT_NE(error.find(L"实际=DISPLAY2 [0,0,1920,1080]"), std::wstring::npos);
    factory.adapters[0].outputs[0].status = DXGI_ERROR_NOT_FOUND;
    ASSERT_FALSE(open_st::detail::FindOverlayAdapter<TestOutput>(factory, LeftScreen(), selected, error));
    EXPECT_NE(error.find(L"GetDesc"), std::wstring::npos);
    EXPECT_NE(error.find(L"0x887A0002"), std::wstring::npos);
}
