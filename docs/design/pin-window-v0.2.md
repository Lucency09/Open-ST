# 截图固定到屏幕方案

状态：待用户审核，尚未获准施工。日期：2026-09-09。
用户已确认将贴图前移至快捷键管理之前；初稿及本次点击聚焦、固定层级和依赖范围修订均已由子 Agent 只读复核。

## 1. 当前基础与范围

复用现有冻结帧裁切、HDR→SDR 输出及截图工具栏命令准入。当前只有取消、保存和复制，贴图尚未实现。
本阶段落实产品需求第 11 节中适用于截图的能力；图片翻译来源随翻译业务接入。

- 工具栏增加“固定到屏幕”图钉按钮，顺序为“取消 | 贴图 保存 复制”，复制仍在最右侧。
- 点击后把当前选区作为无标题栏置顶窗口显示；首次按原图物理像素大小放在原选区位置。
- 多张贴图独立存在，可拖动、缩放、调整透明度、复制、保存和关闭。
- 新贴图准备成功后结束截图；裁切、分配或窗口创建失败则保留选区和工具栏，并显示本地化错误。
- 应用退出关闭全部贴图并释放图像，重启不恢复；不自动落盘，不增加剪贴板图片导入。

## 2. 交互草案（按用户修订）

### 2.1 点击聚焦与滚轮

- 必须先用鼠标左键或右键点击贴图，使该贴图获得焦点，才允许滚轮缩放和 Ctrl+滚轮调整透明度。
- 新建、恢复显示或仅鼠标悬停都不自动获得滚轮操作资格；点击其他窗口或其他贴图后，原贴图失去资格。
- 滚轮处理同时校验点击取得的操作资格、当前前台/焦点窗口及光标是否位于该贴图可见区域。
  光标移到未聚焦的另一张贴图上滚动时，两张贴图都不变化，也不把事件转交给其他贴图。
- 菜单、保存对话框、截图暂停或鼠标穿透期间停止处理贴图滚轮；失焦、隐藏、开启穿透或关闭时清除资格，需重新点击。
- 点击和聚焦只决定操作对象，不改变贴图相对层级。

### 2.2 固定层级与手动提层

- 默认按截图先后排列：后截的贴图在上，先截的在下。普通点击、聚焦、拖动、缩放均不修改排序。
- 右键菜单增加“调整为最上层”，仅把当前贴图移到已有贴图最上方，其他贴图相对顺序不变。
- 手动调整后保留新顺序；以后新截的贴图仍追加到最上方。关闭、隐藏恢复和跨屏移动不会重置已有排序。
- 例如先截 A、B、C，则从上到下是 C/B/A；点击 A 后仍为 C/B/A；对 A 执行提层后是 A/C/B；
  再截 D 后为 D/A/C/B。这里的“最上层”指本程序贴图之间的相对位置。
- 管理器持有唯一的有序 ID 列表，焦点 ID 单独保存。窗口层级由列表生成，不用当前焦点或 HWND 枚举顺序重建业务顺序。
- Windows 激活非活动窗口会将其提层，因此在激活、显示与位置消息边界恢复既定顺序，配合重入保护及
  `SWP_NOACTIVATE`，普通移动缩放使用 `SWP_NOZORDER`。不使用定时器持续抢顶，也不改动其他程序的窗口。
  贴图之间不建立 owner 关系。菜单和模态对话框不纳入贴图列表，打开期间暂停普通层级修复，
  提层菜单命令在菜单退出后应用，确保菜单和对话框仍可操作。点击下层贴图的瞬时层级变化与闪烁须真实窗口验收。
  平台依据：[SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)。

### 2.3 其余交互

- 默认始终置顶、100% 缩放、100% 不透明；置顶指普通桌面窗口层级，不承诺覆盖安全桌面或独占全屏。
- 左键拖动。满足上述聚焦条件时，滚轮以光标所指图像位置为锚点等比缩放，建议范围 10%～500%，每格乘除 1.1；
  最终尺寸同时受有效像素尺寸和资源上限约束，不能溢出或创建零尺寸窗口。
