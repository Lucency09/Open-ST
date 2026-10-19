# 托盘独立文本翻译窗口设计（已批准施工）

日期：2026-10-19。状态：需求与现状已核对，Astra 子 Agent 已参与设计复核；用户明确回复“同意方案”，批准本稿范围施工。

## 1. 需求、历史依据与本次取舍

用户要求：右键托盘图标后增加“打开翻译窗口”，用户可以把文本粘贴到输入框进行翻译；检查其他会话并尽量复用已有功能。

已查看《截图翻译软件开发12、13》的相关记录，并参考：

- [文本翻译设计](text-translation-v0.5.md)：统一客户端、有序提供方、配置快照、取消与结果身份核验。
- [翻译设置列表、文本排布与入口调整](text-translation-ui-format-revision.md)：非模态双栏窗口、唯一原文草稿、显式点击翻译、原生编辑及撤销。
- [截图与存储设置设计](capture-storage-v0.3.md)：现状核对、职责、公共组件复用、逐步施工及定向验收的组织方式。
- [开发规范](../development.md)：公共组件优先、状态唯一所有者、依赖方向和测试范围。

本稿采用“两个独立窗口实例，共用一套界面和会话实现、一个翻译客户端”。截图结果窗与托盘文本窗各保留自己的内容。两窗并存、截图隐藏恢复、首次输入焦点随本稿整体获准。

旧方案中的“先提取文字才能翻译”适用于截图入口；本稿增加直接输入入口。旧方案对截图结果窗随截图关闭的约束继续适用于截图来源。

## 2. 当前实现与实际缺口

| 已核对位置 | 当前能力 | 本次缺口 |
| --- | --- | --- |
| `App::ShowTrayMenu`、`HandleMessage`、`resource.h` | 托盘截图、设置、关于、退出等命令 | 没有独立文本入口及稳定命令 ID。 |
| `app_text.cpp` | 建立截图文字会话，借用 App 唯一 `TranslationClient` | 只从正式截图输出启动 OCR，没有空白输入入口。 |
| `CaptureTextSession` | 固定请求身份和配置修订，协调 OCR、翻译、关闭和轮询 | 初始化依赖 OCR；单一固定轮询 timer；职责名称限定截图。 |
| `TextResultWindow` | 原文编辑、只读译文、语言选择、翻译、取消、复制、过期标记 | 首次显示禁用原文并提示 OCR 准备中；没有手动输入初始化模式。 |
| `App::CloseOverlay` | 调用 `textSession_->EndCapture()` 关闭截图结果窗 | 不能让独立文本窗也走同一关闭路径。 |
| `TranslationActivity(bool)` | 根据共享客户端是否忙刷新按钮 | 不能区分“其他调用方忙”与“本窗拥有可取消的任务”。 |
| `RefreshLocalizedUi` | 刷新设置、工具栏、托盘等 | 当前未刷新文字结果窗；需接入两个实例。 |
| `WindowRenderer` | 多行编辑、绑定、原生输入、布局、DPI、模态、错误和文本刷新 | 没有按控件 ID 设置焦点的公开接口；`Show()` 不保证原文获得焦点。 |
| Application 测试及 `testing/CMakeLists.txt` | OCR-only 交互测试、OCR+Translation 交互测试、模块目标登记 | 翻译交互测试强制打开 OCR，不能验证翻译入口脱离 OCR；缺双实例和真实 App 入口测试。 |

直接给现有 OCR 窗口加菜单存在三个问题：截图关闭会丢失粘贴草稿，截图提取会只激活旧草稿，固定 timer 会让两个实例互相停止补收。施工必须覆盖这些边界。

## 3. 用户界面与行为

### 3.1 托盘入口和窗口

