# 0.2.0 Settings JSON 窗口与绑定方案

状态：用户已批准，首期实现与自动验证完成，待用户验收。日期：2026-09-08。

本稿替代 settings-framework-v0.2.md，首期已经批准并实施；第 13 节记录 2026-09-09 批准的通用化修订。

## 1. 目标和边界

主要服务 Settings：用 JSON 调整页面、分组与排列，用 C++ 按控件 ID 绑定数据和事件。
兼顾迁移：Renderer 不依赖 Settings、Common、Localization、Open-ST 文件路径或日志宏。
首期保留 Win32 原生控件、系统字体、图标、非模态单窗口行为。
不实现浏览器、脚本、通用插件、实时布局重载、独立安装包或任意布局引擎。
新增交互控件需补充 C++ 绑定；移动现有控件、调整间距和页面只改 JSON。

## 2. 职责与依赖

- Settings 通过 Common 读取布局、默认和用户 JSON，决定路径、业务规则与保存行为。
- SettingsWindow 编排布局加载、绑定、显示、按钮状态和 Application 回调。
- Settings 私有 EditSession 管理原始基线、显示基线、草稿和提交结果。
- Common/WindowRenderer 独立子目标解析布局，创建控件，处理布局、焦点及事件分派。
- Application 连接 Localization，执行保存后的语言应用并刷新托盘等现有界面。
- Common 的文件接口及 Localization 的职责保持不变。

依赖方向：Settings/Application -> WindowRenderer；Settings -> Common；Application -> Settings/Localization。
OpenST::Common 本身不链接 Renderer 或 GUI；Common 子树独立目标允许产品按需直接链接。
Renderer 只依赖标准库、Win32 和 nlohmann/json，namespace 继续使用 open_st。
现行目标 OpenST::WindowRenderer；其自身维护 include/private/source 和 CMake。
沿用项目命名空间；Common 跨层例外已明确涵盖其独立通用子目标。首期不保证独立 CMake 安装包；迁移需要提供构建接入和宿主回调。

## 3. JSON 协议

布局文件为 resources/setting_windows.json，由 Settings 使用 Common 具名句柄读取。
布局不包含设置值，不写 bind、optionsSource、action；业务映射仅在 Settings 代码中维护。

```json
{
  "schemaVersion": 1,
  "window": {
    "titleKey": "settings.title",
    "initialSize": [600, 360],
    "minSize": [480, 280],
    "resizable": true
  },
  "pages": [
    {
      "id": "general",
      "titleKey": "settings.general",
      "content": {
        "type": "column",
        "id": "generalContent",
        "padding": 16,
        "gap": 12,
        "children": [
          {
            "type": "select",
            "id": "languageSelector",
            "labelKey": "settings.language.label",
            "width": "fill"
          }
        ]
      }
    }
  ],
  "footer": {
    "leading": [
      { "type": "button", "id": "restoreDefaultsButton", "textKey": "settings.restore_page_defaults" }
    ],
    "trailing": [
      { "type": "button", "id": "acceptButton", "textKey": "settings.accept" },
      { "type": "button", "id": "cancelButton", "textKey": "settings.cancel" },
      { "type": "button", "id": "applyButton", "textKey": "settings.apply" }
    ]
  }
}
```

文本键为提议命名，实施时与已有资源对齐，不维护第二份 C++ 文本键注册表。
schemaVersion 是布局协议版本，与应用版本无关。尺寸使用 DIP，window 尺寸指客户区。
pages 自动形成顶部标签，数组顺序决定显示顺序。首期只有常规语言页，不放空业务页。
column 垂直排列，padding 是四边留白，gap 是相邻元素间距；select 标签位于输入框上方。
width 支持 auto、fill 或正数 DIP；默认 fill。text 使用 textKey，自动换行。
footer 固定，leading 左对齐、trailing 右对齐；小空间允许按钮区域换行，页面垂直滚动。
首期节点仅 column、text、select、button，页面容器自动提供滚动；不开放任意嵌套 scroll/tabs。
全部 ID（含页面和容器）唯一且区分大小写。字段类型、未知属性、未知节点、版本、尺寸必须校验。
建议初期上限为深度 32、节点 2048；文档规模错误在创建 HWND 前返回，不假称可限制 Common 的解析前内存。
错误提供错误码、JSON 路径和 ID；不记录字段值或整个文档。

