// 实现有界历史及标注随截图选区平移的原子事务，不保存图像像素。
#include "capture_annotation_state.h"
#include <algorithm>
#include <annotation_document.h>
#include <annotation_erasure.h>
#include <array>
#include <cmath>
#include <limits>

namespace open_st
{
namespace
{
constexpr std::size_t MAX_OBJECTS = 1024;
constexpr std::size_t MAX_HISTORY = 128;
constexpr std::size_t MAX_BYTES = 32U * 1024U * 1024U;
// 比较裁剪几何。
// 入参：a、b 为物理矩形。返回：各边相等时 true。
bool SameRectangle(RectI a, RectI b) noexcept
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
// 对即将提交的文档及撤销历史深层计费，共享路径、擦除轨迹与对象数组按身份去重。
// 入参：next：新文档；history：候选撤销栈。
// 返回：含历史包装的字节数，统计失败返回最大值。
std::size_t HistoryBytes(const AnnotationSnapshot& next, const std::deque<AnnotationEditSnapshot>& history)
{
    std::vector<AnnotationSnapshot> documents;
    documents.reserve(history.size() + 1U);
    documents.push_back(next);
    for (const AnnotationEditSnapshot& item : history)
        documents.push_back(item.objects);
    const std::size_t bytes = AnnotationDocumentsStorageBytes(documents);
    const std::size_t wrappers = documents.size() * sizeof(AnnotationEditSnapshot);
    return bytes > std::numeric_limits<std::size_t>::max() - wrappers ? std::numeric_limits<std::size_t>::max()
                                                                      : bytes + wrappers;
}
} // namespace
// 判断填充工具。
// 入参：无。返回：填充工具时 true。
bool CaptureAnnotationState::IsFill() const noexcept
{
    return this->tool_ == CaptureAnnotationTool::FilledRectangle ||
           this->tool_ == CaptureAnnotationTool::RoundedRectangle;
}
// 查询当前默认样式。
// 入参：无。返回：工具组样式。
AnnotationStyle CaptureAnnotationState::Style() const noexcept
{
    return this->IsFill() ? this->fill_ : this->stroke_;
}
// 接受有效工具样式。
// 入参：style 为候选。返回：空闲且有效时 true。
bool CaptureAnnotationState::SetStyle(AnnotationStyle style) noexcept
{
    if (this->Active() || !IsValidAnnotationStyle(style) || style.lineWidth < 1.0)
        return false;
    (this->IsFill() ? this->fill_ : this->stroke_) = style;
    return true;
}

// 更新独立橡皮默认直径，进行中手势保持按下时固定的大小。
// 入参：diameter：8、16、32 或 64 物理像素。
// 返回：空闲且直径合法时为 true。
bool CaptureAnnotationState::SetEraserDiameter(unsigned diameter) noexcept
{
    if (this->Active() ||
        std::find(CAPTURE_ANNOTATION_ERASER_DIAMETERS.begin(), CAPTURE_ANNOTATION_ERASER_DIAMETERS.end(), diameter) ==
            CAPTURE_ANNOTATION_ERASER_DIAMETERS.end())
        return false;
    this->eraserDiameter_ = diameter;
    return true;
}

// 设置文字默认字号，活动事务保持按开始时固定的值。
// 入参：fontSize：物理像素字号。
// 返回：空闲且属于允许档位时为 true。
bool CaptureAnnotationState::SetTextFontSize(unsigned fontSize) noexcept
{
    if (this->Active() || !IsValidAnnotationFontSize(fontSize))
        return false;
    this->textFontSize_ = fontSize;
    return true;
}

// 设置后续马赛克手势使用的块大小，不批量改写已有对象。
// 入参：blockSize：物理像素边长。
// 返回：空闲且允许时为 true。
bool CaptureAnnotationState::SetMosaicBlockSize(unsigned blockSize) noexcept
{
    if (this->Active() || !IsValidAnnotationBlockSize(blockSize))
        return false;
    this->mosaicBlockSize_ = blockSize;
    return true;
}

// 替换来源准备钩子，仅允许在编辑空闲时建立会话级依赖。
// 入参：preparation：同步候选和目标裁剪准备回调。
// 返回：接受为 true，活动事务或正在回调时拒绝。
bool CaptureAnnotationState::SetPreviewPreparation(
    std::function<bool(const AnnotationSnapshot&, RectI)> preparation) noexcept
{
    if (this->Active() || this->preparingPreview_)
        return false;
    this->previewPreparation_ = std::move(preparation);
    return true;
}

// 只有候选包含马赛克时调用来源准备，异常与重入均按失败拒绝。
// 入参：next：候选快照；selection：候选实际对应的正式裁剪。
// 返回：无需准备或准备成功为 true。
bool CaptureAnnotationState::PreparePreview(const AnnotationSnapshot& next, RectI selection) noexcept
{
    if (!next || !this->previewPreparation_)
        return true;
    bool mosaic = false;
    for (const AnnotationObject& object : *next)
        mosaic = mosaic || object.kind == AnnotationKind::Mosaic;
    if (!mosaic)
        return true;
    if (selection.IsEmpty() || this->preparingPreview_)
        return false;
    this->preparingPreview_ = true;
    bool success = false;
    try
    {
        const std::function<bool(const AnnotationSnapshot&, RectI)> preparation = this->previewPreparation_;
        success = preparation(next, selection);
    }
    catch (...)
    {
    }
    this->preparingPreview_ = false;
    return success;
}

// 计算当前基线与活动草稿的独立预算，空擦路径也不可无限增长。
// 入参：next：候选文档；erase：可选活动擦除路径；additionalBytes：独立动态缓存占用。
// 返回：总占用在活动 32 MiB 预算内为 true。
bool CaptureAnnotationState::FitsActiveBudget(const AnnotationSnapshot& next,
                                              const std::shared_ptr<const AnnotationEraseStroke>& erase,
                                              std::size_t additionalBytes) const noexcept
{
    const std::array<AnnotationSnapshot, 3U> documents{this->committed_, this->preview_, next};
    const std::size_t bytes = AnnotationDocumentsStorageBytes(documents, erase);
    constexpr std::size_t AVAILABLE_BYTES = MAX_BYTES - sizeof(AnnotationEditSnapshot) - sizeof(AnnotationObject);
    return additionalBytes <= AVAILABLE_BYTES && bytes <= AVAILABLE_BYTES - additionalBytes;
}
// 固定绘图手势的起点和默认样式。
// 入参：point 为桌面物理点，selection 为非空裁剪。返回：接受时 true。
bool CaptureAnnotationState::BeginDraw(PointI point, RectI selection) noexcept
try
{
    if (this->Active() || this->tool_ == CaptureAnnotationTool::Select || this->tool_ == CaptureAnnotationTool::Text ||
        selection.IsEmpty() || point.x < selection.left || point.x >= selection.right || point.y < selection.top ||
        point.y >= selection.bottom ||
        (this->tool_ != CaptureAnnotationTool::Eraser && this->tool_ != CaptureAnnotationTool::Mosaic &&
         this->Style().transparency == 100))
        return false;
    if (this->tool_ == CaptureAnnotationTool::Eraser)
    {
        AnnotationEraseStroke stroke;
        stroke.points.push_back({static_cast<double>(point.x), static_cast<double>(point.y)});
        stroke.radius = static_cast<double>(this->eraserDiameter_) * 0.5;
        stroke.clip = {static_cast<double>(selection.left), static_cast<double>(selection.top),
                       static_cast<double>(selection.right), static_cast<double>(selection.bottom)};
        this->eraseDraft_ = std::make_shared<const AnnotationEraseStroke>(std::move(stroke));
        this->before_ = AnnotationEditSnapshot{selection, this->committed_};
        if (!this->UpdateErase(point))
        {
            this->Cancel();
            return false;
        }
        return true;
    }
    AnnotationObject object;
    object.origin = {static_cast<double>(point.x), static_cast<double>(point.y)};
    object.style = this->Style();
    switch (this->tool_)
    {
    case CaptureAnnotationTool::Rectangle:
        object.kind = AnnotationKind::Rectangle;
        break;
    case CaptureAnnotationTool::Arrow:
        object.kind = AnnotationKind::Arrow;
        break;
    case CaptureAnnotationTool::FilledRectangle:
        object.kind = AnnotationKind::FilledRectangle;
        break;
    case CaptureAnnotationTool::RoundedRectangle:
        object.kind = AnnotationKind::RoundedRectangle;
        break;
    case CaptureAnnotationTool::Pen:
        object.kind = AnnotationKind::Pen;
        object.payload = AnnotationStroke{
            std::make_shared<const std::vector<AnnotationPoint>>(std::vector<AnnotationPoint>{{0.0, 0.0}})};
        break;
    case CaptureAnnotationTool::Line:
        object.kind = AnnotationKind::Line;
        break;
    case CaptureAnnotationTool::Ellipse:
        object.kind = AnnotationKind::Ellipse;
        break;
    case CaptureAnnotationTool::Mosaic:
        object.kind = AnnotationKind::Mosaic;
        object.style.transparency = 0U;
        object.payload = AnnotationMosaic{this->mosaicBlockSize_};
        break;
    default:
        return false;
    }
    this->before_ = AnnotationEditSnapshot{selection, this->committed_};
    this->draft_ = object;
    this->inputValid_ = true;
    if (object.kind == AnnotationKind::Pen && !this->UpdateDraw(point))
    {
        this->Cancel();
        return false;
    }
    return true;
}
catch (...)
{
    this->Cancel();
    return false;
}
// 保留未裁剪的草稿几何，显示与输出统一裁剪到当前选区。
// 入参：point 为物理坐标。返回：成功时 true。
bool CaptureAnnotationState::UpdateDraw(PointI point) noexcept
try
{
    if (this->eraseDraft_)
        return this->UpdateErase(point);
    if (!this->draft_ || !this->before_)
        return false;
    this->inputValid_ = false;
    AnnotationObject object = *this->draft_;
    object.extent = {static_cast<double>(point.x) - object.origin.x, static_cast<double>(point.y) - object.origin.y};
    if (object.kind == AnnotationKind::Pen)
    {
        const AnnotationStroke& stroke = std::get<AnnotationStroke>(object.payload);
        const AnnotationPoint last = stroke.points->back();
        if (last.x != object.extent.x || last.y != object.extent.y)
        {
            if (stroke.points->size() >= ANNOTATION_MAX_PATH_POINTS)
                return false;
            std::vector<AnnotationPoint> points = *stroke.points;
            points.push_back(object.extent);
            object.payload = AnnotationStroke{std::make_shared<const std::vector<AnnotationPoint>>(std::move(points))};
        }
    }
    AnnotationSnapshot next = this->committed_;
    if (IsValidAnnotation(object))
    {
        if (!AppendAnnotation(this->committed_, object, next))
            return false;
    }
    if (!this->FitsActiveBudget(next))
        return false;
    if (!this->PreparePreview(next, this->before_->selection) || !this->before_ || !this->draft_)
        return false;
    this->draft_ = object;
    this->inputValid_ = true;
    this->preview_ = std::move(next);
    return true;
}
catch (...)
{
    return false;
}

// 用首点或新增相邻线段查询固定基线，累计目标后用完整原始轨迹生成整笔预览。
// 入参：point：本次桌面物理坐标。
// 返回：成功为 true；重复点直接复用预览，失败取消整笔并清空累计目标。
bool CaptureAnnotationState::UpdateErase(PointI point) noexcept
try
{
    if (!this->eraseDraft_ || !this->before_)
        return false;
    std::shared_ptr<const AnnotationEraseStroke> stroke = this->eraseDraft_;
    const AnnotationPoint last = stroke->points.back();
    const bool changed = last.x != point.x || last.y != point.y;
    if (this->eraseQueryStarted_ && !changed)
        return true;
    AnnotationEraseStroke segment;
    segment.radius = stroke->radius;
    segment.clip = stroke->clip;
    segment.points.push_back(last);
    if (changed)
    {
        if (stroke->points.size() >= ANNOTATION_MAX_PATH_POINTS)
        {
            this->Cancel();
            return false;
        }
        AnnotationEraseStroke candidate = *stroke;
        const AnnotationPoint current{static_cast<double>(point.x), static_cast<double>(point.y)};
        candidate.points.push_back(current);
        segment.points.push_back(current);
        stroke = std::make_shared<const AnnotationEraseStroke>(std::move(candidate));
    }
    std::vector<std::uint64_t> additions;
    std::wstring error;
    if (!FindAnnotationEraseTargets(this->before_->objects, segment, additions, error))
    {
        this->Cancel();
        return false;
    }
    std::vector<std::uint64_t> targets = this->eraseTargets_;
    for (std::uint64_t id : additions)
    {
        if (std::find(targets.begin(), targets.end(), id) == targets.end())
            targets.push_back(id);
    }
    AnnotationSnapshot next;
    if (!ApplyAnnotationErase(this->before_->objects, targets, stroke, next) ||
        !this->FitsActiveBudget(next, stroke, targets.capacity() * sizeof(std::uint64_t)) ||
        !this->PreparePreview(next, this->before_->selection) || !this->before_)
    {
        this->Cancel();
        return false;
    }
    this->eraseTargets_ = std::move(targets);
    this->eraseQueryStarted_ = true;
    this->eraseDraft_ = std::move(stroke);
    this->preview_ = std::move(next);
    return true;
}
catch (...)
{
    this->Cancel();
    return false;
}
// 提交一笔；无面积或完全透明不产生历史。
// 入参：point 为最终坐标。返回：提交状态。
AnnotationCommitResult CaptureAnnotationState::EndDraw(PointI point) noexcept
{
    if (this->eraseDraft_ && this->before_)
    {
        if (!this->UpdateErase(point))
        {
            this->Cancel();
            return AnnotationCommitResult::Failed;
        }
        if (this->preview_ == this->committed_)
        {
            this->Cancel();
            return AnnotationCommitResult::Unchanged;
        }
        try
        {
            if (this->Commit(this->before_->selection, this->preview_))
                return AnnotationCommitResult::Committed;
        }
        catch (...)
        {
        }
        this->Cancel();
        return AnnotationCommitResult::Failed;
    }
    if (!this->draft_ || !this->before_)
        return AnnotationCommitResult::Unchanged;
    if (!this->UpdateDraw(point))
    {
        if (!this->draft_ || this->draft_->kind != AnnotationKind::Mosaic)
            this->Cancel();
        return AnnotationCommitResult::Failed;
    }
    if (!IsValidAnnotation(*this->draft_))
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    try
    {
        if ((this->committed_ && this->committed_->size() >= MAX_OBJECTS) ||
            this->nextId_ == std::numeric_limits<std::uint64_t>::max())
        {
            this->Cancel();
            return AnnotationCommitResult::Failed;
        }
        AnnotationObject object = *this->draft_;
        object.id = this->nextId_;
        AnnotationSnapshot next;
        if (!AppendAnnotation(this->committed_, object, next))
        {
            this->Cancel();
            return AnnotationCommitResult::Failed;
        }
        if (!this->Commit(this->before_->selection, next))
        {
            this->Cancel();
            return AnnotationCommitResult::Failed;
        }
        ++this->nextId_;
        return AnnotationCommitResult::Committed;
    }
    catch (...)
    {
        this->Cancel();
        return AnnotationCommitResult::Failed;
    }
}
// 保存选区操作基线。
// 入参：selection 为开始前矩形。返回：无。
void CaptureAnnotationState::BeginCrop(RectI selection) noexcept
{
    if (!this->Active() && !selection.IsEmpty())
    {
        this->before_ = AnnotationEditSnapshot{selection, this->committed_};
        this->inputValid_ = true;
    }
}
// 根据相对基线的平移生成预览，避免累计偏差；缩放保持原对象。
// 入参：selection 为新选区，moving 指明整体移动。返回：成功时 true。
bool CaptureAnnotationState::UpdateCrop(RectI selection, bool moving) noexcept
try
{
    if (this->PreviewingObject() || this->EditingText())
        return false;
    if (!this->before_ || this->Drawing())
        return true;
    this->inputValid_ = false;
    AnnotationSnapshot next = this->committed_;
    if (!moving || !this->committed_)
    {
        if (!this->PreparePreview(next, selection) || !this->before_)
            return false;
        this->preview_ = next;
        this->inputValid_ = true;
        return true;
    }
    const double dx = static_cast<double>(selection.left) - this->before_->selection.left;
    const double dy = static_cast<double>(selection.top) - this->before_->selection.top;
    if (!TranslateAnnotations(this->committed_, {dx, dy}, next) || !this->FitsActiveBudget(next) ||
        !this->PreparePreview(next, selection) || !this->before_)
        return false;
    this->preview_ = std::move(next);
    this->inputValid_ = true;
    return true;
}
catch (...)
{
    return false;
}
// 提交选区操作与其标注平移。
// 入参：selection 为结束矩形。返回：提交状态。
AnnotationCommitResult CaptureAnnotationState::EndCrop(RectI selection) noexcept
{
    if (!this->before_ || this->Drawing() || this->EditingText())
        return AnnotationCommitResult::Unchanged;
    if (!this->inputValid_)
    {
        this->Cancel();
        return AnnotationCommitResult::Failed;
    }
    if (SameRectangle(this->before_->selection, selection))
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    try
    {
        if (this->Commit(selection, this->preview_))
            return AnnotationCommitResult::Committed;
    }
    catch (...)
    {
    }
    this->Cancel();
    return AnnotationCommitResult::Failed;
}
// 构造新的历史后一次发布；历史淘汰不影响失败前状态。
// 入参：selection 为新矩形，next 为新文档。返回：预算允许且成功时 true。
bool CaptureAnnotationState::Commit(RectI selection, AnnotationSnapshot next)
{
    if (!this->before_ || selection.IsEmpty() || this->revision_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    std::deque<AnnotationEditSnapshot> history = this->undo_;
    history.push_back(*this->before_);
    while (history.size() > 1 && (history.size() > MAX_HISTORY || HistoryBytes(next, history) > MAX_BYTES))
    {
        history.pop_front();
    }
    if (HistoryBytes(next, history) > MAX_BYTES)
        return false;
    if (!this->PreparePreview(next, selection) || !this->before_)
        return false;
    this->undo_.swap(history);
    this->redo_.clear();
    this->committed_ = std::move(next);
    this->preview_ = this->committed_;
    this->before_.reset();
    this->draft_.reset();
    this->textDraft_.reset();
    this->inputValid_ = true;
    this->eraseDraft_.reset();
    this->eraseTargets_.clear();
    this->eraseQueryStarted_ = false;
    this->objectPreview_.reset();
    ++this->revision_;
    return true;
}
// 固定属性编辑基线，所有临时修改只进入统一预览快照。
// 入参：id 为目标稳定 ID；revision 为调用方固定的文档修订；selection 为候选正式裁剪。
// 返回：目标和修订有效且状态空闲为 true；拒绝不改变已有事务。
bool CaptureAnnotationState::BeginObjectPreview(std::uint64_t id, std::uint64_t revision, RectI selection) noexcept
{
    if (this->Active() || revision != this->revision_)
        return false;
    const AnnotationObject* object = FindAnnotation(this->committed_, id);
    if (object == nullptr || !IsValidAnnotation(*object))
        return false;
    this->objectPreview_ = ObjectPreviewTransaction{id, revision, this->committed_, true, selection};
    this->preview_ = this->committed_;
    return true;
}

// 基于固定属性事务生成新预览，失败保留上次有效预览和所有已提交历史。
// 入参：properties 为目标元素的种类专属候选参数。
// 返回：生成成功为 true；非法、过期或分配失败为 false，并阻止失败候选确认。
bool CaptureAnnotationState::UpdateObjectPreview(const AnnotationProperties& properties) noexcept
{
    if (!this->objectPreview_)
        return false;
    const ObjectPreviewTransaction transaction = *this->objectPreview_;
    this->objectPreview_->valid = false;
    AnnotationSnapshot next;
    if (transaction.revision != this->revision_ || transaction.baseline != this->committed_ ||
        !ReplaceAnnotationProperties(transaction.baseline, transaction.id, properties, next) ||
        !this->FitsActiveBudget(next) || !this->PreparePreview(next, transaction.selection))
    {
        return false;
    }
    if (!this->objectPreview_ || this->objectPreview_->revision != transaction.revision ||
        this->objectPreview_->id != transaction.id || this->committed_ != transaction.baseline)
        return false;
    this->preview_ = std::move(next);
    this->objectPreview_->valid = true;
    return true;
}

// 统一结束属性事务，取消与同值不触碰历史，确认将临时预览原子提交一次。
// 入参：selection 为当前正式裁剪；accept 表示用户确认。
// 返回：成功提交、无变化或失败；失败恢复已提交预览和原有 redo。
AnnotationCommitResult CaptureAnnotationState::EndObjectPreview(RectI selection, bool accept) noexcept
{
    if (!this->objectPreview_)
        return accept ? AnnotationCommitResult::Failed : AnnotationCommitResult::Unchanged;
    if (!accept)
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    const ObjectPreviewTransaction& transaction = *this->objectPreview_;
    if (!transaction.valid || transaction.revision != this->revision_ || transaction.baseline != this->committed_ ||
        selection.IsEmpty() || !this->preview_)
    {
        this->Cancel();
        return AnnotationCommitResult::Failed;
    }
    if (this->preview_ == this->committed_)
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    try
    {
        this->before_ = AnnotationEditSnapshot{selection, this->committed_};
        if (this->Commit(selection, this->preview_))
            return AnnotationCommitResult::Committed;
    }
    catch (...)
    {
    }
    this->Cancel();
    return AnnotationCommitResult::Failed;
}

// 创建零 ID 文字草稿，原位控件绘制正文，统一预览继续只显示原文档。
// 入参：point：选区内落点；selection：开始裁剪。
// 返回：文字工具空闲且准备成功为 true，失败回滚。
bool CaptureAnnotationState::BeginText(PointI point, RectI selection) noexcept
try
{
    if (this->Active() || this->tool_ != CaptureAnnotationTool::Text || selection.IsEmpty() ||
        point.x < selection.left || point.x >= selection.right || point.y < selection.top ||
        point.y >= selection.bottom || this->Style().transparency == 100U)
        return false;
    std::shared_ptr<const std::u16string> text;
    if (!NormalizeAnnotationText({}, text))
        return false;
    AnnotationObject object;
    object.kind = AnnotationKind::Text;
    object.origin = {static_cast<double>(point.x), static_cast<double>(point.y)};
    object.style = this->Style();
    object.payload = AnnotationText{text, static_cast<double>(this->textFontSize_)};
    if (!this->FitsActiveBudget(this->committed_, {},
                                sizeof(AnnotationObject) + sizeof(std::u16string) +
                                    (text->capacity() + 1U) * sizeof(char16_t)))
        return false;
    this->before_ = AnnotationEditSnapshot{selection, this->committed_};
    this->textDraft_ = object;
    this->textRevision_ = this->revision_;
    this->inputValid_ = true;
    if (!this->PreparePreview(this->committed_, selection) || !this->textDraft_)
    {
        this->Cancel();
        return false;
    }
    this->preview_ = this->committed_;
    return true;
}
catch (...)
{
    this->Cancel();
    return false;
}

// 固定已有文字和旧擦痕，并用纯文档操作从临时预览排除目标元素。
// 入参：id：文字目标；revision：期望修订；selection：正式裁剪。
// 返回：有效且预览准备成功为 true；失败不开始事务。
bool CaptureAnnotationState::BeginTextEdit(std::uint64_t id, std::uint64_t revision, RectI selection) noexcept
try
{
    if (this->Active() || revision != this->revision_ || selection.IsEmpty())
        return false;
    const AnnotationObject* object = FindAnnotation(this->committed_, id);
    if (!object || object->kind != AnnotationKind::Text || !IsValidAnnotation(*object))
        return false;
    AnnotationObject candidate = *object;
    AnnotationText& payload = std::get<AnnotationText>(candidate.payload);
    std::shared_ptr<const std::u16string> normalized;
    if (!NormalizeAnnotationText(*payload.text, normalized))
        return false;
    if (*normalized != *payload.text)
        payload.text = std::move(normalized);
    AnnotationSnapshot hidden;
    if (!HideAnnotationForPreview(this->committed_, id, hidden) || !this->FitsActiveBudget(hidden))
        return false;
    this->before_ = AnnotationEditSnapshot{selection, this->committed_};
    this->textDraft_ = std::move(candidate);
    this->textRevision_ = revision;
    this->inputValid_ = true;
    if (!this->PreparePreview(hidden, selection) || !this->textDraft_)
    {
        this->Cancel();
        return false;
    }
    this->preview_ = std::move(hidden);
    return true;
}
catch (...)
{
    this->Cancel();
    return false;
}

// 规范化完整原位文本，不以无效输入回退提交上一次有效候选。
// 入参：text：原始 UTF-16，空白允许暂存。
// 返回：有效且预算允许为 true，否则保留旧草稿并禁止确认。
bool CaptureAnnotationState::UpdateText(std::u16string_view text) noexcept
try
{
    if (!this->textDraft_ || !this->before_ || this->textRevision_ != this->revision_)
        return false;
    this->inputValid_ = false;
    std::shared_ptr<const std::u16string> normalized;
    if (!NormalizeAnnotationText(text, normalized))
        return false;
    AnnotationObject candidate = *this->textDraft_;
    AnnotationText& payload = std::get<AnnotationText>(candidate.payload);
    if (!payload.text || *payload.text != *normalized)
        payload.text = normalized;
    if (!IsBlankAnnotationText(*payload.text))
    {
        AnnotationSnapshot document;
        const bool built = candidate.id == 0U
                               ? AppendAnnotation(this->committed_, candidate, document)
                               : ReplaceAnnotationText(this->committed_, candidate.id, *payload.text, document);
        if (!built || !this->FitsActiveBudget(document))
            return false;
    }
    else if (!this->FitsActiveBudget(this->preview_, {},
                                     sizeof(AnnotationObject) + sizeof(std::u16string) +
                                         (payload.text->capacity() + 1U) * sizeof(char16_t)))
        return false;
    this->textDraft_ = std::move(candidate);
    this->inputValid_ = true;
    return true;
}
catch (...)
{
    this->inputValid_ = false;
    return false;
}

// 检查唯一文字草稿的修订、有效输入及对象数量边界。
// 入参：无。
// 返回：非空白有效候选且存在提交资格为 true。
bool CaptureAnnotationState::CanCommitText() const noexcept
{
    if (!this->textDraft_ || !this->before_ || !this->inputValid_ || this->textRevision_ != this->revision_ ||
        !IsValidAnnotation(*this->textDraft_))
        return false;
    return this->textDraft_->id != 0U || ((!this->committed_ || this->committed_->size() < MAX_OBJECTS) &&
                                          this->nextId_ != std::numeric_limits<std::uint64_t>::max());
}

// 结束文字编辑，合法空白新建不产生对象，已有空白及失败候选继续保持编辑。
// 入参：accept：确认意图；取消总是恢复开始文档。
// 返回：提交、无变化或失败；失败不破坏原文档和历史。
AnnotationCommitResult CaptureAnnotationState::EndText(bool accept) noexcept
try
{
    if (!this->textDraft_ || !this->before_)
        return accept ? AnnotationCommitResult::Failed : AnnotationCommitResult::Unchanged;
    if (!accept)
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    if (!this->inputValid_ || this->textRevision_ != this->revision_)
        return AnnotationCommitResult::Failed;
    const AnnotationText& payload = std::get<AnnotationText>(this->textDraft_->payload);
    if (this->textDraft_->id == 0U && payload.text && IsBlankAnnotationText(*payload.text))
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    if (!this->CanCommitText())
        return AnnotationCommitResult::Failed;
    const bool creating = this->textDraft_->id == 0U;
    AnnotationSnapshot next;
    if (creating)
    {
        AnnotationObject object = *this->textDraft_;
        object.id = this->nextId_;
        if (!AppendAnnotation(this->committed_, object, next))
            return AnnotationCommitResult::Failed;
    }
    else if (!ReplaceAnnotationText(this->committed_, this->textDraft_->id, *payload.text, next))
        return AnnotationCommitResult::Failed;
    if (next == this->committed_)
    {
        this->Cancel();
        return AnnotationCommitResult::Unchanged;
    }
    if (!this->Commit(this->before_->selection, next))
        return AnnotationCommitResult::Failed;
    if (creating)
        ++this->nextId_;
    return AnnotationCommitResult::Committed;
}
catch (...)
{
    return AnnotationCommitResult::Failed;
}
// 取消未提交操作。
// 入参：无。返回：无。
void CaptureAnnotationState::Cancel() noexcept
{
    this->before_.reset();
    this->draft_.reset();
    this->textDraft_.reset();
    this->inputValid_ = true;
    this->eraseDraft_.reset();
    this->eraseTargets_.clear();
    this->eraseQueryStarted_ = false;
    this->objectPreview_.reset();
    this->preview_ = this->committed_;
}
// 清理选区文档并回到选择工具。
// 入参：无。返回：无。
void CaptureAnnotationState::Clear() noexcept
{
    this->Cancel();
    this->committed_.reset();
    this->preview_.reset();
    this->undo_.clear();
    this->redo_.clear();
    this->tool_ = CaptureAnnotationTool::Select;
    if (this->revision_ != std::numeric_limits<std::uint64_t>::max())
        ++this->revision_;
}
// 原子撤销或重做对象与选区，分配失败不改变栈。
// 入参：redo 为方向，current 为当前矩形，restored 接收恢复矩形。返回：成功时 true。
bool CaptureAnnotationState::Restore(bool redo, RectI current, RectI& restored) noexcept
try
{
    if (this->Active() || (redo ? this->redo_.empty() : this->undo_.empty()) ||
        this->revision_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    std::deque<AnnotationEditSnapshot> source = redo ? this->redo_ : this->undo_;
    std::deque<AnnotationEditSnapshot> destination = redo ? this->undo_ : this->redo_;
    const AnnotationEditSnapshot target = source.back();
    if (!this->PreparePreview(target.objects, target.selection))
        return false;
    destination.push_back({current, this->committed_});
    source.pop_back();
    (redo ? this->redo_ : this->undo_).swap(source);
    (redo ? this->undo_ : this->redo_).swap(destination);
    this->committed_ = target.objects;
    this->preview_ = target.objects;
    restored = target.selection;
    ++this->revision_;
    return true;
}
catch (...)
{
    return false;
}
} // namespace open_st
