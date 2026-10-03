# Open-ST

[English](../README.md)

Open-ST 是一款 Windows 截图与标注工具，同时提供安装版和便携版。现已包含完全本地的中英日 OCR、fast/best 模型和可编辑结果；文本翻译支持有序在线接口与本地中英模型、自动识别、原文排布保留和连接诊断，图片翻译仍后置。

当前源码版本为 **1.0.0**，目标平台为 Windows 10 22H2／Windows 11 x64。本次改版本不代表已经公开发布；下方现有下载仍为 v0.4.1，当前待办与验收范围见[实现进度](implementation_progress.md)。

## 下载与使用

从 [v0.4.1 Releases](https://github.com/Lucency09/Open-ST/releases/tag/v0.4.1) 下载：

- **Setup.exe**：支持当前用户／所有用户安装及自定义目录，缺少运行库时经用户确认后安装包内附带的微软运行库。
- **ZIP**：完整解压到可写目录运行 Open-ST.exe；需另行安装[微软 VC++ x64 运行库](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。
- **SHA256SUMS.txt**：校验上述两件资产。程序和 Setup 未签名，哈希校验不等同发布者数字签名。

已发布的 v0.4.1 修复安装更新时托盘无响应：安装器启动后自动关闭关于窗口，用户可正常从托盘退出。旧版升级到本版时，仍需先关闭旧关于窗口并退出；本地OCR继续包含。

默认 Ctrl+Alt+Q 截图，Ctrl+C 或 Enter 复制，Ctrl+S 保存，Esc 分层取消。右键改为编辑标注属性，不再取消截图；文字编辑时 Enter 换行、Ctrl+Enter 确认。

升级前退出使用目标目录的实例并备份 data。安装升级默认保留数据；旧 ZIP 转安装版应安装到新目录后显式迁移数据。所有用户卸载不跨账户清理自启，各用户须事先关闭自己的开机启动登记。

当前用户安装、修复、备份、保留数据卸载及重装已在 Windows 11 验证；Windows 10、所有用户／真实多账户等仍有实机验收待办。既有贴图冗余请求测试仍失败，复杂标注冷构建可能较慢，详见发行说明。

## 构建

环境要求：

- Visual Studio 2022 17.10+
- MSVC v143
- Windows SDK 10.0.22621+
- CMake 3.28+
- PowerShell 5.1 或 7
- vcpkg manifest：负责基础 JSON 依赖、可选功能和测试依赖；未设置 `VCPKG_ROOT` 时使用 Visual Studio 随附的 vcpkg

**构建前注意：** 产品脚本通过 vswhere 或 `-VisualStudioPath` 定位 Visual Studio；测试脚本同样通过 vswhere 查找工具。安装包构建需预先准备经核验的 Inno Setup 和 VC++ Redistributable，缺少时明确报错，不自动安装工具。

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Package
.\scripts\build.ps1 -Configuration Release -Package -Installer -InnoSetupCompiler "<ISCC.exe路径>"
.\scripts\test.ps1
```

Debug 保留增量构建目录；Release 生成 `build/Release/`。`-Package` 生成版本化便携目录和 ZIP，`-Installer` 生成 Setup，可组合使用。包与 `SHA256SUMS.txt` 位于 `artifacts/<版本>/`；便携目录保留在 `artifacts/Open-ST-<版本>-win-x64/`。这些命令不公开发布。

完整的产品与工程规范见 [开发规范](development.md)。

当前默认同时启用 OCR 与翻译。首次缺少本地翻译模型时，使用 Windows x64 Python 3.12 显式运行 `python packaging/translation/convert_models.py --root .`；普通 CMake 配置只校验已有缓存，详见[依赖说明](dependencies.md)。

**开发构建注意：当前 Release 脚本会替换整个 build/Release，包括运行后生成的 data。重建前需备份 data，构建后恢复；这与安装器升级保留数据是两个不同流程。**