- 在“截图”下方增加“打开翻译窗口”，使用独立命令 ID；仅在启用 Translation 的构建中显示，与 OCR 开关无关。
- 首次打开使用现有双栏非模态窗口：左侧为空白可编辑原文，右侧为只读译文；首次显示后将焦点放入原文框。
- 用户通过原生粘贴操作输入文本；打开窗口不读取剪贴板，不提交 OCR 或翻译任务。
- 语言选择、翻译、取消、复制原文、复制译文和关闭沿用现有布局。源/目标语言初值来自同一份已保存翻译设置快照，窗口内选择仅影响本窗口。
- 手动模式使用“原文”和粘贴操作提示，不显示截图快照、正在识别等 OCR 文案。窗口标题与截图文字结果作适当区分。
- 原文为空时翻译不可用；合法输入后由用户点击“翻译”提交。输入预算、无效输入、供应商失败及过期译文遵循现有规则。
- 重复打开只激活该文本窗，保留原文、译文、语言选择、编辑选区和原生撤销历史，不重新填充或全选正文。
- 用户关闭文本窗后，本次草稿随窗口释放；下次打开为新的空白输入。本次不把正文持久化到文件。

### 3.2 与截图及设置的关系

- 截图文字结果和托盘文本窗可分别存在一个实例；截图“提取文字”继续实际执行 OCR，不激活或覆盖托盘草稿。
- 开始截图时，在冻结帧捕获前暂时隐藏已经可见的托盘文本窗，保留控件、草稿和后台任务。该窗口不进入本次截图。
- 截图期间托盘文本窗不重新显示或抢占遮罩；截图完成、取消或捕获失败清理后，恢复此前可见的窗口。恢复不激活窗口、不抢走模态窗口焦点，也不恢复被用户关闭的窗口。
- 隐藏恢复状态只由文本窗口持有；App 仅传递截图暂停/恢复意图，恢复发生在遮罩确实销毁之后。退出期间不恢复。
- 截图结束只关闭截图文字会话；独立文本窗的内容和翻译任务保留。
- 两窗与设置中的连接测试继续共用一个活动翻译任务。其他调用方忙时，本窗不能提交新任务，也不显示可取消其他调用方的按钮；无自动排队、抢占或自动重试。
- 仅当前窗口发出的翻译受其关闭、编辑、语言修改或取消影响。应用翻译配置则通知两个会话，使相关旧任务及译文失效。
- 更换界面语言只刷新界面文字，保留正文和任务资格；不得通过 `ConfigurationChanged()` 代替界面刷新。

## 4. 复用清单与职责归属

| 需求能力 | 已有接口/组件 | 实际扩展 | 唯一状态所有者 |
| --- | --- | --- | --- |
| 普通双栏窗口、编辑、复制 | `TextResultWindow`、`WindowRenderer`、`CopyTextToClipboard` | 增加手动初始化模式与对应文案；继续使用原控件和复制接口 | 每个 `TextResultWindow` 拥有自己的原文及显示译文。 |
| 翻译执行 | `EnsureTranslationClient`、`TranslationClient::Submit/Snapshot/Cancel` | 注入给第二个会话实例；保持领域协议和提供方实现 | App 拥有唯一客户端，Translation 拥有线程和真实任务状态。 |
| 会话身份和迟到结果过滤 | `CaptureTextSession` 的请求、会话和修订核验 | 更名 `TextSession`，增加独立文本入口及每实例 timer | 每个会话拥有自己的请求资格、修订和配置快照，不另存正文。 |
| OCR | `OcrClient`、`GenerateAnnotatedSelection` | OCR 客户端延迟到实际 `BeginOcr` 时创建；所有访问检查是否存在 | 仅截图文字会话拥有 OCR 客户端。 |
| 接口、代理、档位、语言 | `ReadSettingsSnapshot`、`TranslationSettingKeys`、领域配置读取 | 共用现有完整快照适配；配置应用通知两个实例 | Settings 拥有持久配置，窗口拥有临时语言选择。 |
| 截图时窗口隐藏恢复 | 现有 HWND 生命周期和 `ShowWindow` | 窗口内保存本次暂隐资格，恢复时核验仍存活 | 文本窗口拥有可见性恢复状态，App 编排截图边界。 |
| 首次焦点 | Renderer 已有控件表、可见/启用判断及内部焦点机制 | 最小通用 `FocusControl(id)` | Renderer 管理控件焦点机制，业务只提供控件 ID。 |
| 模态、全局编辑键和退出 | App 现有消息、热键、暂停和退出屏障 | 将第二实例纳入每个既有入口 | App 拥有进程和跨模块生命周期。 |

