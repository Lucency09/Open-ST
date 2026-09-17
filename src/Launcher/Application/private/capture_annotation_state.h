// 管理截图标注的工具默认值、绘制草稿及与选区一起撤销的内存历史。
#pragma once
#include <annotation.h>
#include <array>
#include <deque>
#include <functional>
#include <geometry.h>
#include <optional>
#include <selection_model.h>

namespace open_st
{
inline constexpr std::array<unsigned, 4> CAPTURE_ANNOTATION_ERASER_DIAMETERS{8U, 16U, 32U, 64U};

enum class CaptureAnnotationTool
{
    Select,
    Rectangle,
    Arrow,
    FilledRectangle,
    RoundedRectangle,
    Pen,
    Line,
    Ellipse,
    Eraser,
    Text,
    Mosaic
};
enum class AnnotationCommitResult
{
    Unchanged,
    Committed,
    Failed
};
struct AnnotationEditSnapshot
{
    RectI selection{};
    AnnotationSnapshot objects;
};

class CaptureAnnotationState final
{
  public:
    // 查询当前工具。
    // 入参：无。返回：工具值。
    CaptureAnnotationTool Tool() const noexcept
    {
        return this->tool_;
    }
    // 切换空闲工具，重复选择保持状态。
    // 入参：tool 为新工具。返回：无。
    void SetTool(CaptureAnnotationTool tool) noexcept
    {
        if (!this->Active())
            this->tool_ = tool;
    }
    // 查询当前工具组的默认样式。
    // 入参：无。返回：样式值副本。
    AnnotationStyle Style() const noexcept;
    // 保存当前工具组的有效默认值，不修改已有对象。
    // 入参：style 为候选。返回：接受时 true。
    bool SetStyle(AnnotationStyle style) noexcept;
    // 查询当前会话的橡皮直径，与绘图线宽和颜色默认值分离。
    // 入参：无。
    // 返回：8、16、32 或 64 个物理像素。
    unsigned EraserDiameter() const noexcept
    {
        return this->eraserDiameter_;
    }
    // 设置橡皮直径，不修改已有对象与历史。
    // 入参：diameter：物理像素直径，仅允许 8、16、32、64。
    // 返回：空闲且参数有效时为 true。
    bool SetEraserDiameter(unsigned diameter) noexcept;
    // 查询文字工具默认字号。
    // 入参：无。
    // 返回：物理像素字号，初始为 24。
    unsigned TextFontSize() const noexcept
    {
        return this->textFontSize_;
    }
    // 在空闲时设置合法默认字号，不改变已有对象。
    // 入参：fontSize：12、16、20、24、32 或 48。
    // 返回：接受为 true。
    bool SetTextFontSize(unsigned fontSize) noexcept;
    // 查询马赛克默认块大小。
    // 入参：无。
    // 返回：物理像素边长，初始为 16。
    unsigned MosaicBlockSize() const noexcept
    {
        return this->mosaicBlockSize_;
    }
    // 在空闲时设置合法默认块大小。
    // 入参：blockSize：4、8、16 或 32。
    // 返回：接受为 true。
    bool SetMosaicBlockSize(unsigned blockSize) noexcept;
    // 注入可选同步候选准备，仅含马赛克时调用，回调不可保留状态对象借用。
    // 入参：preparation：接收候选及真实目标裁剪的来源准备回调，false 或异常表示不能发布。
    // 返回：状态空闲且回调替换成功为 true。
    bool SetPreviewPreparation(std::function<bool(const AnnotationSnapshot&, RectI)> preparation) noexcept;
    // 判断是否有未提交操作。
    // 入参：无。返回：绘制、选区或属性预览事务存在时 true。
    bool Active() const noexcept
    {
        return this->before_.has_value() || this->objectPreview_.has_value();
    }
    // 判断当前操作是否是绘制而非选区变更。
    // 入参：无。返回：绘制草稿存在时 true。
    bool Drawing() const noexcept
    {
        return this->draft_.has_value() || this->eraseDraft_ != nullptr;
    }
    // 查询当前事务开始前选区，供提交失败恢复。
    // 入参：无。返回：无事务时为空矩形。
    RectI BeforeSelection() const noexcept
    {
        return this->before_ ? this->before_->selection : RectI{};
    }
    // 获取本次预览的不可变文档。
    // 入参：无。返回：共享只读快照，可空表示无标注。
    AnnotationSnapshot Preview() const noexcept
    {
        return this->preview_;
    }
    // 获取已提交文档，供输出固定修订。
    // 入参：无。返回：共享只读快照。
    AnnotationSnapshot Committed() const noexcept
    {
        return this->committed_;
    }
    // 查询已提交文档修订，撤销后也不退回旧编号。
    // 入参：无。返回：当前单调修订。
    std::uint64_t Revision() const noexcept
    {
        return this->revision_;
    }
    // 固定通用对象参数事务的目标和修订。
    // 入参：id：稳定 ID；revision：预期修订；selection：马赛克准备必需的正式裁剪。
    // 返回：空闲且有效时为 true。
    bool BeginObjectPreview(std::uint64_t id, std::uint64_t revision, RectI selection = {}) noexcept;
    // 校验、准备并发布通用参数候选。
    // 入参：properties：种类匹配的参数。
    // 返回：成功为 true；失败保留旧预览并禁止确认，可继续输入恢复。
    bool UpdateObjectPreview(const AnnotationProperties& properties) noexcept;
    // 结束通用参数事务，成功确认一步提交。
    // 入参：selection：当前选区；accept：确认或取消。
    // 返回：提交、无变化或失败。
    AnnotationCommitResult EndObjectPreview(RectI selection, bool accept) noexcept;
    // 判断通用对象参数事务是否存在。
    // 入参：无。
    // 返回：有事务为 true。
    bool PreviewingObject() const noexcept
    {
        return this->objectPreview_.has_value();
    }
    // 开始新文字原位输入，固定选区、落点及当前默认参数。
    // 入参：point：选区内物理落点；selection：正式裁剪。
    // 返回：工具和状态允许时为 true。
    bool BeginText(PointI point, RectI selection) noexcept;
    // 在固定修订上重编辑已有文字，临时预览排除原元素。
    // 入参：id：文字 ID；revision：文档修订；selection：正式裁剪。
    // 返回：准备成功为 true。
    bool BeginTextEdit(std::uint64_t id, std::uint64_t revision, RectI selection) noexcept;
    // 规范化并更新唯一正文草稿，非法输入阻止提交旧候选。
    // 入参：text：原始 UTF-16 正文。
    // 返回：合法且预算可用为 true，空白允许保留；失败保留上次有效草稿。
    bool UpdateText(std::u16string_view text) noexcept;
    // 查询是否处于原位正文事务。
    // 入参：无。
    // 返回：文字草稿存在为 true，不计入 Drawing()。
    bool EditingText() const noexcept
    {
        return this->textDraft_.has_value();
    }
    // 提供原位输入初始化使用的共享正文及参数值副本。
    // 入参：无。
    // 返回：活动草稿，零 ID 表示新建；非文字事务返回空。
    std::optional<AnnotationObject> TextDraft() const noexcept
    {
        return this->textDraft_;
    }
    // 检查当前文字候选是否有效、非空白且可提交。
    // 入参：无。
    // 返回：允许正文提交为 true。
    bool CanCommitText() const noexcept;
    // 确认或取消文字；已有空白和失败候选保留编辑，合法空白新建不产生对象。
    // 入参：accept：是否确认。
    // 返回：一次提交、无变化或失败；失败保留原文档和可继续的事务。
    AnnotationCommitResult EndText(bool accept) noexcept;
    // 在选区内开始一笔，固定工具、颜色和透明度。
    // 入参：point 为物理坐标，selection 为当前裁剪。返回：接受手势时 true。
    bool BeginDraw(PointI point, RectI selection) noexcept;
    // 更新草稿，不记录历史。
    // 入参：point 为当前物理坐标。返回：更新成功为 true，失败保持原文档。
    bool UpdateDraw(PointI point) noexcept;
    // 结束一笔并原子提交。
    // 入参：point 为最终坐标。返回：提交、零面积不变或失败。
    AnnotationCommitResult EndDraw(PointI point) noexcept;
    // 记录选区操作的起始状态。
    // 入参：selection 为开始前矩形。返回：无。
    void BeginCrop(RectI selection) noexcept;
    // 标注随选区移动；缩放只改变裁剪。
    // 入参：selection 为新矩形，moving 区分整体移动。返回：更新成功时 true。
    bool UpdateCrop(RectI selection, bool moving) noexcept;
    // 提交已完成的选区变更到统一历史。
    // 入参：selection 为最终矩形。返回：提交结果。
    AnnotationCommitResult EndCrop(RectI selection) noexcept;
    // 回滚未提交标注，不改变已有历史。
    // 入参：无。返回：无。
    void Cancel() noexcept;
    // 清除选区相关文档与历史，保留本会话默认颜色。
    // 入参：无。返回：无。
    void Clear() noexcept;
    // 查询是否允许撤销。
    // 入参：无。返回：存在上一已提交状态时 true。
    bool CanUndo() const noexcept
    {
        return !this->Active() && !this->undo_.empty();
    }
    // 查询是否允许重做。
    // 入参：无。返回：存在下一状态时 true。
    bool CanRedo() const noexcept
    {
        return !this->Active() && !this->redo_.empty();
    }
    // 原子切换历史，同时返回要恢复的截图矩形。
    // 入参：redo 为方向，current 为当前矩形，restored 为成功输出。返回：成功时 true。
    bool Restore(bool redo, RectI current, RectI& restored) noexcept;

