# Open-ST 技术选型与架构说明

文档状态：选型已确认，实现细节将随开发补充  
目标平台：Windows 10 22H2 / Windows 11，x64

本文同时记录已选定的目标架构和当前实现；技术选型不等于对应功能已完成。`0.1.0` 当前覆盖托盘、
语言设置、桌面冻结与自由选区、HDR 到 SDR 转换、复制与 PNG/JPEG 保存、A2 应用图标。
工具栏、窗口/UIA 自动识别、标注、贴图、OCR、翻译、欢迎页与开机自启尚未实现；相关组件职责、
数据流及降级条目为后续约束，具体状态见 [实现进度](implementation_progress.md)。

## 1. 最终技术组合

Open-ST 采用：

- C++20、MSVC v143。
- Win32。
- D3D11 + DXGI。
- Direct2D 1.1 设备上下文架构。
- 按需使用 DirectComposition。
- DirectWrite。
- WIC。
- UI Automation。
- WinHTTP。
- Tesseract + Leptonica。
- nlohmann/json。
- WRL `ComPtr` 和项目自己的 RAII 封装。
- CMake 3.28+、vcpkg manifest。
- GoogleTest + GoogleMock + CTest。

这不是把多套重复框架堆在一起。各组件位于不同层次：Win32 负责系统窗口和消息；D3D11/DXGI 管 GPU、桌面图像和呈现；Direct2D 在 GPU 图面上画二维界面；DirectWrite 画文字；WIC 读写图片；其余组件分别解决系统控件识别、网络、OCR、配置和测试。

## 2. 各组件具体负责什么

### 2.1 C++20

职责：

- 应用主体语言。
- 用 RAII 管理窗口句柄、GDI 对象、COM 初始化、GPU 资源、线程和网络句柄。
- 用标准库实现容器、字符串、文件系统、并发和异步任务边界。

选择原因：

- 用户更熟悉 C++。
- 能直接调用 Windows 原生 API，不需要大型托管运行时或 UI 框架。
- 便于细致控制启动延迟、内存、线程、GPU 资源和发布体积。

代价：

- 生命周期、线程同步、DPI、COM 和错误处理需要项目自行建立规范。
- UI 组件和设置页不会像 Qt/WPF 那样开箱即用，开发成本更高。

### 2.2 Win32

Win32 不是绘图库，而是 Windows 桌面程序与操作系统交互的基础 API。本项目用它处理：

- 创建隐藏消息窗口、截图遮罩窗口、工具栏窗口、设置窗口、结果窗口和贴图窗口。
- 消息循环、鼠标键盘输入、焦点、窗口层级和置顶行为。
- `RegisterHotKey` 全局截图快捷键。
- `Shell_NotifyIcon` 系统托盘。
- 系统剪贴板。
- 系统打开/保存文件对话框。
- 当前用户开机启动入口。
- 单实例互斥体和实例间消息。
- 显示器枚举、虚拟桌面坐标、DPI 和窗口命中测试。

为什么仍适合现代项目：

- Windows 10/11 的传统桌面应用、系统工具、游戏启动器和许多高性能客户端仍以 Win32 窗口与消息模型为底层。
- D3D、Direct2D、DirectComposition、UI Automation 等现代 Windows 图形/辅助功能 API 都能直接与 Win32 HWND 协作。
- 它是受 Windows 长期兼容性承诺保护的系统层，不需要随应用发布一套 UI 运行时。

代价：

- 控件风格、布局、无障碍、DPI 适配和状态管理需要更多自有代码。
- 原始句柄 API 容易泄漏，所以必须通过 RAII 封装，禁止裸 owning `new/delete`。

### 2.3 D3D11

Direct3D 11 在本项目中不负责画复杂 3D 场景，而是提供稳定的 GPU 设备和纹理基础：

- 创建 GPU 设备和设备上下文。
- 保存从桌面捕获得到的纹理。
- 为 Direct2D 提供可共享的 DXGI 表面。
- 支持硬件加速的合成、缩放和颜色处理。
- 为 HDR 的 FP16/scRGB 中间图面提供底层资源。

选择 D3D11 的原因：

- Windows 10/11 原生支持，驱动覆盖成熟。
- 与 DXGI Desktop Duplication、Direct2D 1.1 和 DirectComposition 协作直接。
- 对二维截图编辑器而言，抽象层次足够低，性能和资源控制足够强，但同步模型远比 D3D12 简单。
- 无需随包分发大型第三方图形运行时。

### 2.4 DXGI

