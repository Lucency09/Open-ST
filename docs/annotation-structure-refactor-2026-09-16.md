# 标注结构整改记录

日期：2026-09-16。用户已批准按 [结构审计](annotation-structure-audit-2026-09-16.md) 施工，
并要求把重复实现和模块职责退化的教训写入开发规范。本记录随验证结果更新。

## 实际整改

| 模块／类 | 拥有的数据与提供的能力 | 禁止承担的职责 |
| --- | --- | --- |
| Annotation | 对象值、颜色校验、不可变文档查找／追加／属性替换／平移及存储估算 | HWND、D2D、截图选区模型、模态窗口、历史策略 |
| CaptureAnnotationState | 工具默认值、正式文档、统一 preview、绘制／裁剪／属性事务、修订和共同历史 | 窗口构建、图形资源、控件事件 |
| App | 会话与窗口生命周期、输入准入、事务调用、两屏重绘、输出编排 | 复制对象数组、逐对象修改、第二套属性预览或历史 |
| Application/annotation_style_dialog | 标注属性表单的标量草稿、有效性、预设色／线宽选项及预览回调适配 | 自己创建 HWND／字体、布局引擎、DPI 分派、模态消息循环、文档编辑 |
| Common/WindowRenderer | 通用布局、绑定、校验展示、数值色块、指定控件刷新、DPI、模态与置顶 | 标注对象、颜色文本业务解析、截图状态、撤销规则 |
| CaptureToolbar | 工具按钮、图标、选中／启用状态、位置和命令投递 | 属性窗口、样式文档或图形处理 |
| Graphics | 共享 D2D 几何、命中、预览与输出合成 | 表单或编辑历史 |

属性窗口改用 Application 私有 `ShowAnnotationStyleDialog`，调用原有 WindowRenderer 的
LoadLayout、BindString、BindInteger、BindOptions、BindAction、SetStatus、ShowModal 等能力。
实际缺口仅补通用 swatch／BindColor、RefreshValue(id) 与 RendererWindowOptions.topmost：
色块消费数值 RGB，颜色原文统一调用 Annotation 的 ParseAnnotationColor；单字段刷新不覆盖其他未完成输入。

CaptureAnnotationState 新增 BeginStylePreview／UpdateStylePreview／EndStylePreview／PreviewingStyle，
属性和绘制共用 preview_。App 不再持有 annotationPropertyPreview_ 或复制对象列表。
纯文档操作已经被正式绘制提交、平移和属性事务调用，不是仅供测试的空接口。
截图边界与标注一起撤销的历史仍留在会话状态层，没有把 Capture／Graphics 类型移入 Annotation。

原 CaptureToolbar 的 toolbar_style_dialog.h、toolbar_style_values.h、toolbar_style_dialog.cpp 及重复解析已移除，
原生属性测试迁到 Application；工具栏本身的图标、选中态与换行布局继续保留。
没有新增 CMake 目标、第三方依赖或兄弟模块链接；App 沿用已有 WindowRenderer 和 Annotation 依赖。

## 保持的行为与必要验证

颜色、透明度、线宽与预设色继续有效；填充元素不展示线宽；右键实时预览、取消恢复、确认一步撤销。
初始值或重复成功值不触发冗余预览；有效候选预览失败后，改回先前值也重新核验回调，避免状态层与表单不同步。
无效输入保持原文，预设颜色不能覆盖未完成透明度；模态期间跨屏编辑仍被阻止，显示失效安全回收。
公共色块、局部刷新和置顶分别在 WindowRenderer 测试；纯文档操作在 Annotation；事务和属性窗口在 Application。

全量执行 618 项：605 通过、11 跳过、2 失败。Application 178 项全部通过，Annotation 7 项全部通过，
WindowRenderer 69 通过／1 人工跳过，CaptureToolbar 19 通过／1 人工跳过；设置与其他受影响使用方未发现失败。
新增或迁移覆盖：4 项纯文档、6 项状态、12 项原生属性表单、8 项通用色块／刷新／置顶测试。

两项失败均位于本轮未修改的 PinWindow 目标，该目标不链接 WindowRenderer：

- `real_click_focus_keeps_three_window_order` 首次拖动位置有 1 像素差异，单独复跑通过；保留首次记录，不将它写成首次通过。
- `repeated_correct_order_emits_no_position_requests` 首次和复跑均观察到 10 条位置请求；
  这是 D-066 已记录并后置的冗余请求事项，见 [原验证](pin-window-order-validation-2026-09-14.md)。本次没有删除、放宽或跳过该断言。

全量日志：`testing/testoutput/annotation-refactor-all.log`；失败复核：`testing/testoutput/annotation-refactor-pin-recheck.log`。
本轮未将全量结果标为全部通过，也未据自动控件测试宣称重构后真实跨屏外观验收完成。
Debug／Release `/W4 /WX` 产品构建通过，可运行产物已更新；日志分别为 `build/annotation-refactor-debug.log` 和 `build/annotation-refactor-release.log`。

## 防止再次退化

开发规范新增“先核对已有能力”和“模块职责与能力不退化”两节，要求方案提供接口复用清单及真实缺口，
结构复核同时检查依赖合法、职责内聚、状态唯一所有者、生产调用和旧实现清理。
不得用依赖更少、文件拆分或功能测试通过替代这些检查；公共组件缺口优先最小扩展，特殊 UI 例外不得泛化。
本次审计和整改记录保留，用于后续功能设计核对，而不是在功能扩展后再回头整理结构。
