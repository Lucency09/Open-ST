// 定义贴图缩放与透明度的纯计算规则，坐标统一使用屏幕物理像素。
#pragma once
#include <windows.h>

namespace open_st
{
struct PinInteractionState final
{
    double scale{1.0};
    float opacity{1.0F};
};
// 按高精度滚轮量更新等比缩放并保持光标锚点。
// 入参：state 为输入输出状态，original 为原图像素大小，current 为屏幕矩形，anchor 为屏幕光标，delta 为滚轮量。
// 返回：新矩形。
[[nodiscard]] RECT ZoomPin(PinInteractionState& state, SIZE original, RECT current, POINT anchor, int delta) noexcept;
// 恢复原图物理像素尺寸并保持窗口左上位置。
// 入参：state 为输入输出状态、original 为原尺寸、current 为现矩形。
// 返回：新矩形。
[[nodiscard]] RECT ResetPinZoom(PinInteractionState& state, SIZE original, RECT current) noexcept;
// 更新窗口不透明度。
// 入参：state 为输入输出状态，delta 为滚轮量。
// 返回：无返回值，范围为 10% 至 100%。
void AdjustPinOpacity(PinInteractionState& state, int delta) noexcept;
// 平移矩形并避免 LONG 坐标溢出。
// 入参：rectangle 为原矩形、dx/dy 为物理像素位移。
// 返回：保持尺寸的平移矩形。
[[nodiscard]] RECT MovePinRectangle(RECT rectangle, long long dx, long long dy) noexcept;
} // namespace open_st
