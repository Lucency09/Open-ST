// 文件职责：声明单显示输出的 Desktop Duplication 捕获接口，供桌面捕获器聚合各屏结果。

#pragma once

#include <frozen_desktop_frame.h>

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIOutput;

namespace open_st
{
// 捕获单显示输出并回读原生像素，形成不依赖 DXGI 资源的冻结 plane。
// 入参：output：借用的显示输出；device：对应适配器的 D3D11 设备；context：该设备的立即上下文；capturedOutput：输出参数，接收自有原生
// plane；errorMessage：输出参数，接收失败诊断。
// 返回：捕获、映射、复制及资源归还成功时为 true；失败为 false，capturedOutput 保持无效。
[[nodiscard]] bool CaptureSingleOutput(IDXGIOutput* output, ID3D11Device* device,
                                       ID3D11DeviceContext* context, CapturedOutputPlane& capturedOutput,
                                       std::wstring& errorMessage);
} // namespace open_st
