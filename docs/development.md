# Open-ST 开发规范

本文档记录 Open-ST 已确认的工程约束和协作规则。产品行为以 [产品需求规格](product_requirements.md) 为准；关键取舍及其原因见 [决策记录](decision_log.md)。实现不得静默改变这些约束。

## 1. 文档与变更原则

- 新需求先确认产品语义，再修改代码；重要取舍写入 `decision_log.md`。
- 文档必须描述当前真实实现。阶段完成时同步更新 `implementation_progress.md`，不能保留已经失效的脚本参数、测试数量或目录。
- 产品需求、架构、依赖、开发规范和实现进度分别维护，避免把尚未实现的目标写成已完成能力。
- 注释和项目文档使用中文；标识符、API 名称及业内固定术语可保留英文。

## 2. 源码模块布局

`src/` 通过目录嵌套直接表达产品模块的依赖方向。上级目录中的模块可以依赖其子孙目录中的模块；子模块不得反向依赖祖先模块，也不得依赖不在自身子树中的兄弟模块。`common` 是唯一的跨层依赖例外：产品模块可以依赖 Common 及其独立通用子目标，但它们不得依赖任何产品模块。

```text
src/
├── CMakeLists.txt
├── Common/                     # OpenST::Common，唯一跨层依赖例外
│   ├── CMakeLists.txt
│   ├── include/
│   ├── private/
│   ├── source/
│   └── WindowRenderer/         # 独立 OpenST::WindowRenderer，Common 库不链接 GUI
│       ├── CMakeLists.txt
│       ├── include/
│       ├── private/
│       └── source/
└── Launcher/                   # Open-ST.exe
    ├── CMakeLists.txt
    ├── source/
    ├── resources/              # RC 图标资源，引用根 resources/icons/
    └── Application/            # OpenST::Application
        ├── CMakeLists.txt
        ├── include/
        ├── private/
        ├── source/
        ├── SystemIntegration/  # OpenST::SystemIntegration：当前用户启动项与单实例通信
        │   ├── CMakeLists.txt
        │   ├── include/
        │   └── source/
        ├── Localization/       # OpenST::Localization
        │   ├── include/
        │   ├── private/
        │   └── source/
        ├── Settings/           # OpenST::Settings，SettingsWindow 为同一目标的兼容别名
        │   ├── CMakeLists.txt
        │   ├── include/
        │   ├── private/
        │   └── source/
        ├── Export/             # OpenST::Export：SDR 编码、文件与剪贴板写入
        │   ├── CMakeLists.txt
        │   ├── include/
        │   ├── private/
        │   └── source/
        └── Graphics/           # OpenST::Graphics
            ├── CMakeLists.txt
            ├── include/
            ├── private/
            ├── source/
            ├── Capture/        # OpenST::Capture
            │   ├── CMakeLists.txt
            │   ├── include/
            │   ├── private/
            │   └── source/
            └── HDR/            # OpenST::HDR：颜色解码与原生色调映射
                ├── CMakeLists.txt
                ├── include/
                └── source/
```

具体规则：

- 产品模块目录统一使用 PascalCase（如 `Common`、`Application`、`Graphics`、`Capture`）；
  模块职责目录 `include/`、`private/`、`source/` 保持小写，源码文件继续使用 snake_case。
- `src/`、`testing/`、`test/`、`mock/`、`docs/`、`scripts/` 等工程基础目录保持小写；
  test 和 mock 中已经存在的模块目录必须与源码模块路径及大小写一致，不为镜像而预建空模块。
- 目录命名不改变 CMake target/alias、C++ namespace、设置键或测试筛选名称；
  例如 `OpenST::Capture`、`open_st_capture` 和 `test.ps1 capture` 保持原有契约。
