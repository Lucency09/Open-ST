// 文件职责：按冻结帧身份逐适配器查找活动输出，供覆盖窗口在该屏所属设备上创建交换链。

#pragma once

#include <desktop_preview.h>
#include <windows_util.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cwchar>
#include <utility>

namespace open_st::detail
{
// 将活动输出与冻结帧的设备名及物理边界比对。
// 入参：output 为实时描述，frame 为单屏冻结预览。
// 返回：活动状态、名称及边界全部匹配时为 true。
bool MatchesOverlayOutput(const DXGI_OUTPUT_DESC& output, const OutputPreviewFrame& frame) noexcept;

// 格式化日志中的显示源名称和边界。
// 入参：name 为设备名，bounds 为虚拟桌面物理坐标。
// 返回：日志用的设备名及四条边界。
std::wstring DescribeOutput(const wchar_t* name, RectI bounds);

// 格式化枚举或交换链查询返回的显示输出。
// 入参：output 为实时 DXGI 显示描述。
// 返回：含名称、边界和断开状态的输出说明。
std::wstring DescribeOutput(const DXGI_OUTPUT_DESC& output);

// 扫描同一个工厂快照，选择拥有冻结输出的适配器；测试可以使用固定的 DXGI 拓扑。
// 入参：factory 为 DXGI 工厂，frame 为有身份的冻结预览，selected 接收适配器，errorMessage 接收失败原因。
// 返回：找到附着输出时为 true；枚举失败或名称、边界已失效时为 false。
template <typename Output = IDXGIOutput, typename Factory, typename Adapter>
bool FindOverlayAdapter(Factory& factory, const OutputPreviewFrame& frame, Microsoft::WRL::ComPtr<Adapter>& selected,
                        std::wstring& errorMessage)
{
    const std::wstring expected = L"，目标=" + DescribeOutput(frame.colorMetadata.deviceName.data(), frame.bounds);
    std::wstring actual = L"未找到同名输出";
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        Microsoft::WRL::ComPtr<Adapter> adapter;
        HRESULT result = factory.EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result))
        {
            errorMessage = open_st::FormatHResult(L"EnumAdapters1", result) + expected;
            return false;
        }
        for (UINT outputIndex = 0;; ++outputIndex)
        {
            Microsoft::WRL::ComPtr<Output> output;
            result = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result))
            {
                errorMessage = open_st::FormatHResult(L"EnumOutputs", result) + expected;
                return false;
            }
            DXGI_OUTPUT_DESC desc{};
            result = output->GetDesc(&desc);
            if (FAILED(result))
            {
                errorMessage = open_st::FormatHResult(L"IDXGIOutput::GetDesc", result) + expected;
                return false;
            }
            if (MatchesOverlayOutput(desc, frame))
            {
                selected = std::move(adapter);
                return true;
            }
            if (std::wcscmp(desc.DeviceName, frame.colorMetadata.deviceName.data()) == 0)
                actual = DescribeOutput(desc);
        }
    }
    errorMessage = L"当前显示输出与冻结帧不一致，请重新截图。" + expected + L"，实际=" + actual;
    return false;
}
} // namespace open_st::detail
