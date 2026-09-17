# 右键元素属性实施报告

日期：2026-09-16。依据：[已批准方案](design/annotation-element-properties-v0.3.md)。
结论：实现与自动回归完成，实际界面验收未完成。
同日追加的实时预览实现与增量验证见本文末节；下文原右键属性验证保留为该阶段记录。
后续模块职责已按用户批准整改，最新窗口归属与预览所有权见 [结构整改记录](annotation-structure-refactor-2026-09-16.md)。

## 行为

- 截图遮罩右键不再触发取消；Esc 保持原层级。
- 选择工具和绘图工具空闲时，右键命中元素直接打开“元素属性”；空白、区外、未框选及活动手势不操作。
- 修改单个元素颜色、透明度，空心矩形／箭头还可改线宽；其他对象、当前工具、默认样式及几何不变。
- 确认一次对应一步撤销／重做；取消、无变化或失败保留原文档与 redo。已有元素改为完全透明后可撤销恢复。
- 几何逆序精确命中优先；描边／箭头仅在全部精确命中失败后使用按点击屏 DPI 换算的 3 DIP 容差。
- 右键拖动、离窗、失焦、失捕、取消及新左键手势使旧点击失效；旧消息不能消费新会话请求。

## 实现范围

`Application/source/app_annotation_properties.cpp` 管理点击与模态属性事务，消息只携带序号；
`CaptureAnnotationState` 新增单调修订与按稳定 ID 替换样式，撤销同样推进修订。
`Graphics/source/annotation_drawing.cpp` 复用既有绘制几何并提供 `HitTestAnnotations`。
复用原样式窗口，补充中英日标题；预览及最终输出继续消费不可变快照，不改变 HDR 或 Present。
子 Agent 对请求失效、模态寿命、事务及命中实现进行了只读复核，未发现阻断问题。

## 自动验证

| 项目 | 结果 |
| --- | --- |
| Debug、Release 产品构建 | `/W4 /WX` 通过 |
| Application | 157 通过 |
| Graphics／Selection | 68 通过，1 基准默认跳过 |
| Annotation | 3 通过 |
| CaptureToolbar | 24 通过，1 人工窗口默认跳过 |
| Hotkeys | 15 通过 |
| 合计 | 269 项，267 通过、2 跳过、0 失败 |

本轮新增 Graphics 命中 8 项和独立 Application 属性测试 13 项。
属性测试使用真实隐藏 HWND、正式 OverlayProc 和消息队列，并替换同步弹窗，验证点击、历史、修订、模态和失败恢复。
原窗口选择测试中的右键取消预期已改为 Esc，同时保留新增右键无取消断言。
首次沙箱运行中的真实桌面捕获返回 `0x80070005`；在允许桌面访问的环境复跑后通过。
没有将本次相关模块回归写成全量测试通过。

本地日志：`testing/testoutput/annotation-properties-application.log`、
`testing/testoutput/annotation-properties-related.log`、`build/annotation-properties-debug.log`、
`build/annotation-properties-release.log`。可运行产物：`build/Release/Open-ST.exe`。

## 实际界面检查与剩余验收

本次 Computer Use 已能启动 Release，首次启动欢迎页直接退出，未确认自启或修改设置。
随后启动保留现有设置的 Debug，通过现有 F1 快捷键看到了截图遮罩画面。
自动化窗口列表只返回设置窗口，没有独立遮罩目标；输入后的选区与弹窗无法可靠核验，因此不据此判定交互通过。
未复制截图到用户剪贴板或保存测试截图文件。

仍需人工检查：四种元素右键属性窗口、颜色／透明度／线宽实际效果，取消与 Ctrl+Z，
重叠元素、跨屏及混合 DPI 连续操作，以及修改后的复制／保存／贴图体验。
局部橡皮擦、文字和多档马赛克仍属后续阶段，本次不计完成。

## 同日追加：实时预览与模块依赖说明

用户明确批准后接入颜色、透明度、线宽的有效值同步预览。App 每次从打开时的文档生成仅替换目标的临时快照，
统一绘制入口消费它；committed、修订、默认样式和历史均不变。确认只提交最终值一次，取消／关闭／异常回退；
改回原值不产生历史，100% 透明预览期间仍能调回可见。会话失效后拒绝新的预览并在模态返回后清理。
属性窗口新增可选预览回调，输入无效保持上次画面；预览失败显示错误并禁止确认，成功重试后恢复。
默认样式窗口不使用此回调，不增加模块链接依赖。

两个方案分别在 [标注方案第 5.1 节](design/annotation-v0.3.md#51-当前模块依赖情况)
与 [元素属性方案第 7 节](design/annotation-element-properties-v0.3.md#7-模块依赖情况) 补充模块依赖图、链接性质和数据适配职责。

新增 3 项 App 测试和 6 项原生样式弹窗测试，并增强已有确认／取消／失败回退测试。
本次执行 Application 160 项、CaptureToolbar 31 项、Graphics 48 项、Annotation 3 项：
合计 242 项，240 通过、2 默认跳过、0 失败；不包含上轮的 Selection 与 Hotkeys 分组。
日志：`testing/testoutput/annotation-live-preview-application.log`、`testing/testoutput/annotation-live-preview-related.log`。
Debug／Release `/W4 /WX` 构建通过，可运行产物已更新；日志分别为 `build/annotation-live-preview-debug.log` 和
`build/annotation-live-preview-release.log`。本次没有新增实际产品界面验收结论，限制同上一节。
