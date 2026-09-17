// 文件职责：以共享 D2D 几何绘制标注，并将 SDR 预乘透明层合成到冻结输出。
#include "annotation_drawing.h"
#include "annotation_text_geometry.h"
#include <algorithm>
#include <annotation_erasure.h>
#include <annotation_hit_test.h>
#include <annotation_mosaic_source.h>
#include <array>
#include <climits>
#include <cmath>
#include <color_conversion.h>
#include <d2d1helper.h>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <wincodec.h>
#include <windows_util.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT32 MAX_GEOMETRY_SEGMENTS = 262144U;
constexpr std::size_t MAX_GEOMETRY_CACHE_ENTRIES = 64U;
constexpr std::size_t MAX_GEOMETRY_CACHE_POINTS = 65536U;
constexpr std::size_t MAX_GEOMETRY_CACHE_BYTES = 16U * 1024U * 1024U;

// 为同一线程的命中、擦除和离屏输出复用兼容工厂，预览仍使用真实目标自身工厂。
// 入参：factory 接收当前线程借用工厂的共享 COM 引用。
// 返回：工厂可用为 S_OK，首次创建失败原样返回错误。
HRESULT SharedAnnotationFactory(ComPtr<ID2D1Factory>& factory)
{
    thread_local ComPtr<ID2D1Factory> shared;
    if (!shared)
    {
        const HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, shared.GetAddressOf());
        if (FAILED(result))
            return result;
    }
    factory = shared;
    return S_OK;
}
// 管理本次离屏 WIC 操作借用的 COM 初始化计数。
struct ComScope final
{
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    // 仅撤销本对象实际取得的 COM 初始化引用。
    // 入参：无。
    // 返回：无返回值，不改变已有其他 apartment 的所有权。
    ~ComScope()
    {
        if (SUCCEEDED(this->result))
        {
            CoUninitialize();
        }
    }
};

// 将标准 sRGB 色值适配到实际目标的通道域。
// 入参：byte：8 位通道；hdr：是否 scRGB；whiteScale：HDR 参考白倍数。
// 返回：SDR 编码值或 HDR 线性光值。
float Channel(std::uint32_t byte, bool hdr, float whiteScale) noexcept
{
    const float value = static_cast<float>(byte) / 255.0F;
    return hdr ? open_st::SrgbToLinear(value) * whiteScale : value;
}

// 将有符号局部终点转换为目标坐标中的矩形，预览和命中共用同一计算。
// 入参：object：有效标注；origin：目标左上桌面物理坐标。
// 返回：规范化后的 D2D 物理像素矩形。
D2D1_RECT_F ObjectRectangle(const open_st::AnnotationObject& object, open_st::AnnotationPoint origin) noexcept
{
    const double left = object.origin.x + std::min(0.0, object.extent.x) - origin.x;
    const double top = object.origin.y + std::min(0.0, object.extent.y) - origin.y;
    return D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
                       static_cast<float>(left + std::abs(object.extent.x)),
                       static_cast<float>(top + std::abs(object.extent.y)));
}

// 将圆角半径夹到形状短边的一半，保持绘制和命中边界一致。
// 入参：object：有效的圆角矩形对象，尺寸为物理像素。
// 返回：非负且不超过半短边的物理像素半径。
float ObjectRadius(const open_st::AnnotationObject& object) noexcept
{
    return static_cast<float>(
        std::min({object.cornerRadius, std::abs(object.extent.x) * 0.5, std::abs(object.extent.y) * 0.5}));
}

// 创建无重叠的封闭箭头轮廓，使头部与杆身共享一次透明度合成。
// 入参：factory：借用工厂；object：有效箭头；origin：目标桌面原点；geometry：接收轮廓。
// 返回：完成路径创建时为 S_OK，否则返回 D2D 错误码。
HRESULT CreateArrow(ID2D1Factory* factory, const open_st::AnnotationObject& object, open_st::AnnotationPoint origin,
                    ComPtr<ID2D1PathGeometry>& geometry)
{
    HRESULT result = factory->CreatePathGeometry(geometry.GetAddressOf());
    if (FAILED(result))
    {
        return result;
    }
    ComPtr<ID2D1GeometrySink> sink;
    result = geometry->Open(sink.GetAddressOf());
    if (FAILED(result))
    {
        return result;
    }
    const double length = std::hypot(object.extent.x, object.extent.y);
    const double axisX = object.extent.x / length;
    const double axisY = object.extent.y / length;
    const double halfWidth = object.style.lineWidth * 0.5;
    const double headLength = std::min(length, std::max(8.0, object.style.lineWidth * 4.0));
    const double headWidth = std::min(headLength * 0.5, std::max(4.0, object.style.lineWidth * 2.0));
    const double shoulder = length - headLength;
    const std::array<open_st::AnnotationPoint, 7U> outline{{{0.0, halfWidth},
                                                            {shoulder, halfWidth},
                                                            {shoulder, headWidth},
                                                            {length, 0.0},
                                                            {shoulder, -headWidth},
                                                            {shoulder, -halfWidth},
                                                            {0.0, -halfWidth}}};
    std::array<D2D1_POINT_2F, 7U> points{};
    for (std::size_t index = 0U; index < points.size(); ++index)
    {
        points[index] = D2D1::Point2F(
            static_cast<float>(object.origin.x - origin.x + axisX * outline[index].x - axisY * outline[index].y),
            static_cast<float>(object.origin.y - origin.y + axisY * outline[index].x + axisX * outline[index].y));
    }
    sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points.data() + 1U, static_cast<UINT32>(points.size() - 1U));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    return sink->Close();
}

