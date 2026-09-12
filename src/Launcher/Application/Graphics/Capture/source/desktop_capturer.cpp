// 文件职责：枚举显示适配器与输出并聚合原生冻结帧，确保任一屏捕获失败时不发布残缺桌面。

#include <desktop_capturer.h>

#include "output_capture.h"

#include <log.h>
#include <windows_util.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
using open_st::FormatHResult;

// 为指定显示适配器建立桌面捕获所需的 D3D11 设备和立即上下文。
// 入参：adapter：借用的目标适配器；device、context：输出参数，分别接收 D3D11
// 设备和立即上下文；errorMessage：输出参数，失败时写入诊断。
// 返回：设备创建成功时为 true；D3D11 初始化失败时为 false
// 并写入 HRESULT 诊断。
bool CreateDeviceForAdapter(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
                            std::wstring& errorMessage)
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

// 比较两个显示输出的全部矩形边界以识别相同桌面区域。
// 入参：first、second：虚拟桌面物理像素半开矩形。
// 返回：四条边界均相等时为 true，否则为 false。
bool EqualBounds(open_st::RectI first, open_st::RectI second) noexcept
{
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}

// 判断输出是否与已捕获显示区域完全相同，以跳过镜像输出。
// 入参：capturedBounds：已经捕获的物理像素矩形集合；candidate：待捕获输出的物理像素矩形。
// 返回：集合中存在边界完全相同的矩形时为 true，否则为 false。
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
// 冻结当前虚拟桌面的全部真实显示输出，供一次截图会话重复读取。
// 入参：frame：输出参数，接收拥有全部原生 plane 的冻结桌面；errorMessage：输出参数，接收捕获失败原因。
// 返回：全部输出捕获并验证成功时为 true；任一阶段失败时为 false，frame 被清空且提供诊断。
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
