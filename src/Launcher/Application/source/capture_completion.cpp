// 组织截图复制与保存的同步步骤，并保证忙状态在取消或异常后恢复。

#include "capture_completion.h"

namespace
{
// 在异常和正常返回路径统一恢复命令入口，阻止模态消息重入。
class BusyGuard final
{
  public:
    // 在同步截图完成流程内设置忙标记以拒绝重入。
    // 入参：busy：借用的忙标记引用，必须比守卫存活更久。
    // 返回：构造函数无返回值；busy 被设为 true。
    explicit BusyGuard(bool& busy) noexcept : busy_(busy)
    {
        this->busy_ = true;
    }
    // 在完成流程退出时恢复非忙状态。
    // 入参：无。
    // 返回：析构函数无返回值；将借用的 busy 设为 false。
    ~BusyGuard()
    {
        this->busy_ = false;
    }
    // 禁止复制构造，确保忙状态守卫只由原对象管理。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    BusyGuard(const BusyGuard&) = delete;
    // 禁止复制赋值，避免忙状态守卫出现多个所有者。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    BusyGuard& operator=(const BusyGuard&) = delete;

  private:
    bool& busy_;
};
} // namespace
namespace open_st
{
// 按生成图像、发布剪贴板的顺序完成截图复制。
// 入参：ready：选区是否已稳定且允许输出；actions：必须提供有效 generate 和 copy 同步回调。
// 返回：未就绪或忙时 Ignored；生成失败 ConversionFailed；复制成功 Copied，否则 CopyFailed。
CompletionResult CaptureCompletion::CopySelection(bool ready, const CompletionActions& actions)
{
    if (!ready || this->busy_)
    {
        return CompletionResult::Ignored;
    }
    BusyGuard guard(this->busy_);
    if (!actions.generate())
    {
        return CompletionResult::ConversionFailed;
    }
    return actions.copy() ? CompletionResult::Copied : CompletionResult::CopyFailed;
}
// 按选择路径、生成图像、写文件和记录目录的顺序完成截图保存。
// 入参：ready：是否允许输出；actions：有效 chooseSave、generate、save、rememberDirectory 同步回调。
// 返回：返回取消、忽略、转换失败或保存失败状态；成功为 Saved，目录记录失败为 SavedDirectoryWarning。
CompletionResult CaptureCompletion::SaveSelection(bool ready, const CompletionActions& actions)
{
    if (!ready || this->busy_)
    {
        return CompletionResult::Ignored;
    }
    BusyGuard guard(this->busy_);
    const SaveChoice choice = actions.chooseSave();
    if (choice == SaveChoice::Cancelled)
    {
        return CompletionResult::Cancelled;
    }
    if (choice == SaveChoice::Failed)
    {
        return CompletionResult::SaveFailed;
    }
    if (!actions.generate())
    {
        return CompletionResult::ConversionFailed;
    }
    if (!actions.save())
    {
        return CompletionResult::SaveFailed;
    }
    try
    {
        return actions.rememberDirectory() ? CompletionResult::Saved : CompletionResult::SavedDirectoryWarning;
    }
    catch (...)
    {
        // 文件已经写入成功；路径转换或设置回调异常不能把保存误报为失败。
        return CompletionResult::SavedDirectoryWarning;
    }
}
// 查询截图完成协调器是否正在执行同步业务步骤。
// 入参：无。
// 返回：同步完成流程尚未结束时为 true，否则 false。
bool CaptureCompletion::IsBusy() const noexcept
{
    return this->busy_;
}
} // namespace open_st
