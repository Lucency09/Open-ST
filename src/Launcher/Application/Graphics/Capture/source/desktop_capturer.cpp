#include <desktop_capturer.h>

#include "output_capture.h"

#include <log.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
// 把失败阶段和 HRESULT 格式化为供上层诊断使用的宽字符串。
std::wstring FormatHResult(const wchar_t* operation, HRESULT result)
{
    std::wostringstream stream;
    stream << operation << L"失败，HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

// 为指定显示适配器创建同属该适配器的 D3D11 设备和立即上下文。
bool CreateDeviceForAdapter(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device,
                            ComPtr<ID3D11DeviceContext>& context, std::wstring& errorMessage)
{
    constexpr std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedLevel{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const HRESULT result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, featureLevels.data(),
                                             static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                                             device.GetAddressOf(), &selectedLevel, context.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"为显示适配器创建 D3D11 设备", result);
        return false;
    }
    return true;
}

// 判断两个虚拟桌面半开矩形是否完全相同，用于跳过镜像输出的重复坐标。
bool EqualBounds(open_st::RectI first, open_st::RectI second) noexcept
{
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}

// 判断当前输出边界是否已被先前镜像输出捕获，保证重叠坐标具有确定来源。
bool ContainsCapturedBounds(const std::vector<open_st::RectI>& capturedBounds, open_st::RectI candidate) noexcept
{
    for (const open_st::RectI current : capturedBounds)
    {
        if (EqualBounds(current, candidate))
        {
            return true;
        }
    }
    return false;
}
} // namespace

namespace open_st
{
// 枚举全部适配器和已附着输出，只有全部原生 plane 成功后才发布完整冻结桌面。
bool DesktopCapturer::Capture(FrozenDesktopFrame& frame, std::wstring& errorMessage) const
{
    frame.Clear();
    errorMessage.clear();

    const RectI virtualBounds{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    if (virtualBounds.IsEmpty())
    {
        errorMessage = L"Windows 返回了无效的虚拟桌面范围。";
        return false;
    }

    try
    {
        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(result))
        {
            errorMessage = FormatHResult(L"创建 DXGI 工厂", result);
            return false;
        }

        std::vector<CapturedOutputPlane> outputs;
        std::vector<RectI> capturedBounds;
        for (UINT adapterIndex = 0U;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            result = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                errorMessage = FormatHResult(L"枚举显示适配器", result);
                return false;
            }

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            for (UINT outputIndex = 0U;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                result = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
                if (result == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (FAILED(result))
                {
                    errorMessage = FormatHResult(L"枚举显示输出", result);
                    return false;
                }

                DXGI_OUTPUT_DESC description{};
                result = output->GetDesc(&description);
                if (FAILED(result))
                {
                    errorMessage = FormatHResult(L"读取显示输出信息", result);
                    return false;
                }
                if (description.AttachedToDesktop == FALSE)
                {
                    continue;
                }

                const RectI outputBounds{description.DesktopCoordinates.left, description.DesktopCoordinates.top,
                                         description.DesktopCoordinates.right, description.DesktopCoordinates.bottom};
                if (ContainsCapturedBounds(capturedBounds, outputBounds))
                {
                    OPEN_ST_LOG_WARNING("Skipping a mirrored desktop output with duplicate virtual bounds.");
                    continue;
                }

                // 仅为真正附着且尚未捕获的输出创建设备；无桌面输出的辅助适配器不应阻止截图。
                if (!device && !CreateDeviceForAdapter(adapter.Get(), device, context, errorMessage))
                {
                    return false;
                }

                CapturedOutputPlane capturedOutput;
                if (!CaptureSingleOutput(output.Get(), device.Get(), context.Get(), capturedOutput, errorMessage))
                {
                    return false;
                }
                outputs.push_back(std::move(capturedOutput));
                capturedBounds.push_back(outputBounds);
            }
        }

        FrozenDesktopFrame candidate(virtualBounds, std::move(outputs));
        if (!candidate.IsValid())
        {
            errorMessage = L"没有形成包含全部显示输出的有效冻结桌面。";
            return false;
        }
        frame = std::move(candidate);
        return true;
    }
    catch (const std::exception&)
    {
        frame.Clear();
        errorMessage = L"无法为冻结桌面分配原生像素缓冲区。";
        return false;
    }
}
} // namespace open_st
