// 文件职责：定义原生显示输出 plane、像素与颜色格式及不可复制的冻结桌面数据模型。

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

// 确定原生像素格式的存储宽度以计算行跨度和复制偏移。
// 入参：format：待查询的捕获像素格式。
// 返回：BGRA8/RGB10A2 返回 4 字节，FP16 返回 8 字节，未知格式返回 0。
[[nodiscard]] std::size_t CapturedBytesPerPixel(CapturedPixelFormat format) noexcept;

// 拥有单个显示输出已经旋转到虚拟桌面方向的紧凑原生像素。
class CapturedOutputPlane final
{
  public:
    // 创建无效空 plane，供失败清理和移动后状态使用。
    // 入参：无。
    // 返回：无返回值；构造后的图像对象为空，IsValid() 为 false。
    CapturedOutputPlane() = default;
    // 构造并独占一个显示输出的原生冻结像素，校验颜色格式和内存布局。
    // 入参：bounds：桌面方向物理像素半开边界；format：原生像素格式；pixelColorSpace：像素编码颜色空间；metadata：显示输出颜色信息；pixels：转移所有权的紧凑原生像素字节。
    // 返回：无返回值；尺寸或存储不合法时对象无效。
    CapturedOutputPlane(RectI bounds, CapturedPixelFormat format, CapturedColorSpace pixelColorSpace,
                        OutputColorMetadata metadata, std::vector<std::uint8_t> pixels) noexcept;

    // 禁止复制原生桌面像素，避免高分辨率缓冲区被意外复制。
    // 入参：未命名 const CapturedOutputPlane 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    CapturedOutputPlane(const CapturedOutputPlane&) = delete;
    // 禁止复制赋值，确保每块原生像素只有一个所有者。
    // 入参：未命名 const CapturedOutputPlane 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    CapturedOutputPlane& operator=(const CapturedOutputPlane&) = delete;
    // 转移原生冻结输出的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 CapturedOutputPlane 右值引用：交出像素所有权的源对象。
    // 返回：无返回值；构造的新对象接管源像素缓冲区。
    CapturedOutputPlane(CapturedOutputPlane&&) noexcept = default;
    // 转移原生冻结输出的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 CapturedOutputPlane 右值引用：交出像素所有权的源对象。
    // 返回：赋值后的当前对象引用。
    CapturedOutputPlane& operator=(CapturedOutputPlane&&) noexcept = default;

    // 检查原生输出 plane是否可供下游读取和处理。
    // 入参：无。
    // 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
    [[nodiscard]] bool IsValid() const noexcept;
    // 提供图像在虚拟桌面中的物理像素范围。
    // 入参：无。
    // 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 查询矩形或图像的水平物理像素尺寸。
    // 入参：无。
    // 返回：right 减 left 的有符号宽度，空或反向边界可能为零或负数。
    [[nodiscard]] int Width() const noexcept;
    // 查询矩形或图像的垂直物理像素尺寸。
    // 入参：无。
    // 返回：bottom 减 top 的有符号高度，空或反向边界可能为零或负数。
    [[nodiscard]] int Height() const noexcept;
    // 查询原生 plane 使用的像素编码格式。
    // 入参：无。
    // 返回：捕获时保存的 CapturedPixelFormat 枚举值。
    [[nodiscard]] CapturedPixelFormat Format() const noexcept;
    // 查询原生像素自身的颜色空间以选择正确解码方式。
    // 入参：无。
    // 返回：plane 保存的像素颜色空间，不等同于显示输出的传输颜色空间。
    [[nodiscard]] CapturedColorSpace PixelColorSpace() const noexcept;
    // 提供显示输出颜色能力、参考白及兼容回退信息。
    // 入参：无。
    // 返回：当前 plane 所持颜色元数据的只读引用，其生命周期不超过当前对象。
    [[nodiscard]] const OutputColorMetadata& ColorMetadata() const noexcept;
    // 查询逐行访问图像所需的字节步长。
    // 入参：无。
    // 返回：一行紧凑像素的字节数，不包含驱动行填充。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 向图像消费者提供只读像素视图，避免复制完整缓冲区。
    // 入参：无。
    // 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
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
    // 入参：无。
    // 返回：无返回值；构造后的图像对象为空，IsValid() 为 false。
    FrozenDesktopFrame() = default;
    // 接管一次完整捕获的全部显示输出，形成可重复读取的冻结桌面。
    // 入参：bounds：虚拟桌面物理像素半开边界；outputs：转移所有权的全部显示输出 plane。
    // 返回：无返回值；尺寸或存储不合法时对象无效。
    FrozenDesktopFrame(RectI bounds, std::vector<CapturedOutputPlane> outputs) noexcept;

    // 禁止复制冻结桌面，避免原生像素被隐式复制。
    // 入参：未命名 const FrozenDesktopFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    FrozenDesktopFrame(const FrozenDesktopFrame&) = delete;
    // 禁止复制赋值，保证一次截图会话独占冻结像素。
    // 入参：未命名 const FrozenDesktopFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    FrozenDesktopFrame& operator=(const FrozenDesktopFrame&) = delete;
    // 转移冻结桌面的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 FrozenDesktopFrame 右值引用：交出像素所有权的源对象。
    // 返回：无返回值；构造的新对象接管源像素缓冲区。
    FrozenDesktopFrame(FrozenDesktopFrame&&) noexcept = default;
    // 转移冻结桌面的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 FrozenDesktopFrame 右值引用：交出像素所有权的源对象。
    // 返回：赋值后的当前对象引用。
    FrozenDesktopFrame& operator=(FrozenDesktopFrame&&) noexcept = default;

    // 检查冻结桌面是否可供下游读取和处理。
    // 入参：无。
    // 返回：桌面边界非空、输出集合非空且所有有效 plane 均被边界包含时为 true，否则为 false。
    [[nodiscard]] bool IsValid() const noexcept;
    // 提供图像在虚拟桌面中的物理像素范围。
    // 入参：无。
    // 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 向预览和选区裁切提供冻结桌面的原生输出集合。
    // 入参：无。
    // 返回：按捕获枚举顺序排列的只读 plane 视图，借用当前冻结桌面的内存。
    [[nodiscard]] std::span<const CapturedOutputPlane> Outputs() const noexcept;
    // 丢弃冻结桌面的全部原生输出并复位图像几何信息。
    // 入参：无。
    // 返回：无返回值；对象恢复为空且 IsValid() 为 false。
    void Clear() noexcept;

  private:
    RectI bounds_{};
    std::vector<CapturedOutputPlane> outputs_{};
};
} // namespace open_st
