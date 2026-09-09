// 声明可注入的截图完成步骤、结果类型及复制保存防重入接口。

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
    // 从有效冻结帧及稳定选区生成后续输出所需的图像。
    // 入参：无显式参数；由宿主回调捕获选区、冻结帧和结果存储。
    // 返回：图像生成成功 true，转换或会话有效性检查失败 false。
    std::function<bool()> generate;
    // 把已经生成的选区图像发布到剪贴板。
    // 入参：无显式参数；由宿主回调借用生成的图像和所属窗口。
    // 返回：剪贴板发布成功 true，失败 false。
    std::function<bool()> copy;
    // 让用户确认保存目标和编码格式。
    // 入参：无显式参数；由宿主回调保存所选目标。
    // 返回：确认 Accepted，取消 Cancelled，对话框失败 Failed。
    std::function<SaveChoice()> chooseSave;
    // 将生成的选区图像写入已确认的目标文件。
    // 入参：无显式参数；由宿主回调借用图像、目标及诊断输出。
    // 返回：文件保存成功 true，失败 false。
    std::function<bool()> save;
    // 在文件保存成功后记录下次保存可复用的目录。
    // 入参：无显式参数；由宿主回调借用已保存文件的目标路径。
    // 返回：目录记录成功 true；false 或异常转为目录告警，不撤销已保存文件。
    std::function<bool()> rememberDirectory;
};
class CaptureCompletion final
{
  public:
    // 按生成图像、发布剪贴板的顺序完成截图复制。
    // 入参：ready：选区是否已稳定且允许输出；actions：必须提供有效 generate 和 copy 同步回调。
    // 返回：未就绪或忙时 Ignored；生成失败 ConversionFailed；复制成功 Copied，否则 CopyFailed。
    [[nodiscard]] CompletionResult CopySelection(bool ready, const CompletionActions& actions);
    // 按选择路径、生成图像、写文件和记录目录的顺序完成截图保存。
    // 入参：ready：是否允许输出；actions：有效 chooseSave、generate、save、rememberDirectory 同步回调。
    // 返回：返回取消、忽略、转换失败或保存失败状态；成功为 Saved，目录记录失败为 SavedDirectoryWarning。
    [[nodiscard]] CompletionResult SaveSelection(bool ready, const CompletionActions& actions);
    // 查询截图完成协调器是否正在执行同步业务步骤。
    // 入参：无。
    // 返回：同步完成流程尚未结束时为 true，否则 false。
    [[nodiscard]] bool IsBusy() const noexcept;

  private:
    bool busy_{};
};
} // namespace open_st