DXGI 是 DirectX 图形基础设施，负责 GPU 适配器、显示输出、图面格式、交换链和桌面捕获相关衔接：

- 枚举显卡与显示输出。
- 管理 D3D11 与 Direct2D 共享的表面。
- 创建向窗口呈现的交换链。
- 获取桌面图像，处理多显示器坐标和输出边界。
- 描述 BGRA8、FP16 等像素格式以及颜色空间。
- HDR/WCG 输出优先通过 `IDXGIOutput5::DuplicateOutput1` 按 `R16G16B16A16_FLOAT`、`R10G10B10A2_UNORM`、`B8G8R8A8_UNORM` 顺序协商；只有接口缺失或明确不支持时才使用固定 BGRA8 的旧 `DuplicateOutput`。
- `AcquireNextFrame` 可能只因硬件指针变化成功返回；当前捕获路径保留 `LastPresentTime != 0` 的首帧保护，仅指针更新会释放后继续等待。这是本机直接接受首个资源出现黑屏后保留的策略，不表示所有仅指针更新返回的纹理都无效。纯黑桌面本身是合法内容，不能用“是否存在非黑像素”推断 API 成败。
- 每个显示输出在首次获取帧前建立一次 `steady_clock` 500 ms 截止时间，后续调用只使用剩余预算，不再限制三次。预算不包含设备初始化和纹理读取，也不是多屏共用的总时限；毫秒取整与系统调度可能使实际耗时略超预算。真正的接口错误立即失败，超时诊断记录显示输出与跳过的指针帧数。

每次捕获按显示输出建立一个 `CapturedOutputPlane`：复制并丢弃驱动 RowPitch padding、旋转到虚拟桌面方向、保留实际像素格式和颜色元数据，随后解除 Map 并归还 Desktop Duplication 帧。DXGI 的 `G22/P709` 在模型中明确记录为 `SdrGamma22P709`，不冒充标准 sRGB；元数据也分别记录“使用旧 DuplicateOutput”和“已知 HDR/scRGB 输出实际得到 BGRA8”两件事。`FrozenDesktopFrame` 只在所有已附着输出全部成功后一次性接管这些 plane；对外只暴露只读 span 且对象只能移动，避免半成品、悬空映射和高分辨率缓冲区隐式复制。

D3D11 与 DXGI 的关系可以理解为：D3D11 管“GPU 如何创建和处理资源”，DXGI 管“资源属于哪个适配器/显示输出、采用什么交换和呈现方式”。

### 2.5 Direct2D 1.1 设备上下文

职责：

- 绘制冻结桌面帧。
- 只在选区外绘制暗色遮罩。
- 绘制选框、八个控制点和候选窗口边界。
- 绘制截图工具栏和工具图标。
- 绘制画笔、直线、箭头、矩形、椭圆、马赛克预览、实心遮挡和其他二维标注。
- 对贴图窗口执行高质量缩放和透明度合成。

为什么名字带“1.1”：

- Direct2D 1.0 的主要对象是传统 HWND render target。
- Windows 8 引入的 Direct2D 1.1 增加了以 `ID2D1Device` / `ID2D1DeviceContext` 为核心的架构，更适合与 D3D11、DXGI 图面和效果管线共享资源。
- “1.1”是 API 架构代际名称，不代表它只适用于旧系统，也不代表存在一个必须改用的“Direct2D 12”。Windows 10/11 仍在此架构上继续扩展接口。

选择原因：

- 专为高质量硬件加速二维绘制设计，线条、几何、位图、裁切、透明合成正好匹配截图编辑器。
- 与 D3D11 共享 GPU 图面，避免频繁把完整桌面图像在 CPU/GPU 之间来回复制。
- 是 Windows 自带 API，发布体积小，系统集成自然。

代价：

- 不是完整 UI 框架；按钮状态、布局、命中测试、快捷键和无障碍需要自行实现。
- 需要正确处理设备丢失、DPI、像素坐标与 DIP 的转换。
- 截图覆盖链路统一使用虚拟桌面物理像素；交换链目标和 D2D context 显式使用 96 DPI / pixel unit mode，避免高 DPI 下把客户区像素再次当作 DIP 缩放。

当前自由选区状态机直接属于 `OpenST::Graphics`，不建立依赖同级 `capture` 的兄弟目标。状态机以虚拟桌面物理像素接收 `GetCursorPos` 输入和冻结帧边界，对外提供 `Unselected / Dragging / Selected` 三态，拖动内部再区分创建、整体移动和八控制点缩放。选区使用半开矩形；鼠标真实可达的最右/最下像素会按拖动方向吸附到 exclusive 桌面边界，保证后续裁切不会漏掉最后一行或一列。已有选区时，控制点命中优先于内部移动，选区外按下无效果；移动保持宽高并整体限制在桌面内，缩放跨越对边时翻转活动控制点。