  private:
    // 判断工具是否使用填充默认值。
    // 入参：无。返回：直角或圆角填充时 true。
    bool IsFill() const noexcept;
    // 在临时历史上检查预算并提交，不破坏失败前的 redo。
    // 入参：selection 为提交后的裁剪，next 为文档。返回：成功时 true。
    bool Commit(RectI selection, AnnotationSnapshot next);
    // 单独检查活动草稿的深层共享数据预算，不通过删历史放宽草稿上限。
    // 入参：next：候选预览；erase：活动擦除手势；additionalBytes：增量命中缓存等独立动态存储。
    // 返回：基线和草稿共同不超过 32 MiB 时为 true。
    bool FitsActiveBudget(const AnnotationSnapshot& next,
                          const std::shared_ptr<const AnnotationEraseStroke>& erase = {},
                          std::size_t additionalBytes = 0U) const noexcept;
    // 仅查询新增线段并累计受影响对象，完整轨迹仍从开始文档一次生成候选掩码。
    // 入参：point：桌面物理像素位置。
    // 返回：全部成功为 true；同点复用预览；失败取消整笔并清理增量缓存。
    bool UpdateErase(PointI point) noexcept;
    // 对含马赛克的候选执行同步准备，并捕获异常。
    // 入参：next：拟发布快照；selection：候选对应的真实目标裁剪。
    // 返回：不含马赛克或准备成功为 true。
    bool PreparePreview(const AnnotationSnapshot& next, RectI selection) noexcept;
    CaptureAnnotationTool tool_{CaptureAnnotationTool::Select};
    AnnotationStyle stroke_{};
    AnnotationStyle fill_{0, 0, 3.0};
    unsigned eraserDiameter_{16U};
    unsigned textFontSize_{24U};
    unsigned mosaicBlockSize_{16U};
    std::function<bool(const AnnotationSnapshot&, RectI)> previewPreparation_;
    bool preparingPreview_{};
    bool inputValid_{true};
    AnnotationSnapshot committed_;
    AnnotationSnapshot preview_;
    std::optional<AnnotationEditSnapshot> before_;
    std::optional<AnnotationObject> draft_;
    std::optional<AnnotationObject> textDraft_;
    std::uint64_t textRevision_{};
    std::shared_ptr<const AnnotationEraseStroke> eraseDraft_;
    std::vector<std::uint64_t> eraseTargets_;
    bool eraseQueryStarted_{};
    struct ObjectPreviewTransaction
    {
        std::uint64_t id{};
        std::uint64_t revision{};
        AnnotationSnapshot baseline;
        bool valid{true};
        RectI selection{};
    };
    std::optional<ObjectPreviewTransaction> objectPreview_;
    std::deque<AnnotationEditSnapshot> undo_, redo_;
    std::uint64_t nextId_{1};
    std::uint64_t revision_{};
};
} // namespace open_st