依赖保持 `Application -> Translation/OCR/Settings/Export/Common/WindowRenderer`；不增加兄弟模块依赖。
Common 只增加按 ID 设置焦点的通用能力，不认识翻译、OCR、设置键或截图来源。

## 5. 会话和接口调整

### 5.1 从 CaptureTextSession 演进为 TextSession

直接更名两份私有文件及类，更新全部实际引用，不保留旧名称兼容包装。App 现有 `textSession_` 继续表示截图来源，新增 `translationWindowSession_` 表示托盘来源，避免无关成员重命名。

| 拟调整接口 | 职责及边界 |
| --- | --- |
| 构造函数增加 `UINT_PTR pollTimer` | 借用稳定宿主、图标、设置读取回调和唯一翻译客户端；固定本实例的 timer ID。构造不建立 OCR 客户端或加载模型。 |
| `Begin` 更名 `BeginOcr` | 只供截图实例消费正式图像；按需建立 OCR 客户端，显示等待识别的窗口并提交识别。 |
| 新增 `BeginText()` | 只供托盘实例建立可编辑空草稿；窗口已存在则激活；不生成图像或调用 OCR。 |
| 私有共用窗口初始化 | 统一绑定关闭、编辑、翻译和取消回调；以来源参数决定初始文案及编辑就绪状态，不复制两套绑定代码。 |
| `EndCapture` 更名 `Close()` | 关闭被调用的会话实例，取消它拥有的 OCR/翻译发布资格；不影响另一实例。截图宿主只调用截图实例。 |
| 新增 `SetCaptureSuspended(bool)` | 将隐藏/恢复意图转交该窗口，不撤销翻译资格；App 只对托盘实例调用。 |
| 新增 `RefreshTexts()` | 刷新本地化文本及能从状态重新生成的提示，不回填原文、不修改输入/配置修订或提交任务。 |
| 原 `Poll/Process/RegisteredHotkey/Pause/Shutdown/ShutdownComplete` | 支持无 OCR 客户端的实例，保持非阻塞关闭、退出等待和已有输入核验。 |

截图与托盘实例各使用一个稳定且互不相同的轮询 timer ID，继续复用同一个 `TextWakeMessage`。App 收到唤醒、两个会话 timer 或设置连接测试 timer 时，补收两个会话。每个实例只能停止自己的 timer。

TranslationClient 的单调 request ID 仍是全局任务身份。发布继续核验 request ID、该实例会话号、原文修订、配置修订及语言；各实例会话号不需要另建全局注册表。取消仍须核验本实例拥有的 request ID 与客户端当前请求一致。

### 5.2 TextResultWindow 和通用焦点

- `Show` 接收明确的输入来源模式，手动模式直接就绪，OCR 模式继续等待首次识别；两者共用窗口布局及绑定。
- `TranslationActivity` 接收“全局 busy”和“当前窗口拥有可取消请求”两个事实，不缓存第二套后台忙状态。
- 增加窗口暂隐恢复与 `RefreshTexts` 窄接口。界面语言刷新复用 Renderer；状态提示保留可重新解析的文本键或安全状态描述，任务提示由快照生成，不能靠重填正文或推进修订实现刷新。
- 当前 `origin_` 已拼入本地化“本地/在线”，`StatusText` 也接收格式化后的字符串；本次在既有会话/窗口两文件内将显示来源保留为接口名及网络标记，状态保留资源键、分类及必要安全参数，刷新时重新格式化。状态描述不借用会失效的快照引用，不保存原文、凭据或任务忙状态副本。
- `WindowRenderer::FocusControl(std::string_view id)` 复用控件索引及可交互判断，拒绝未知、隐藏、禁用、非交互控件或忙状态；返回标准 `RendererResult`。仅操作当前可交互窗口，不激活后台窗口。
- `FocusControl` 调用 `SetFocus` 后通过 `GetFocus` 核验，不能把 `SetFocus` 返回的旧焦点当作成功值。首次手动输入初始化后调用；重复激活与截图恢复保留原焦点/选区。