应用层负责鼠标捕获、光标形状和分层取消：拖动中取消恢复操作前状态，已有选区时再次取消清空选区，未选择时再取消才关闭覆盖窗口。渲染器只消费按值的 `SelectionSnapshot`，集中减去冻结帧左上角，把虚拟桌面坐标转换为客户区像素。Application 每次截图经 Settings 读取 `capture.selection_border_color`，将同一份配置传入各屏渲染器；Graphics 严格解析 `#RRGGBB`，缺失或非法时使用纯黑，不依赖同级 Settings。八个控制点为 8×8 物理像素白色填充，命中区为 12×12 半开矩形。渲染器把帧内有效选区之外拆成上、下、左、右最多四个互不重叠的半开矩形，只在这些条带绘制 48% 黑色暗层，选区内部始终保持冻结帧原色。

当前覆盖预览通过 `BuildOutputPreview` 按输出准备：SDR BGRA8 保留字节，FP16/scRGB 保留 RGB 位模式（包括负值和大于 1 的高光），HDR10 RGB10A2 执行 PQ 解码和 BT.2020→线性 scRGB 转换。不再对原生 HDR 预览使用 ACES 或 HDR→SDR 曲线。每个物理显示器拥有自己的 HWND、交换链和 D2D 目标；FP16 交换链检查并显式设置 `RGB_FULL_G10_NONE_P709`，SDR 使用 BGRA8/G22/P709。

`Capture/private/display_color_info.h` 与对应 source 按 DXGI 适配器 LUID、GDI 显示源名称匹配活动 DisplayConfig 路径，再按目标适配器和 target ID 查询 SDR 白亮度；未知或克隆歧义不猜测。`graphics` 仅对 HDR 界面色和系统已转换 SDR 的兼容内容执行参考白缩放，原生捕获 RGB 不重复缩放。HDR 缺少有效参考白时明确失败；HDR 显示器仅取得 SDR 像素时在预览状态和日志中标记兼容模式，不作为原生 HDR 验收通过。

`Application/private/capture_overlay_session.h` 与对应 source 管理全部输出窗口；`App` 长期持有一个不可变 `FrozenDesktopFrame` 和一个全局 `SelectionModel`。所有窗口隐藏准备、预绘制成功后才显示；鼠标捕获保持在起始窗口，输入始终使用 `GetCursorPos` 的全局物理像素。选区变动使所有窗口重绘，轮廓使用完整全局矩形并由各目标裁剪，避免显示器拼缝出现伪边框。同步创建/显示期间的失效延迟回收，关闭时先解除全部 HWND 回调，再释放共享状态。

显示、设置和 DPI 消息取消过期会话；正常绘制前还检查 DXGI 工厂是否过期、缓存显示目标的 SDR 白亮度是否变化。Windows 10 的窗口消息不保证覆盖所有参考白变化，静止且没有通知时要等下一次绘制才会发现；当前没有额外定时轮询。裁切、剪贴板、保存以及后续 OCR 和翻译均只能消费原生冻结帧，不得反向读取临时预览。

`Graphics/SelectionOutputRenderer` 按虚拟桌面物理像素交集裁切各原生 plane，再拼接为紧凑 BGRA8 `SdrSelectionFrame`；显示器间空洞填不透明黑色，选区内来源重叠时明确失败。SDR BGRA8 直接复制原字节，SDR RGB10A2 只量化位深；原生 HDR 交给子模块 `Graphics/HDR` 的 `NativeToneMapper`，不读取覆盖窗口或重新捕获桌面。HDR 接口借用自身的 `HdrImageView`，不反向依赖 Graphics 或兄弟 Capture；颜色解码与 Windows 原生 HDR→SDR 效果链统一归该模块。原生映射的固定参数与验证记录见 [HDR 原生映射验证报告](native_hdr_tone_map_validation_report.md)。

`Application/Export` 借用自身的 `SdrImageView`，将图像编码为带 sRGB 语义的不透明 PNG/JPEG，或构造兼容的 `CF_DIB` 剪贴板数据。该模块不依赖兄弟 Graphics、Settings 或 Localization；视图适配、保存对话框与错误本地化由 Application 完成。全部编码成功后才写目标文件；本次新建文件写入失败时按句柄清理残缺文件，覆盖已有文件不承诺中途失败后原内容完整，不创建临时截图文件。

