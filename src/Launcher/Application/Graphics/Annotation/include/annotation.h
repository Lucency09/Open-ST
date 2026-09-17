// 文件职责：定义独立于窗口和捕获模块的标注对象、样式及不可变文档快照。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace open_st
{
enum class AnnotationKind
{
    Rectangle,
    Arrow,
    FilledRectangle,
    RoundedRectangle,
    Pen,
    Line,
    Ellipse,
    Text,
    Mosaic
};

struct AnnotationPoint
{
    double x{};
    double y{};
};

struct AnnotationStyle
{
    std::uint32_t rgb{0xff0000U};
    unsigned transparency{};
    double lineWidth{3.0};
};

inline constexpr std::size_t ANNOTATION_MAX_PATH_POINTS = 8192U;
inline constexpr std::size_t ANNOTATION_MAX_TEXT_UNITS = 8192U;
inline constexpr std::u16string_view ANNOTATION_FONT_FAMILY = u"Segoe UI";
inline constexpr std::array<unsigned, 6> ANNOTATION_FONT_SIZES{12U, 16U, 20U, 24U, 32U, 48U};
inline constexpr std::array<unsigned, 4> ANNOTATION_MOSAIC_BLOCK_SIZES{4U, 8U, 16U, 32U};

struct AnnotationStroke
{
    std::shared_ptr<const std::vector<AnnotationPoint>> points;
};

struct AnnotationText
{
    std::shared_ptr<const std::u16string> text;
    double fontSize{24.0};
};

struct AnnotationMosaic
{
    unsigned blockSize{16U};
};

struct AnnotationProperties
{
    AnnotationStyle style{};
    std::optional<double> fontSize;
    std::optional<unsigned> blockSize;
};

struct AnnotationRect
{
    double left{};
    double top{};
    double right{};
    double bottom{};
};

struct AnnotationEraseStroke
{
    std::vector<AnnotationPoint> points;
    double radius{};
    AnnotationRect clip{};
};

struct AnnotationEraseMask
{
    std::shared_ptr<const AnnotationEraseStroke> stroke;
    AnnotationPoint offset{};
};

struct AnnotationObject
{
    std::uint64_t id{};
    AnnotationKind kind{AnnotationKind::Rectangle};
    AnnotationPoint origin{};
    AnnotationPoint extent{};
    AnnotationStyle style{};
    double cornerRadius{8.0};
    std::variant<std::monostate, AnnotationStroke, AnnotationText, AnnotationMosaic> payload;
    std::shared_ptr<const std::vector<AnnotationEraseMask>> erasures;
};

using AnnotationSnapshot = std::shared_ptr<const std::vector<AnnotationObject>>;

// 校验所有标注共用的颜色、透明度和物理线宽，不限制界面预设档位。
// 入参：style：待检查的样式值。
// 返回：24 位颜色、0 至 100 透明度及有限的 (0, 256] 线宽为 true。
[[nodiscard]] bool IsValidAnnotationStyle(AnnotationStyle style) noexcept;

// 验证对象的种类、有限物理坐标和可渲染样式，不要求事务分配后的非零 ID。
// 入参：object：桌面原点加局部有符号终点的标注对象。
// 返回：几何非退化且颜色、透明度、线宽和圆角合法时为 true。
[[nodiscard]] bool IsValidAnnotation(const AnnotationObject& object) noexcept;

// 验证共享擦除手势的桌面物理坐标、路径长度、半径及正式裁剪。
// 入参：stroke：不可变擦除手势；最多 8192 个采样点，允许一个圆形点。
// 返回：全部有限且半径及裁剪有效为 true，否则为 false。
[[nodiscard]] bool IsValidAnnotationEraseStroke(const AnnotationEraseStroke& stroke) noexcept;

// 解析严格的 #RRGGBB 标注颜色，接受大小写十六进制。
// 入参：text：调用期间借用的颜色文本；rgb：成功时接收 24 位 RGB。
// 返回：格式合法时为 true；失败时保持 rgb 不变。
[[nodiscard]] bool ParseAnnotationColor(std::string_view text, std::uint32_t& rgb) noexcept;

// 校验原始 UTF-16 后将 CRLF 和裸 CR 规范为 LF，不裁剪长度、不移除空白。
// 入参：source：原始文本，最多 8192 代码单元且不含 NUL；normalized：成功接收共享正文。
// 返回：合法且分配成功为 true，空白也可成功；失败保留 normalized 原值。
[[nodiscard]] bool NormalizeAnnotationText(std::u16string_view source,
                                           std::shared_ptr<const std::u16string>& normalized) noexcept;

// 判断正文是否全部由 Unicode 空白字符组成，不改变正文内容。
// 入参：text：待检查的 UTF-16 文本。
// 返回：空串或全为空白时为 true。
[[nodiscard]] bool IsBlankAnnotationText(std::u16string_view text) noexcept;

// 检查已保存文本的 UTF-16、长度和 LF 规范，不要求正文非空白。
// 入参：text：规范化候选。
// 返回：有效并且没有 CR 时为 true。
[[nodiscard]] bool IsNormalizedAnnotationText(std::u16string_view text) noexcept;

// 验证本阶段提供的字号档位。
// 入参：fontSize：物理像素字号。
// 返回：12、16、20、24、32 或 48 时为 true。
[[nodiscard]] bool IsValidAnnotationFontSize(double fontSize) noexcept;

// 验证马赛克物理块边长。
// 入参：blockSize：物理像素边长。
// 返回：4、8、16 或 32 时为 true。
[[nodiscard]] bool IsValidAnnotationBlockSize(unsigned blockSize) noexcept;
} // namespace open_st