### 5.3 失败和退出

窗口创建失败沿用 App 统一提示和门禁；配置读取失败保留可编辑草稿，显示配置错误并禁止使用不完整配置提交。timer 创建失败禁止提交本轮任务并显示现有不可用分类；不建立无法可靠补收的任务。

关窗立即撤销发布资格，旧工作实际结束前继续返回忙。普通退出、退出清理和析构兜底均覆盖两个实例；先停止会话，再停止 App 唯一翻译客户端，后台释放后才回收消息窗口及日志。隐藏恢复和延后消息不得在退出后重新显示窗口。

## 6. 文件级代码变更范围

以下为本稿已批准的实现范围。共 14 个生产/公共组件/文案/模块构建文件，其中两项为原文件更名；9 个测试及测试构建文件，其中 3 项新增。文档同步另列。范围内的改动必须直接服务于本文职责，遇到实质新增边界重新审核。

### 6.1 生产代码、公共组件与文案（14 项）

表内 Application 路径均以 `src/Launcher/Application/` 为根；公共组件与资源使用仓库相对路径。

| 编号 | 文件 | 具体变动与原因 |
| --- | --- | --- |
| P01 | `include/app.h` | 前置声明改为 `TextSession`；保留截图实例并增加托盘实例；声明打开入口和必要的双实例协调接口；增加 App 专项测试访问友元。 |
| P02 | `private/resource.h` | 增加稳定 `ID_TRAY_TRANSLATION`，不按菜单位置分派。 |
| P03 | `source/app.cpp` | 托盘菜单/命令；双实例消息及 timer 补收；暂停和界面语言刷新；截图前暂隐和实际清理后恢复；普通退出、退出清理、析构和退出条件均覆盖两实例。工具栏依旧只关联截图结果窗。 |
| P04 | `source/app_text.cpp` | 新增 `ShowTranslationWindow`；共用会话建立和配置读取适配；截图入口改用 `BeginOcr`；两实例借用同一 `EnsureTranslationClient`。 |
| P05 | `source/app_callbacks.cpp` | `translationSettingsApplied` 通知两个实例；设置连接测试保持当前客户端及请求身份规则。 |
| P06 | `source/app_hotkeys.cpp` | 已核验的注册编辑组合依次转交实际前台文字窗口，保留焦点、IME、禁用及消息时效检查。 |
| P07 | `private/capture_text_session.h` → `private/text_session.h` | 更名类及声明，补文本入口、独立 timer、关闭、暂隐和文字刷新接口。 |
| P08 | `source/capture_text_session.cpp` → `source/text_session.cpp` | 抽出共用窗口初始化；OCR 懒建及判空；双来源入口、timer 隔离、按钮归属、文字刷新和各自取消。 |
| P09 | `private/text_result_window.h` | 声明输入模式、全局忙/本窗取消事实、暂隐恢复及文字刷新；窗口继续独占自身正文。 |
| P10 | `source/text_result_window.cpp` | 复用双栏布局和绑定；按来源初始化文案/就绪状态；首次焦点、按钮状态、暂隐恢复和无正文覆盖的本地化刷新。 |
| P11 | `CMakeLists.txt` | 更新 P07/P08 文件名；维持既有模块链接及 OCR/Translation 独立功能开关。 |
| P12 | `resources/ui_text.json` | 新增托盘入口、手动窗口标题、原文标签、粘贴/就绪提示和必要失败文案；补齐中英日，既有共用按钮直接复用。 |
| P13 | `src/Common/WindowRenderer/include/window_renderer.h` | 增加通用 `FocusControl(id)` 声明及完整错误/焦点边界说明。 |
| P14 | `src/Common/WindowRenderer/source/window_renderer.cpp` | 复用控件表、可见/启用判断和原生焦点机制实现 P13；不加入业务判断。 |