Application 以 `CopySelection` / `SaveSelection` 为统一业务入口，由 Ctrl+C/Enter 和 Ctrl+S 触发；仅接受稳定且非空的选区。私有 `CaptureCompletion` 通过同步回调协调转换、复制、选择路径、保存和记录目录。保存取消或输出失败保留选区与冻结帧并恢复焦点；成功关闭会话，目录记录失败只单独告警。忙状态阻止重复命令和选区修改；模态对话框期间布局失效只标记取消，返回后停止输出并回收，避免回调仍借用数据时销毁会话。当前不实现双击完成、工具栏或独立快捷键模块。系统保存对话框初始选择 PNG，JPEG 质量为 95；默认文件名按打开对话框时的本地时间生成 `openst-yy-mm-dd_hh-mm-ss.mmm.png`，可切换文件类型并编辑名称。格式偏好设置尚未实现。

### 2.6 DirectComposition

职责：

- 在适合的窗口中由系统合成多个视觉图层。
- 改善透明窗口、贴图透明度、窗口动画和低拷贝呈现。

它不是首版所有窗口的强制依赖。只有在 Direct2D 直接呈现无法满足透明度、撕裂或组合效果时才引入，以控制复杂度。

### 2.7 DirectWrite

职责：

- 工具栏和自绘界面的 Unicode 文本。
- 标注文字的字体选择、测量、换行和绘制。
- 正确处理中文、英文、日文和字体回退。

Direct2D 负责把图元画到目标上，DirectWrite 负责文字布局和字形；两者是互补关系。

### 2.8 WIC

Windows Imaging Component 负责：

- JPEG 编码与质量参数。
- PNG 编码。
- 需要时解码图像翻译结果。
- 像素格式转换、色彩配置文件衔接和元数据处理。

选择原因是它随 Windows 提供、与 COM 和 Direct2D 配合自然，不需要为首版额外引入一个完整图片库。

### 2.9 UI Automation

窗口矩形检测只能得到顶层或子窗口边界，现代应用中的按钮、列表项等控件未必各自拥有 HWND。UI Automation 用于：

- 在光标位置查找可访问性元素。
- 读取其屏幕边界。
- 把按钮、输入框、列表项等作为候选截图区域。

它必须是可降级能力：目标程序不暴露 UIA、调用超时或边界异常时，退回顶层窗口检测或自由选区，不能卡住截图界面。

### 2.10 WinHTTP

职责：

- 调用文本和图片翻译 API。
- 使用 Windows 系统代理或项目设置的直连/自定义代理。
- 使用系统 TLS 和证书校验。
- 实现连接/总超时、取消和响应大小限制。

选择原因：

- 是 Windows 自带网络栈，不增加 libcurl/OpenSSL 等发布依赖。
- 能遵循 Windows 证书和代理行为。
- 足够覆盖 REST/JSON 和图片表单请求。

代价：

- 异步状态机和取消逻辑较底层。
- 各翻译供应商的签名、限流、错误码仍需要单独实现。

### 2.11 Tesseract 与 Leptonica

- Tesseract：OCR 引擎，负责版面分析和文字识别。
- Leptonica：为 Tesseract 提供图像预处理和底层图像操作。

选择原因：

- 可完全本地工作，满足截图不上传的隐私要求。
- 中文、英文、日文模型成熟，并提供 fast/best 两组官方模型。
- C++ 可直接集成，无需在用户机器部署 Python 环境。

代价：

- 模型文件会显著增加发布体积；这也是 50 MiB 只能作为目标而非硬限制的主要原因之一。
- 首次加载模型较慢且占用内存，需要后台加载、缓存和取消策略。
- OCR 结果质量受缩放、字体、背景和语言组合影响，需要图像预处理与可编辑结果窗口。

### 2.12 nlohmann/json

用于：

- 读写 `data/settings.json`。
- 构造和解析翻译 API 的 JSON。
- 对设置做版本迁移与默认值合并。

它是头文件库，使用直观；但仍需避免把 API Key 或完整请求对象输出到日志。

### 2.13 WRL `ComPtr` 与 RAII

Windows 图形、WIC、UI Automation 和部分系统 API 基于 COM。`ComPtr` 用于自动管理 `AddRef/Release`。项目自有 RAII 封装负责 `HANDLE`、`HICON`、`HBITMAP`、注册热键等非 COM 资源。

