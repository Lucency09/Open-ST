#pragma once

#include <geometry.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace open_st
{
// 标识冻结显示输出在 CPU 内存中保留的原生像素格式。
enum class CapturedPixelFormat
{
    Bgra8Unorm,
    Rgb10A2Unorm,
    Rgba16FloatScRgb
};

// 标识像素数据采用的颜色空间；Unknown 要求调用方使用明确的保守降级策略。
enum class CapturedColorSpace
{
    Unknown,
    SdrGamma22P709,
    ScRgb,
    Hdr10
};

// 保存显示输出报告的颜色能力和亮度元数据，不拥有任何系统接口。
struct OutputColorMetadata final
{
    CapturedColorSpace displayColorSpace{CapturedColorSpace::Unknown};
    std::uint32_t bitsPerColor{};
    float minimumLuminance{};
    float maximumLuminance{};
    float maximumFullFrameLuminance{};
    bool usedLegacyDuplication{}; // true 表示使用固定 BGRA8 的旧 DuplicateOutput 接口。
    bool systemConvertedToSdr{};  // true 表示已知 HDR/scRGB 输出实际取得系统转换后的 BGRA8。
    std::array<wchar_t, 32> deviceName{}; // DXGI/GDI 显示源名称，固定容量且以空字符终止。
    std::uint32_t adapterLowPart{}; // DisplayConfig 目标适配器 LUID，用于精确关联显示目标。
    std::int32_t adapterHighPart{};
    std::uint32_t targetId{};
    bool hasDisplayConfigIdentity{}; // false 表示未找到唯一的活动显示目标。
    float sdrWhiteLevelNits{}; // Windows 将 SDR 白色呈现在 HDR 输出上的亮度，单位 nit。
    bool hasSdrWhiteLevel{}; // 查询失败时保持 false，不能把零值或显示器峰值当成参考白。
};

// 返回指定冻结像素格式每个像素占用的字节数；未知枚举值返回零。
[[nodiscard]] std::size_t CapturedBytesPerPixel(CapturedPixelFormat format) noexcept;

// 拥有单个显示输出已经旋转到虚拟桌面方向的紧凑原生像素。
class CapturedOutputPlane final
{
  public:
    // 创建无效空 plane，供失败清理和移动后状态使用。
    CapturedOutputPlane() = default;
    // 接管紧凑像素缓冲区；边界或字节数不符合格式时保持无效状态。
    CapturedOutputPlane(RectI bounds, CapturedPixelFormat format, CapturedColorSpace pixelColorSpace,
                        OutputColorMetadata metadata, std::vector<std::uint8_t> pixels) noexcept;

    // 禁止复制原生桌面像素，避免高分辨率缓冲区被意外复制。
    CapturedOutputPlane(const CapturedOutputPlane&) = delete;
    // 禁止复制赋值，确保每块原生像素只有一个所有者。
    CapturedOutputPlane& operator=(const CapturedOutputPlane&) = delete;
    // 允许移动 plane 所拥有的像素缓冲区。
    CapturedOutputPlane(CapturedOutputPlane&&) noexcept = default;
    // 允许通过移动赋值转移 plane 所拥有的像素缓冲区。
    CapturedOutputPlane& operator=(CapturedOutputPlane&&) noexcept = default;

    // 验证边界、格式、stride 和像素缓冲区大小组成完整一致的 plane。
    [[nodiscard]] bool IsValid() const noexcept;
    // 返回 plane 在虚拟桌面物理像素中的半开边界。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 返回已经旋转到桌面方向的像素宽度。
    [[nodiscard]] int Width() const noexcept;
    // 返回已经旋转到桌面方向的像素高度。
    [[nodiscard]] int Height() const noexcept;
    // 返回该 plane 的原生像素格式。
    [[nodiscard]] CapturedPixelFormat Format() const noexcept;
    // 返回 plane 像素自身的颜色空间，不等同于显示器输出线缆颜色空间。
    [[nodiscard]] CapturedColorSpace PixelColorSpace() const noexcept;
    // 返回显示输出能力、亮度和兼容降级信息。
    [[nodiscard]] const OutputColorMetadata& ColorMetadata() const noexcept;
    // 返回去除驱动 RowPitch padding 后的紧凑行跨度。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 返回只读原生像素；其生命周期不超过当前 plane。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

  private:
    RectI bounds_{};
    CapturedPixelFormat format_{CapturedPixelFormat::Bgra8Unorm};
    CapturedColorSpace pixelColorSpace_{CapturedColorSpace::Unknown};
    OutputColorMetadata metadata_{};
    std::size_t stride_{};
    std::vector<std::uint8_t> pixels_{};
};

// 拥有一次截图会话的虚拟桌面边界和所有原生显示输出 plane。
class FrozenDesktopFrame final
{
  public:
    // 创建不包含桌面数据的无效冻结帧。
    FrozenDesktopFrame() = default;
    // 接管一次完整捕获产生的全部输出；任一 plane 越界或无效时保持无效状态。
    FrozenDesktopFrame(RectI bounds, std::vector<CapturedOutputPlane> outputs) noexcept;

    // 禁止复制冻结桌面，避免原生像素被隐式复制。
    FrozenDesktopFrame(const FrozenDesktopFrame&) = delete;
    // 禁止复制赋值，保证一次截图会话独占冻结像素。
    FrozenDesktopFrame& operator=(const FrozenDesktopFrame&) = delete;
    // 允许移动整次冻结桌面的所有权。
    FrozenDesktopFrame(FrozenDesktopFrame&&) noexcept = default;
    // 允许通过移动赋值替换冻结桌面所有权。
    FrozenDesktopFrame& operator=(FrozenDesktopFrame&&) noexcept = default;

    // 验证虚拟桌面边界、输出集合以及所有 plane 的包含关系。
    [[nodiscard]] bool IsValid() const noexcept;
    // 返回冻结时刻的虚拟桌面物理像素边界。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 返回只读显示输出集合，禁止下游修改原始捕获数据。
    [[nodiscard]] std::span<const CapturedOutputPlane> Outputs() const noexcept;
    // 清空全部原生像素并恢复为无效状态。
    void Clear() noexcept;

  private:
    RectI bounds_{};
    std::vector<CapturedOutputPlane> outputs_{};
};
} // namespace open_st
