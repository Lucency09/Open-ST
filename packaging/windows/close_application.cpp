// 安装器专用退出助手：只处理原交互用户在目标安装目录运行的实例，不参与下载和文件替换。
#include <Windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr int InvalidPath = 10;
constexpr int InvalidIdentity = 11;
constexpr int SystemFailure = 12;
constexpr int StillRunning = 13;
constexpr ULONGLONG GracePeriod = 15000;

struct Handle final
{
    HANDLE value{};
    explicit Handle(HANDLE handle = nullptr) noexcept : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value)
    {
        other.value = nullptr;
    }
    // 回收独占拥有的系统句柄；没有业务输出。
    ~Handle()
    {
        if (this->value != nullptr && this->value != INVALID_HANDLE_VALUE)
            CloseHandle(this->value);
    }
};
struct Process final
{
    Handle handle;
    DWORD id{};
    bool requested{};
};

// 读取进程真实令牌的用户SID，返回独立字节副本；查询失败返回空。
std::vector<BYTE> UserSid(HANDLE process)
{
    HANDLE raw{};
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return {};
    Handle token(raw);
    DWORD size{};
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (size == 0 || size > 65536)
        return {};
    std::vector<BYTE> storage(size);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), size, &size))
        return {};
    PSID sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
    if (!IsValidSid(sid))
        return {};
    std::vector<BYTE> result(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(result.size()), result.data(), sid))
        return {};
    return result;
}

// 查询持有句柄的映像绝对路径，不信任进程显示名称或命令行。
std::wstring ImagePath(HANDLE process)
{
    std::wstring result(32768, L'\0');
    DWORD size = static_cast<DWORD>(result.size());
    if (!QueryFullProcessImageNameW(process, 0, result.data(), &size))
        return {};
    result.resize(size);
    return result;
}

