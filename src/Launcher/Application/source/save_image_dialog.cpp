// 配置并显示系统图片保存对话框，返回用户选择的路径与编码格式。

#include "save_image_dialog.h"
#include "save_image_dialog_events.h"
#include <format>
#include <memory>
#include <shobjidl.h>
#include <stdexcept>
#include <ui_text.h>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace
{
// 释放 Shell 以 COM task allocator 返回的路径字符串。
struct TaskMemoryDeleter final
{
    // 释放 Shell 返回的 COM 任务分配字符串。
    // 入参：value：GetDisplayName 返回的字符串指针，可为 nullptr。
    // 返回：无返回值；调用 CoTaskMemFree，释放后不能继续使用 value。
    void operator()(wchar_t* value) const noexcept
    {
        CoTaskMemFree(value);
    }
};
} // namespace
namespace open_st
{
// 取得保存格式的标准后缀，供初始名称、类型切换和最终规范化共同使用。
// 入参：format：待保存的 PNG 或 JPEG 格式。
// 返回：静态后缀字符串；未知格式返回空串，不猜测目标编码格式。
const wchar_t* SaveImageDefaultExtension(ImageFileFormat format) noexcept
{
    switch (format)
    {
    case ImageFileFormat::Jpeg:
        return L"jpg";
    case ImageFileFormat::Png:
        return L"png";
    default:
        return L"";
    }
}

// 只替换最后一个不匹配的扩展名，不给用户文件名追加双重后缀。
// 入参：path：包含目录的候选路径；format：明确的本次编码格式。
// 返回：规范后的独立路径，非法格式抛出 invalid_argument。
std::filesystem::path NormalizeSaveImagePath(const std::filesystem::path& path, ImageFileFormat format)
{
    const wchar_t* defaultExtension = SaveImageDefaultExtension(format);
    if (*defaultExtension == L'\0')
    {
        throw std::invalid_argument("Unsupported save dialog format");
    }
    const std::wstring extension = path.extension().wstring();
    const bool matches = format == ImageFileFormat::Jpeg
                             ? (_wcsicmp(extension.c_str(), L".jpg") == 0 || _wcsicmp(extension.c_str(), L".jpeg") == 0)
                             : _wcsicmp(extension.c_str(), L".png") == 0;
    if (matches)
    {
        return path;
    }
    std::filesystem::path corrected = path;
    corrected.replace_extension(std::wstring(L".") + defaultExtension);
    return corrected;
}

// 修正动作发生于原对话框确认事件，返回 S_FALSE 要求用户重新确认最终路径。
// 入参：target：当前系统候选；setFilename：原窗口文件名更新；notify：修正原因提示。
// 返回：无需改动为 S_OK；修正成功为 S_FALSE；异常或更新失败为失败 HRESULT。
HRESULT CheckSaveImageFileOk(const SaveImageTarget& target,
                             const std::function<HRESULT(const std::wstring&)>& setFilename,
                             const std::function<void(const std::wstring&)>& notify) noexcept
{
    try
    {
        const std::filesystem::path corrected = NormalizeSaveImagePath(target.path, target.format);
        if (corrected == target.path)
        {
            return S_OK;
        }
        // 保留完整目录，避免用户输入的子目录或绝对路径在第二次确认时丢失。
        const HRESULT result = setFilename(corrected.wstring());
        if (FAILED(result))
        {
            return result;
        }
        notify(std::wstring(L".") + SaveImageDefaultExtension(target.format));
        return S_FALSE;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

namespace
{
// 从系统已选择结果读取完整文件路径和格式，不在返回后修改用户确认过的目标。
// 入参：dialog：调用期间借用的保存对话框；target：读取成功后发布的候选目标。
// 返回：完整有效为 S_OK；系统调用、类型或分配失败返回失败 HRESULT 并保留 target。
HRESULT ReadSaveImageTarget(IFileDialog* dialog, SaveImageTarget& target) noexcept
{
    try
    {
        Microsoft::WRL::ComPtr<IShellItem> item;
        UINT index{};
        HRESULT result = dialog->GetResult(item.GetAddressOf());
        if (FAILED(result))
        {
            return result;
        }
        result = dialog->GetFileTypeIndex(&index);
        if (FAILED(result) || (index != 1U && index != 2U))
        {
            return FAILED(result) ? result : E_INVALIDARG;
        }
        PWSTR rawPath{};
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        const std::unique_ptr<wchar_t, TaskMemoryDeleter> path(rawPath);
        if (FAILED(result) || !path || *path == L'\0')
        {
            return FAILED(result) ? result : E_INVALIDARG;
        }
        SaveImageTarget candidate{path.get(), index == 2U ? ImageFileFormat::Jpeg : ImageFileFormat::Png};
        target = std::move(candidate);
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

// COM 事件对象不持有窗口引用，订阅由外层守卫释放；所有异常都截留于 ABI 边界。
class SaveImageDialogEvents final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IFileDialogEvents>
{
  public:
    // 校验当前候选后缀并在同一窗口中修正，错误时关闭为失败而不是接受旧路径。
    // 入参：dialog：系统借用的当前文件对话框。
    // 返回：允许确认 S_OK；修正或事件失败均返回 S_FALSE，失败另存于事件状态。
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override
    {
        // 尽力关闭原窗口；首个错误先由状态保存，不依赖 Close 是否成功。
        // 入参：error：已保存的首个事件失败 HRESULT。
        // 返回：系统 Close 结果，不能改变确认拒绝语义。
        const auto close = [dialog](HRESULT error) { return dialog->Close(error); };
        if (FAILED(this->state_.ShowResult(S_OK)))
        {
            return this->state_.CheckFileOk(S_OK, close);
        }
        SaveImageTarget target;
        HRESULT result = ReadSaveImageTarget(dialog, target);
        if (SUCCEEDED(result))
        {
            try
            {
                // 将文件名更新留在原窗口；传完整路径使目录与主体同时保持。
                // 入参：filename：已经规范后缀的完整候选路径。
                // 返回：系统更新结果；失败不允许继续确认。
                const std::function<HRESULT(const std::wstring&)> setFilename = [dialog](const std::wstring& filename)
                { return dialog->SetFileName(filename.c_str()); };
                // 在保存窗口前解释修正，用户关闭提示后需再次点击保存。
                // 入参：extension：带点的标准后缀。
                // 返回：无；不替用户触发确认或覆盖。
                const std::function<void(const std::wstring&)> notify = [dialog](const std::wstring& extension)
                {
                    HWND window{};
                    Microsoft::WRL::ComPtr<IOleWindow> oleWindow;
                    if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&oleWindow))))
                    {
                        (void)oleWindow->GetWindow(&window);
                    }
                    const std::wstring message =
                        GetUiText("export.dialog.extension_corrected", {{L"extension", extension}});
                    const std::wstring title = GetUiText("export.dialog.title");
                    (void)MessageBoxW(window, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
                };
                result = CheckSaveImageFileOk(target, setFilename, notify);
            }
            catch (...)
            {
                result = E_FAIL;
            }
        }
        return this->state_.CheckFileOk(result, close);
    }
    // 放行文件夹导航，不改变系统选择行为。
    // 入参：dialog：当前窗口；folder：候选文件夹，均借用且不保存。
    // 返回：S_OK。
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override
    {
        return S_OK;
    }
    // 接受系统文件夹变更通知，不写上次目录。
    // 入参：dialog：系统当前窗口，借用且不保存。
    // 返回：S_OK。
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override
    {
        return S_OK;
    }
    // 接受选择变更通知，不修改任何保存设置。
    // 入参：dialog：系统当前窗口，借用且不保存。
    // 返回：S_OK。
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override
    {
        return S_OK;
    }
    // 共享冲突继续交给系统处理，避免替用户批准冲突目标。
    // 入参：dialog、item：系统借用对象；response：系统处理方式输出。
    // 返回：S_OK，response 为默认行为。
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*,
                                               FDE_SHAREVIOLATION_RESPONSE* response) override
    {
        *response = FDESVR_DEFAULT;
        return S_OK;
    }
    // 类型切换只更新当前系统默认后缀，不持久化设置页默认格式。
    // 入参：dialog：当前系统文件对话框。
    // 返回：更新成功为 S_OK；失败关闭窗口并返回原始 HRESULT。
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override
    {
        // 尝试关闭已失败窗口，关闭结果不覆盖首个事件错误。
        // 入参：error：状态保存的首个事件失败 HRESULT。
        // 返回：系统 Close 返回值。
        const auto close = [dialog](HRESULT error) { return dialog->Close(error); };
        if (FAILED(this->state_.ShowResult(S_OK)))
        {
            (void)this->state_.CheckFileOk(S_OK, close);
            return this->state_.ShowResult(S_OK);
        }
        UINT index{};
        HRESULT result = dialog->GetFileTypeIndex(&index);
        if (SUCCEEDED(result))
        {
            result =
                index == 1U || index == 2U ? dialog->SetDefaultExtension(index == 2U ? L"jpg" : L"png") : E_INVALIDARG;
        }
        (void)this->state_.CheckFileOk(result, close);
        return this->state_.ShowResult(result);
    }
    // 对系统最终结果施加事件错误，阻止系统意外成功或取消掩盖已发生的失败。
    // 入参：result：Show 返回的 HRESULT。
    // 返回：存在事件错误为 Failed，否则遵循系统的成功、取消、失败结果。
    SaveChoice CompleteShow(HRESULT result) const noexcept
    {
        return this->state_.CompleteShow(result);
    }
    // 覆盖许可完全交给系统，修正后的路径必须重新执行本事件及系统确认。
    // 入参：dialog、item：借用对象；response：系统覆盖处理方式输出。
    // 返回：S_OK，response 始终为默认覆盖询问，不自动接受。
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override
    {
        *response = FDEOR_DEFAULT;
        return S_OK;
    }

  private:
    SaveImageDialogEventState state_;
};

// 借用仍由外层 ComPtr 持有的对话框，在所有退出路径解除订阅。
class DialogSubscription final
{
  public:
    // 注册当前事件，保留明确的订阅结果以避免静默失去最终文件名校验。
    // 入参：dialog：覆盖本对象寿命的借用窗口；events：系统订阅后自行持有引用的事件对象。
    // 返回：无；注册结果由 Result 查询。
    DialogSubscription(IFileDialog* dialog, IFileDialogEvents* events) : dialog_(dialog)
    {
        this->result_ = this->dialog_->Advise(events, &this->cookie_);
    }
    // 在关闭、取消、错误或异常路径解除有效订阅。
    // 入参：无。
    // 返回：无；不拥有 dialog，不释放外层引用。
    ~DialogSubscription()
    {
        if (SUCCEEDED(this->result_))
        {
            (void)this->dialog_->Unadvise(this->cookie_);
        }
    }
    // 禁止复制订阅 cookie，防止重复解除。
    // 入参：源订阅对象。
    // 返回：无；编译期禁止。
    DialogSubscription(const DialogSubscription&) = delete;
    // 禁止复制赋值以维护单一订阅所有权。
    // 入参：源订阅对象。
    // 返回：无；编译期禁止。
    DialogSubscription& operator=(const DialogSubscription&) = delete;
    // 查询订阅是否成功，调用方必须在显示窗口前检查。
    // 入参：无。
    // 返回：Advise 的原始结果。
    HRESULT Result() const noexcept
    {
        return this->result_;
    }

  private:
    IFileDialog* dialog_{};
    DWORD cookie_{};
    HRESULT result_{E_FAIL};
};
} // namespace

// 让用户选择截图保存路径及 PNG 或 JPEG 编码格式。
// 入参：owner：借用的所属窗口；lastDirectory：上次目录，空或失效时使用系统默认；initialFormat：本次初始格式；target：成功时输出目标；errorMessage：失败诊断输出；调用线程须初始化
// STA COM。 返回：确认返回 Accepted；取消返回 Cancelled 且保留 target；系统失败返回 Failed 并写诊断。
SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path& lastDirectory, ImageFileFormat initialFormat,
                               SaveImageTarget& target, std::wstring& errorMessage)
{
    errorMessage.clear();
    const wchar_t* initialExtension = SaveImageDefaultExtension(initialFormat);
    if (*initialExtension == L'\0')
    {
        errorMessage = L"Unsupported initial save format";
        return SaveChoice::Failed;
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileSaveDialog> dialog;
    HRESULT result =
        CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result))
    {
        errorMessage = L"无法创建保存对话框。";
        return SaveChoice::Failed;
    }
    const std::wstring png = GetUiText("export.dialog.png");
    const std::wstring jpeg = GetUiText("export.dialog.jpeg");
    const std::wstring title = GetUiText("export.dialog.title");
    // 使用打开保存对话框时的本地时间；显式扩展名避免毫秒部分被系统误认为扩展名。
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const std::wstring filename = std::format(
        L"openst-{:02}-{:02}-{:02}_{:02}-{:02}-{:02}.{:03}.{}", localTime.wYear % 100, localTime.wMonth, localTime.wDay,
        localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds, initialExtension);
    const COMDLG_FILTERSPEC filters[]{{png.c_str(), L"*.png"}, {jpeg.c_str(), L"*.jpg;*.jpeg"}};
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST)) ||
        FAILED(dialog->SetFileTypes(2U, filters)) ||
        FAILED(dialog->SetFileTypeIndex(initialFormat == ImageFileFormat::Jpeg ? 2U : 1U)) ||
        FAILED(dialog->SetDefaultExtension(initialExtension)) || FAILED(dialog->SetTitle(title.c_str())) ||
        FAILED(dialog->SetFileName(filename.c_str())))
    {
        errorMessage = L"无法配置保存对话框。";
        return SaveChoice::Failed;
    }
    if (!lastDirectory.empty())
    {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(lastDirectory.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()))))
        {
            // 目录已移动或不可访问时让系统选择默认目录，不阻断保存。
            (void)dialog->SetFolder(folder.Get());
        }
    }
    const ComPtr<SaveImageDialogEvents> events = Microsoft::WRL::Make<SaveImageDialogEvents>();
    if (!events)
    {
        errorMessage = L"Cannot allocate save dialog events";
        return SaveChoice::Failed;
    }
    const DialogSubscription subscription(dialog.Get(), events.Get());
    if (FAILED(subscription.Result()))
    {
        errorMessage = L"Cannot subscribe save dialog events";
        return SaveChoice::Failed;
    }
    result = dialog->Show(owner);
    const SaveChoice choice = events->CompleteShow(result);
    if (choice != SaveChoice::Accepted)
    {
        if (choice == SaveChoice::Failed)
        {
            errorMessage = L"Save dialog failed during display or event validation";
        }
        return choice;
    }
    SaveImageTarget candidate;
    if (FAILED(result) || FAILED(ReadSaveImageTarget(dialog.Get(), candidate)))
    {
        errorMessage = L"保存对话框无法返回目标路径。";
        return SaveChoice::Failed;
    }
    // Show 返回后只核对，绝不再改路径；系统最终许可必须对应实际写入目标。
    try
    {
        if (NormalizeSaveImagePath(candidate.path, candidate.format) != candidate.path)
        {
            errorMessage = L"Save dialog returned an unconfirmed extension";
            return SaveChoice::Failed;
        }
    }
    catch (...)
    {
        errorMessage = L"Cannot validate final save path";
        return SaveChoice::Failed;
    }
    target = std::move(candidate);
    return SaveChoice::Accepted;
}
} // namespace open_st
