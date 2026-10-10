// 安装退出集成测试专用子进程：只创建v1.0兼容消息窗口，不读取任何用户配置。
#include <Windows.h>
#include <shellapi.h>
#include <string>

namespace
{
std::wstring closedPath;
bool ignoreClose{};

// 写入测试同步标记；路径仅来自父测试创建的隔离目录。
bool Mark(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    constexpr char value[] = "ready";
    DWORD written{};
    const bool result = WriteFile(file, value, sizeof(value), &written, nullptr) != FALSE;
    CloseHandle(file);
    return result;
}

// 模拟正常退出或无响应模式，以标记区分WM_CLOSE与TerminateProcess。
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CLOSE)
    {
        if (!ignoreClose)
        {
            (void)Mark(closedPath);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

// 参数为模式、就绪标记和正常关闭标记；非法输入不创建窗口。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 1;
    if (count != 4)
    {
        LocalFree(arguments);
        return 2;
    }
    ignoreClose = std::wstring(arguments[1]) == L"ignore";
    const std::wstring readyPath = arguments[2];
    closedPath = arguments[3];
    LocalFree(arguments);
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"OpenST.MessageWindow";
    windowClass.lpfnWndProc = Procedure;
    if (!RegisterClassW(&windowClass))
        return 3;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Installer test only", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                  nullptr, instance, nullptr);
    if (!window || !Mark(readyPath))
        return 4;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
        DispatchMessageW(&message);
    DestroyWindow(window);
    return 0;
}