// 从实际渲染公式建立可供 D2D 查询的几何，不用外接框代替箭头和圆角。
// 入参：factory：借用工厂；object：有效对象；origin：查询目标原点；geometry：接收几何所有权。
// 返回：成功为 S_OK，失败返回 D2D 错误码。
HRESULT CreateHitGeometry(ID2D1Factory* factory, const open_st::AnnotationObject& object,
                          open_st::AnnotationPoint origin, ComPtr<ID2D1Geometry>& geometry)
{
    if (object.kind == open_st::AnnotationKind::Arrow)
    {
        ComPtr<ID2D1PathGeometry> arrow;
        const HRESULT result = CreateArrow(factory, object, origin, arrow);
        geometry = arrow;
        return result;
    }
    const D2D1_RECT_F rectangle = ObjectRectangle(object, origin);
    if (object.kind == open_st::AnnotationKind::Ellipse)
    {
        ComPtr<ID2D1EllipseGeometry> ellipse;
        const HRESULT result = factory->CreateEllipseGeometry(
            D2D1::Ellipse(
                D2D1::Point2F((rectangle.left + rectangle.right) * 0.5F, (rectangle.top + rectangle.bottom) * 0.5F),
                (rectangle.right - rectangle.left) * 0.5F, (rectangle.bottom - rectangle.top) * 0.5F),
            ellipse.GetAddressOf());
        geometry = ellipse;
        return result;
    }
    if (object.kind == open_st::AnnotationKind::RoundedRectangle)
    {
        ComPtr<ID2D1RoundedRectangleGeometry> rounded;
        const float radius = ObjectRadius(object);
        const HRESULT result = factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rectangle, radius, radius),
                                                                       rounded.GetAddressOf());
        geometry = rounded;
        return result;
    }
    ComPtr<ID2D1RectangleGeometry> rectangular;
    const HRESULT result = factory->CreateRectangleGeometry(rectangle, rectangular.GetAddressOf());
    geometry = rectangular;
    return result;
}

// 检查组合或展开后的复杂度，避免擦痕累计产生无限制路径。
// 入参：path 为已经关闭的 D2D 路径。
// 返回：预算内为 S_OK，超预算明确返回失败而非保留未擦对象。
HRESULT CheckGeometryBudget(ID2D1PathGeometry* path)
{
    UINT32 count{};
    const HRESULT result = path->GetSegmentCount(&count);
    return FAILED(result) ? result : count <= MAX_GEOMETRY_SEGMENTS ? S_OK : E_OUTOFMEMORY;
}

// 将两个填充区域进行向量组合，所有擦除、裁剪和实际交集共用此路径。
// 入参：factory 为工厂；first、second 为填充几何；mode 为组合操作；output 接收完整结果。
// 返回：创建、组合及复杂度检查全部成功为 S_OK。
HRESULT CombineGeometry(ID2D1Factory* factory, ID2D1Geometry* first, ID2D1Geometry* second, D2D1_COMBINE_MODE mode,
                        ComPtr<ID2D1Geometry>& output)
{
    ComPtr<ID2D1PathGeometry> path;
    HRESULT result = factory->CreatePathGeometry(path.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(result))
        result = path->Open(sink.GetAddressOf());
    if (FAILED(result))
        return result;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    result = first->CombineWithGeometry(second, mode, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, sink.Get());
    const HRESULT closed = sink->Close();
    if (FAILED(result) || FAILED(closed))
        return FAILED(result) ? result : closed;
    result = CheckGeometryBudget(path.Get());
    if (SUCCEEDED(result))
        output = path;
    return result;
}

// 把中心线展开为一次填充的实际笔迹，避免半透明线段交叠时重复合成。
// 入参：factory 为工厂；centerline 为中心路径；width 为物理宽度；round 为圆帽圆接角；output 接收结果。
// 返回：完整展开成功为 S_OK，超预算或 D2D 失败返回错误。
HRESULT WidenGeometry(ID2D1Factory* factory, ID2D1Geometry* centerline, float width, bool round,
                      ComPtr<ID2D1Geometry>& output)
{
    ComPtr<ID2D1StrokeStyle> style;
    if (round)
    {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
        properties.startCap = D2D1_CAP_STYLE_ROUND;
        properties.endCap = D2D1_CAP_STYLE_ROUND;
        properties.dashCap = D2D1_CAP_STYLE_ROUND;
        properties.lineJoin = D2D1_LINE_JOIN_ROUND;
        const HRESULT styled = factory->CreateStrokeStyle(properties, nullptr, 0, style.GetAddressOf());
        if (FAILED(styled))
            return styled;
    }
    ComPtr<ID2D1PathGeometry> path;
    HRESULT result = factory->CreatePathGeometry(path.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(result))
        result = path->Open(sink.GetAddressOf());
    if (FAILED(result))
        return result;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    result = centerline->Widen(width, style.Get(), nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, sink.Get());
    const HRESULT closed = sink->Close();
    if (FAILED(result) || FAILED(closed))
        return FAILED(result) ? result : closed;
    result = CheckGeometryBudget(path.Get());
    if (SUCCEEDED(result))
        output = path;
    return result;
}