P07/P08 的路径更名随本次真实职责演进进行，不扩展成公共产品模块。其引用更新均落在以上文件或下列测试范围。

### 6.2 测试与测试构建（9 项）

| 编号 | 文件（仓库相对路径） | 具体变动 |
| --- | --- | --- |
| T01 | `testing/test/Launcher/Application/source/text_translation_interaction_tests.cpp` | 迁移 `TextSession` 接口；增加手动输入、双实例、请求归属、timer、配置/语言刷新用例；同一源码按功能宏编译为双功能和 Translation-only 测试，OCR 用例有明确条件。 |
| T02 | `testing/test/Launcher/Application/source/ocr_interaction_tests.cpp` | 迁移文件名/接口/timer 参数，按懒建调整 OCR 创建时机断言；保留 OCR-only 验证及当前较小的自身替身。 |
| T03 | **新增** `testing/test/Launcher/Application/source/app_text_integration_tests.cpp` | 真实 App 分派与生命周期，覆盖托盘命令准入、重复打开、双窗、设置通知、注册编辑热键、截图暂隐恢复及两类退出。 |
| T04 | **新增** `testing/mock/Launcher/Application/include/text_task_fake.h` | 发布可控任务状态与计数，供会话和 App 专项测试共用；不对产品公开。 |
| T05 | **新增** `testing/mock/Launcher/Application/source/text_task_fake.cpp` | 从 T01 提取 OCR/Translation 客户端任务替身，以编译选项支持带/不带 OCR 的测试变体，避免复制整套 fake。 |
| T06 | `testing/mock/Launcher/Application/CMakeLists.txt` | 定义上述测试替身目标及必要变体，保持 mock 只进入测试链接。 |
| T07 | `testing/test/Launcher/Application/CMakeLists.txt` | 更新会话文件名和共享 fake 链接，新增 App 集成与 Translation-only 目标；Translation-only 不定义 `OPEN_ST_HAS_OCR`，也不链接 OCR 目标。复用源码的两个可执行目标使用不同 Suite 名，保持 `application.Suite.case` 规则及唯一 CTest 名。 |
| T08 | `testing/CMakeLists.txt` | 将新增可执行目标登记到 `application_targets`，使既有 `test.ps1 application <Suite.case>` 可以构建并执行。 |
| T09 | `testing/test/Common/WindowRenderer/source/renderer_edit_tests.cpp` | 验证按 ID 聚焦、拒绝隐藏/禁用/忙/无效节点、错误不会改正文/选区、不会激活后台窗口。 |

T04/T05 仅共享任务替身；窗口测试现有的文本键解析、窗口查找及剪贴板替身保持窗口测试专用。App 集成使用真实 Localization 和隔离设置，防止其 `GetUiText` 与测试假实现重复链接。复制专项用替身，不写用户剪贴板；翻译请求用 fake，不调用真实供应商。

### 6.3 文档同步（8 项）

| 文件 | 变动 |
| --- | --- |
| 本文 `docs/design/standalone-text-translation.md` | 记录完整批准范围、审核结论和最终实施结果。 |
| `docs/product_requirements.md` | 增加托盘直接输入需求；将“翻译必须从冻结帧取得输入”和 OCR 唯一入口的全称约束限定为截图来源。 |
| `docs/technical_architecture.md` | 补充两类文本入口、两个窗口实例及唯一翻译客户端的数据流/生命周期。 |
| `docs/development.md` | 同步通用会话职责、两来源与各自关闭规则；冻结帧要求明确针对截图路径。 |
| `docs/implementation_progress.md` | 记录实际完成项、定向验证和人工验收待办。 |
| `docs/decision_log.md` | 记录双实例复用、独立 timer、共享客户端和截图暂隐恢复的原因及取舍。 |
| `docs/design/text-translation-v0.5.md` | 仅补后续扩展指向，不改写历史批准范围或把本文写成已批准。 |
| `docs/design/text-translation-ui-format-revision.md` | 对历史“只能先提取文字”补后续扩展指向，保留原审核记录。 |

