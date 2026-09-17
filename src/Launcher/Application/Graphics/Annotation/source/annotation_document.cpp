// 文件职责：实现不可变文档追加、单元素样式替换及平移，失败时保留调用方快照。
#include <annotation_document.h>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace open_st
{
namespace
{
// 验证已有快照中的每个对象，防止文档操作发布无效内容。
// 入参：document：只读对象快照，可空。
// 返回：空文档或全部对象有效为 true。
bool ValidDocument(const AnnotationSnapshot& document) noexcept
{
    if (document)
    {
        for (const AnnotationObject& object : *document)
        {
            if (!IsValidAnnotation(object))
            {
                return false;
            }
        }
    }
    return true;
}
// 比较擦除路径的覆盖定义，使等值轨迹不会重复附加。
// 入参：left、right：有效不可变擦除手势。
// 返回：路径、半径和裁剪逐值一致时为 true。
bool SameEraseStroke(const AnnotationEraseStroke& left, const AnnotationEraseStroke& right) noexcept
{
    if (left.radius != right.radius || left.clip.left != right.clip.left || left.clip.top != right.clip.top ||
        left.clip.right != right.clip.right || left.clip.bottom != right.clip.bottom ||
        left.points.size() != right.points.size())
        return false;
    for (std::size_t index = 0U; index < left.points.size(); ++index)
    {
        if (left.points[index].x != right.points[index].x || left.points[index].y != right.points[index].y)
            return false;
    }
    return true;
}
} // namespace

// 从稳定 ID 解析文档中的只读对象。
// 入参：document：借用快照；id：非零对象 ID。
// 返回：借用对象指针，空文档、零 ID 或未找到返回空。
const AnnotationObject* FindAnnotation(const AnnotationSnapshot& document, std::uint64_t id) noexcept
{
    if (document && id != 0U)
    {
        for (const AnnotationObject& object : *document)
        {
            if (object.id == id)
            {
                return &object;
            }
        }
    }
    return nullptr;
}

// 将新对象追加到独立数组后一次发布结果，允许零 ID 绘制草稿。
// 入参：document：原快照；object：待追加对象；result：成功写入的新快照。
// 返回：完成为 true；输入或分配失败为 false，原快照及 result 保持不变。
bool AppendAnnotation(const AnnotationSnapshot& document, const AnnotationObject& object,
                      AnnotationSnapshot& result) noexcept
try
{
    if (!ValidDocument(document) || !IsValidAnnotation(object) ||
        (object.id != 0U && FindAnnotation(document, object.id) != nullptr))
    {
        return false;
    }
    std::vector<AnnotationObject> objects = document ? *document : std::vector<AnnotationObject>{};
    objects.push_back(object);
    result = std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
    return true;
}
catch (...)
{
    return false;
}

// 提取种类专属参数，不复制正文或擦除轨迹。
// 入参：object：有效对象。
// 返回：业务属性 DTO。
AnnotationProperties PropertiesOf(const AnnotationObject& object) noexcept
{
    AnnotationProperties properties{object.style, {}, {}};
    if (const AnnotationText* text = std::get_if<AnnotationText>(&object.payload))
        properties.fontSize = text->fontSize;
    if (const AnnotationMosaic* mosaic = std::get_if<AnnotationMosaic>(&object.payload))
        properties.blockSize = mosaic->blockSize;
    return properties;
}

// 按对象种类校验属性并只替换允许字段，正文、几何和旧擦痕保持。
// 入参：document：基线；id：稳定 ID；properties：候选参数；result：结果。
// 返回：成功为 true，同值共享原文档；失败保留 result。
bool ReplaceAnnotationProperties(const AnnotationSnapshot& document, std::uint64_t id,
                                 const AnnotationProperties& properties, AnnotationSnapshot& result) noexcept
try
{
    const AnnotationObject* found = FindAnnotation(document, id);
    if (found == nullptr || !ValidDocument(document))
    {
        return false;
    }
    AnnotationObject candidate = *found;
    candidate.style = properties.style;
    const AnnotationProperties original = PropertiesOf(*found);
    if (candidate.kind == AnnotationKind::Text)
    {
        if (!properties.fontSize || properties.blockSize || !IsValidAnnotationFontSize(*properties.fontSize))
            return false;
        std::get<AnnotationText>(candidate.payload).fontSize = *properties.fontSize;
    }
    else if (candidate.kind == AnnotationKind::Mosaic)
    {
        if (properties.fontSize || !properties.blockSize || !IsValidAnnotationBlockSize(*properties.blockSize) ||
            properties.style.rgb != original.style.rgb ||
            properties.style.transparency != original.style.transparency ||
            properties.style.lineWidth != original.style.lineWidth)
            return false;
        std::get<AnnotationMosaic>(candidate.payload).blockSize = *properties.blockSize;
    }
    else if (properties.fontSize || properties.blockSize)
        return false;
    if (!IsValidAnnotation(candidate))
    {
        return false;
    }
    if (original.style.rgb == properties.style.rgb && original.style.transparency == properties.style.transparency &&
        original.style.lineWidth == properties.style.lineWidth && original.fontSize == properties.fontSize &&
        original.blockSize == properties.blockSize)
    {
        result = document;
        return true;
    }
    const std::size_t index = static_cast<std::size_t>(found - document->data());
    std::vector<AnnotationObject> objects = *document;
    objects[index] = candidate;
    result = std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
    return true;
}
catch (...)
{
    return false;
}

// 统一规范化正文后替换文字对象，等值 LF 正文不产生新快照。
// 入参：document：基线；id：文字 ID；text：原始 UTF-16；result：结果。
// 返回：有效非空白正文替换成功为 true；失败保留 result。
bool ReplaceAnnotationText(const AnnotationSnapshot& document, std::uint64_t id, std::u16string_view text,
                           AnnotationSnapshot& result) noexcept
try
{
    const AnnotationObject* found = FindAnnotation(document, id);
    if (!found || found->kind != AnnotationKind::Text || !ValidDocument(document))
        return false;
    std::shared_ptr<const std::u16string> normalized;
    if (!NormalizeAnnotationText(text, normalized) || IsBlankAnnotationText(*normalized))
        return false;
    const AnnotationText& original = std::get<AnnotationText>(found->payload);
    if (*original.text == *normalized)
    {
        result = document;
        return true;
    }
    const std::size_t index = static_cast<std::size_t>(found - document->data());
    std::vector<AnnotationObject> objects = *document;
    std::get<AnnotationText>(objects[index].payload).text = std::move(normalized);
    result = std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
    return true;
}
catch (...)
{
    return false;
}

// 生成原位控件替代目标后的预览数组，原文档及所有对象共享载荷均保持不变。
// 入参：document：完整文档；id：暂时隐藏的稳定 ID；result：预览结果。
// 返回：目标有效且生成成功为 true，否则不修改 result。
bool HideAnnotationForPreview(const AnnotationSnapshot& document, std::uint64_t id, AnnotationSnapshot& result) noexcept
try
{
    if (!FindAnnotation(document, id) || !ValidDocument(document))
        return false;
    std::vector<AnnotationObject> objects;
    objects.reserve(document->size() - 1U);
    for (const AnnotationObject& object : *document)
    {
        if (object.id != id)
            objects.push_back(object);
    }
    result = std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
    return true;
}
catch (...)
{
    return false;
}

// 基于原快照平移全部对象，避免将中途越界的局部结果泄漏给调用方。
// 入参：document：原快照；delta：物理像素偏移；result：成功结果。
// 返回：完成为 true；非法偏移、对象越界或分配失败为 false，保留原 result。
bool TranslateAnnotations(const AnnotationSnapshot& document, AnnotationPoint delta,
                          AnnotationSnapshot& result) noexcept
try
{
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !ValidDocument(document))
    {
        return false;
    }
    if (!document || document->empty() || (delta.x == 0.0 && delta.y == 0.0))
    {
        result = document;
        return true;
    }
    std::vector<AnnotationObject> objects = *document;
    for (AnnotationObject& object : objects)
    {
        object.origin.x += delta.x;
        object.origin.y += delta.y;
        if (!IsValidAnnotation(object))
        {
            return false;
        }
    }
    result = std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
    return true;
}
catch (...)
{
    return false;
}