// 用局部点列建立圆帽笔迹，单点或全部重合点仍形成一个圆。
// 入参：factory 为工厂；points 为点列；offset 为目标坐标偏移；radius 为物理半径；output 接收填充区域。
// 返回：成功为 S_OK；空点列及超预算输入为 E_INVALIDARG。
HRESULT CreateStrokeGeometry(ID2D1Factory* factory, std::span<const open_st::AnnotationPoint> points,
                             open_st::AnnotationPoint offset, float radius, ComPtr<ID2D1Geometry>& output)
{
    if (points.empty() || points.size() > open_st::ANNOTATION_MAX_PATH_POINTS)
        return E_INVALIDARG;
    bool distinct{};
    for (const open_st::AnnotationPoint point : points)
        distinct = distinct || point.x != points.front().x || point.y != points.front().y;
    if (!distinct)
    {
        ComPtr<ID2D1EllipseGeometry> circle;
        const HRESULT result =
            factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(static_cast<float>(points.front().x + offset.x),
                                                                       static_cast<float>(points.front().y + offset.y)),
                                                         radius, radius),
                                           circle.GetAddressOf());
        output = circle;
        return result;
    }
    ComPtr<ID2D1PathGeometry> path;
    HRESULT result = factory->CreatePathGeometry(path.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(result))
        result = path->Open(sink.GetAddressOf());
    if (FAILED(result))
        return result;
    sink->BeginFigure(
        D2D1::Point2F(static_cast<float>(points.front().x + offset.x), static_cast<float>(points.front().y + offset.y)),
        D2D1_FIGURE_BEGIN_HOLLOW);
    for (std::size_t index = 1; index < points.size(); ++index)
        sink->AddLine(D2D1::Point2F(static_cast<float>(points[index].x + offset.x),
                                    static_cast<float>(points[index].y + offset.y)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    result = sink->Close();
    return FAILED(result) ? result : WidenGeometry(factory, path.Get(), radius * 2.0F, true, output);
}

// 构造未擦除对象的实际填充几何，描边、折线和单点使用同一可见区域。
// 入参：factory 为工厂；object 为有效对象；origin 为目标桌面原点；output 接收填充几何。
// 返回：成功为 S_OK，失败不返回退化替代图形。
HRESULT CreateObjectGeometry(ID2D1Factory* factory, const open_st::AnnotationObject& object,
                             open_st::AnnotationPoint origin, ComPtr<ID2D1Geometry>& output)
{
    if (object.kind == open_st::AnnotationKind::Text)
        return open_st::CreateAnnotationTextGeometry(factory, std::get<open_st::AnnotationText>(object.payload),
                                                     output);
    const open_st::AnnotationPoint offset{object.origin.x - origin.x, object.origin.y - origin.y};
    if (object.kind == open_st::AnnotationKind::Pen)
    {
        const open_st::AnnotationStroke& stroke = std::get<open_st::AnnotationStroke>(object.payload);
        return CreateStrokeGeometry(factory, *stroke.points, offset, static_cast<float>(object.style.lineWidth * 0.5),
                                    output);
    }
    if (object.kind == open_st::AnnotationKind::Line)
    {
        const std::array<open_st::AnnotationPoint, 2> points{{{}, object.extent}};
        return CreateStrokeGeometry(factory, points, offset, static_cast<float>(object.style.lineWidth * 0.5), output);
    }
    ComPtr<ID2D1Geometry> geometry;
    const HRESULT result = CreateHitGeometry(factory, object, origin, geometry);
    if (FAILED(result))
        return result;
    if (object.kind == open_st::AnnotationKind::Rectangle || object.kind == open_st::AnnotationKind::Ellipse)
        return WidenGeometry(factory, geometry.Get(), static_cast<float>(object.style.lineWidth), false, output);
    output = geometry;
    return S_OK;
}

// 将擦除圆笔迹裁到该手势开始时的选区，偏移与对象平移保持一致。
// 入参：factory 为工厂；stroke 为桌面擦痕；offset 为到目标坐标的变换；output 接收裁剪后的擦除区域。
// 返回：成功为 S_OK，输入无效或组合失败返回错误。
HRESULT CreateEraseGeometry(ID2D1Factory* factory, const open_st::AnnotationEraseStroke& stroke,
                            open_st::AnnotationPoint offset, ComPtr<ID2D1Geometry>& output)
{
    if (!open_st::IsValidAnnotationEraseStroke(stroke))
        return E_INVALIDARG;
    ComPtr<ID2D1Geometry> path;
    HRESULT result = CreateStrokeGeometry(factory, stroke.points, offset, static_cast<float>(stroke.radius), path);
    ComPtr<ID2D1RectangleGeometry> clip;
    if (SUCCEEDED(result))
        result = factory->CreateRectangleGeometry(D2D1::RectF(static_cast<float>(stroke.clip.left + offset.x),
                                                              static_cast<float>(stroke.clip.top + offset.y),
                                                              static_cast<float>(stroke.clip.right + offset.x),
                                                              static_cast<float>(stroke.clip.bottom + offset.y)),
                                                  clip.GetAddressOf());
    return FAILED(result) ? result
                          : CombineGeometry(factory, path.Get(), clip.Get(), D2D1_COMBINE_MODE_INTERSECT, output);
}

// 从已缓存的共同前缀继续扣除局部擦痕，完整保留原组合顺序和精度。
// 入参：factory 为工厂；object 为有效对象；first 为尚未应用的 mask 下标；output、cuts 保存对象局部结果。
// 返回：完整构造成功为 S_OK；任何擦痕失败都不回退为未擦对象。
HRESULT ApplyErasureSuffix(ID2D1Factory* factory, const open_st::AnnotationObject& object, std::size_t first,
                           ComPtr<ID2D1Geometry>& output, std::vector<ComPtr<ID2D1Geometry>>& cuts)
{
    if (!object.erasures)
        return S_OK;
    for (std::size_t index = first; index < object.erasures->size(); ++index)
    {
        const open_st::AnnotationEraseMask& mask = (*object.erasures)[index];
        ComPtr<ID2D1Geometry> erased;
        HRESULT result = CreateEraseGeometry(factory, *mask.stroke, mask.offset, erased);
        if (FAILED(result))
            return result;
        ComPtr<ID2D1Geometry> next;
        result = CombineGeometry(factory, output.Get(), erased.Get(), D2D1_COMBINE_MODE_EXCLUDE, next);
        if (FAILED(result))
            return result;
        output = std::move(next);
        cuts.push_back(std::move(erased));
    }
    return S_OK;
}

struct CachedObjectGeometry
{
    ComPtr<ID2D1Factory> factory;
    open_st::AnnotationKind kind{};
    open_st::AnnotationPoint extent{};
    double width{};
    double radius{};
    const void* pointsIdentity{};
    const void* masksIdentity{};
    const void* textIdentity{};
    double fontSize{};
    std::weak_ptr<const std::vector<open_st::AnnotationPoint>> points;
    std::weak_ptr<const std::vector<open_st::AnnotationEraseMask>> masks;
    std::weak_ptr<const std::u16string> text;
    ComPtr<ID2D1Geometry> visible;
    std::vector<ComPtr<ID2D1Geometry>> cuts;
    std::size_t pointCount{};
    std::size_t bytes{};

    // 判断弱身份是否已结束生命周期，防止旧历史长期占据缓存。
    // 入参：无。
    // 返回：任一存在的共享数据身份过期为 true。
    bool Expired() const noexcept
    {
        return (this->pointsIdentity != nullptr && this->points.expired()) ||
               (this->masksIdentity != nullptr && this->masks.expired()) ||
               (this->textIdentity != nullptr && this->text.expired());
    }

    // 比较几何参数和共享所有者身份，不能仅凭释放后可能重用的裸地址命中。
    // 入参：source 为工厂；object 为当前对象；strokePoints 为可空点列所有权。
    // 返回：几何可复用为 true；颜色、透明度、ID 和原点不影响对象局部几何。
    bool MatchesBase(ID2D1Factory* source, const open_st::AnnotationObject& object,
                     const std::shared_ptr<const std::vector<open_st::AnnotationPoint>>& strokePoints) const noexcept
    {
        const open_st::AnnotationText* annotationText = std::get_if<open_st::AnnotationText>(&object.payload);
        const std::shared_ptr<const std::u16string> content = annotationText ? annotationText->text : nullptr;
        return this->factory.Get() == source && this->kind == object.kind && this->extent.x == object.extent.x &&
               this->extent.y == object.extent.y && this->width == object.style.lineWidth &&
               this->radius == object.cornerRadius && this->pointsIdentity == strokePoints.get() &&
               !this->points.owner_before(strokePoints) && !strokePoints.owner_before(this->points) &&
               this->textIdentity == content.get() &&
               this->fontSize == (annotationText ? annotationText->fontSize : 0) && !this->text.owner_before(content) &&
               !content.owner_before(this->text);
    }

    // 在共同基础几何之外核对完整擦痕向量的共享所有者。
    // 入参：source 为工厂；object 为对象；strokePoints 为可空点列所有权。
    // 返回：全部可见几何可以直接复用为 true。
    bool Matches(ID2D1Factory* source, const open_st::AnnotationObject& object,
                 const std::shared_ptr<const std::vector<open_st::AnnotationPoint>>& strokePoints) const noexcept
    {
        return this->MatchesBase(source, object, strokePoints) && this->masksIdentity == object.erasures.get() &&
               !this->masks.owner_before(object.erasures) && !object.erasures.owner_before(this->masks);
    }
};

struct GeometryCache
{
    std::list<CachedObjectGeometry> entries;
    std::size_t points{};
    std::size_t bytes{};

    // 删除单个缓存项并同步预算，所有 COM 几何及所属工厂随项释放。
    // 入参：entry 为有效链表迭代器。
    // 返回：被删除项的下一位置。
    std::list<CachedObjectGeometry>::iterator Remove(std::list<CachedObjectGeometry>::iterator entry)
    {
        this->points -= entry->pointCount;
        this->bytes -= entry->bytes;
        return this->entries.erase(entry);
    }

    // 清理已过期数据身份，不强持已被宿主淘汰的历史快照。
    // 入参：无。
    // 返回：无。
    void Prune()
    {
        for (auto entry = this->entries.begin(); entry != this->entries.end();)
        {
            if (entry->Expired())
                entry = this->Remove(entry);
            else
                ++entry;
        }
    }
};

// 保守估算最终几何占用，用段数加固定对象开销限制缓存；超上限不缓存但仍可正常绘制。
// 入参：geometry 为最终几何。
// 返回：估算字节数，路径查询失败按超预算处理。
std::size_t GeometryBytes(ID2D1Geometry* geometry)
{
    ComPtr<ID2D1TransformedGeometry> transformed;
    if (SUCCEEDED(geometry->QueryInterface(IID_PPV_ARGS(transformed.GetAddressOf()))))
    {
        ComPtr<ID2D1Geometry> source;
        transformed->GetSourceGeometry(source.GetAddressOf());
        return 512U + GeometryBytes(source.Get());
    }
    ComPtr<ID2D1GeometryGroup> group;
    if (SUCCEEDED(geometry->QueryInterface(IID_PPV_ARGS(group.GetAddressOf()))))
    {
        std::vector<ID2D1Geometry*> sources(group->GetSourceGeometryCount());
        std::vector<ComPtr<ID2D1Geometry>> owned(sources.size());
        group->GetSourceGeometries(sources.data(), static_cast<UINT32>(sources.size()));
        for (std::size_t index = 0; index < sources.size(); ++index)
            owned[index].Attach(sources[index]);
        std::size_t total = 512U;
        for (const ComPtr<ID2D1Geometry>& source : owned)
        {
            total += GeometryBytes(source.Get());
        }
        return total;
    }
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(geometry->QueryInterface(IID_PPV_ARGS(path.GetAddressOf()))))
        return 512U;
    UINT32 segments{};
    if (FAILED(path->GetSegmentCount(&segments)))
        return MAX_GEOMETRY_CACHE_BYTES + 1U;
    return 512U + static_cast<std::size_t>(segments) * 64U;
}