- `.cpp` 放在 `source/`，不放进 `private/`。`private/` 表示可见性，不表示“所有实现文件”。
- 只有其他模块确实需要包含的接口才能进入 `include/`；公开头路径由模块自己的 CMake `FILE_SET` 和 `BASE_DIRS` 明确定义。
- 每个模块在自己的 `CMakeLists.txt` 中列出源码、头文件、包含目录和链接依赖，并先通过 `add_subdirectory()` 声明自己的直接子模块。
- `src/CMakeLists.txt` 只声明 `Common` 和 `Launcher` 两个顶层入口，不得集中罗列模块内部文件。
- 根 `CMakeLists.txt` 只管理项目级选项、公共配置、依赖入口和顶层目录。
- 模块公开依赖用 `PUBLIC`，仅实现所需依赖用 `PRIVATE`，避免把无关依赖传播给上层。
- 不提前创建没有真实接口或实现的空模块。
- `capture` 拥有桌面捕获接口、冻结帧和虚拟桌面物理像素几何；`graphics` 通过公开依赖消费这些类型。
- `Graphics` 拥有冻结帧选区裁切、跨屏拼接和 `SdrSelectionFrame`；`Graphics/HDR` 只接受自己的借用像素视图，不依赖父级 Graphics 或兄弟 Capture。
- `Application/Export` 只接受自己的 SDR 像素视图，负责剪贴板、WIC 编码和文件写入，不依赖兄弟 Graphics、Settings 或 Localization。Application 负责视图适配、保存对话框、设置和本地化。
- `localization` 是 `application` 的子模块，只通过 `GetUiText("text.key", ...)` 向业务代码提供用户可见文本；文本键和语言代码直接来自 JSON，不在 C++ 中维护枚举或注册表。
- `Common` 承载 Logger 和通用 JSON 文件管理；具体文件路径、业务结构、默认值与字段规则归各业务模块。
  动态设置接口属于 `Settings`，不设置独立的 `foundation`、`logging` 或业务专用 JSON 目标。

当前依赖方向为：

```text
launcher -> application
application -> localization
application -> system_integration
application -> settings
application -> graphics
application -> export
application -> capture
application -> common
graphics -> capture
graphics -> hdr
graphics -> common
settings -> common
settings -> window_renderer
application -> window_renderer
localization -> common
```

`Application` 协调设置窗口、本地化、图形、桌面捕获和日志；每次截图读取一份边框颜色设置并传入各屏渲染器，`Graphics` 只负责解析与渲染，不依赖同级 `Settings`。设置窗口通过 Application 注入的窄回调查询语言与文本、通知保存完成，由 Application 切换运行时语言，不依赖同级 `Localization`。`Launcher` 只保留 Windows 程序入口并生成 `Open-ST.exe`。`Common` 只提供通用能力，不得反向依赖产品模块。

`App::CopySelection` 与 `App::SaveSelection` 是统一完成入口，目前分别由 Ctrl+C/Enter 和 Ctrl+S 触发。Application 的私有 `CaptureCompletion` 编排同步转换和输出，忙状态覆盖保存对话框与错误提示；取消或失败保留有效会话，显示布局失效则在模态调用返回后回收。本阶段不实现双击完成、工具栏或独立快捷键模块。

## 3. CMake 辅助文件

`cmake/` 保存供根 `CMakeLists.txt` 调用的可复用 CMake 函数，不是生成目录，也不只负责第三方依赖：

- `OpenSTProjectOptions.cmake`：统一 C++ 标准、MSVC 警告、预处理定义等项目编译规则。
- `OpenSTDependencies.cmake`：根据功能选项解析第三方包并提供导入目标。

模块自己的文件清单和模块间链接仍由模块内 `CMakeLists.txt` 维护。

## 4. 第三方依赖

- 使用根目录 `vcpkg.json` 的 manifest mode 和固定 `builtin-baseline`。
- 大型或可选功能使用 manifest feature；当前有 `ocr`、`translation` 和 `tests`。
- 新增依赖前先确认标准库或 Windows SDK 不能合理解决。
- 禁止在模块中用 `FetchContent`、临时下载脚本或手工复制 DLL/头文件绕开 manifest。
- 包在 `cmake/OpenSTDependencies.cmake` 中统一 `find_package()`，但只由实际使用它的模块链接。
- 产品基础构建不启用测试依赖；`build.ps1` 永远令 `OPEN_ST_BUILD_TESTS=OFF`。
- 第三方版本、来源和许可证进入 `THIRD_PARTY_NOTICES.txt` 与 `licenses/`；发布物只能复制可追踪的构建依赖。
- nlohmann/json 是所有构建都需要的基础依赖；无论是否启用可选 feature，产品构建都会进入 vcpkg manifest mode。

