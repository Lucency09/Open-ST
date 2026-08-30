#pragma once
#include <functional>

namespace open_st
{
enum class CompletionResult
{
    Ignored,
    Cancelled,
    ConversionFailed,
    CopyFailed,
    SaveFailed,
    Copied,
    Saved,
    SavedDirectoryWarning
};
enum class SaveChoice
{
    Accepted,
    Cancelled,
    Failed
};
// 注入同步业务步骤，使快捷键与后续菜单共用同一流程，并可隔离系统边界测试。
struct CompletionActions final
{
    std::function<bool()> generate;
    std::function<bool()> copy;
    std::function<SaveChoice()> chooseSave;
    std::function<bool()> save;
    std::function<bool()> rememberDirectory;
};
class CaptureCompletion final
{
  public:
    // 非稳定选区或重复命令不执行任何步骤；生成或复制失败可重试。
    [[nodiscard]] CompletionResult CopySelection(bool ready, const CompletionActions& actions);
    // 取消不生成图像；文件已保存时，目录记录失败仍视为保存成功。
    [[nodiscard]] CompletionResult SaveSelection(bool ready, const CompletionActions& actions);
    // 返回是否正在执行含模态回调的同步操作。
    [[nodiscard]] bool IsBusy() const noexcept;

  private:
    bool busy_{};
};
} // namespace open_st