// 查询或生成对象局部可见几何，按所属工厂及弱共享身份执行有界 LRU 复用。
// 入参：factory 为实际目标工厂；object 为有效对象；visible、cuts 接收局部几何引用。
// 返回：成功为 S_OK；预算只控制缓存保留，不降低文档几何精度。
HRESULT LocalVisibleGeometry(ID2D1Factory* factory, const open_st::AnnotationObject& object,
                             ComPtr<ID2D1Geometry>& visible, std::vector<ComPtr<ID2D1Geometry>>& cuts)
try
{
    thread_local GeometryCache cache;
    cache.Prune();
    const std::shared_ptr<const std::vector<open_st::AnnotationPoint>> strokePoints =
        object.kind == open_st::AnnotationKind::Pen ? std::get<open_st::AnnotationStroke>(object.payload).points
                                                    : nullptr;
    for (auto entry = cache.entries.begin(); entry != cache.entries.end(); ++entry)
    {
        if (entry->Matches(factory, object, strokePoints))
        {
            visible = entry->visible;
            cuts = entry->cuts;
            cache.entries.splice(cache.entries.begin(), cache.entries, entry);
            return S_OK;
        }
    }
    CachedObjectGeometry entry;
    entry.factory = factory;
    entry.kind = object.kind;
    entry.extent = object.extent;
    entry.width = object.style.lineWidth;
    entry.radius = object.cornerRadius;
    entry.pointsIdentity = strokePoints.get();
    entry.masksIdentity = object.erasures.get();
    entry.points = strokePoints;
    entry.masks = object.erasures;
    const open_st::AnnotationText* text = std::get_if<open_st::AnnotationText>(&object.payload);
    if (text != nullptr)
    {
        entry.textIdentity = text->text.get();
        entry.text = text->text;
        entry.fontSize = text->fontSize;
    }
    HRESULT result = S_OK;
    std::size_t prefix{};
    if (object.erasures && !object.erasures->empty())
    {
        auto best = cache.entries.end();
        for (auto candidate = cache.entries.begin(); candidate != cache.entries.end(); ++candidate)
        {
            if (!candidate->MatchesBase(factory, object, strokePoints))
                continue;
            const auto previous = candidate->masks.lock();
            const std::size_t count = previous ? previous->size() : 0U;
            if (count < prefix || count > object.erasures->size())
                continue;
            bool matches = true;
            for (std::size_t index = 0; index < count; ++index)
            {
                const open_st::AnnotationEraseMask& before = (*previous)[index];
                const open_st::AnnotationEraseMask& after = (*object.erasures)[index];
                if (before.offset.x != after.offset.x || before.offset.y != after.offset.y ||
                    before.stroke.get() != after.stroke.get() || before.stroke.owner_before(after.stroke) ||
                    after.stroke.owner_before(before.stroke))
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                best = candidate;
                prefix = count;
            }
        }
        if (best != cache.entries.end())
        {
            entry.visible = best->visible;
            entry.cuts = best->cuts;
            cache.entries.splice(cache.entries.begin(), cache.entries, best);
        }
        else
        {
            open_st::AnnotationObject base = object;
            base.erasures.reset();
            result = LocalVisibleGeometry(factory, base, entry.visible, entry.cuts);
        }
        if (SUCCEEDED(result))
            result = ApplyErasureSuffix(factory, object, prefix, entry.visible, entry.cuts);
    }
    else
    {
        result = CreateObjectGeometry(factory, object, object.origin, entry.visible);
    }
    if (FAILED(result))
        return result;
    entry.pointCount = strokePoints ? strokePoints->size() : 0U;
    if (text != nullptr)
        entry.pointCount += text->text->size();
    if (object.erasures)
    {
        for (const open_st::AnnotationEraseMask& mask : *object.erasures)
        {
            entry.pointCount += mask.stroke->points.size();
            if (entry.pointCount > MAX_GEOMETRY_CACHE_POINTS)
                break;
        }
    }
    entry.bytes = sizeof(CachedObjectGeometry) + GeometryBytes(entry.visible.Get());
    for (const ComPtr<ID2D1Geometry>& cut : entry.cuts)
    {
        entry.bytes += GeometryBytes(cut.Get()) + sizeof(ComPtr<ID2D1Geometry>);
        if (entry.bytes > MAX_GEOMETRY_CACHE_BYTES)
            break;
    }
    visible = entry.visible;
    cuts = entry.cuts;
    if (entry.pointCount > MAX_GEOMETRY_CACHE_POINTS || entry.bytes > MAX_GEOMETRY_CACHE_BYTES)
        return S_OK;
    while (!cache.entries.empty() && (cache.entries.size() >= MAX_GEOMETRY_CACHE_ENTRIES ||
                                      cache.points + entry.pointCount > MAX_GEOMETRY_CACHE_POINTS ||
                                      cache.bytes + entry.bytes > MAX_GEOMETRY_CACHE_BYTES))
        cache.Remove(std::prev(cache.entries.end()));
    const std::size_t points = entry.pointCount;
    const std::size_t bytes = entry.bytes;
    cache.entries.push_front(std::move(entry));
    cache.points += points;
    cache.bytes += bytes;
    return S_OK;
}
catch (const std::exception&)
{
    return E_OUTOFMEMORY;
}

