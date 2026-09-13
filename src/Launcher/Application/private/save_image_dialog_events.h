// 声明保存对话框最终文件名规则与可注入确认边界，供原窗口事件和独立测试共同使用。

#pragma once
#include "save_image_dialog.h"
#include <functional>

namespace open_st
{
// 保存首个事件错误；关闭窗口只是尽力操作，不能把关闭失败变成保存许可。
class SaveImageDialogEventState final
{
  public:
    // 保存事件错误并阻止确认，已失败后即使后续校验成功也继续拒绝。
    // 入参：result：本次校验结果；close：接收首个错误并尝试关闭窗口的同步回调。
    // 返回：无错误时保留 S_OK/S_FALSE；已有错误时始终返回 S_FALSE，关闭失败或异常不清除错误。
    template <typename CloseAction> HRESULT CheckFileOk(HRESULT result, CloseAction&& close) noexcept
    {
        if (SUCCEEDED(this->error_) && FAILED(result))
        {
            this->error_ = result;
        }
        if (SUCCEEDED(this->error_))
        {
            return result;
        }
        try
        {
            (void)close(this->error_);
        }
        catch (...)
        {
            // COM 关闭正常以 HRESULT 报错；替身或边界异常也不能丢失原始事件失败。
        }
        return S_FALSE;
    }
    // 对最终显示结果优先应用已发生的事件错误。
    // 入参：result：系统 Show 的返回值，可能成功、取消或失败。
    // 返回：首个事件错误优先，否则返回系统结果；调用方须在读取或发布目标前检查。
    [[nodiscard]] HRESULT ShowResult(HRESULT result) const noexcept
    {
        return FAILED(this->error_) ? this->error_ : result;
    }
    // 区分事件失败、系统取消和成功；即使事件错误恰为取消码也仍报告事件失败。
    // 入参：result：系统 Show 的返回值。
    // 返回：存在事件错误为 Failed；无事件错误时遵循系统成功、取消、失败结果。
    [[nodiscard]] SaveChoice CompleteShow(HRESULT result) const noexcept
    {
        if (FAILED(this->error_))
            return SaveChoice::Failed;
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            return SaveChoice::Cancelled;
        return FAILED(result) ? SaveChoice::Failed : SaveChoice::Accepted;
    }

  private:
    HRESULT error_{S_OK};
};

// 取得所选格式对应的默认后缀，不含点。
// 入参：format：PNG 或 JPEG；未知格式返回空串。
// 返回：静态字符串借用指针，JPEG 为 jpg、PNG 为 png。
[[nodiscard]] const wchar_t* SaveImageDefaultExtension(ImageFileFormat format) noexcept;
// 规范最终路径后缀，保留目录和文件主体，JPEG 接受大小写不敏感的 jpg/jpeg。
// 入参：path：系统对话框返回的候选路径；format：本次选定的格式。
// 返回：符合所选格式的独立路径；未知格式抛出 invalid_argument，调用方负责捕获。
[[nodiscard]] std::filesystem::path NormalizeSaveImagePath(const std::filesystem::path& path, ImageFileFormat format);
// 在原保存窗口内拒绝不匹配的后缀，确保修改后的路径必须再次经过系统保存及覆盖确认。
// 入参：target：系统当前候选；setFilename：同步更新原窗口文件名的回调；notify：更新成功后的原因提示回调。
// 返回：原路径合法为 S_OK；已修正为 S_FALSE；修正失败返回错误 HRESULT，不通知成功、不允许退出窗口。
[[nodiscard]] HRESULT CheckSaveImageFileOk(const SaveImageTarget& target,
                                           const std::function<HRESULT(const std::wstring&)>& setFilename,
                                           const std::function<void(const std::wstring&)>& notify) noexcept;
} // namespace open_st