## 4. Renderer 接口与生命周期

接口草案，施工可调整参数封装，但不得改变职责和错误语义：

| 接口 | 契约 |
| --- | --- |
| LoadLayout(const nlohmann::json&) | 解析、校验并复制为内部布局树，不保留调用方文档引用；失败不发布半成品。 |
| SetTextResolver(callback) | 统一文本查询，所有标题、标签、按钮都使用此入口。 |
| BindString(id, read, change) | 给支持字符串值的控件绑定草稿读取与修改回调。 |
| BindOptions(id, query) | 为 select 绑定动态选项查询。 |
| BindAction(id, callback) | 为 button 绑定操作。 |
| SetCloseHandler(callback) | 将右上角关闭和未被控件消费的 Esc 转交 Settings。 |
| SetErrorHandler(callback) | 将控件事件异常或失效值按结构化错误交给宿主本地化，避免仅有调试输出。 |
| SetDefaultAction(id) | 设置未被控件消费的 Enter 对应按钮，不依赖固定按钮 ID。 |
| ValidateBindings() | 检查控件必需绑定、文本解析器及窗口级处理入口。 |
| Show(windowOptions) | 校验通过后创建和显示窗口，失败清理本次 HWND/资源。 |
| RefreshValues()/RefreshTexts() | 重读草稿或文本，程序刷新不触发 change；文本变化重新布局。 |
| SetEnabled(id, bool)/SetBusy(bool) | Settings 控制按钮可用及提交期间交互，避免业务规则进入 Renderer。 |
| GetActivePageId() | 为恢复本页默认提供当前页身份。 |
| GetControlPageId(id) | 返回控件实际所在页面，移动控件后恢复默认范围随布局变化。 |
| SetFieldError(id, text)/SetStatus(text) | 展示宿主本地化错误与状态；错误区域参与布局。 |
| RequestClose() | 延迟到当前事件分派完成后关闭，避免回调销毁自身。 |
| ProcessDialogMessage(MSG&) | 接入现有非模态消息导航。 |

所有会失败的注册/更新操作返回结构化结果，不能静默忽略错误。
回调用 std::function，允许普通函数指针、成员适配及捕获 lambda；首期不做反射或任意函数签名。
Read 返回成功/失败及字符串值；Change 返回接受/拒绝和可显示错误；Options 返回成功/失败及稳定值/显示文字列表。
首期只有 select：拒绝修改时恢复上一份已接受草稿选择；草稿值不在选项中时显示无效选择，不自动改选第一项。
Options 成功空列表与失败区分处理，两者都不允许提交不可用语言。动态查询失败保留草稿意图。
未知 ID、不兼容控件、空回调、同类重复绑定均失败；同一 select 的数据与选项绑定合法。
每个 select 必须有数据和选项绑定，每个 button 必须有动作；纯展示节点无需业务绑定。
运行中不更换布局或绑定。重复打开由 SettingsWindow 激活现有窗口，保留草稿。
所有窗口与回调在 UI 线程执行；回调异常在边界转换为失败，不穿过 Win32 窗口过程。
SettingsWindow 持有 Renderer 和 EditSession，保证被捕获对象活到 Renderer 停止分派并释放回调之后。
HICON 是借用资源，字体/子窗口等由 Renderer 按明确所有权释放；窗口关闭不结束应用消息循环。
Renderer 公共头暴露 nlohmann::json，因此 JSON CMake 依赖为 PUBLIC。这是复用现有基础依赖的明确取舍。

## 5. Settings 绑定方式

加载布局后显式建立以下关系，不把设置键作为控件 ID 的隐式解释：

| 控件 ID | 绑定 |
| --- | --- |
| languageSelector | BindString：读/改 EditSession 的 ui.language；BindOptions：查询 Application 提供的语言列表。 |
| applyButton | ApplyChanges，保持窗口。 |
| acceptButton | 同一提交入口，全部成功才关闭；无修改无待生效时直接关闭。 |
| cancelButton | 丢弃未提交草稿，RequestClose。 |
| restoreDefaultsButton | 确认后恢复当前页对应字段的草稿，再 RefreshValues。 |