上述既有正式文档在获准施工后按实际实现同步，避免提前把待实现方案写成当前能力。

### 6.4 范围边界

本次复用 Translation、Local、OCR、Graphics、Export、Settings 领域能力，不修改其供应商协议、模型、推理、输入预算、配置结构或图像算法。焦点扩展使用现有 Renderer 目标，无新增产品模块或第三方依赖。

根 CMake、vcpkg 清单、构建/测试脚本、下载器、打包与发布流程均无需改动；测试目标登记在 T08 完成。已有 `.gitignore` 改动保持原样。本文不申请提交、推送、打包或发布。

## 7. 分步施工与每步完成条件

| 步骤 | 主要文件 | 完成条件 |
| --- | --- | --- |
| S1：最小通用焦点 | P13/P14、T09 | Renderer 按 ID 安全聚焦用例通过，复用现有编辑/模态边界，不改业务。 |
| S2：共用会话和双实例隔离 | P07～P11、T01/T02/T04～T07，以及 P01/P03～P06 的机械引用迁移 | 同步迁移 App 中的 include、类名、构造及方法调用，使本步可独立构建；纯文本零 OCR；独立 timer；关闭/编辑/取消不影响他窗；OCR-only 仍可用。T07 此步登记会话及 Translation-only 目标，App 新目标在 S3 随 T03 接入。 |
| S3：托盘及 App 生命周期 | P01～P06、P12、T03/T07/T08 | 真实命令到正确窗口；所有消息/设置/热键/暂停/截图/退出入口覆盖两实例。 |
| S4：定向验证和文档同步 | 第 8 节及文档范围 | Debug `/W4 /WX` 构建；定向用例通过；diff/格式/产物检查；交付可运行程序和人工检查项。 |

以上步骤属于同一次方案批准范围，按小步执行并由子 Agent 监督；新增产品行为、依赖或实质扩张文件职责时先停止相关施工并重新审核。

## 8. 验证范围和验收标准

自动验证优先运行下列 Suite/指定 case，不调用无筛选的全量入口，也不因增加 Renderer API 扩大为全部业务回归。

| 覆盖层次 | 必须验证的功能/边界 |
| --- | --- |
| Renderer 焦点 | 首次焦点、只允许可交互节点、前台边界、隐藏/禁用/忙/错误 ID；选区和正文保持。 |
| 纯文本会话 | 新窗空草稿且可粘贴；OCR 创建和提交次数为零；打开/编辑/激活均无翻译提交，明确点击才提交；空输入及配置读取失败。 |
| 两窗共存 | 提取仍实际执行 OCR，原文/译文/语言/撤销互不覆盖；窗口重复打开不清空；关窗重开不能绕过旧任务收尾。 |
| 请求和 timer | 一窗关/改/取消不影响另一窗或设置测试；迟到结果不串窗；全局忙与取消按钮归属正确；一实例停表不停止另一实例，通知丢失可补收。 |
| 配置和界面语言 | 翻译配置应用通知两窗并标过期；界面语言刷新不取消请求或修改草稿；新增文案在中英日资源可用。 |
| App 入口和输入 | 托盘稳定命令及忙状态准入；前台/焦点/IME/禁用下的全局编辑键；主循环和设置子模态共用分派。 |
| 截图和退出 | 捕获前暂隐、成功/取消/失败恢复、模态及退出不抢焦点/不复活；截图结束保留独立草稿；普通退出和退出清理均等待后台释放。 |
| 编译边界 | 默认 OCR+Translation 产品构建；OCR-only 原交互目标；新增 Translation-only 目标不引入 OCR；另外针对 App 目标检查 Translation-only、OCR-only、两个功能均关闭的编译，关闭 Translation 时无新托盘入口。 |

继续复跑既有 `OcrInteractionTest`、`TextTranslationInteractionTest` 中受本次接口/生命周期影响的用例，以及相关 App 热键、工具栏文字结果层级和退出用例。Translation、Local、HDR、模型质量的既有通过测试没有代码影响时不重复整套回归。