- Ctrl+滚轮每格调整 5 个百分点的不透明度，范围 10%～100%；双击只恢复 100% 缩放。
- 100% 表示图像一个像素对应屏幕一个物理像素；跨 DPI 移动保持该比例，菜单等 UI 随 DPI 更新。
- 右键菜单提供复制、保存、调整为最上层、鼠标穿透、关闭；当前获得焦点的贴图按 Esc 关闭。
- 托盘提供“取消全部贴图鼠标穿透”和“关闭全部贴图”，穿透开启后仍有可达的恢复入口。
- 复制和保存始终使用原图，不烘焙显示缩放及透明度；PNG/JPEG 及保存目录沿用已有业务规则。
- 显示器移除或布局变化后，把完全不可达的贴图移回有效工作区；大图保证有可操作区域。

待审产品取舍：新截图开始前临时隐藏已有贴图，使它们不进入新的冻结帧，也不挡住选区。
隐藏后确认桌面合成更新再捕获；完成、取消、捕获/窗口初始化失败及异常均恢复原有可见性、位置、
透明度与穿透状态，恢复不抢焦点。这是拟新增行为，尚未实施。

## 3. 模块与资源

- 新增 `Application/PinWindow`、独立 `OpenST::PinWindow` 目标及完整镜像测试。
  模块只依赖 Common 和 Windows SDK，负责自有图像、专用窗口、绘制与交互，不直接依赖兄弟模块。
- App 拥有一个 PinWindowManager；Manager 在 PinWindow 模块内管理多贴图集合、稳定 ID、焦点和层级。
  App 将 Graphics 的裁切结果适配为模块自己的像素输入，注入文本解析和业务命令回调。
  不把贴图业务放进 Common/WindowRenderer，也不在模块中读 JSON。
- 使用已有 D3D11/DXGI/Direct2D 图形技术与原生窗口合成能力，不新增第三方依赖。
  绘制失败明确报告，设备资源可从保留的原图重建；不能把黑窗口当作成功。
- 贴图保留独立 SDR/sRGB 图像，关闭截图会话后仍有效，不长期持有整个冻结桌面。
  HDR 输入只通过现有链转换一次，贴图显示与 SDR 输出采用相同图像语义，原生 HDR 贴图另行设计。
- 现有 `SdrSelectionFrame` 为 BGRX，第四字节不是透明度。原始输出保持不变，绘制上传时将显示数据的 alpha 设为不透明；
  窗口透明度单独处理，复制保存不受影响。
- App 将工具栏 Pin 命令接入现有待处理/忙状态与代次检查。贴图复制、保存通过 App 适配已有 Export 服务，
  保持贴图存活，不套用“成功后关闭截图”的完成语义。
- 模态保存、错误提示与退出期间按稳定 ID 校验命令，并持有图像快照；关闭窗口不会使排队命令访问失效引用。
  提交命令采用异步投递，销毁前解除回调，避免同步窗口消息重入释放。

### 3.1 依赖与被依赖

箭头表示代码/链接依赖，回调不构成对回调实现模块的反向依赖：

```text
Launcher → Application → PinWindow → Common（日志）
                       → Graphics → Capture / HDR
                       → CaptureToolbar
                       → Export
                       → Settings
                       → Localization
```

| 目标 | 依赖内容与用途 | 可见性 / 被谁依赖 |
| --- | --- | --- |
| OpenST::PinWindow | open_st_project_options：C++20、警告等统一编译规则 | PUBLIC 编译规则 |
| OpenST::PinWindow | OpenST::Common：日志；不使用业务 JSON | PRIVATE；Common 不依赖 PinWindow |
| OpenST::PinWindow | Windows SDK 的 user32、gdi32：窗口、菜单、输入及必要窗口资源 | PRIVATE 系统库 |
| OpenST::PinWindow | d3d11、dxgi、d2d1：设备、交换链、图像绘制；dcomp：窗口视觉透明度；dwmapi：桌面合成同步 | PRIVATE 系统库，非新增第三方包 |
| OpenST::Application | OpenST::PinWindow：创建与关闭贴图、图像快照、隐藏恢复、托盘命令 | PRIVATE；产品代码中只有 Application 直接使用新模块 |
| PinWindow 测试目标 | OpenST::PinWindow、GTest::gtest_main 及实际使用的 SDK 测试 API | 仅测试链接；产品不依赖 GTest |

