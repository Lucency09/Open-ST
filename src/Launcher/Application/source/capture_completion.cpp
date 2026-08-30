#include "capture_completion.h"

namespace
{
// 在异常和正常返回路径统一恢复命令入口，阻止模态消息重入。
class BusyGuard final
{
  public:
    // 标记借用状态为忙。
    explicit BusyGuard(bool& busy) noexcept : busy_(busy)
    {
        this->busy_ = true;
    }
    // 离开同步流程后恢复可用状态。
    ~BusyGuard()
    {
        this->busy_ = false;
    }
    // 禁止复制守卫。
    BusyGuard(const BusyGuard&) = delete;
    // 禁止复制赋值。
    BusyGuard& operator=(const BusyGuard&) = delete;

  private:
    bool& busy_;
};
} // namespace
namespace open_st
{
// 生成成功才触及剪贴板；忙状态覆盖整个系统调用。
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
// 先确认保存路径，取消不会浪费转换资源；保存成功与设置失败独立返回。
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
// 供应用消息路由查询状态，不泄漏回调内容。
bool CaptureCompletion::IsBusy() const noexcept
{
    return this->busy_;
}
} // namespace open_st