// 对指定对象追加共享擦除引用，使用其当前原点计算固定局部偏移。
// 入参：document：手势基线；ids：受影响的稳定 ID；stroke：完整擦除路径；result：结果快照。
// 返回：成功为 true；没有变化共享原文档，失败保持旧 result。
bool ApplyAnnotationErase(const AnnotationSnapshot& document, std::span<const std::uint64_t> ids,
                          std::shared_ptr<const AnnotationEraseStroke> stroke, AnnotationSnapshot& result) noexcept
try
{
    if (!ValidDocument(document) || !stroke || !IsValidAnnotationEraseStroke(*stroke))
        return false;
    if (ids.empty())
    {
        result = document;
        return true;
    }
    std::unordered_set<std::uint64_t> targets;
    for (std::uint64_t id : ids)
    {
        if (FindAnnotation(document, id) == nullptr)
            return false;
        targets.insert(id);
    }
    std::vector<AnnotationObject> objects = *document;
    bool changed = false;
    for (AnnotationObject& object : objects)
    {
        if (!targets.contains(object.id))
            continue;
        const AnnotationPoint offset{-object.origin.x, -object.origin.y};
        bool exists = false;
        if (object.erasures)
        {
            for (const AnnotationEraseMask& mask : *object.erasures)
            {
                if (mask.offset.x == offset.x && mask.offset.y == offset.y &&
                    (mask.stroke == stroke || SameEraseStroke(*mask.stroke, *stroke)))
                {
                    exists = true;
                    break;
                }
            }
        }
        if (exists)
            continue;
        std::vector<AnnotationEraseMask> masks =
            object.erasures ? *object.erasures : std::vector<AnnotationEraseMask>{};
        masks.push_back({stroke, offset});
        object.erasures = std::make_shared<const std::vector<AnnotationEraseMask>>(std::move(masks));
        changed = true;
    }
    result = changed ? std::make_shared<const std::vector<AnnotationObject>>(std::move(objects)) : document;
    return true;
}
catch (...)
{
    return false;
}

