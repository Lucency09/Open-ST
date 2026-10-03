# 第三方依赖管理

Open-ST 使用 vcpkg manifest mode。依赖声明位于仓库根目录的 `vcpkg.json`，端口集合固定到其中的 `builtin-baseline`。不得向源码目录手工复制 DLL、头文件或静态库。

## 功能与依赖对应关系

| manifest feature | CMake 选项或入口 | 第三方包 | 使用范围 |
| --- | --- | --- | --- |
| 基础依赖 | 所有产品与测试构建 | nlohmann/json 3.12.0 | Common 通用 JSON 文件管理、本地化资源和设置持久化 |
| `ocr` | `OPEN_ST_ENABLE_OCR` / 脚本配置 `$EnableOcr` | Tesseract（含 Leptonica） | Application/OCR（私有链接） |
| `translation` | `OPEN_ST_ENABLE_TRANSLATION` / 脚本配置 `$EnableTranslation` | CTranslate2 4.6.0、SentencePiece 0.2.0 | Translation/Local 私有链接；在线适配器复用 Common/Http |
| `tests` | `OPEN_ST_BUILD_TESTS=ON` / `test.ps1` | GoogleTest/GoogleMock | 按 `src/` 相对路径镜像的 `testing/test/`、`testing/mock/` |

vcpkg 清单本身不隐式启用 feature，由脚本及 CMake 选择。构建/测试脚本顶部固定 `$EnableOcr = $true`、`$EnableTranslation = $true`，普通命令默认包含两项能力；关闭某项时直接改为 `$false`，不提供对应命令行开关。基础 JSON 始终使用 vcpkg。测试入口另外启用 `tests` feature 和 GoogleTest；它与产品构建仍然隔离。OCR 模型由 `packaging/ocr/models.json` 固定官方提交、长度及 SHA-256；缓存位于 `.cache/ocr-models`，运行时完全离线。

启用 OCR 的实际依赖为 Tesseract 5.5.1、Leptonica 1.85.0；当前固定端口配置未启用 OpenMP。实际运行依赖闭包及许可证随统一 Release staging 进入 ZIP/Setup。六模型原始体积约 49.5 MiB。

## 本地翻译依赖与模型

固定 baseline 没有 CTranslate2 端口，`vcpkg-configuration.json` 指向版本化的 `packaging/vcpkg-ports/ctranslate2`。该 overlay 固定 CTranslate2 4.6.0 提交及 SHA-512，同时固定其 cpu_features 与 spdlog 子模块；不升级整个 baseline。CPU 后端采用 oneDNN 3.7、MSVC OpenMP、动态运行库；禁用 CUDA、cuDNN、MKL、OpenBLAS、ruy 和独立 CLI。补丁检查 oneDNN 运算返回值，并限定 C++20 下 bfloat16 的 bit_cast 调用，避免 ADL 歧义。端口显式依赖 `nlohmann-json`，配置前移除上游内嵌 JSON 头；编译与导出的 CMake target 均使用该依赖，版本变化会计入端口 ABI。

SentencePiece 使用 baseline 的 0.2.0 端口；Windows 端口只提供静态库，未导出 CMake Config，因此 `FindSentencePiece.cmake` 在当前 vcpkg installed tree 内导入 Debug/Release 库及上游声明的 Abseil、Protobuf-lite、Threads 依赖。实际传递版本为 Abseil 20250127.1、Protobuf/utf8-range 5.29.5。项目源码只链接 Local 的私有依赖，不手工复制 DLL；发行阶段遍历 EXE 可达的 DLL 闭包。

