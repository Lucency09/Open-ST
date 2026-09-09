# Application 回调审计与整理建议

日期：2026-09-09。状态：只读源码审计及待审设计，不代表获准施工。
主 Agent 与独立子 Agent 分别核对 App/工具栏/单实例/输出，以及 Settings/Welcome/Renderer 路径，结论一致。
行号基于本轮工作区；未运行构建或测试，本文测试结论仅指现有测试源码覆盖内容。

## 1. 结论

后续贴图、快捷键、OCR 和翻译会增加 App 跨模块连接代码，但并非所有通信都必须通过回调：
App 向子模块发起操作通常直接调用；子模块向宿主查询能力、上报命令或结果才需要注入接口。

建议现阶段新增 `Application/source/app_callbacks.cpp`，集中回调组装，继续使用类型明确的工厂函数。
暂不建立拥有所有回调的管理器对象、全局注册表、字符串事件总线或统一 Subscribe/Unsubscribe 系统。
新增文件仍编入 OpenST::Application，既不增加模块/target，也不增加第三方依赖。

集中组装有助于查看谁连接谁，但不自动提供取消、线程安全、失效检查或对象寿命保障；这些必须保留在各自业务边界。

## 2. 当前回调清单

| 回调组 | 注册与持有 | 调用方式 | 释放与保护 | 源码依据 |
| --- | --- | --- | --- | --- |
| Settings / Welcome | App::MakeSettingsCallbacks 组装 7 个函数回调；模块按值接收并持有 | UI 线程同步查询文本/语言/自启状态，同步返回应用结果，忙状态直接通知 App | Settings 空闲 Close 释放；忙时延迟；Welcome 模态返回/异常清理后释放 | app.cpp:783；Settings/include/settings_window.h:13；Settings/source/settings_window.cpp:18,143；welcome_window.cpp:19,62,76 |
| CaptureToolbar | Create 注入文本查询与命令回调，共 2 个；Impl 持有 | 文本同步查询；命令回调只投递 App 消息 | pending/busy、CaptureCommandGate 代次；Close 先清回调再毁窗 | app.cpp:871,899,1023；CaptureToolbar/source/capture_toolbar.cpp:77,593 |
| SingleInstance | 监听前 SetCaptureGate 注入 1 个查询回调 | 后台接收线程读取 App 原子门禁，并向 UI 窗口投递命令 | 监听中禁止替换回调；App 析构先 Stop，等待线程 join | app.cpp:376,229；SystemIntegration/source/single_instance.cpp:114,235,327 |
| CaptureCompletion | 每次 CompleteSelection 建立 5 个 CompletionActions 回调 | 同步逐步生成、选路径、复制/保存及记目录，可进入模态循环 | 局部数据在同步流程期间存活；RAII busy 防重入，异常恢复 | app.cpp:1535；private/capture_completion.h:24；source/capture_completion.cpp |
| 简单提示 / 退出清理 | 局部 Renderer 绑定回调；App 临时借用活动 renderer 和状态刷新闭包 | 同步 UI 回调，模态消息循环可能重入 | ActiveGuard/RefreshGuard 清借用，dialogActive 防重入 | source/simple_message_window.cpp:9；app.cpp:1652,1790 |

当前没有全局回调注册表，也没有通过名称动态查找业务回调的机制。
多数跨模块回调在 App 中构造，由子模块拥有 std::function；App 内少量临时借用另有作用域守卫。
Win32 窗口过程及用户数据解绑属于窗口生命周期，不适合塞入通用 std::function 注册表。
子模块内部的 Renderer 控件绑定属于本模块 UI 实现，也不等同于 App 的跨模块通信。

## 3. 已有保障与问题

### 已有保障

- Settings 回调参数类型明确，保存与系统副作用分别返回结果；处理异常、成对通知忙状态。
- 工具栏投递命令与实际执行分离，携带命令值和代次，避免按钮回调栈内同步销毁工具栏。
- 单实例线程只通过原子门禁和 PostMessage 与 App 协作，不直接调用 UI；先停止线程再拆除 App 资源。
- 临时输出/对话框回调保留在同步作用域中，不把借用局部变量的闭包存入长寿命容器。
- 本次核对的接线路径没有发现已证实的 App 回调悬空调用；这不是对未运行路径的全面安全证明。

### 应改善的点

1. App 回调组装散落在 Run、MakeSettingsCallbacks、CreateCaptureToolbar 和各模态业务中；
   后续模块接入会继续增加 app.cpp 的职责，集中跨模块组装有实际价值。