// 对当前文档和历史引用执行深层计费，同一对象数组、路径及擦除轨迹只计一次。
// 入参：documents：待计费的全部只读快照。
// 返回：总动态字节数；分配或整数溢出按超预算返回最大值。
std::size_t AnnotationDocumentsStorageBytes(std::span<const AnnotationSnapshot> documents,
                                            const std::shared_ptr<const AnnotationEraseStroke>& activeErase) noexcept
try
{
    constexpr std::size_t SHARED_OVERHEAD = 2U * sizeof(void*);
    std::unordered_set<const void*> visited;
    std::size_t bytes{};
    // 累加容量并检查溢出，共享管理开销包含在每个新分配块中。
    // 入参：amount：新增存储字节。
    // 返回：累加成功为 true。
    const auto add = [&bytes](std::size_t amount)
    {
        if (amount > std::numeric_limits<std::size_t>::max() - bytes)
            return false;
        bytes += amount;
        return true;
    };
    for (const AnnotationSnapshot& document : documents)
    {
        if (!document || !visited.insert(document.get()).second)
            continue;
        if (!add(sizeof(*document) + SHARED_OVERHEAD + document->capacity() * sizeof(AnnotationObject)))
            return std::numeric_limits<std::size_t>::max();
        for (const AnnotationObject& object : *document)
        {
            const AnnotationText* text = std::get_if<AnnotationText>(&object.payload);
            if (text && text->text && visited.insert(text->text.get()).second &&
                !add(sizeof(*text->text) + SHARED_OVERHEAD + (text->text->capacity() + 1U) * sizeof(char16_t)))
                return std::numeric_limits<std::size_t>::max();
            const AnnotationStroke* stroke = std::get_if<AnnotationStroke>(&object.payload);
            if (stroke && stroke->points && visited.insert(stroke->points.get()).second &&
                !add(sizeof(*stroke->points) + SHARED_OVERHEAD + stroke->points->capacity() * sizeof(AnnotationPoint)))
                return std::numeric_limits<std::size_t>::max();
            if (!object.erasures || !visited.insert(object.erasures.get()).second)
                continue;
            if (!add(sizeof(*object.erasures) + SHARED_OVERHEAD +
                     object.erasures->capacity() * sizeof(AnnotationEraseMask)))
                return std::numeric_limits<std::size_t>::max();
            for (const AnnotationEraseMask& mask : *object.erasures)
            {
                if (mask.stroke && visited.insert(mask.stroke.get()).second &&
                    !add(sizeof(*mask.stroke) + SHARED_OVERHEAD +
                         mask.stroke->points.capacity() * sizeof(AnnotationPoint)))
                    return std::numeric_limits<std::size_t>::max();
            }
        }
    }
    if (activeErase && visited.insert(activeErase.get()).second &&
        !add(sizeof(*activeErase) + SHARED_OVERHEAD + activeErase->points.capacity() * sizeof(AnnotationPoint)))
        return std::numeric_limits<std::size_t>::max();
    return bytes;
}
catch (...)
{
    return std::numeric_limits<std::size_t>::max();
}
} // namespace open_st
