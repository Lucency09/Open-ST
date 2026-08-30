#pragma once

#include "captured_plane_writer.h"

#include <dxgi1_6.h>

#include <span>

namespace open_st
{
// 返回 DuplicateOutput1 的原生格式协商优先级，高色深格式必须排在兼容 BGRA8 之前。
[[nodiscard]] std::span<const DXGI_FORMAT> PreferredDuplicationFormats() noexcept;
// 仅在 IDXGIOutput5 缺失或 DuplicateOutput1 明确不支持时允许使用旧复制接口。
[[nodiscard]] bool ShouldUseLegacyDuplication(HRESULT output5QueryResult,
                                               HRESULT duplicateOutput1Result) noexcept;
// 把 DXGI 输出颜色空间映射为冻结帧稳定枚举；未识别值返回 Unknown。
[[nodiscard]] CapturedColorSpace MapCapturedColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept;
// 把 Desktop Duplication 实际返回格式映射为冻结帧像素格式。
[[nodiscard]] bool TryMapCapturedPixelFormat(DXGI_FORMAT format,
                                             CapturedPixelFormat& mappedFormat) noexcept;
// 根据实际像素格式和显示颜色空间确定 plane 像素自身的颜色空间。
[[nodiscard]] CapturedColorSpace ResolveCapturedPixelColorSpace(
    CapturedPixelFormat format, CapturedColorSpace displayColorSpace) noexcept;
// 把 DXGI 表面旋转映射为不依赖系统接口的纯复制枚举。
[[nodiscard]] bool TryMapCapturedRotation(DXGI_MODE_ROTATION rotation,
                                          CapturedSurfaceRotation& mappedRotation) noexcept;
// 仅当已知 HDR/scRGB 显示输出实际得到 BGRA8 时标记系统已转换为 SDR。
[[nodiscard]] bool WasSystemConvertedToSdr(CapturedPixelFormat format,
                                           CapturedColorSpace displayColorSpace) noexcept;
} // namespace open_st
