#pragma once

#include <frozen_desktop_frame.h>

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIOutput;

namespace open_st
{
// 捕获单个已附着显示输出并返回拥有原生像素的 plane；失败时 output 保持无效。
[[nodiscard]] bool CaptureSingleOutput(IDXGIOutput* output, ID3D11Device* device,
                                       ID3D11DeviceContext* context, CapturedOutputPlane& capturedOutput,
                                       std::wstring& errorMessage);
} // namespace open_st