// 将缓存的对象局部几何变换到本次输出原点，不重新展开或裁减原路径。
// 入参：factory 为目标工厂；object 为有效对象；origin 为目标原点；output 为可见几何；cuts 可选接收擦痕。
// 返回：变换与缓存查询完成为 S_OK；失败返回图形或分配错误。
HRESULT CreateVisibleGeometry(ID2D1Factory* factory, const open_st::AnnotationObject& object,
                              open_st::AnnotationPoint origin, ComPtr<ID2D1Geometry>& output,
                              std::vector<ComPtr<ID2D1Geometry>>* cuts = nullptr)
try
{
    ComPtr<ID2D1Geometry> local;
    std::vector<ComPtr<ID2D1Geometry>> localCuts;
    HRESULT result = LocalVisibleGeometry(factory, object, local, localCuts);
    if (FAILED(result))
        return result;
    const D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(static_cast<float>(object.origin.x - origin.x),
                                                                      static_cast<float>(object.origin.y - origin.y));
    ComPtr<ID2D1TransformedGeometry> transformed;
    result = factory->CreateTransformedGeometry(local.Get(), transform, transformed.GetAddressOf());
    if (FAILED(result))
        return result;
    output = transformed;
    if (cuts != nullptr)
    {
        for (const ComPtr<ID2D1Geometry>& cut : localCuts)
        {
            ComPtr<ID2D1TransformedGeometry> moved;
            result = factory->CreateTransformedGeometry(cut.Get(), transform, moved.GetAddressOf());
            if (FAILED(result))
                return result;
            cuts->push_back(moved);
        }
    }
    return S_OK;
}
catch (const std::exception&)
{
    return E_OUTOFMEMORY;
}

} // namespace