详细流程见 [第三方依赖管理](dependencies.md)。

## 5. 构建脚本

产品构建与测试构建是两个严格分离的入口。

### 5.1 产品构建

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Clean
```

- `build.ps1` 只构建产品，永远不加载 `testing/test` 或 `testing/mock`，也不提供开启测试的参数。
- 不指定配置时为 Debug；Debug 保留完整增量构建树。Release 在临时工作目录完成构建后，只把 EXE、运行时 DLL、资源和许可证整理到 `build/Release/`，随后删除 CMake/Ninja 中间产物。
- `-Clean` 是终止型操作：Debug 只清理自身构建树；Release 同时清理运行目录与可能残留的临时工作/整理目录，随后立即返回，不重新配置或构建。
- `-EnableOcr`、`-EnableTranslation` 按需启用对应 vcpkg feature。
- `-Package` 只允许用于 Release，并把已经修剪过的 `build/Release/` 复制到版本化 `artifacts/` 目录；复制前会清理同名旧包，避免残留过期 DLL 或资源。
- 当前脚本和 preset 暂时使用 Visual Studio 随附的 Ninja，工具路径仍与开发机安装位置绑定，其他机器需先调整路径。这是现阶段保留的实现状态，不是已经确认、不可更改的永久架构约束；若以后切换生成器，应统一修改脚本、preset 和文档。

### 5.2 测试构建

`test.ps1` 接受最多两个位置参数，不使用交互菜单：

```powershell
.\scripts\test.ps1
.\scripts\test.ps1 common
.\scripts\test.ps1 capture union_rectangles
.\scripts\test.ps1 capture GeometryTest.union_rectangles
```

- 第一个参数是模块名，第二个参数是 case 名；为空即扩大到全部范围。
- 测试脚本独立配置、构建并运行 CTest，不调用 `build.ps1`。
- CMake、vcpkg 和测试二进制等全部测试产物写入 `testing/testoutput/Debug/`。
- `testing/testoutput/` 必须被 Git 忽略。

## 6. 测试与 Mock

测试和 Mock 都按源码相对目录完整镜像：

```text
testing/
├── test/
│   ├── Common/
│   │   └── WindowRenderer/     # 通用布局、绑定及模态窗口测试
│   └── Launcher/Application/
│       ├── source/             # 应用编排与会话测试
│       ├── Export/
│       ├── Localization/
│       ├── Settings/
│       ├── SystemIntegration/
│       └── Graphics/
│           ├── source/
│           ├── selection/      # Graphics 内部选区测试
│           ├── Capture/
│           └── HDR/
└── mock/
    ├── Common/
    └── Launcher/Application/
        ├── Export/
        └── Graphics/Capture/
