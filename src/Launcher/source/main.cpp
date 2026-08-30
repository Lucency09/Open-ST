#include <app.h>

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    // 必须在创建任何窗口前启用 Per-Monitor V2；之后 Win32 返回的窗口与虚拟桌面几何
    // 才能统一按当前显示器的物理像素解释。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // App 的栈生命周期覆盖整个消息循环，析构时集中释放托盘、热键、窗口和互斥体。
    open_st::App app(instance);
    return app.Run(showCommand);
}