首版不强制引入 WIL，避免在尚未需要其大量辅助能力时扩大依赖。若后续引入，必须通过单独决策记录说明收益。

### 2.14 CMake 与 vcpkg

CMake 负责：

- 定义应用、库和测试目标。
- 统一 MSVC 编译选项和 Windows SDK 要求。
- 按所选 CMake generator 生成构建系统。当前脚本和 preset 暂时使用 Visual Studio 随附的 Ninja，但这不是已确认的永久架构约束。
- 生成版本和项目主页常量。
- 安装与打包规则。

vcpkg manifest 负责声明和锁定 Tesseract、Leptonica、nlohmann/json、GoogleTest 等第三方依赖。产品与测试脚本都优先通过 `VCPKG_ROOT` 定位用户已有的 vcpkg，未设置时使用当前 Visual Studio 随附的 vcpkg。脚本与 preset 的工具路径仍与开发机安装位置绑定，其他机器需先调整；项目不会静默安装全局工具。

`build/`、vcpkg installed tree、模型缓存和 `artifacts/` 都不进入 Git。Debug 在 `build/Debug/` 保留完整增量构建树；Release 在 `build/.release-work/` 完成编译后，将 EXE、同目录运行时 DLL、资源和许可证整理到 `build/Release/`，成功后删除临时工作树。`-Package` 只把这份已修剪的运行环境复制到版本化 `artifacts/`，不得携带 CMakeFiles、Ninja 元数据、OBJ、LIB 或 PDB。发布目录中的 DLL 必须来自固定依赖和可追踪来源。nlohmann/json 已是基础产品依赖，因此产品与测试构建都始终进入 manifest mode。

### 2.15 GoogleTest、GoogleMock 与 CTest

- GoogleTest：单元测试和断言。
- GoogleMock：模拟翻译供应商、HTTP、文件系统/系统边界，避免测试调用真实付费接口。
- CTest：由 CMake 统一发现和运行测试，输出失败详情并用于以后接入 CI。
- gTest 目标使用 `gtest_main` 和 `gtest_discover_tests()`；平台不满足的集成 case 通过显式前置检查和 `GTEST_SKIP()` 报告跳过。

选择它们的直接原因是用户更熟悉 GoogleTest，同时该组合适合 C++ 和 CMake 项目。

### 2.16 项目自有同步日志

Logger 位于 `common` 公共模块，只依赖 C++20 标准库和 Windows SDK，不引入第三方日志库。`common` 是产品依赖树唯一的跨层例外，并禁止依赖任何产品模块。进程内所有写入由一个互斥量串行化，直接写入可执行文件旁的 `data/logs/`，不创建异步线程，也不改变托盘空闲时的线程模型。

- 业务模块只包含公开头 `include/log.h`，通过 5 个日志宏及初始化/关闭函数使用日志；`Logger`、`LogOptions` 和测试目录注入仅在 `private/logger.h` 中可见。
- Debug 构建的最低级别为 Debug；Release 的最低级别为 Info。Debug 宏由 CMake 配置宏控制，在 Release 预处理阶段完整消失，参数表达式不会求值。
- 每行以本地 `YYYY-MM-DD HH:mm:ss.SSS` 开头；Debug 宏通过 `std::source_location` 注入相对项目根目录的源码路径和行号，Release 宏不采集、不格式化源码位置。
- 每条记录规范化为单个物理行。文件名包含创建时刻的本地毫秒时间戳；默认达到 10000 行或 2 MiB 中任一上限即按轮换当刻的新时间戳创建下一份，并在启动和轮换时把项目日志收敛到最近 3 个。
- 初始化会扫描已有项目日志、统计当天最后一个时间戳文件的实际行数和字节数；未满则追加，已满则按当前时间戳创建新文件，仅同毫秒命名碰撞时追加序号。
- Error/Fatal 写入后立即刷新；普通记录使用 `std::ofstream` 缓冲。任何目录、流、分配、轮换或清理异常都限制在日志模块内部，不向截图、OCR、翻译等调用方传播。
- 调用点只记录阶段、结果类别和必要的数值错误码；禁止把截图内容、OCR/翻译正文、API Key、Authorization 请求头或完整响应体交给日志接口。

### 2.17 通用 JSON 文件管理

`Common` 中的 `JsonFileManager` 是进程内单例。业务模块自行决定卡名、路径和内容，取得 `JsonFileHandle`；管理器规范化路径，不维护业务卡名、文件路径或 JSON key 清单。同名同路径复用一份强持有状态，同名异路径或异名同路径均拒绝。正常业务只释放自身句柄，管理器保留状态至进程退出；仅测试通过私有辅助入口清理绑定，以模拟冷启动和隔离用例。

