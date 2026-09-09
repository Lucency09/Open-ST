// 提供 Windows 程序入口，设置 DPI 感知并将运行生命周期交给 App。

#include <app.h>

#include <windows.h>

// 设置进程 DPI 感知并启动应用消息循环。
// 入参：instance：当前模块句柄；未命名 HINSTANCE、PWSTR：未使用的旧实例和命令行参数；showCommand：传给 App 的初始显示方式。
// 返回：App::Run 的退出码，交还 Windows。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    // 必须在创建任何窗口前启用 Per-Monitor V2；之后 Win32 返回的窗口与虚拟桌面几何
    // 才能统一按当前显示器的物理像素解释。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // App 的栈生命周期覆盖整个消息循环，析构时集中释放托盘、热键、窗口和互斥体。
    open_st::App app(instance);
    return app.Run(showCommand);
}