控件 ID 是 UI 身份，设置键是业务数据身份；两者映射由这段代码明确维护。
首期不自动扫描任意设置键注册，不建设完整设置键注册表。
新增展示节点和重新排列只改 JSON；新增交互节点或改 ID 必须同步绑定，否则打开前失败。
布局加载失败由 Settings 通过本地化报告，不显示残缺新窗口，不破坏已经打开的窗口。

## 6. 草稿、保存和恢复

保留公开 Get/Set*Setting 接口；Settings 内部增加私有编辑入口，不新增公开 JSON 快照。
打开时分别保留原始存在性/类型/值、有效显示基线、当前草稿。仅使用 getter 默认回退不足以建立原始基线。
用户文件读取失败禁用提交，重试成功才建基线；不得把默认值冒充可靠用户数据。
修改控件仅更新草稿；改回显示基线视为无修改。默认回退本身不使字段成为 dirty。
恢复本页默认只涉及本页已绑定业务字段，需要确认，默认资源无效则保持草稿不变。
提交步骤：锁外完成校验 -> 一次 Common Write 编辑回调 -> 锁内检查最新结构和已改字段 -> 更新差异字段。
最新原始值等于基线或目标才允许写入，否则冲突；比较包含存在性和类型，保留其他最新字段。
用户文档删除、损坏或不可写时不覆盖重建。Common 的既有文件语义保持不变，不宣称跨进程全局事务。
不能循环调用单键 Set 代替一次提交。写失败不更新基线，不触发运行时应用。
冲突保留草稿，提供确认丢弃草稿后重新加载的宿主操作；不自动强制覆盖。
取消/Esc/关闭丢弃未提交草稿，不撤销之前应用成功的值，不另追问。
恢复默认确认或提交期间保持忙状态，防止重复提交及关闭重入。

## 7. Localization 联动

沿用 Application 注入的 text/currentLanguage/availableLanguages 链路，Renderer 不引用 Localization。
SetTextResolver 转发现有 GetUiText。语言列表仍动态发现，不硬编码语言枚举。
打开、展开选项和提交前检查可用语言。原选择消失则保留其意图并报错，不自动保存另一个语言。
沿用既有最后有效本地化快照规则；不能仅以兜底 en-US 证明资源可用。
保存成功后 SettingsWindow 调用 Application 的语言应用回调，回调由 void 改为可报告结果。
Application 调用 SetUiLanguage 并刷新托盘等已有界面，返回后 SettingsWindow RefreshTexts，按实际语言显示。
刷新不重建编辑会话，保留焦点、选中页、草稿和有效滚动位置。
保存失败不切换语言；保存成功但生效失败显示独立状态，保留待应用目标和窗口。
重试生效前核对持久化值仍为目标且语言可用；外部更改走冲突，未更改只重试生效不重复写盘。
取消不能撤销已保存结果。无修改且无待生效时确定直接关闭，不因已存失效语言偷偷写配置。

## 8. 布局、输入和错误

Renderer 集中负责 DIP/像素转换、字体测量、纵向布局和滚动。DPI/文本变化重新测量。
窗口按当前操作显示器工作区定位；最小设计尺寸放不下时优先保证按钮可达和页面滚动。
Tab/Shift+Tab、页签键盘切换正常；下拉框展开时 Enter/Esc 先交控件，不误保存或关窗。
只读状态变化不触发写回。错误使用宿主本地化文本，详细结构错误仅记录定位信息。
首期不承诺自绘主题或完整 UI Automation 覆盖；验证原生控件标签、键盘与高对比度基本可用性。

## 9. 文件范围与步骤

预计改动：Settings 私有编辑与窗口实现/公开回调头；新增 WindowRenderer 子模块及其 CMake；
Application 的语言应用结果适配；resources/setting_windows.json、ui_text.json；镜像测试及设计/进度文档。
不修改 Common、Localization 核心、Capture、Graphics、Export 的实现；不新增第三方依赖。
保持 OpenST::Settings 及现有别名。测试目录按 testing/test/Common/WindowRenderer 镜像。