句柄只公开 `IsValid()`、`Read(json&)` 和 `Write(...)` 三个操作名。取得句柄不访问磁盘；实际读写按文件编号、大小和最后写入时间检查变化，未变化时复用内部解析结果，变化后执行读前/读后签名一致性校验。语法无效仍保留最后有效缓存并抑制重复解析，文件恢复后重新加载，I/O 瞬时失败在下一请求重试。缓存指针、内部状态和修订号均不对业务公开。

`Read` 将可用内容复制给业务，当前读取失败返回 `false` 且输出不变，不用旧缓存冒充成功。`Write(const json&)` 完整写入；`Write(const JsonDocumentEditor&)` 在同文件锁内读改写，编辑参数为 `optional<json>&`，仅目标缺失时为 `nullopt`；损坏或 I/O 失败时不调用编辑器。编辑器只执行一次，返回 `false` 取消，不得重入同文件操作。完整文档的独立 Read/Write 不构成跨调用事务。

写入保留提交前冲突检查、仅缺失时安全创建、同目录临时文件和安全替换；成功才发布新缓存。已有文档与目标相同时仍执行原有权限探测及验签，但不重写目标文件，以保留启动只读告警。不同文件使用独立锁，正常锁竞争等待而不报错；外部程序不受本进程锁约束。Common 只解释 JSON 语法和通用文件错误，具体 schema、默认值和字段修改属于业务。异常在内部捕获并记录非敏感详细日志，对外返回 `false`，UI 通过本地化显示粗略告警，不展示底层诊断原文。

### 2.18 设置存储与设置窗口

`Launcher/Application/Settings` 同时拥有设置业务与设置窗口，不再将业务存储放入 Common。`include/settings.h`、`source/settings.cpp`、`private/settings_internal.h` 分别提供动态设置接口、实现和测试目录注入；`OpenST::Settings` 与保留的 `OpenST::SettingsWindow` 指向同一静态库，不新增子模块。

Settings 通过 `settings.user` 和 `settings.default` 两个动态卡名复用通用 JSON 句柄。初始化读取默认资源，再用编辑 Write 仅在 `data/settings.json` 缺失时填入 `resources/default_settings.json`；已有文档仅校验业务结构而不修改内容，写入返回值用于持久化告警。损坏或暂时不可读的用户文件不会被默认值覆盖；原文件权限和同目录临时探针检查已归入 Common。`GetStringSetting`、`GetBoolSetting`、`GetIntegerSetting` 及对应 setter 接受动态 key；用户值不可用时由 Settings 显式读取默认文档，编辑写入保留未知字段。文件路径、JSON 结构和键名不变，无需用户数据迁移。

设置窗口为非模态、单实例 Win32 窗口。Application 注入窄回调，转发文本、语言列表查询与保存完成通知，Settings 不直接依赖同级 Localization。语言下拉框在窗口打开和每次展开时动态重建，直接显示资源中的语言代码；取消、Escape 和关闭按钮不写配置，确定按钮先持久化、成功后通知 Application 切换运行时语言并刷新界面，失败显示粗略本地化告警。Windows 11 通过 `DWMWA_WINDOW_CORNER_PREFERENCE/DWMWCP_ROUND` 请求系统圆角，旧系统自然退回标准方角。控件以 96 DPI 为基准，首次创建按主显示器 DPI 缩放；`WM_DPICHANGED` 采用建议矩形，按目标 DPI 重排控件、重建系统字体。

A2 应用图标由 Launcher 的 RC 从 `resources/icons/open-st.ico` 嵌入 EXE，运行期不读取外部图标路径。App 独立加载并拥有大小 HICON，托盘与 Settings 借用；Settings 通过注入句柄设置窗口图标，在窗口关闭和托盘移除后由 App 销毁。资源加载失败时记录警告，托盘借用系统默认图标。当前尚无跨 DPI 动态图标重载机制。

### 2.19 本地化文本模块

`Launcher/Application/Localization` 只持有 Common 返回的 `localization.ui_text` 句柄，不再读取 `settings.json`。默认运行时语言是 `en-US`，App 从设置接口取得 `ui.language` 后显式调用 `SetUiLanguage`。

业务层唯一的取文入口是 `GetUiText("text.key", ...)`；不能直接依赖 Localization 的同级模块，经 Application 注入的窄回调转发到该入口。调用处直接提交 JSON key，不维护 C++ 枚举或硬编码 key 映射；带参数文本使用受控的命名占位符替换。底层诊断内容不得绕过该入口直接显示给用户。

