// 将本地安装包交给系统标准启动流程，不解析远程内容或拼接命令行。
#include <installer_launcher.h>
#include <shellapi.h>

namespace open_st
{
// 校验本地路径形态并执行 Shell 启动，系统负责安装器需要的权限提示。
// 入参：verifiedLocalInstaller 为调用方保护的安装包；owner 为 UI 所有者。
// 返回：启动交接结果与 Windows 错误；不表示安装完成。
InstallerLaunchResult LaunchInstaller(const std::filesystem::path& verifiedLocalInstaller, HWND owner)
{
    const auto text = verifiedLocalInstaller.native();
    if (!verifiedLocalInstaller.is_absolute() || text.starts_with(L"\\\\") ||
        text.find_first_of(L"\"\r\n") != std::wstring::npos || text.find(L'\0') != std::wstring::npos ||
        CompareStringOrdinal(verifiedLocalInstaller.extension().c_str(), -1, L".exe", -1, TRUE) != CSTR_EQUAL)
        return {false, ERROR_INVALID_PARAMETER};
    const DWORD attributes = GetFileAttributesW(text.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {false, GetLastError()};
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return {false, ERROR_INVALID_PARAMETER};
    SHELLEXECUTEINFOW request{sizeof(request)};
    request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    request.hwnd = owner;
    request.lpVerb = L"open";
    request.lpFile = text.c_str();
    request.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&request))
        return {false, GetLastError()};
    if (request.hProcess != nullptr)
        CloseHandle(request.hProcess);
    return {true, ERROR_SUCCESS};
}
} // namespace open_st