// 比较Windows路径大小写；参数必须已经是完整本地路径。
bool EqualPath(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// 拒绝相对路径、网络路径、重解析点和父目录穿越，返回可信安装目录中的固定EXE路径。
std::wstring TargetPath(const std::wstring& directory, std::vector<Handle>& anchors)
{
    const std::filesystem::path path(directory);
    if (directory.size() < 3 || directory[1] != L':' || directory[2] != L'\\' || !path.is_absolute() ||
        directory.find_first_of(L"\"\r\n") != std::wstring::npos)
        return {};
    std::filesystem::path current;
    for (const std::filesystem::path& component : path)
    {
        if (component == L".." || component == L".")
            return {};
        current /= component;
        if (!current.has_root_directory())
            continue;
        // 每层都禁止删除/改名并检查同一无跟随句柄，避免属性检查后目录被换成重解析点。
        Handle anchor(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (anchor.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(anchor.value, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return {};
        anchors.push_back(std::move(anchor));
    }
    return (path / L"Open-ST.exe").lexically_normal().native();
}

// 同一会话的桌面Shell用户必须等于ExecAsOriginalUser得到的令牌；手工换账户提权不能猜测原用户。
bool InteractiveIdentity(const std::vector<BYTE>& sid, DWORD& session)
{
    if (sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return false;
    DWORD shellId{};
    const HWND shell = GetShellWindow();
    if (!shell || GetWindowThreadProcessId(shell, &shellId) == 0)
        return false;
    DWORD shellSession{};
    if (!ProcessIdToSessionId(shellId, &shellSession) || shellSession != session)
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellId));
    if (!process.value)
        return false;
    const std::vector<BYTE> shellSid = UserSid(process.value);
    return !shellSid.empty() && EqualSid(const_cast<BYTE*>(sid.data()), const_cast<BYTE*>(shellSid.data()));
}

// 核验待操作进程仍属于原交互用户和会话；持有进程句柄防止PID复用影响强制退出。
bool MatchesIdentity(const Process& process, const std::vector<BYTE>& sid, DWORD session)
{
    DWORD processSession{};
    if (!ProcessIdToSessionId(process.id, &processSession) || processSession != session)
        return false;
    const std::vector<BYTE> actual = UserSid(process.handle.value);
    return !actual.empty() && EqualSid(const_cast<BYTE*>(sid.data()), const_cast<BYTE*>(actual.data()));
}

// 只向PID匹配的消息专用窗口投递关闭，兼容已经发布的v1.0；没有窗口时允许稍后重试。
bool RequestClose(Process& process)
{
    HWND window = nullptr;
    while ((window = FindWindowExW(HWND_MESSAGE, window, L"OpenST.MessageWindow", nullptr)) != nullptr)
    {
        DWORD windowProcess{};
        if (GetWindowThreadProcessId(window, &windowProcess) != 0 && windowProcess == process.id)
        {
            if (!PostMessageW(window, WM_CLOSE, 0, 0))
                return false;
            process.requested = true;
            return true;
        }
    }
    return true;
}

// 枚举并一次性验证所有匹配实例，再合作退出；15秒后仅终止持有的同一身份句柄。
int CloseTarget(const std::wstring& directory)
{
    std::vector<Handle> anchors;
    const std::wstring target = TargetPath(directory, anchors);
    if (target.empty())
        return InvalidPath;
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : InvalidPath;
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return InvalidPath;
    // 在退出阶段禁止替换目标映像；安装器只会在助手返回且句柄关闭之后覆盖文件。
    Handle image(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (image.value == INVALID_HANDLE_VALUE)
        return SystemFailure;
    BY_HANDLE_FILE_INFORMATION expected{};
    if (!GetFileInformationByHandle(image.value, &expected) || expected.nNumberOfLinks != 1 ||
        (expected.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        return InvalidPath;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE)
        return SystemFailure;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry))
        return SystemFailure;
    std::vector<Process> processes;
    do
    {
        if (!EqualPath(entry.szExeFile, L"Open-ST.exe"))
            continue;
        Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!query.value)
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                continue; // 枚举之后已经退出。
            return SystemFailure;
        }
        if (WaitForSingleObject(query.value, 0) == WAIT_OBJECT_0)
            continue;
        const std::wstring actual = ImagePath(query.value);
        if (actual.empty())
            return SystemFailure;
        if (!EqualPath(actual, target))
            continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE,
                                   entry.th32ProcessID));
        if (!process.value)
        {
            if (WaitForSingleObject(query.value, 0) == WAIT_OBJECT_0)
                continue;
            return SystemFailure;
        }
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
            continue;
        if (!EqualPath(ImagePath(process.value), target))
            return InvalidIdentity;
        processes.push_back({std::move(process), entry.th32ProcessID, false});
    } while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES)
        return SystemFailure;
    if (processes.empty())
        return 0;
    DWORD session{};
    const std::vector<BYTE> sid = UserSid(GetCurrentProcess());
    if (!InteractiveIdentity(sid, session))
        return InvalidIdentity;
    for (const Process& process : processes)
    {
        if (WaitForSingleObject(process.handle.value, 0) != WAIT_OBJECT_0 && !MatchesIdentity(process, sid, session))
            return InvalidIdentity;
    }
    const ULONGLONG deadline = GetTickCount64() + GracePeriod;
    for (;;)
    {
        bool running = false;
        for (Process& process : processes)
        {
            const DWORD wait = WaitForSingleObject(process.handle.value, 0);
            if (wait == WAIT_OBJECT_0)
                continue;
            if (wait != WAIT_TIMEOUT)
                return SystemFailure;
            running = true;
            if (!process.requested && !RequestClose(process))
                return SystemFailure;
        }
        if (!running)
            return 0;
        if (GetTickCount64() >= deadline)
            break;
        Sleep(50);
    }
    for (const Process& process : processes)
    {
        if (WaitForSingleObject(process.handle.value, 0) == WAIT_OBJECT_0)
            continue;
        if (!EqualPath(ImagePath(process.handle.value), target) || !MatchesIdentity(process, sid, session))
            return InvalidIdentity;
        if (!TerminateProcess(process.handle.value, ERROR_PROCESS_ABORTED))
        {
            if (WaitForSingleObject(process.handle.value, 0) == WAIT_OBJECT_0)
                continue;
            return SystemFailure;
        }
        if (WaitForSingleObject(process.handle.value, 5000) != WAIT_OBJECT_0)
            return StillRunning;
    }
    return 0;
}
} // namespace

// 隐藏安装助手只接收一个固定目录参数；错误以退出码返回，禁止通过Shell解释命令。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return InvalidPath;
    std::wstring directory;
    if (count == 3 && std::wstring(arguments[1]) == L"--directory")
        directory = arguments[2];
    LocalFree(arguments);
    if (directory.empty())
        return InvalidPath;
    try
    {
        return CloseTarget(directory);
    }
    catch (...)
    {
        return SystemFailure;
    }
}