```

规则：

- 每一级测试目录维护自己的 `CMakeLists.txt`，并像源码目录一样逐级 `add_subdirectory()`；`testing/test/CMakeLists.txt` 只加载 `Common` 与 `Launcher`。
- 采用 GoogleTest + GoogleMock + CTest，并使用 `gtest_discover_tests()` 注册 case。
- 标准 gTest 目标链接 `GTest::gtest_main`，测试源码不得自定义 `main()` 或 `wmain()`。
- 确有必要的人工诊断/集成探针可以是独立可执行程序并拥有入口函数，但不能伪装成普通自动通过的 gTest case。经用户批准的人工窗口 gTest 必须通过显式环境开关启用，默认 GTEST_SKIP，并明确等待真人关闭，不能用定时器替代人工验收。
- 每个测试 case 上方必须写中文说明，明确“测试什么功能”和关键边界，不能只复述 case 名。
- Mock 只进入测试链接关系，产品代码不得依赖 GoogleMock。
- 付费 API、网络供应商和系统边界用 mock/fake 验证；自动测试不得调用真实付费服务。
- 平台相关集成测试仍应注册到全量测试和所属模块测试。先显式检查运行平台；环境不满足时用 `GTEST_SKIP()` 并在输出中说明原因。环境满足后若功能失败，必须按失败处理，不能跳过掩盖缺陷。
- 测试名称统一为 `<module>.<Suite>.<case>`，确保脚本能按模块或 case 精确过滤。

## 7. 代码、注释与格式

- C++20、namespace `open_st`。
- Microsoft/Allman 风格，四空格，不使用 Tab，单行上限 120；以仓库 `.clang-format` 为准。
- 类型和函数使用 `PascalCase`，局部变量和参数使用 `camelCase`，私有成员使用尾随下划线。
- 常量使用 `UPPER_SNAKE_CASE`，文件名使用 `snake_case.cpp/.h`。
- 成员函数体内访问当前对象的任何非静态成员变量或调用非静态成员函数时，一律显式使用 `this->`，不得因为当前不存在同名局部变量、参数或其他歧义而省略。仅有两个语法例外：构造函数成员初始化列表保持 `member_(value)` 形式；静态成员函数不存在 `this`，其中的静态成员使用类名限定。
- 默认显式写出变量类型；常规基础类型、指针以及名称简短的项目类型不得使用 `auto`、`const auto` 或 `auto*` 进行类型推导。只有显式类型名称特别长，或属于冗长的结构体、数组、迭代器、嵌套模板等复杂组合类型时，才允许使用 `auto` 以维持可读性和可维护性。
- 用户可见文本（包括窗口标题、托盘文字、菜单、普通提示和错误弹窗）一律使用 `GetUiText("text.key", ...)` 取得；同级模块不能直接依赖 Localization 时，通过 Application 注入的窄回调转发到该入口。业务代码负责写明所需 key，但不得直接读文本 JSON、维护第二份 key 清单或硬编码译文。只有本地化资源本身无法加载时的最小三语紧急提示例外。
- 产品模块不得自行打开 JSON 文件；由各业务模块决定卡名、路径和内容，通过 Common 单例取得懒加载句柄。句柄只公开 `IsValid`、`Read` 和 `Write` 三个操作名；`Write` 保留完整文档及短小编辑回调两种重载。缓存、内部状态、修订号与清理接口不得泄漏给产品调用方。
- JSON 读取失败返回 `false` 且不改输出，写入失败不发布候选缓存；Common 保留内部缓存，但不得以旧缓存冒充本次读取成功。详细错误由 Common 日志记录，业务/UI 仅依据成功与否显示本地化粗略告警。业务结构验证、默认值与已生效业务状态回退由各模块负责。
- 运行期读取告警由业务模块去重、Application 在消息处理后合并显示，不在 Common 或高频 getter 内弹窗；截图会话期间保留待提示状态，结束后再告知。
- 同一文件的读写由内部锁串行处理；编辑写入在锁内完成读改写，完整文档 `Read` 后再 `Write` 不承诺跨调用事务。正常产品关闭只释放自身句柄，管理器强持有状态至进程退出；测试隔离清理仅通过 Common 私有测试入口。
- 设置文件专属业务通过 Settings 的 `Get/Set*Setting("setting.key")` 访问，不维护设置 key 枚举或注册表；其他业务的 JSON 不集中交由 Settings 管理。
- 项目与测试使用 `/W4`；默认 `/WX`，只允许用 `build.ps1 -AllowWarnings` 临时放宽。
- 项目文本使用 UTF-8、CRLF、文件末尾保留换行；以 `.editorconfig` 为准。
- 注释应解释设计原因、边界、所有权、单位或平台限制；不要给显而易见的代码逐行翻译。
- 每个函数定义前必须有中文功能说明；所有函数声明也必须分别说明职责、关键输入输出或边界，包含构造/析构、删除函数、私有及测试辅助函数，不能用一条笼统注释代替多个函数。
- 对外接口说明输入输出、错误语义和坐标/颜色单位；实现复杂 Windows API 时补充资源生命周期说明。
- 禁止裸 owning `new/delete`；COM 使用 `ComPtr`，HANDLE 等资源使用 RAII。

## 8. Windows 与图形约束

- 目标为 Windows 10 22H2 / Windows 11 x64，进程使用 Per-Monitor V2 DPI awareness。
- 所有几何必须明确是虚拟桌面物理像素还是 D2D DIP；跨边界转换集中完成，禁止隐式混用。
- 捕获必须发生在遮罩窗口显示前，冻结帧在一次截图会话中不可变。
- 变暗层只绘制在选区外；选区内不得提亮、增饱和或改变原始像素。
- 保存、复制、贴图、OCR 和翻译都从冻结帧裁切，不得重新捕获已经显示遮罩的桌面。
- SDR 无标注输出应与 BGRA8 冻结帧对应像素一致；HDR 到 SDR 必须只有一次显式、可测试的色调映射。
- 捕获失败不得以“成功但返回全黑帧”掩盖；图形设备丢失和桌面复制失效必须产生明确错误或执行受控重建。

## 9. 错误、隐私与日志

- 不能吞掉错误；能恢复的错误要有受控降级，不能恢复的错误要返回上下文充分的错误信息。
- 日志不得记录截图、OCR 文本、翻译正文、API Key、Authorization 请求头、完整请求体或完整响应体。
- 图片翻译只能上传已经扁平化遮挡后的选区；本地 OCR 不上传图片。
- 程序可写状态仅位于可执行文件旁的 `data/`，不得静默改写到 AppData。
- 测试、构建和运行产物不得写入源码模块目录。

## 10. 仓库卫生与阶段验收

Git 跟踪源码、测试、CMake、清单、脚本、配置、文档和许可证；忽略 `build/`、`testing/testoutput/`、`artifacts/`、vcpkg installed tree、缓存、二进制、PDB 和运行期 `data/`。

每个阶段结束前至少完成：

1. Debug `/W4 /WX` 构建。
2. 与改动范围相称的模块测试或全量测试。
3. 检查生成物只出现在约定输出目录。
4. 同步实现进度、依赖说明和新增决策。
5. 向用户说明完成内容、验证结果、已知限制，并等待阶段审核。

性能验收目标包括：快捷键到冻结遮罩典型不超过 150 ms、交互 60 FPS、托盘空闲 CPU 约为零，以及 OCR 加载前空闲内存目标不超过 80 MiB。包体积 50 MiB 是优化目标，不是删功能或降低质量的理由。

### 自行添加界面语言

编辑运行目录中的 `resources/ui_text.json`（开发时编辑仓库同名资源并重新构建）：

```json
{
  "schemaVersion": 1,
  "languages": ["zh-CN", "en-US", "ja-JP", "fr-FR"],
  "texts": {
    "dialog.ok": {"zh-CN": "确定", "en-US": "OK", "ja-JP": "OK", "fr-FR": "OK"}
  }
}
```

这是结构示例，修改实际文件时保留其他文本键。先把新语言代码加入 `languages`，再在 `texts` 的各文本键下补充对应翻译。
下拉框只使用 `languages`，并按数组顺序显示语言代码；仅添加译文不会自动加入候选列表。
数组必须非空、成员为非空且仅包含 ASCII 字母、数字或连字符的字符串、无重复，并保留英文回退语言 `en-US`。
缺少当前语言译文时使用该文本键的英文；英文也缺少则显示 `?`。新语言可以逐步补全译文。

保存文件后，下次展开语言下拉框会重新查询列表，无需重新编译运行中的程序。
从列表移除当前语言后，下次资源读取会将运行时语言退回 `en-US`，不会自动改写用户设置。
旧版资源必须补上 `languages`；字段缺失或无效时保留最后有效资源，没有有效资源时报告不可用。

### WindowRenderer 人工窗口验收

欢迎窗口也提供独立人工用例。在仓库根目录运行以下命令，使用临时设置和模拟自启回调，不修改真实启动项：

```powershell
$env:OPEN_ST_INTERACTIVE_UI_TESTS = '1'
try {
    .\scripts\test.ps1 settings WelcomeManualTest.waits_for_user_close
} finally {
    Remove-Item Env:OPEN_ST_INTERACTIVE_UI_TESTS -ErrorAction SilentlyContinue
}
```

窗口一直等待确认、退出或关闭，默认测试跳过该用例。

`OpenST::WindowRenderer` 位于 `Common/WindowRenderer`，是独立目标；`OpenST::Common` 本身不链接该目标或 GUI 库。
需要界面的产品模块显式链接 Renderer。测试镜像为 `testing/test/Common/WindowRenderer`，模块筛选名为 `window_renderer`。
`RendererManualTest.waits_for_user_close` 默认跳过。在仓库根目录打开 PowerShell，仅在人工验收时运行：

```powershell
$env:OPEN_ST_INTERACTIVE_UI_TESTS = '1'
try {
    .\scripts\test.ps1 window_renderer RendererManualTest.waits_for_user_close
} finally {
    Remove-Item Env:OPEN_ST_INTERACTIVE_UI_TESTS -ErrorAction SilentlyContinue
}
```

窗口弹出后一直等待用户点击“确定”、按 Esc 或点击右上角关闭按钮，没有自动关闭计时器。
`finally` 在测试结束或报错后清除环境开关，避免之后的普通回归意外等待人工操作。
普通回归中的 skip 不表示人工验收通过。自动模态测试使用消息驱动关闭，另行报告。

### 欢迎、开机启动与退出清理人工验收

`SystemIntegration` 是 Application 的子模块，公开 `StartupRegistration`、`SingleInstance` 与固定启动参数解析，
不读取业务 JSON，不调用本地化或弹窗。Application 注入 `startupApplied/startupStatus` 回调给 Settings；
Renderer 的 `BindBool(id, read, change)` 只负责 checkbox 绑定，布尔草稿、条件提交和副作用重试属于 Settings。
`WelcomeWindow` 使用内嵌布局，确认后同时提交 `startup.enabled` 与 `onboarding.completed`，保存成功才操作系统。
第二实例通过 `ReadStartupLanguage()` 只读保存语言，不能初始化或改写用户设置。

自动回归可运行 `./scripts/test.ps1 system_integration`，注册表用例只操作临时 HKCU 测试键。
`SingleInstanceProcessTest.child_receiver` 仅由父用例启动为子进程，单独执行时默认跳过；不是人工测试失败。
普通回归不会写真实 Run 项、注销登录或自动确认欢迎窗口。

真实验收由用户主动启动程序并操作，建议使用单独的完整便携运行目录，先备份现有 `data/settings.json`。
复制目录仍属于同一用户实例，验收前从托盘正常退出已有 Open-ST，避免命令转发到另一目录的实例。
以下命令只读取当前启动项，验收前后各运行一次并保留结果；不要仅记“开启”，应记录值原先是否存在及完整内容：

```powershell
Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
    -Name 'Open-ST' -ErrorAction SilentlyContinue | Select-Object -Property 'Open-ST'