PinWindow 不包含 Graphics/Capture/HDR/Export/Settings/Localization 的头文件，也不调用 App。
公开接口仅暴露本模块图像、稳定 ID、创建参数、状态操作、文本与命令回调；DirectX 对象隐藏在实现中。
所有兄弟模块协作由 App 完成：Graphics 生成图像，App 适配为 PinWindow 输入；复制保存时 App 将贴图快照
适配为 Export::SdrImageView；文字由 App 注入 GetUiText；保存目录由 App 访问 Settings。
CaptureToolbar 只上报 Pin 命令，不依赖 PinWindow。Launcher 仍只调用 Application；链接器最终带入静态库，
不表示 Launcher 需要直接包含新模块头文件。Common/WindowRenderer 保持现有表单窗口职责。

相对初稿，本次将“App 直接持有多贴图集合”细化为“App 持有 Manager，Manager 管集合与窗口规则”。
这使焦点、排序和窗口生命周期留在同一模块，App 仅负责跨模块业务；当前仍是待审设计，尚未施工。

## 4. 拟变动文件范围

以下均为获批后的计划，不表示文件已经创建。新增头文件只有确需跨模块使用的才放入 include。

### 4.1 新模块

新增 `src/Launcher/Application/PinWindow/`：

| 文件 | 职责 |
| --- | --- |
| CMakeLists.txt | 声明独立目标、文件清单、上述依赖 |
| include/pin_window_manager.h | 对外管理器、ID、命令与回调契约；内部窗口通过 PImpl 隐藏 |
| include/pin_image.h、source/pin_image.cpp | 受校验的自有不可变图像与可持有快照，明确尺寸、步幅、BGRX 语义 |
| source/pin_window_manager.cpp | 多窗口集合、稳定排序、焦点资格、隐藏恢复、关闭与命令生命周期 |
| private/pin_window.h、source/pin_window.cpp | 单张贴图 HWND、鼠标与菜单消息、与管理器通信 |
| private/pin_renderer.h、source/pin_renderer.cpp | GPU 上传、绘制、透明度及绘制资源重建 |
| private/pin_interaction_model.h、source/pin_interaction_model.cpp | 可单测的缩放、透明度、排序与操作资格规则 |

### 4.2 现有模块接入

| 文件 | 计划变动 |
| --- | --- |
| src/Launcher/Application/CMakeLists.txt | 加载 PinWindow、PRIVATE 链接新目标，登记 app_pin.cpp |
| src/Launcher/Application/include/app.h | 前置声明 Manager、增加自有实例及贴图编排函数声明 |
| src/Launcher/Application/source/app.cpp | Pin 命令准入/分派，工具栏按钮描述，托盘菜单，截图隐藏恢复入口，退出与语言刷新 |
| src/Launcher/Application/source/app_pin.cpp（新增） | 放置 App 的贴图创建、像素适配、复制保存和错误处理，控制 app.cpp 增量 |
| src/Launcher/Application/CaptureToolbar/include/capture_toolbar.h | 追加 Pin 命令及图标枚举，保留既有 ID 值 |
| src/Launcher/Application/CaptureToolbar/source/capture_toolbar.cpp | 图钉图标绘制 |
| resources/ui_text.json | 新按钮、菜单、窗口及错误文本，中英日同步 |

复用而不计划修改：Graphics 的 SelectionOutputRenderer/SdrSelectionFrame、Capture/HDR、Export 的复制/编码接口、
Settings 存取接口、Localization、Common/WindowRenderer，以及 Application 的 save_image_dialog 与 capture_completion。
CaptureCommandGate 已接受通用整数命令，无需为 Pin 改结构；只扩展 App 中现有命令白名单和分派。
贴图复制保存调用现有导出服务与保存对话框，不直接调用会关闭截图会话的 App::CompleteSelection。
不改根 CMake、vcpkg 清单和构建脚本，也不手工同步 Debug/Release 运行资源。

### 4.3 测试与文档

- 新增 `testing/test/Launcher/Application/PinWindow/CMakeLists.txt` 及 source 下的图像、交互模型、
  窗口层级/焦点、渲染与人工窗口测试；模块筛选名拟为 `pin_window`。
- 修改 `testing/test/Launcher/Application/CMakeLists.txt` 加载镜像模块和 App 集成测试；
  新增其 `source/pin_window_integration_tests.cpp`，扩展现有 `source/capture_toolbar_integration_tests.cpp` 的 Pin 命令覆盖。
