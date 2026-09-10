// 定义应用托盘菜单等原生资源命令标识，供消息分派使用。

#pragma once

// TrackPopupMenu 选中后通过 WM_COMMAND 返回这些 ID；只在 application 模块内部使用。
#define ID_TRAY_CAPTURE 1001
#define ID_TRAY_ABOUT 1002
#define ID_TRAY_EXIT 1003
#define ID_TRAY_SETTINGS 1004
#define ID_TRAY_CLEANUP 1005
#define ID_TRAY_PIN_CLOSE_ALL 1006
