#include "save_image_dialog.h"
#include <format>
#include <memory>
#include <shobjidl.h>
#include <ui_text.h>
#include <wrl/client.h>

namespace
{
// 释放 Shell 以 COM task allocator 返回的路径字符串。
struct TaskMemoryDeleter final
{
    // 接管 GetDisplayName 返回值的唯一释放职责。
    void operator()(wchar_t* value) const noexcept
    {
        CoTaskMemFree(value);
    }
};
} // namespace
namespace open_st
{
// 配置系统过滤器、扩展名、默认文件名与覆盖确认，不创建任何临时截图文件。
SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path& lastDirectory, SaveImageTarget& target,
                               std::wstring& errorMessage)
{
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
    const std::wstring filename =
        std::format(L"openst-{:02}-{:02}-{:02}_{:02}-{:02}-{:02}.{:03}.png", localTime.wYear % 100,
                    localTime.wMonth, localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond,
                    localTime.wMilliseconds);
    const COMDLG_FILTERSPEC filters[]{{png.c_str(), L"*.png"}, {jpeg.c_str(), L"*.jpg;*.jpeg"}};
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST)) ||
        FAILED(dialog->SetFileTypes(2U, filters)) || FAILED(dialog->SetFileTypeIndex(1U)) ||
        FAILED(dialog->SetDefaultExtension(L"png")) || FAILED(dialog->SetTitle(title.c_str())) ||
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
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return SaveChoice::Cancelled;
    }
    ComPtr<IShellItem> item;
    UINT index{};
    if (FAILED(result) || FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(dialog->GetFileTypeIndex(&index)))
    {
        errorMessage = L"保存对话框无法返回目标路径。";
        return SaveChoice::Failed;
    }
    PWSTR rawPath{};
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    std::unique_ptr<wchar_t, TaskMemoryDeleter> path(rawPath);
    if (FAILED(result) || !path)
    {
        errorMessage = L"保存路径无效。";
        return SaveChoice::Failed;
    }
    target.path = path.get();
    target.format = index == 2U ? ImageFileFormat::Jpeg : ImageFileFormat::Png;
    return SaveChoice::Accepted;
}
} // namespace open_st