施工分为小步骤，每步检查 diff 并由子 Agent 监督：
1. 布局解析、ID 索引和绑定校验，无产品切换。
2. Settings 私有编辑会话、差异提交和结果处理。
3. 原生 Renderer、常规页及 SettingsWindow 接入。
4. Application 语言联动、本地化、焦点/DPI/异常处理。
5. 构建、针对性回归和文档同步，提交阶段验收。

全部步骤属于本首期方案，不包含开机启动、快捷键、额外输入控件或其他设置业务。
实质偏离范围须重新审核；获批前不修改实现文件。

## 10. 验收

- 布局/绑定：非法版本、重复 ID、未知 ID、类型不匹配、缺少绑定、重复绑定、解析输入释放后仍有效。
- 交互：选择只改草稿，程序刷新不触发回调，取消不写，确定/应用共用保存，重复打开保留草稿。
- 存储：默认显示不自动写、改回基线无修改、保留未知字段、一次提交、外部冲突、损坏/删除/只读及重试。
- 语言：选项动态失效、保存失败不生效、保存成功生效失败、仅重试生效、按实际语言刷新文本。
- 窗口：关闭分派生命周期、重复提交防护、Tab/Enter/Esc、长译文、100%/150%/200% DPI、跨屏及小工作区。
- Renderer 使用 fake 回调验证无需 Settings/Common/Localization 即可工作；首期不要求独立安装包。
- Debug /W4 /WX 产品构建和相称模块测试；Release 验证资源进入运行目录；产物不进入源码目录。
- 自动测试不进入阻塞模态提示；人工检查与自动测试结果分别报告，多页实际产品验收留待真实页加入。

## 11. 设计复核

独立子 Agent 一轮复核未发现阻断问题。已纳入 PUBLIC JSON 依赖、输入文档不借用、
按控件类型验证必需绑定、数据/选项双绑定合法，以及新增交互控件需宿主绑定的要求。
用户最初批准本稿及 Settings 子目录安排；2026-09-09 又明确批准迁移至 Common 子树独立目标，见 D-054。

## 12. 首期历史验证记录

- 已实现全部首期步骤，产品布局另有 reloadButton 提供实际重试/重载入口。
- Debug /W4 /WX 和 Release 产品构建通过；Debug 首次链接被沙箱拒绝访问，沙箱外重试成功。
- Settings 62 项测试通过；全量 217 项中 215 项首次通过，2 项桌面捕获因 0x80070005 失败，沙箱外定向重跑通过。
- 自动窗口检查覆盖两页切换、长文本与小客户区、144/192 DPI 消息、焦点滚动、展开下拉框键盘行为和忙态滚轮。
- Release 布局资源与源文件 SHA256 一致，运行目录未含编译中间产物。
- 真实跨屏观感、中英日完整视觉检查、高对比度和辅助技术仍待人工验收；未提交或发布。

## 13. 已批准的通用窗口复用修订（2026-09-09）

Renderer 位于 `src/Common/WindowRenderer`，公开类为 WindowRenderer，头为 window_renderer.h。
Settings 原 pages 文档不变。Application 错误与关于提示使用顶层 content，内容节点仍为 column，
content/pages 恰好存在一种；content 不创建标签控件，其内部匿名页面不产生业务页面 ID。

新增 `ShowModal(options, processThreadMessage)`：同步等待关闭，hook 可处理其他宿主导航和无 HWND 线程消息。
仅临时禁用有效且原本启用的 owner；创建失败、hook 异常和正常关闭均恢复，原先 disabled 状态保留。
GetMessage 返回 -1 时报告错误；收到 WM_QUIT 销毁窗口并将原码重投给外层。正常关闭返回成功，
Application 不因此触发兜底。Application 的弹窗重入防护与本地化仍留在宿主。

本轮 Renderer 33 项自动测试通过，新增覆盖 content/互斥、模态 owner 状态、关闭、quit 和异常恢复。
人工入口为设置 `OPEN_ST_INTERACTIVE_UI_TESTS=1` 后运行
`.\scripts\test.ps1 window_renderer RendererManualTest.waits_for_user_close`；默认跳过，启用后等待真人关闭。
本轮 Debug/Release 产品构建和 225 项自动测试已通过；人工 case 默认跳过，完整视觉验收仍待用户操作。