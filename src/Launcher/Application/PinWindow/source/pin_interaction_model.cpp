// 实现贴图物理像素缩放、光标锚点和窗口透明度的边界计算。
#include "pin_interaction_model.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
// 在可表示坐标内保留指定长度。
// 入参：origin 为拟定起点、extent 为正像素长度。
// 返回：有效 LONG 起点。
LONG SafeOrigin(double origin, LONG extent) noexcept
{
    const double minimum = static_cast<double>(std::numeric_limits<LONG>::min());
    const double maximum = static_cast<double>(std::numeric_limits<LONG>::max()) - extent;
    return static_cast<LONG>(std::clamp(std::round(origin), minimum, maximum));
}
} // namespace
namespace open_st
{
// 使用连续滚轮比例更新缩放，保留光标下图像位置。
// 入参：state 为输入输出缩放状态；original 为原图物理像素宽高；current 为屏幕矩形；anchor 为屏幕光标位置；delta
// 为原始滚轮量，每格 120。 返回：保持光标锚点且边长不超过 16384 的屏幕矩形；原图尺寸无效时保持 current。
RECT ZoomPin(PinInteractionState& state, SIZE original, RECT current, POINT anchor, int delta) noexcept
{
    if (original.cx <= 0 || original.cy <= 0)
        return current;
    const double maximum = std::min(5.0, 16384.0 / static_cast<double>(std::max(original.cx, original.cy)));
    const double minimum = std::min(0.1, maximum);
    const double next =
        std::clamp(state.scale * std::pow(1.1, static_cast<double>(delta) / WHEEL_DELTA), minimum, maximum);
    const LONG width = std::max(1L, static_cast<LONG>(std::lround(original.cx * next)));
    const LONG height = std::max(1L, static_cast<LONG>(std::lround(original.cy * next)));
    const double ratio = next / state.scale;
    const LONG left = SafeOrigin(anchor.x - (static_cast<double>(anchor.x) - current.left) * ratio, width);
    const LONG top = SafeOrigin(anchor.y - (static_cast<double>(anchor.y) - current.top) * ratio, height);
    state.scale = next;
    return {left, top, left + width, top + height};
}
// 只恢复缩放而不改透明度。
// 入参：state 为状态、original 为原尺寸、current 为位置。
// 返回：原图尺寸矩形。
RECT ResetPinZoom(PinInteractionState& state, SIZE original, RECT current) noexcept
{
    state.scale = 1.0;
    const LONG left = SafeOrigin(current.left, original.cx);
    const LONG top = SafeOrigin(current.top, original.cy);
    return {left, top, left + original.cx, top + original.cy};
}
// 按每格五个百分点调整不透明度。
// 入参：state 为状态、delta 为滚轮量。
// 返回：无返回值，范围固定为 0.1 至 1。
void AdjustPinOpacity(PinInteractionState& state, int delta) noexcept
{
    state.opacity = std::clamp(state.opacity + 0.05F * static_cast<float>(delta) / WHEEL_DELTA, 0.1F, 1.0F);
}
// 平移时保证四边均可由 LONG 表示。
// 入参：rectangle 为矩形、dx/dy 为位移。
// 返回：平移结果。
RECT MovePinRectangle(RECT rectangle, long long dx, long long dy) noexcept
{
    const LONG width = rectangle.right - rectangle.left;
    const LONG height = rectangle.bottom - rectangle.top;
    const LONG left = SafeOrigin(static_cast<double>(rectangle.left) + static_cast<double>(dx), width);
    const LONG top = SafeOrigin(static_cast<double>(rectangle.top) + static_cast<double>(dy), height);
    return {left, top, left + width, top + height};
}
} // namespace open_st