namespace open_st
{
// 向已有绘制周期提交共享几何，保留调用者的冻结底图颜色与变换。
// 入参：target：有效 D2D 目标；annotations：只读对象；selection：裁剪；targetOrigin：桌面原点；
// hdr、whiteScale：颜色映射参数；error：资源或输入失败原因。
// 返回：全部提交为 true；失败为 false，调用者须 EndDraw 后丢弃本次输出。
bool DrawAnnotations(ID2D1RenderTarget* target, const AnnotationSnapshot& annotations, RectI selection,
                     AnnotationPoint targetOrigin, bool hdr, float whiteScale, std::wstring& error,
                     AnnotationMosaicSource* source)
{
    if (!annotations || annotations->empty())
    {
        return true;
    }
    bool mosaic{};
    for (const AnnotationObject& object : *annotations)
    {
        if (!IsValidAnnotation(object))
        {
            error = L"标注几何或样式无效。";
            return false;
        }
        mosaic = mosaic || object.kind == AnnotationKind::Mosaic;
    }
    if (mosaic && source == nullptr)
    {
        error = L"马赛克绘制缺少冻结桌面来源。";
        return false;
    }
    if (mosaic && !source->Prepare(annotations, selection, error))
        return false;
    ComPtr<ID2D1SolidColorBrush> brush;
    HRESULT result = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush.GetAddressOf());
    if (FAILED(result))
    {
        error = FormatHResult(L"创建标注画刷", result);
        return false;
    }
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    const D2D1_ANTIALIAS_MODE previousAntialias = target->GetAntialiasMode();
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(selection.left - targetOrigin.x),
                                            static_cast<float>(selection.top - targetOrigin.y),
                                            static_cast<float>(selection.right - targetOrigin.x),
                                            static_cast<float>(selection.bottom - targetOrigin.y)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    for (const AnnotationObject& object : *annotations)
    {
        if (object.style.transparency == 100U)
        {
            continue;
        }
        const std::uint32_t rgb = object.style.rgb;
        brush->SetColor(D2D1::ColorF(Channel((rgb >> 16U) & 255U, hdr, whiteScale),
                                     Channel((rgb >> 8U) & 255U, hdr, whiteScale), Channel(rgb & 255U, hdr, whiteScale),
                                     static_cast<float>(100U - object.style.transparency) / 100.0F));
        if ((object.erasures && !object.erasures->empty()) || object.kind == AnnotationKind::Pen ||
            object.kind == AnnotationKind::Line || object.kind == AnnotationKind::Ellipse ||
            object.kind == AnnotationKind::Text || object.kind == AnnotationKind::Mosaic)
        {
            ComPtr<ID2D1Geometry> geometry;
            result = CreateVisibleGeometry(factory.Get(), object, targetOrigin, geometry);
            if (FAILED(result))
            {
                target->PopAxisAlignedClip();
                target->SetAntialiasMode(previousAntialias);
                error = FormatHResult(L"创建标注可见几何", result);
                return false;
            }
            if (object.kind == AnnotationKind::Mosaic)
            {
                if (!source->Draw(target, object, geometry.Get(), targetOrigin, selection, hdr, whiteScale, error))
                {
                    target->PopAxisAlignedClip();
                    target->SetAntialiasMode(previousAntialias);
                    return false;
                }
            }
            else
                target->FillGeometry(geometry.Get(), brush.Get());
            continue;
        }
        const D2D1_RECT_F rectangle = ObjectRectangle(object, targetOrigin);
        switch (object.kind)
        {
        case AnnotationKind::Rectangle:
            target->DrawRectangle(rectangle, brush.Get(), static_cast<float>(object.style.lineWidth));
            break;
        case AnnotationKind::FilledRectangle:
            target->FillRectangle(rectangle, brush.Get());
            break;
        case AnnotationKind::RoundedRectangle:
        {
            const float radius = ObjectRadius(object);
            target->FillRoundedRectangle(D2D1::RoundedRect(rectangle, radius, radius), brush.Get());
            break;
        }
        case AnnotationKind::Arrow:
        {
            ComPtr<ID2D1PathGeometry> geometry;
            result = CreateArrow(factory.Get(), object, targetOrigin, geometry);
            if (FAILED(result))
            {
                target->PopAxisAlignedClip();
                target->SetAntialiasMode(previousAntialias);
                error = FormatHResult(L"创建标注箭头轮廓", result);
                return false;
            }
            target->FillGeometry(geometry.Get(), brush.Get());
            break;
        }
        default:
            break;
        }
    }
    target->PopAxisAlignedClip();
    target->SetAntialiasMode(previousAntialias);
    return true;
}

