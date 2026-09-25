# 第三方依赖管理

Open-ST 使用 vcpkg manifest mode。依赖声明位于仓库根目录的 `vcpkg.json`，端口集合固定到其中的 `builtin-baseline`。不得向源码目录手工复制 DLL、头文件或静态库。

## 功能与依赖对应关系

| manifest feature | CMake 选项或入口 | 第三方包 | 使用范围 |
| --- | --- | --- | --- |
| 基础依赖 | 所有产品与测试构建 | nlohmann/json 3.12.0 | Common 通用 JSON 文件管理、本地化资源和设置持久化 |
| `ocr` | `OPEN_ST_ENABLE_OCR` / 脚本配置 `$EnableOcr` | Tesseract（含 Leptonica） | Application/OCR（私有链接） |
| `translation` | `OPEN_ST_ENABLE_TRANSLATION` / `build.ps1 -EnableTranslation` | 暂无额外包 | 为后续翻译服务保留的功能边界 |
| `tests` | `OPEN_ST_BUILD_TESTS=ON` / `test.ps1` | GoogleTest/GoogleMock | 按 `src/` 相对路径镜像的 `testing/test/`、`testing/mock/` |

vcpkg 清单本身不隐式启用 feature，由脚本及 CMake 选择。构建/测试脚本顶部固定 `$EnableOcr = $true`，普通命令默认包含 OCR；仅需基础构建时直接改为 `$false`，不再提供 OCR 命令行开关。基础 JSON 始终使用 vcpkg。测试入口另外启用 `tests` feature 和 GoogleTest；它与产品构建仍然隔离。`ocr` 已接入识别、结果窗口和设置页；`translation` 仍仅为后续依赖边界。OCR 模型由 packaging/ocr/models.json 固定官方提交、长度及 SHA-256；缓存位于 .cache/ocr-models，运行时完全离线。

启用 OCR 的实际依赖为 Tesseract 5.5.1、Leptonica 1.85.0；当前固定端口配置未启用 OpenMP。实际运行依赖闭包及许可证随统一 Release staging 进入 ZIP/Setup。六模型原始体积约 49.5 MiB。

## 当前 baseline

`builtin-baseline` 固定为 `4334d8b4c8916018600212ab4dd4bbdc343065d1`（vcpkg 2025.09.17 端口集合）。固定 baseline 是为了让当前 Visual Studio 随附的 vcpkg 工具稳定解析 manifest 并复现 GoogleTest 1.17；升级必须作为显式变更验证，不能无说明跟随最新 HEAD。

## 新增依赖的流程

1. 确认标准库或 Windows SDK 不能合理解决问题。
2. 在 `vcpkg.json` 的对应 feature 中声明端口，不使用 `FetchContent` 或自定义下载脚本绕开清单。
3. 在 `cmake/OpenSTDependencies.cmake` 中用 `find_package(... CONFIG REQUIRED)` 解析包。
4. 只在实际使用依赖的模块 `CMakeLists.txt` 中链接导入目标，不全局链接给所有模块。
5. 把组件、版本、来源、许可证和是否进入发布包写入 `THIRD_PARTY_NOTICES.txt`，并将需随二进制分发的许可证放入 `licenses/`。
6. 执行 Debug、Release 和干净目录构建；测试不得连接真实付费服务。

## 工具定位与产物

- `build.ps1` 和 `test.ps1` 都优先使用 `VCPKG_ROOT`；未设置时使用当前 Visual Studio 安装随附的 vcpkg。
- `VCPKG_ROOT` 只定位工具，依赖内容和版本仍由 manifest 与 baseline 决定。构建和测试脚本使用 vswhere 发现 Visual Studio；preset 的工具路径仍需按使用环境核对。
- Debug 产品构建树位于 `build/Debug/`；Release 在 `build/.release-work/` 编译并整理运行文件到 `build/Release/`，成功后移除中间产物。`-Clean` 为终止型操作：Debug 只清理自身构建树，Release 清理运行目录及残留的 `.release-work/`、`.release-staging/`。
- vcpkg installed tree 位于 `.cache/vcpkg_installed/x64-windows/<profile>/`，按产品 feature 组合和测试 profile 隔离，可跨干净构建复用。
- 基础测试位于 `testing/testoutput/Debug/`；默认 OCR 测试 使用独立 `testing/testoutput/Debug-OCR/` 与 tests-ocr 依赖 profile，保留模块、case 两个位置参数。
- `build/`、`.cache/`、`testing/testoutput/`、`artifacts/` 和 vcpkg installed tree 均不进入 Git；仅跟踪 `.cache/README.md` 作为目录说明。
- 发布包只能复制 CMake/vcpkg 解析出的运行时文件，禁止提交来源不明的预编译二进制。

## 构建示例

```powershell
# 默认产品构建（恢复 JSON/OCR 依赖并复用模型缓存）
.\scripts\build.ps1 -Configuration Debug

# 翻译功能仍仅保留构建边界
.\scripts\build.ps1 -Configuration Debug -EnableTranslation

# 独立解析测试依赖并运行全量测试
.\scripts\test.ps1
```