每次 `GetUiText` 和语言列表查询通过句柄按需读取资源，无目录监听线程。模块锁串行完成读取、业务结构校验与接受内容，避免旧结果覆盖新结果，不依赖 Common 的版本号或缓存指针。语言代码从文本属性取并集、去重和稳定排序；缺当前译文回退 `en-US`，仍缺返回 `?`。读取失败或业务结构无效时继续使用已生效业务文本，Common 本次读取仍报告失败；连续结构故障日志去重，不保存第二份拒绝文档。当前语言从新资源中消失时退回 `en-US`，不擅自改写设置文件。关闭清空业务文本；重新初始化时底层旧缓存不能冒充成功读取。

Settings 和 Localization 分别记录去重的运行期读取告警，经 `ConsumeSettingsReadWarning` 与 `ConsumeUiTextReadWarning` 一次性消费；持续故障不会反复提示，读取恢复后可再次报告，关闭清空。Application 在消息处理结束后合并显示 `common_resources.read_failed`，截图会话期间保留待提示状态，关闭会话后再显示；不在 getter 中弹窗，也不新增后台监听或公共事件框架。

启动时资源文件本身无法加载，应用停止启动并显示一条最小三语紧急信息；这是唯一允许的用户可见文本硬编码。运行中读取失败则保留已生效文本并按上述方式提示。

## 3. 为什么不选替代方案

### 3.1 Qt

优点：

- 控件、布局、设置页、多语言、网络和绘制能力完整。
- 用户有少量实际经验，开发界面会更快。

未采用原因：

- Windows 发布需携带 Qt 运行库和插件，基础包体积通常明显高于纯系统 API 方案。
- Open-ST 不需要跨平台；Qt 的跨平台抽象在此项目中收益有限。
- 截图、系统托盘、快捷键、窗口命中、HDR 和底层图形仍会大量触达 Windows 原生 API。

结论不是“Qt 不好”，而是本项目接受更高开发成本来换取更小、更原生、依赖更少的发布形态。

### 3.2 SDL

优点：

- 窗口、输入、渲染循环轻量，适合游戏或跨平台图形应用。

未采用原因：

- 它不是桌面控件框架。
- 设置窗口、托盘、全局快捷键、剪贴板、UI Automation、文件对话框和开机启动仍需大量 Win32 代码。
- 引入 SDL 不会消除 Windows 专用层，反而可能形成两套窗口/输入抽象。

### 3.3 WPF / .NET

优点：

- 设置页和数据绑定开发效率高。
- Windows 桌面生态成熟。

未采用原因：

- 用户没有相关开发经验，而 C++ 更熟悉。
- 核心截图、D3D/DXGI、HDR 和低延迟遮罩仍需要原生互操作。
- 用户希望控制运行时依赖和发布形态。

### 3.4 D3D12

优点：

- 更直接控制命令队列、同步、描述符和显存。
- 在大型渲染器中可以降低驱动开销。

未采用原因：

- 本项目是二维截图编辑器，渲染负载不足以体现 D3D12 的主要优势。
- 资源状态、队列同步、帧生命周期和描述符管理会显著增加代码与维护成本。
- Direct2D 的常用设备互操作路径以 D3D11/DXGI 为主。

D3D11 能满足 60 FPS、HDR 图面和低延迟呈现目标，因此首版不使用 D3D12。

### 3.5 纯 GDI/GDI+

它们可快速完成基本截图与绘制，但对高 DPI、GPU 合成、透明效果、HDR、复杂标注和持续 60 FPS 的扩展能力不如 D3D11 + Direct2D。GDI 可作为极端兼容或诊断路径，但不作为主渲染架构。

## 4. 关键数据流

### 4.1 一次截图会话

以下为完整目标流程；当前实现自由矩形选区、复制和保存，冻结指针、窗口/UIA 命中、标注、贴图、OCR 与翻译尚未接入。

1. 全局快捷键或托盘命令到达隐藏消息窗口。
2. 在任何遮罩窗口出现之前枚举显示器，通过 `DuplicateOutput1` 优先捕获各输出的原生 SDR/HDR plane；全部成功后原子发布 `FrozenDesktopFrame`。
3. 如指针模式为 `FROZEN`，同时保存指针位图、热点和位置。
4. 从每个原生输出派生 BGRA8 或 FP16/scRGB 呈现缓冲，各屏隐藏窗口上传 GPU、预绘制全部成功后统一显示，共享同一全局选区。
5. 用户通过窗口/UIA 命中或自由拖动形成选区。
6. 只在当前选区之外画暗色遮罩。
7. 标注以独立内存对象叠加显示。
8. 保存、复制、贴图、OCR 和翻译从不可变帧裁切，再合成标注与可选冻结指针。
9. 普通输出走 WIC；OCR 走本地 Tesseract；翻译走供应商接口。

