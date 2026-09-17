# 标注代码结构审计

日期：2026-09-16。范围：当前工作区的 M1 标注、右键元素属性和实时预览。
用户本轮表示功能无疑问，要求审查模块分工；本轮只审计并新增报告，不调整实现，不重跑功能测试。
后续用户已批准整改；本文保留审计时事实，最新实现见 [结构整改记录](annotation-structure-refactor-2026-09-16.md)。

## 结论

当前已检查的模块依赖方向符合项目层级约束，但依赖合法不等于职责划分充分。
Graphics 承担图形实现、Application 协调截图生命周期合理；属性窗口重复实现通用窗口能力、
App 直接操作标注文档及预览状态分散，是需要整改的结构性问题。
Annotation 实际是共享值类型与校验层，可以很小；它尚不是完整的标注编辑核心。

## 1. 优先整改：属性窗口没有复用 WindowRenderer

`CaptureToolbar/CMakeLists.txt:13` 只链接统一编译选项和 user32／gdi32／comctl32，没有 WindowRenderer。
`CaptureToolbar/source/toolbar_style_dialog.cpp` 独立实现控件创建（121）、布局与字体（146）、
输入校验（240）、owner 恢复（424）、窗口创建及模态循环（453）。这不是公共窗口生成器的业务适配层。

公共 `Common/WindowRenderer/include/window_renderer.h` 已有：

| 现有能力 | 入口行号 | 属性窗口可复用的部分 |
| --- | --- | --- |
| 字符串即时绑定 | 111 | 颜色输入原文与错误保留 |
| 整数及可选滑条绑定 | 116 | 透明度输入／拖动 |
| 动态下拉选项 | 145 | 线宽档位 |
| 关闭、默认动作、模态窗口 | 153、161、173 | 确认／取消、owner 与消息循环 |
| 控件启用、字段错误和状态 | 186、202、206 | 预览失败与非法候选阻止确认 |

维护两套字体、布局、DPI、输入与模态基础设施，会让后续行为修复和界面一致性修改重复发生。
原 D-057 的独立原生工具栏约束针对不抢焦点的紧凑工具条；它不能充分证明普通属性表单也应重新实现。
现有开发规范明确 Toolbar 不链接 WindowRenderer，所以迁移时应调整属性窗口归属，不能直接违反这条规则。

迁移并非零成本：当前 Renderer 的 NodeType 没有专用色块／调色板，RendererWindowOptions 没有显式置顶选项。
应先核验遮罩 owner 上的层级及色块需求，仅补必要的通用能力；标注对象和业务历史不能移入 Common。

## 2. 优先整改：文档操作与预览状态未完全收敛到编辑状态层

`Application/source/capture_annotation_state.cpp` 已承载对象创建（52）、平移（156）、历史提交（207）和按 ID 改样式（234）。
但 `app_annotation_properties.cpp:175` 又在 App 回调里复制对象、验证、复制整个文档并替换目标；
`app.h:419` 单独持有属性预览，`app_annotation.cpp:25` 优先读取它，而绘制草稿预览在 CaptureAnnotationState 内。

目前原子提交与取消测试通过，本审计不将其判为已确认功能故障。
结构问题是同一类文档修改有两处实现、预览有两个所有者；继续增加文字、路径和擦除会扩大维护面。
建议由编辑状态层提供开始属性预览、更新、确认、取消接口，App 只负责准入、弹窗回调和请求重绘。
统一历史同时记录截图边界和标注，保留在截图编辑会话层有合理理由，不要求全部搬进纯 Annotation 模块。

## 3. Annotation 的小体积不是错误，但职责与名称要明确

`Graphics/Annotation/include/annotation.h` 定义类型、点、样式、对象及不可变快照；
`source/annotation.cpp` 只有 IsValidAnnotation（10）和 ParseAnnotationColor（37）。
作为供 App 与 Graphics 共用的独立类型层，这是有效边界，不能按函数数目判断模块价值。

但当前纯对象创建、平移和样式替换都在 Application，Annotation 尚未承载编辑核心；
颜色解析函数也只有测试引用，生产样式窗口在 `toolbar_style_dialog.cpp:17` 重新实现了一遍解析。
因此不能把新增这个目录等同于已经完成“标注功能模块化”。

建议下沉纯对象／文档变换与统一校验，或明确命名为 AnnotationModel；不为增加函数数量搬入 D2D、窗口及截图状态。
不能原样搬迁 CaptureAnnotationState：其头文件直接使用 Capture 的 geometry.h 与 Graphics 的 selection_model.h，
搬到 Graphics/Annotation 会产生兄弟／父级依赖。应先拆开纯值操作与截图边界适配。

## 4. Graphics 与 CaptureToolbar 中哪些变化合理

- `Graphics/source/annotation_drawing.cpp:192` 的 D2D 绘制、274 的输出合成及 372 的几何命中放在 Graphics 合理；
  命中复用绘制几何可以避免箭头和圆角规则分叉，HDR／冻结底图也由原图形链处理。
- App 的输入分派、跨屏会话、模态保护、复制／保存／贴图协调属于应用编排，不应全部移入 Annotation。
- Toolbar 的命令／图标从原四项扩展到工具和撤销等项、持续选中态、窄屏换行属于工具栏职责。
  证据：`capture_toolbar.h:17`、`capture_toolbar.cpp:116`、`toolbar_layout.cpp:44`。
- 不够合理的是将已经同时服务“工具默认样式”和“单元素属性”的完整属性窗口继续放在 Toolbar。
  此文件现有约 573 行（含注释和空行），使工具栏模块承担了第二种 UI 职责。
  普通 git diff 的统计不包括尚未跟踪的新文件，评估本轮规模必须连同该文件一起看。

## 建议顺序

1. 将属性编辑窗口移出 CaptureToolbar，业务适配由 Application 的标注 UI 实现承担，复用 Common/WindowRenderer。
2. 收拢编辑状态的属性预览／提交／取消接口，去掉 App 的直接文档复制和第二份预览所有权。
3. 将纯文档操作及真实使用的校验下沉 Annotation，保留 Graphics 图形实现和会话层统一裁剪历史。

这些是审计建议，不是已经实施的重构。落地前需按项目规则提出变动范围及兼容性验证方案。
子 Agent 已独立核对模块内聚、公共窗口能力和依赖方向，结论一致；功能测试通过不能替代本次结构结论。