- 按图标与按钮变化扩展 `testing/test/Launcher/Application/CaptureToolbar/source/capture_toolbar_window_tests.cpp`。
  系统剪贴板、文件写入等有副作用边界用已有 fake/mock 或注入回调隔离；按实际需要在同层镜像 mock 中加文件。
- 同步本方案、product_requirements.md、technical_architecture.md、development.md、implementation_progress.md 和 decision_log.md。
  文档将明确新交互与实际完成情况，人工验收不以自动通过替代。

## 5. 粗略调用顺序

### 5.1 创建贴图

1. CaptureToolbar 图钉按钮 → App::PostToolbarCommand(Pin, token) → 消息窗口 → DispatchToolbarCommand。
2. 现有闸门检查选区稳定、忙状态、代次与重复请求 → App::PinSelection（拟新增）。
3. SelectionOutputRenderer::Render(冻结桌面, 选区) → SdrSelectionFrame；HDR 需要时在现有链中转换一次。
4. App 适配宽高/步幅/像素 → PinImage 持有独立图像 → Manager 准备隐藏窗口和渲染资源。
5. 准备成功才发布新 ID，并在排序列表顶部加入贴图 → 关闭截图覆盖层 → 恢复旧贴图并按列表显示新贴图。
   新贴图不自动取得滚轮资格；准备失败回收候选窗口、保留选区供重试。

### 5.2 聚焦、滚轮与提层

1. 点击贴图 → 窗口通知 Manager → 设置本次点击资格并确认实际焦点 → 恢复既定 Z 序，排序列表不变。
2. 滚轮消息 → 校验点击资格、焦点、光标和忙状态 → 交互模型更新缩放或透明度 → 渲染/窗口位置更新，保持排序。
3. 右键菜单“调整为最上层” → Manager 校验 ID → 仅将该 ID 移到列表顶部 → 不激活其他贴图地应用 Z 序。
4. 失焦、关闭或开始截图 → 清除操作资格；关闭还从列表删除对应 ID，其余顺序保持。

### 5.3 复制与保存

1. 贴图菜单 → 注入回调投递稳定 ID 与命令到 App 消息窗口 → 检查存活/忙状态并持有图像快照。
2. 复制：App 适配 SdrImageView → CopyImageToClipboard。
3. 保存：App 读取上次目录 → ShowSaveImageDialog → 确认后 WriteImageFile → 成功后记录目录。
4. 释放忙状态和临时快照；成功、取消或失败均保留贴图。文件成功但目录保存失败单独提示。
   模态期间通过稳定 ID 和自有快照处理关闭/退出，不缓存可能失效的裸窗口对象指针。

### 5.4 再次截图与退出

- StartCapture → Manager 保存当前可见性并清空操作资格、隐藏贴图 → 等待合成更新 → 现有桌面捕获/选区流程。
  完成、取消与所有失败出口 → 按保存的可见性和当前有效排序恢复，不自动恢复焦点资格。
- 应用退出 → 阻止新命令 → 结束截图且不再恢复贴图 → Manager 解除回调并关闭全部窗口 → 释放图像/GPU 资源。

## 6. 施工与验收

获得批准后分步实施，每步由子 Agent 监督范围和完成情况：

1. 独立图像/窗口生命周期、置顶显示及多贴图管理，补基础模型和隐藏窗口测试。
2. 工具栏与 App 接入，验证创建成功/失败、命令防重复及旧贴图隐藏恢复。
3. 拖动缩放、透明度、菜单、穿透恢复、复制保存和退出清理，补交互边界及模态重入测试。
4. Debug/Release 构建、相应模块与全量回归、文档同步及人工验收。

重点验证：未点击/仅悬停/失焦时滚轮无效，点击下层贴图后可操作但层级不变，手动提层保留其他相对顺序，
新建/隐藏恢复/关闭后的排序正确，高精度滚轮输入不串到其他贴图；多个贴图独立操作、BGRX alpha、
原图输出不随显示变化、创建失败保留选区、保存取消/失败继续操作、
旧贴图不进入新冻结帧且各出口正确恢复、负坐标与不同 DPI、显示器移除、穿透可恢复、退出全部释放。
人工测试显式启用后等待用户关闭，默认跳过；不以自动回归代替置顶、颜色、跨屏及真实保存交互验收。
本阶段不建设快捷键管理、标注或翻译功能；既有 HDR 性能与视觉待办继续保留。