```

1. 首次启动显示欢迎与默认勾选自启。先按取消/X/Esc 验证不写启动项；再次运行仍显示欢迎。
2. 确认欢迎时可选择关闭自启以避免改变真实登录行为；验证完成标记保存、后续不重复欢迎。
   要验收实际自启时，由用户明确勾选并确认，检查 Run 内容为带引号的程序绝对路径及 `--startup`。
3. 设置页勾选/取消只改草稿；取消窗口不写系统。点击应用后检查 JSON 与 Run；关闭选项应立即删除入口。
   写入失败应保留可见错误和重试入口，不能显示为全部成功或自动关窗。
4. 运行同一 EXE 的普通启动、`--capture`、`--startup`：分别打开现有设置、请求一次截图、静默退出。
   欢迎期间普通启动应激活欢迎；忙状态下不积压截图。跨用户及同用户跨会话需分别在真实登录环境验证。
5. 在设置切换语言后，再验收重复启动失败/未知参数提示、托盘、设置、关于、截图错误、欢迎和清理文字。
   应用自有提示随所选语言变化；系统文件对话框与 MessageBox 的系统按钮由 Windows 语言控制。
6. 手动移动完整便携目录后运行新位置，核验只提示旧路径、不自动覆盖；明确点击修复后才更新。
   系统侧删除入口应提示不一致；任务管理器禁用后入口存在不代表实际允许登录启动。
7. 托盘普通退出应保留启动项。单独“退出并清理”确认后删除入口；勾选清理日志只删除本程序命名的日志，
   日志清理失败应保留窗口供重试或保留日志退出，不能删设置、资源或无关文件。
8. 实际注销/登录由用户自行执行；完成后将设置、自启值和测试目录恢复至验收前状态，并记录恢复结果。
   若原有值属于其他程序或无法确认归属，保留它，不以测试名义覆盖。

以上为人工操作步骤，本轮自动化不执行真实 Run 写入或登录操作。通用窗口视觉 case 的显式开关仍见上一节。
