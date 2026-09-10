# Open-ST

[English](../README.md)

Open-ST 是一款面向 Windows 的便携式截图工具；标注、离线 OCR 与翻译功能仍在规划中。

项目支持 Windows 10 22H2 和 Windows 11 x64，最新发布版本为 `0.2.0`，当前开发版本为 `0.3.0`。

## 下载与使用

从 [Releases](https://github.com/Lucency09/Open-ST/releases) 下载 Windows x64 ZIP，完整解压到可写目录后运行 `Open-ST.exe`。
需要安装 [Microsoft Visual C++ v14 x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)，发布包不包含运行库安装程序。

0.2.0 新增截图工具栏、多张置顶贴图、重做的设置窗口、首次欢迎页及可选的开机启动；继续支持多显示器选区、复制到剪贴板、PNG/JPEG 保存、HDR 捕获与 SDR 输出，以及中英日界面。
`Ctrl+Alt+Q` 开始截图，`Ctrl+C` 或 `Enter` 复制，`Ctrl+S` 保存，`Esc` 或右键分层取消。
工具栏可将选区固定为贴图；点击贴图后滚轮缩放、`Ctrl+滚轮` 调整透明度，右键菜单可复制、保存、提层或关闭。再次截图包含已有贴图当前外观。
窗口自动识别、标注、OCR、翻译及截图快捷键配置尚未实现；HDR 颜色及跨屏性能仍有待完善，详见[发行说明](releases/v0.2.0.md)。

## 构建

环境要求：

- Visual Studio 2022 17.10+
- MSVC v143
- Windows SDK 10.0.22621+
- CMake 3.28+
- PowerShell 5.1 或 7
- vcpkg manifest：负责基础 JSON 依赖、可选功能和测试依赖；未设置 `VCPKG_ROOT` 时使用 Visual Studio 随附的 vcpkg

**构建前注意：** `scripts/build.ps1` 和 `scripts/test.ps1` 目前写死了部分开发机工具路径，包括 Visual Studio 的 MSVC 环境脚本、CMake 和 Ninja。在其他电脑上，仅安装上述依赖不一定能直接构建，请先检查并按自己的安装位置修改这些路径。

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Package
.\scripts\test.ps1
```

Debug 会保留增量构建目录。Release 会在 `build/Release/` 生成修剪后的可运行环境；使用 `-Package` 时，还会把该目录复制到带版本号的 `artifacts/` 目录。

完整的产品与工程规范见 [开发规范](development.md)。