`packaging/translation/models.json` 固定英→中、中→英两份 OPUS 2020-07-17 原始归档及七个交付文件的长度和 SHA-256。原始归档缓存在 `.cache/translation-sources`；CTranslate2 4.6.0 OpusMTConverter 以 INT8 准备模型。普通 CMake 配置只读取并验证缓存，缺失或损坏时报告显式准备命令 `python packaging/translation/convert_models.py --root .`，不创建 Python 环境、不安装依赖、不转换模型。准备工具需要清单指定的 Windows x64 Python 3.12，可通过 `--cmake <路径>` 指定已有 CMake；只有缓存无效才使用隔离的 `.cache/translation-converter` 环境，依赖由 `converter-requirements.txt` 固定版本和 wheel 哈希。

显式准备持有跨进程写锁，取得锁后重新检查缓存。下载资产使用独立文件锁及唯一临时文件；转换在唯一临时目录完成。整套验证后发布到 `.cache/translation-models/packs/<ID>`，最后原子替换 `active.txt`；构建固定引用选中的目录，准备失败保留原指针及旧模型。没有指针时兼容已有的根目录双向缓存，不重复转换或搬运；指针损坏时不静默回退。旧包不自动清理，避免影响仍引用它的构建。转换产物必须逐项匹配已有清单，不能在构建时重新定义信任值。产品运行不需要 Python，不下载模型。

两份实际使用的原始归档均自带 CC-BY-4.0；不能套用 Hugging Face 英→中重打包模型的 Apache-2.0 元数据。每份模型的原始 LICENSE/README、统一 NOTICE 及转换清单随资源进入发行视图；运行验证使用编译生成的白名单，不把随包 JSON 作为信任来源。引擎及内嵌组件的完整许可保存在 `licenses/translation/`。本地模型、许可、推理预算和实际性能验收仍须依照设计逐项完成，构建成功不代替发布验收。

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
- 产品及测试脚本共用 `scripts/build_environment.ps1`：下载归档为 `.cache/vcpkg/downloads/`，
  二进制包为 `.cache/vcpkg/archives/`，注册表为 `.cache/vcpkg/registries/`。
  `VCPKG_BINARY_SOURCES` 明确使用工作区文件缓存，不受宿主 AppData 重定向或已有远端源设置影响；
  `X_VCPKG_REGISTRIES_CACHE` 使用当前 vcpkg 已支持的注册表缓存配置。全部变量只作用于原生配置／构建，结束后恢复。
- vcpkg 自管辅助工具启用 `VCPKG_FORCE_DOWNLOADED_BINARIES`，CMake／PowerShell／Git 按当前 vcpkg 的
  `scripts/vcpkg-tools.json` 固定版本并复用 `downloads/tools/`，避免不同宿主 PATH 引起的 ABI 抖动。
  AppLocal 的 `Z_VCPKG_PWSH_PATH`／`Z_VCPKG_POWERSHELL_PATH` 也显式指向工作区 PowerShell，覆盖历史宿主路径。
  端口 CMake helper 对 Ninja 等另有探测机制，当前仍使用所选 VS 的 Ninja 1.12.1；不声称该环境变量覆盖全部探测。
  首次统一或明确升级 vcpkg／编译器时可能重建，不能把旧 ABI 的二进制冒充新工具链产物。
- 默认测试位于 `testing/testoutput/Debug-OCR-Translation/`，依赖 profile 为 `tests-ocr-translation`。仅 OCR 为 `Debug-OCR`／`tests-ocr`，仅翻译为 `Debug-Translation`／`tests-translation`，两项关闭为 `Debug`／`tests`；保留模块、case 两个位置参数。
- `build/`、`.cache/`、`testing/testoutput/`、`artifacts/` 和 vcpkg installed tree 均不进入 Git；仅跟踪 `.cache/README.md` 作为目录说明。
- 发布包只能复制 CMake/vcpkg 解析出的运行时文件，禁止提交来源不明的预编译二进制。

## 构建示例

```powershell
# 默认产品构建（恢复 JSON/OCR/翻译依赖并复用模型缓存）
.\scripts\build.ps1 -Configuration Debug

# 独立解析测试依赖，只构建并运行本次涉及的模块
.\scripts\test.ps1 translation_local
```