### 4.2 文本翻译（规划）

选区图像 → 本地预处理 → 本地 OCR → 用户可修改文字 → 只上传文字 → 翻译结果窗口。

### 4.3 图片翻译（规划）

选区图像 → 合成普通标注 → 扁平化马赛克和实心遮挡 → 首次上传隐私确认 → 上传处理后的图片 → 结果窗口/贴图。

## 5. 线程边界

- UI 线程：Win32 消息、窗口状态、选区与绘制命令提交。
- 当前复制/保存及 HDR 转换同步运行于 UI 线程；进程初始化 STA COM，原生转换器在同线程预热并复用设备，输出完成后释放中间图像。模态消息循环由应用忙状态和延迟回收保护。
- 捕获/图形资源主要与创建它们的设备和线程模型保持一致，不能在未知线程随意访问。
- 后续 OCR 必须在线程池或专用工作线程执行，可取消，结果通过消息投递回 UI。
- 后续 HTTP 请求必须在后台执行，可取消；任何供应商回调都不能直接操作窗口。
- 设置写入使用临时文件加原子替换，避免崩溃留下半份 JSON。

具体并发实现需另写设计，但不得让 OCR 或网络阻塞 UI。

## 6. 错误与降级原则

- UI Automation 不可用：降级为窗口检测或自由选区。
- 某显示输出捕获失败：报告具体阶段和 HRESULT；只有高色深接口缺失或明确不支持时才进入旧 SDR 兼容后端，访问拒绝、设备丢失等错误不得被兼容降级掩盖，也不得发布其他输出组成的部分帧。
- GPU 设备丢失：销毁并重建设备相关资源，保留能安全保留的编辑状态。
- OCR 模型缺失或哈希不符：禁止加载并提示重新构建/恢复模型。
- 快捷键冲突：保留旧的可用注册，要求用户另选组合。
- 网络超时/取消：明确区分，不自动重复可能计费的请求。
- 程序目录不可写：提示便携模式目录权限问题，不改写到 AppData。

## 7. 体积判断

原生 Win32、D3D11、Direct2D、DirectWrite、WIC、WinHTTP 都由 Windows 提供，不随应用复制，因此主程序与自有代码可保持较小。

发布体积的主要来源预计是：

- Tesseract/Leptonica 及其依赖。
- 六份 OCR 语言模型（fast/best × 三种语言）。
- MSVC 运行时的选择与第三方库链接方式。
- 调试符号不进入普通发布包。

因此 50 MiB 可作为持续测量的优化目标，但在同时打包 fast/best 中英日模型的前提下可能超出。若超出，应先给出组成报告，再讨论模型按需下载、独立语言包等方案；不能未经确认删除已同意的功能。

## 8. 维护性约束

- 源码目录以 `Launcher/Application/Graphics/Capture` 的嵌套关系表达依赖方向；上级模块只依赖子孙模块，公共能力只通过独立 `common` 例外提供。
- `FrozenDesktopFrame`、各输出原生 plane 和虚拟桌面物理像素几何属于 `capture` 的公开数据契约，不作为无业务语义的全局工具类型；逐输出 BGRA8/FP16 呈现缓冲属于父级 `graphics` 的派生数据。
- Graphics 依赖子模块 Capture 与 HDR；Application 依赖子模块 Graphics、Export、Settings、Localization。HDR 与 Export 各自公开窄像素视图，不跨兄弟模块借用业务类型；Common 仍是唯一允许的跨层依赖例外。
- 核心逻辑与 Win32 窗口过程分离，避免所有代码集中在 `WndProc`。
- 捕获、绘制、OCR、翻译、设置、贴图通过接口边界解耦。
- Windows 和第三方头文件按外部头处理，不降低项目自身 `/W4` 要求。
- 默认 `/WX`；临时调试可用 `build.ps1 -AllowWarnings`，不需要手改 CMake。
- 依赖用 manifest 和基线固定；模型用 URL + SHA-256 固定。
- 所有第三方组件在 `THIRD_PARTY_NOTICES.txt` 列明，许可证原文放入 `licenses/` 并随发布包分发。