功能组合编译使用工作区内独立 CMake 构建目录及现有依赖缓存，不改脚本顶部固定开关、不执行 Clean。具体用例名称和实际数量以新增后 CTest 清单为准，不提前填写通过数。

人工验收由用户操作正式窗口：托盘入口、粘贴/中文输入/撤销、双栏缩放及跨 DPI、两窗共存、截图暂隐恢复、翻译期间关窗/退出。自动通过或跳过不代替真实视觉验收。

## 9. 审核记录

- 初步方案已由 Astra 子 Agent 只读复核，推荐双实例复用，指出截图关闭、OCR 提取被激活替代、固定 timer、取消按钮归属、Translation-only 和截图期间层级问题。
- 文件范围补审进一步确认 `app_callbacks.cpp`、`app_hotkeys.cpp`、`RefreshLocalizedUi`、`testing/CMakeLists.txt` 与测试 fake 链接边界必须纳入；首次焦点需要最小 Renderer 通用扩展。
- 全文只读补审已完成；已纳入产品 App 的独立功能组合编译、S2 同步机械引用迁移、来源显示保存事实、双测试目标 Suite 唯一性四项修订。子 Agent 未发现需要扩大模块范围的理由。
- 用户已批准本稿施工，范围内实现已完成；最终只读监督未发现剩余实质漏洞或超范围变更。

## 10. 实施与验证结果（2026-10-19）

实现沿本文14项生产/公共组件/文案/模块构建与9项测试文件范围完成，两份会话文件直接更名，新增3份测试文件；已有`.gitignore`保持原样。正式文档、历史方案扩展指向及D-088同步完成，无新增产品模块或依赖。

| 验证 | 结果与证据 |
| --- | --- |
| OCR、双功能、纯翻译会话 | 36项通过；`testing/testoutput/tray-session-tests.log`。 |
| Renderer焦点及直接相关编辑 | 14项最终通过；原相关编辑/边界用例见`tray-app-focus-tests.log`，六项前台焦点复测见`tray-focus-desktop-tests.log`。 |
| 真实App入口、设置、截图及退出 | 13项最终通过；首轮现有用例见`tray-app-focus-tests.log`，修复清理计时器及新增状态/恢复/热键的五项见`tray-app-new-retest.log`。 |
| 去重汇总 | 63个不同用例通过、0失败、0跳过；按每项最近结果生成`testing/testoutput/tray-verification-summary.json`，修复复跑不计为新增用例。未运行全量。 |
| 默认产品 | 正式`build.ps1 -Configuration Debug`，`/W4 /WX`通过；可运行文件为`build/Debug/src/Launcher/Open-ST.exe`。 |
| 功能组合 | `build/tray-translation-only`、`build/tray-ocr-only`、`build/tray-features-off`的`open_st_application`目标分别编译通过；独立目录、禁用manifest安装并复用现有依赖，脚本开关未改。 |

首次构建定位到沙箱内vcpkg子进程标准输入句柄`CreateFileW`返回拒绝访问5；MSVC匹配14.38环境后普通沙箱仍出现PDB子进程C1902，同工具链沙箱外成功。仅受此限制的构建及需要真实前台输入队列的焦点用例使用对应执行权限，普通会话验证仍在沙箱内完成；没有用跳过或放宽产品门禁掩盖失败。

施工中修正了私有窗口helper与Win32宏重名、前台测试夹具、测试链接缺失OCR同对象能力替身，以及模态测试使用空HWND线程timer的问题。模态夹具改为关联App消息HWND，真实退出业务和强断言保留；未因此扩大生产模块职责。

clang-format限定修改行及新增C++文件，UTF-8/CRLF、末尾换行、git diff和产物范围已核查。实际剪贴板粘贴、中文输入法、混合DPI及窗口视觉仍由用户验收；本节记录功能实现阶段；后续用户已明确授权提交、推送并发布v1.2.0，公开状态以发行页面为准。
