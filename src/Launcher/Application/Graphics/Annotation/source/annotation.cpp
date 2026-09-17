// 文件职责：实现标准库标注输入校验与颜色解析，不依赖产品兄弟模块。
#include <annotation.h>
#include <cmath>

namespace open_st
{
namespace
{
// 检查桌面物理坐标是否有限且适合整数桌面范围。
// 入参：point：物理坐标。
// 返回：两个坐标均有效为 true。
bool ValidDesktopPoint(AnnotationPoint point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::abs(point.x) <= 2147483647.0 &&
           std::abs(point.y) <= 2147483647.0;
}
} // namespace

// 提供文档与工具共用的基础样式校验，界面和工具可进一步限制档位。
// 入参：style：待检查样式。
// 返回：基础字段全部合法为 true。
bool IsValidAnnotationStyle(AnnotationStyle style) noexcept
{
    return style.rgb <= 0xffffffU && style.transparency <= 100U && std::isfinite(style.lineWidth) &&
           style.lineWidth > 0.0 && style.lineWidth <= 256.0;
}

// 验证擦除路径及其固定裁剪，空路径和无效半径不得进入图形查询。
// 入参：stroke：桌面坐标的擦除手势。
// 返回：非空有界路径、有限正半径和有效裁剪为 true。
bool IsValidAnnotationEraseStroke(const AnnotationEraseStroke& stroke) noexcept
{
    if (stroke.points.empty() || stroke.points.size() > ANNOTATION_MAX_PATH_POINTS || !std::isfinite(stroke.radius) ||
        stroke.radius <= 0.0 || stroke.radius > 32.0 || !ValidDesktopPoint({stroke.clip.left, stroke.clip.top}) ||
        !ValidDesktopPoint({stroke.clip.right, stroke.clip.bottom}) || stroke.clip.left >= stroke.clip.right ||
        stroke.clip.top >= stroke.clip.bottom)
        return false;
    for (const AnnotationPoint point : stroke.points)
    {
        if (!ValidDesktopPoint(point))
            return false;
    }
    return true;
}

// 验证物理像素几何和样式，阻止非法对象进入预览或输出。
// 入参：object：待检查对象，箭头允许水平或垂直，其余形状须有面积。
// 返回：全部字段有效时为 true，否则为 false。
bool IsValidAnnotation(const AnnotationObject& object) noexcept
{
    if (!std::isfinite(object.origin.x) || !std::isfinite(object.origin.y) || !std::isfinite(object.extent.x) ||
        !std::isfinite(object.extent.y) || std::abs(object.origin.x) > 2147483647.0 ||
        std::abs(object.origin.y) > 2147483647.0 || std::abs(object.origin.x + object.extent.x) > 2147483647.0 ||
        std::abs(object.origin.y + object.extent.y) > 2147483647.0 || !IsValidAnnotationStyle(object.style) ||
        !std::isfinite(object.cornerRadius) || object.cornerRadius < 0.0)
    {
        return false;
    }
    if (object.erasures)
    {
        for (const AnnotationEraseMask& mask : *object.erasures)
        {
            if (!mask.stroke || !IsValidAnnotationEraseStroke(*mask.stroke) || !std::isfinite(mask.offset.x) ||
                !std::isfinite(mask.offset.y) || std::abs(mask.offset.x) > 2147483647.0 ||
                std::abs(mask.offset.y) > 2147483647.0)
                return false;
        }
    }
    if (object.kind == AnnotationKind::Pen)
    {
        const AnnotationStroke* stroke = std::get_if<AnnotationStroke>(&object.payload);
        if (stroke == nullptr || !stroke->points || stroke->points->empty() ||
            stroke->points->size() > ANNOTATION_MAX_PATH_POINTS)
            return false;
        for (const AnnotationPoint point : *stroke->points)
        {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !ValidDesktopPoint({object.origin.x + point.x, object.origin.y + point.y}))
                return false;
        }
        return true;
    }
    if (object.kind == AnnotationKind::Text)
    {
        const AnnotationText* text = std::get_if<AnnotationText>(&object.payload);
        return text != nullptr && text->text && IsValidAnnotationFontSize(text->fontSize) &&
               IsNormalizedAnnotationText(*text->text) && !IsBlankAnnotationText(*text->text) &&
               object.extent.x == 0.0 && object.extent.y == 0.0;
    }
    if (object.kind == AnnotationKind::Mosaic)
    {
        const AnnotationMosaic* mosaic = std::get_if<AnnotationMosaic>(&object.payload);
        return mosaic != nullptr && IsValidAnnotationBlockSize(mosaic->blockSize) && object.style.transparency == 0U &&
               std::abs(object.extent.x) >= 1.0 && std::abs(object.extent.y) >= 1.0;
    }
    if (!std::holds_alternative<std::monostate>(object.payload))
        return false;
    switch (object.kind)
    {
    case AnnotationKind::Arrow:
    case AnnotationKind::Line:
        return std::hypot(object.extent.x, object.extent.y) >= 1.0;
    case AnnotationKind::Rectangle:
    case AnnotationKind::FilledRectangle:
    case AnnotationKind::RoundedRectangle:
    case AnnotationKind::Ellipse:
        return std::abs(object.extent.x) >= 1.0 && std::abs(object.extent.y) >= 1.0;
    default:
        return false;
    }
}

// 将完整十六进制颜色文本转换为数值，拒绝前后空格和部分输入。
// 入参：text：#RRGGBB 文本；rgb：成功时写入的颜色值。
// 返回：解析成功为 true，失败不修改输出。
bool ParseAnnotationColor(std::string_view text, std::uint32_t& rgb) noexcept
{
    if (text.size() != 7U || text.front() != '#')
    {
        return false;
    }
    std::uint32_t candidate{};
    for (std::size_t index = 1U; index < text.size(); ++index)
    {
        const char digit = text[index];
        unsigned value{};
        if (digit >= '0' && digit <= '9')
        {
            value = static_cast<unsigned>(digit - '0');
        }
        else if (digit >= 'a' && digit <= 'f')
        {
            value = static_cast<unsigned>(digit - 'a') + 10U;
        }
        else if (digit >= 'A' && digit <= 'F')
        {
            value = static_cast<unsigned>(digit - 'A') + 10U;
        }
        else
        {
            return false;
        }
        candidate = candidate * 16U + value;
    }
    rgb = candidate;
    return true;
}
} // namespace open_st