// 创建本次输出专用透明层并以预乘 source-over 合成，保持空层底图字节。
// 入参：selection：物理边界；annotations：对象快照；base：原 BGRX；pixels：接收结果；error：失败信息。
// 返回：完整合成成功为 true；任何错误清空结果并返回 false。
bool CompositeAnnotations(RectI selection, const AnnotationSnapshot& annotations, std::span<const std::uint8_t> base,
                          std::vector<std::uint8_t>& pixels, std::wstring& error, AnnotationMosaicSource* source)
{
    pixels.clear();
    const ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
    {
        error = FormatHResult(L"初始化标注输出 COM", com.result);
        return false;
    }
    ComPtr<IWICImagingFactory> imaging;
    HRESULT result =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(imaging.GetAddressOf()));
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(result))
    {
        result = imaging->CreateBitmap(static_cast<UINT>(selection.Width()), static_cast<UINT>(selection.Height()),
                                       GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf());
    }
    ComPtr<ID2D1Factory> factory;
    if (SUCCEEDED(result))
    {
        result = SharedAnnotationFactory(factory);
    }
    ComPtr<ID2D1RenderTarget> target;
    if (SUCCEEDED(result))
    {
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
        result = factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, target.GetAddressOf());
    }
    if (FAILED(result))
    {
        error = FormatHResult(L"创建标注透明输出层", result);
        return false;
    }
    struct TargetGuard
    {
        AnnotationMosaicSource* source{};
        ID2D1RenderTarget* target{};
        // 离屏目标每次输出结束立即解除缓存引用，不保留整张 WIC 图像。
        // 入参：无。
        // 返回：无。
        ~TargetGuard()
        {
            if (this->source != nullptr)
                this->source->ReleaseTarget(this->target);
        }
    } targetGuard{source, target.Get()};
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
    const bool drawn = DrawAnnotations(target.Get(), annotations, selection,
                                       {static_cast<double>(selection.left), static_cast<double>(selection.top)}, false,
                                       1.0F, error, source);
    result = target->EndDraw();
    if (!drawn)
    {
        return false;
    }
    if (FAILED(result))
    {
        error = FormatHResult(L"绘制标注透明输出层", result);
        return false;
    }
    const WICRect region{0, 0, selection.Width(), selection.Height()};
    ComPtr<IWICBitmapLock> lock;
    result = bitmap->Lock(&region, WICBitmapLockRead, lock.GetAddressOf());
    UINT stride{};
    UINT length{};
    BYTE* bytes{};
    if (SUCCEEDED(result))
    {
        result = lock->GetStride(&stride);
    }
    if (SUCCEEDED(result))
    {
        result = lock->GetDataPointer(&length, &bytes);
    }
    if (FAILED(result))
    {
        error = FormatHResult(L"读取标注透明输出层", result);
        return false;
    }
    pixels.assign(base.begin(), base.end());
    for (int y = 0; y < selection.Height(); ++y)
    {
        for (int x = 0; x < selection.Width(); ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * selection.Width() + x) * 4U;
            const BYTE* layer = bytes + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4U;
            const unsigned alpha = layer[3U];
            if (alpha == 0U)
            {
                continue;
            }
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                pixels[index + channel] = static_cast<std::uint8_t>(std::min(
                    255U, static_cast<unsigned>(layer[channel]) +
                              (static_cast<unsigned>(pixels[index + channel]) * (255U - alpha) + 127U) / 255U));
            }
            pixels[index + 3U] = 255U;
        }
    }
    return true;
}
// 查询最上层可见元素，精确覆盖优先于任意上层元素的辅助点击容差。
// 入参：annotations：只读对象；selection：正式物理选区；point：物理点击点；tolerance：物理容差；
// objectId：接收元素 ID，未命中为零；error：失败诊断。
// 返回：查询完成为 true；输入或图形错误为 false，输出 ID 始终清零。
bool HitTestAnnotations(const AnnotationSnapshot& annotations, RectI selection, AnnotationPoint point, float tolerance,
                        std::uint64_t& objectId, std::wstring& error)