2. 同步查询、同步副作用、异步命令和后台门禁的线程/返回值/生命周期契约尚未统一列明。
   应按接口注明调用线程、捕获对象寿命、异常结果和停用时机，而不是把它们强制改成同一调用方式。
3. SettingsWindow::Close 的接口注释不准确：include/settings_window.h:51 声称同步关闭并释放回调，
   source/settings_window.cpp:145–148 在 busy 时仅设置 closeAfterBusy_ 后返回。
   这是刻意的安全设计，现有 settings_window_tests.cpp:439 的 close_inside_application_callback_is_deferred 覆盖此行为。
   后续应修正注释：UI 线程调用；空闲时同步清理，业务执行中仅请求延迟关闭，调用者不得因此立刻销毁捕获对象。
   用户关闭 HWND 也不保证立刻释放 Settings 容器中的 callbacks_，其寿命可延续至再次 Show 或宿主 Close。
4. App 析构当前先 Stop 单实例、Close/reset Settings，再 ShutdownSettings/ShutdownUiText，随后 CloseOverlay。
   当前工具栏 Close 先清回调，不据此报告现存故障；贴图接入时应明确所有业务窗口/接收端先停用关闭，
   再释放所依赖的设置、本地化和消息窗口。新文件的位置不能替代这一销毁顺序。
5. 未来 OCR/翻译后台结果需要请求 ID、取消/失效检查、结果数据所有权及线程收尾。
   应在实际异步任务接入时设计带类型的结果投递，不能从现有 UI 回调外推“捕获 this 就安全”。

## 4. 建议的最小整理方案

### 4.1 本次可单独审批的范围

- 新增 `src/Launcher/Application/source/app_callbacks.cpp`。
- 将现有 `App::MakeSettingsCallbacks()` 定义移入该文件，行为与返回结构保持。
- 将工具栏两类回调、SingleInstance 门禁闭包整理为 App 的类型明确的 Make* 工厂，定义同置该文件。
  具体签名施工时保持子模块既有接口，不为了凑统一结构而改它们。
- 声明留在 `Application/include/app.h`，其他运行入口在 app.cpp 调用工厂；现阶段不需要管理器成员或额外 private 头文件。
- Application/CMakeLists.txt 登记新 cpp；Settings 公开头仅修正上述 Close 契约注释，保持实现不变。
- 文档记录统一接入规则；贴图施工时把它的回调工厂加在同一位置，不预先创建 OCR/翻译空接口。

保留在原处：局部 CompletionActions、Renderer 控件动作、作用域守卫、命令准入/业务分派及模块自身回调成员。
理由是它们与局部数据或业务状态机紧密相关；搬到长期管理器会拉长借用寿命，甚至把简单同步流程变成隐式注册流程。

### 4.2 调用关系

```text
App 初始化某模块
  → App::Make*Callbacks()       定义放 app_callbacks.cpp
  → 模块 Create/Show/Set*       模块按契约持有回调
  → 模块需要能力时调用
      同步查询/应用 → 对应服务或 App 业务入口 → 返回明确结果
      UI 命令      → App 消息队列 → 身份/代次/忙状态检查 → App 业务入口
      后台门禁     → 原子读取 → 允许或拒绝 → 固定消息投递
  → 停止异步生产者、窗口关闭/解除回调 → 释放模块和宿主依赖
```

回调类型仍由消费它的模块定义，组装文件只负责连接，不接管订阅关系，也不让兄弟模块知道彼此。
新增依赖为零；最终业务仍由 App 和现有子模块负责。

### 4.3 验证范围

- 运行与行为相关的 Settings、SystemIntegration、CaptureToolbar、Application 回归及 Debug /W4 /WX 构建。
- 保留已有真实消息队列旧命令失效、设置回调内延迟关闭、busy 通知异常恢复及单实例代次测试。
- 若整理引入新生命周期状态再补相应边界测试；纯函数搬移不为逐项 lambda 转发另写镜像测试。

## 5. 何时再考虑真正的管理器

出现动态模块加载/卸载、多订阅者、独立订阅寿命，或多个异步来源需要统一结果投递与取消时，再评估专门组件。
届时可以仍放在 Application/private 与 source 下，不需要新模块；应按实际需求选择事件分发器或任务结果队列。
当前回调数量增加主要意味着组装代码变长，还不足以证明需要动态订阅系统。