try
{
    objectId = 0U;
    error.clear();
    const std::int64_t width = static_cast<std::int64_t>(selection.right) - selection.left;
    const std::int64_t height = static_cast<std::int64_t>(selection.bottom) - selection.top;
    if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX || !std::isfinite(point.x) ||
        !std::isfinite(point.y) || !std::isfinite(tolerance) || tolerance < 0.0F ||
        tolerance > std::numeric_limits<float>::max() * 0.5F)
    {
        error = L"标注命中的选区、点击位置或容差无效。";
        return false;
    }
    if (!annotations || annotations->empty() || point.x < selection.left || point.x >= selection.right ||
        point.y < selection.top || point.y >= selection.bottom)
    {
        return true;
    }
    for (const AnnotationObject& object : *annotations)
    {
        if (object.id != 0U && object.style.transparency != 100U && !IsValidAnnotation(object))
        {
            error = L"标注命中的对象几何或样式无效。";
            return false;
        }
    }
    ComPtr<ID2D1Factory> factory;
    HRESULT result = SharedAnnotationFactory(factory);
    if (FAILED(result))
    {
        error = FormatHResult(L"创建标注命中工厂", result);
        return false;
    }
    const AnnotationPoint origin{static_cast<double>(selection.left), static_cast<double>(selection.top)};
    const D2D1_POINT_2F localPoint =
        D2D1::Point2F(static_cast<float>(point.x - origin.x), static_cast<float>(point.y - origin.y));
    std::vector<ComPtr<ID2D1Geometry>> geometries(annotations->size());
    std::vector<std::vector<ComPtr<ID2D1Geometry>>> erasures(annotations->size());
    for (std::size_t index = annotations->size(); index > 0U; --index)
    {
        const AnnotationObject& object = (*annotations)[index - 1U];
        if (object.id == 0U || object.style.transparency == 100U)
        {
            continue;
        }
        ComPtr<ID2D1Geometry>& geometry = geometries[index - 1U];
        result = CreateVisibleGeometry(factory.Get(), object, origin, geometry, &erasures[index - 1U]);
        BOOL contains = FALSE;
        if (SUCCEEDED(result))
        {
            result = geometry->FillContainsPoint(localPoint, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &contains);
        }
        if (FAILED(result))
        {
            error = FormatHResult(L"查询标注精确命中", result);
            return false;
        }
        if (contains != FALSE)
        {
            objectId = object.id;
            return true;
        }
    }
    if (tolerance == 0.0F)
    {
        return true;
    }
    ComPtr<ID2D1RectangleGeometry> clip;
    result = factory->CreateRectangleGeometry(
        D2D1::RectF(0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)), clip.GetAddressOf());
    if (FAILED(result))
    {
        error = FormatHResult(L"创建标注命中裁剪", result);
        return false;
    }
    // 圆接角将辅助容差限制在真实轮廓附近，避免箭头尖端的斜接角放大点击距离。
    D2D1_STROKE_STYLE_PROPERTIES toleranceProperties{};
    toleranceProperties.lineJoin = D2D1_LINE_JOIN_ROUND;
    toleranceProperties.miterLimit = 1.0F;
    ComPtr<ID2D1StrokeStyle> toleranceStyle;
    result = factory->CreateStrokeStyle(toleranceProperties, nullptr, 0U, toleranceStyle.GetAddressOf());
    if (FAILED(result))
    {
        error = FormatHResult(L"创建标注命中容差样式", result);
        return false;
    }
    for (std::size_t index = annotations->size(); index > 0U; --index)
    {
        const AnnotationObject& object = (*annotations)[index - 1U];
        if (!geometries[index - 1U] || (object.kind != AnnotationKind::Rectangle &&
                                        object.kind != AnnotationKind::Arrow && object.kind != AnnotationKind::Pen &&
                                        object.kind != AnnotationKind::Line && object.kind != AnnotationKind::Ellipse))
        {
            continue;
        }
        bool erased{};
        for (const ComPtr<ID2D1Geometry>& cut : erasures[index - 1U])
        {
            BOOL within{};
            result = cut->FillContainsPoint(localPoint, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &within);
            if (FAILED(result))
            {
                error = FormatHResult(L"查询标注擦痕边界", result);
                return false;
            }
            erased = erased || within != FALSE;
        }
        if (erased)
            continue;
        ComPtr<ID2D1Geometry> clipped;
        result = CombineGeometry(factory.Get(), geometries[index - 1U].Get(), clip.Get(), D2D1_COMBINE_MODE_INTERSECT,
                                 clipped);
        BOOL contains = FALSE;
        if (SUCCEEDED(result))
        {
            result = clipped->StrokeContainsPoint(localPoint, tolerance * 2.0F, toleranceStyle.Get(), nullptr,
                                                  D2D1_DEFAULT_FLATTENING_TOLERANCE, &contains);
        }
        if (FAILED(result))
        {
            error = FormatHResult(L"查询标注容差命中", result);
            return false;
        }
        if (contains != FALSE)
        {
            objectId = object.id;
            return true;
        }
    }
    return true;
}
catch (const std::exception&)
{
    objectId = 0U;
    error = L"无法分配标注命中资源。";
    return false;
}

// 查找新擦痕与扣除旧擦痕后的对象交集，不按图层遮挡忽略下层对象。
// 入参：annotations 为固定文档；stroke 为桌面坐标圆笔迹及手势选区；ids 接收稳定目标 ID；error 接收诊断。
// 返回：查询成功为 true，空擦除返回空 ID；非法输入、超预算或图形错误为 false 并清空 ID。
bool FindAnnotationEraseTargets(const AnnotationSnapshot& annotations, const AnnotationEraseStroke& stroke,
                                std::vector<std::uint64_t>& ids, std::wstring& error)
try
{
    ids.clear();
    error.clear();
    if (!IsValidAnnotationEraseStroke(stroke))
    {
        error = L"擦除路径、半径或裁剪无效。";
        return false;
    }
    if (!annotations || annotations->empty())
        return true;
    ComPtr<ID2D1Factory> factory;
    HRESULT result = SharedAnnotationFactory(factory);
    const AnnotationPoint origin{stroke.clip.left, stroke.clip.top};
    ComPtr<ID2D1Geometry> erase;
    if (SUCCEEDED(result))
        result = CreateEraseGeometry(factory.Get(), stroke, {-origin.x, -origin.y}, erase);
    if (FAILED(result))
    {
        error = FormatHResult(L"创建擦除目标区域", result);
        return false;
    }
    std::vector<std::uint64_t> candidate;
    for (const AnnotationObject& object : *annotations)
    {
        if (object.id == 0 || object.style.transparency == 100)
            continue;
        if (!IsValidAnnotation(object))
        {
            error = L"擦除目标包含无效标注。";
            return false;
        }
        ComPtr<ID2D1Geometry> visible;
        result = CreateVisibleGeometry(factory.Get(), object, origin, visible);
        ComPtr<ID2D1Geometry> intersection;
        if (SUCCEEDED(result))
            result =
                CombineGeometry(factory.Get(), visible.Get(), erase.Get(), D2D1_COMBINE_MODE_INTERSECT, intersection);
        float area{};
        if (SUCCEEDED(result))
            result = intersection->ComputeArea(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &area);
        if (FAILED(result) || !std::isfinite(area))
        {
            error = FormatHResult(L"查询擦除实际交集", FAILED(result) ? result : E_FAIL);
            return false;
        }
        if (area > 0.0F)
            candidate.push_back(object.id);
    }
    ids = std::move(candidate);
    return true;
}
catch (const std::exception&)
{
    ids.clear();
    error = L"无法分配擦除几何资源。";
    return false;
}
} // namespace open_st
