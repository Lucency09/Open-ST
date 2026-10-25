# 翻译自动分段子模块方案（最小范围版，已批准施工）

日期：2026-10-25。基线：v1.2.0。用户已明确“同意方案”，批准“独立分段子模块、复用现有统一调用、用户无感、固定长度”的核心5项范围施工。本稿替换此前扩大范围的待审版本。

## 1. 本次只做什么

用户输入全文并点击一次翻译，程序内部先按固定长度及语义边界分段，顺序调用原有翻译接口，全部成功后拼接并一次显示完整译文。

明确限定：

- 新建Translation/Segmentation子模块，专门实现分段与拼接。
- 父Translation只接入子模块，并把一次调用改为对片段顺序调用。
- 本地和在线继续走原ITranslationProvider::Validate/Execute，不扩展接口。
- Local整体复用，包含自动语种识别、模型加载、512 token保护及逐行排版。
- 用户界面、配置、状态结构和文案保持现有流程，不增加分段信息。

取消此前方案里的整篇语种锁定、公开Local检测接口、动态Fits、模型准备拆分、公共校验整理，以及按段数增长的新超时体系。

## 2. 模块及调用顺序

```text
Translation/
├── CMakeLists.txt                         修改：加载、链接子模块
├── source/translation_client.cpp          修改：调用接入及顺序执行
├── Segmentation/                         新增
│   ├── CMakeLists.txt
│   ├── include/text_segmentation.h
│   └── source/text_segmentation.cpp
└── Local/                                原样复用
```

Segmentation是独立目标OpenST::TranslationSegmentation，对应open_st_translation_segmentation；只依赖标准库及统一编译选项，不依赖父Translation、Local、HTTP、Settings或窗口。

```text
全文Submit
  → Translation原有工作线程
  → Segmentation::Split：生成一次计划
  → 选择当前提供方
  → Validate/Execute第1段、第2段……第N段
  → Segmentation::Assemble
  → 原请求身份核验 → 一次发布完整译文
```

Segmentation负责文本边界与拼接规则；Translation负责原有接口顺序、取消、期限、临时结果和最终发布。不得把HTTP/模型调用放进Segmentation，也不在Local重复实现分段算法。

## 3. 固定200字符，按语义向下取整

### 3.1 固定长度

子模块唯一写死MAX_SEGMENT_CODEPOINTS=200，按Unicode标量计数，不按UTF-8字节或UTF-16单元计数。所有提供方使用同一计划，不根据接口返回重新缩短或切分。

不足200的原文保持一段。超过200时，从上限向前查找合适边界。例如第176字符是完整句末、下一句到218才结束，则本段到176，下一句留给后段。

已完成离线校准：既有两方向SentencePiece 0.2.0，15类样本、7个候选长度及4个偏移，共840次分词。200长度的普通样本最高199 token，方块兼容字符压力最高502；224和256的同类压力样本超过512。另有7个真实INT8/beam2推理片段均正常EOS，包含满200的中英输入。

200只是经验工程值；特殊兼容字符仍可能超512，原本的实际token和请求大小校验继续生效。这些结果是容量验证，不代表现有模型翻译质量通过验收。

校准输出位于testing/testoutput/segmentation-calibration/fixed-length-token-calibration.json和fixed-length-inference-calibration.json；未调用付费在线服务、下载模型或修改实现。

### 3.2 边界规则

按以下级别查找，同级取200上限内最靠后的有效位置：

1. 硬换行或完整句末：CRLF/LF/CR、。！？及适用的.!?。
2. 分号；再找逗号、顿号及适用的冒号等从句边界。
3. 词间空白，保证不从英文单词或数字内部断开。
4. 没有安全边界时返回现有输入过大错误，不截断、不把长度扩大到200以上。

必要的排除规则：

- 小数、版本号中的句点，例如3.14、1.2.0，不算句末。
- 保护常见缩写Mr./Mrs./Ms./Dr./Prof./e.g./i.e./etc.及U.S.式首字母串。
- 不在明显网址、邮件、数字分组或时间语法内部切割。
- 连续句末标点及尾随闭合引号/括号尽量留在前段，仍须满足200上限。
- 不拆UTF-8序列、CRLF、组合附加字符、变体选择符或ZWJ序列；只能向前回退。

这是一套标点与文本边界规则，不额外引入语义识别模型。完整句优先，极长句才退到从句或词边界；跨段上下文可能减少。

## 4. 拼接规则

子模块计划记录正文和原始分隔内容，要求各原文范围无遗漏、无重复。Assemble按计划顺序拼接：

- 切分边界的换行、空行、缩进及空白保留原字节，使用一次。
- 仅对保存了分隔内容的对应侧清理译文外缘空白，正文内部空白不改。
- 无原始分隔时，中/日目标直接衔接；英文在句末、从句或词边界补必要空格，避免词语粘连，不重复加空格。
- 父Translation把目标语言映射为子模块自有的Compact/Spaced等简单接缝选项；Segmentation不读取Settings或父层语言类型。
- 不增加、删除或去重译文标点。
- 只有单一正文范围覆盖全文且没有分隔内容/人工接缝时才直接返回单段结果；只有一个正文段但保留了首尾换行时仍需Assemble。
- 父层收集临时译文时就检查聚合内存预算，最终完整输出仍遵守既有1MiB限制，不能先积累无限片段再检查。

Local的TranslateLines继续处理一个已给定片段内部的硬行排版；Segmentation处理片段之间的接缝。Local无需知道分段计划。

## 5. 父Translation只修改两个既有文件

### CMakeLists.txt

增加add_subdirectory(Segmentation)，将新目标作为Translation的私有依赖。原Local及其他依赖关系不调整。

### source/translation_client.cpp

只做以下调用编排改动，切句算法不写在此文件：

1. 普通翻译的整文准入上限由10000调整为100000 Unicode标量，让超过旧门禁的全文能进入分段；只在本文件修改。已有UTF-8及配置校验继续使用。
2. 在原接口循环之前调用一次Split。原文、语言、接口配置与请求身份仍是一份整篇快照。
   计划没有任何可翻译正文时，使用既有InvalidInput并零次调用接口，不能把纯分隔内容作为成功译文。
3. 原外层接口顺序保留，内部增加片段循环；每段调用原Validate/Execute，不改变提供方接口或本地实现。
4. 当前提供方全部完成后调用Assemble。中间任何段失败则清空本家临时结果，再按原错误分类决定停止或由下一家从第一段开始；不混合不同接口的结果。
5. 保留原取消、关窗/编辑失效、真实后台收尾和完整结果发布；每个提供方仍只记一条TranslationAttempt，只有零次Execute才是skipped。
6. 原文及上一次完整译文保持，过程不发布部分译文，不向窗口暴露分段计数。

### 语言处理直接复用

源语言手动指定时，每段都使用相同设置；source=auto时，每段由原接口自行识别。本地也按段识别，这是用户选择最小方案后采用的调用语义，不再添加整篇检测或锁定。

最终结果仍使用用户原始语言选项和整篇身份。只有全部片段都返回非空且一致的detectedLanguage才保留该值，否则留空，不用第一段结果推断全文。

## 6. 保留原有控制规则

- 继续使用现有ClientDependencies.itemBudget/totalBudget，不修改provider头。
- 生产默认每次Validate/Execute最多30秒，整轮最多120秒；整轮从worker开始处理该任务计时，规划也计入，段间和后备不能重新起表。
- 单次实际截止为“整轮截止”和“本次调用开始＋30秒”的较早者；同一片段的Validate与Execute共用这一个deadline。校验结束后已到期则不执行；段前、段后检查取消与整轮期限。
- 截止时间沿用逻辑取消和结果失效语义，不强杀底层线程；不可立即中断的模型或系统调用实际收尾后才允许后续任务。
- 普通输入上限100000是接收上限，不保证所有最大长度文本都能在现有120秒内完成；超时仍使用原错误提示。
- 不增加上一版1024段/2048次调用上限、300/600秒上限或按K增长公式；输入规模、既有接口列表上限和总期限继续约束任务。
- 真实单请求字符/编码限制及Local 512 token仍由原Validate/Execute检查；被拒绝时按原分类处理，不动态重分或无限重试。
- 设置中的测试连接保留原10000码点和单次完整请求语义，不进入普通自动分段分支。

## 7. 文件范围

### 生产及构建：严格5项

以下路径以src/Launcher/Application/Translation为根。

| 文件 | 类型 | 改动 |
| --- | --- | --- |
| Segmentation/CMakeLists.txt | 新增 | 独立子模块目标。 |
| Segmentation/include/text_segmentation.h | 新增 | 固定长度、计划及Split/Assemble契约，全部使用自身类型。 |
| Segmentation/source/text_segmentation.cpp | 新增 | 语义分段、边界保护、原分隔内容和拼接。 |
| CMakeLists.txt | 修改 | 加载并私有链接子模块。 |
| source/translation_client.cpp | 修改 | 整文准入、分段调用、顺序执行、聚合及现有生命周期适配。 |

Local、translation_provider.h/.cpp、translation_validation.cpp、translation_protocol.h、公开translation_client.h、App、窗口、设置和资源文案均不纳入本次实现修改。

### 必要测试及测试构建：5项

测试文件与上述5项生产文件分开计数，不能因为“最小生产范围”省略验证。

| 文件（仓库相对路径） | 类型 | 作用 |
| --- | --- | --- |
| testing/test/Launcher/Application/Translation/Segmentation/CMakeLists.txt | 新增 | 子模块镜像测试目标，前缀translation_segmentation。 |
| testing/test/Launcher/Application/Translation/Segmentation/source/text_segmentation_tests.cpp | 新增 | 固定200、边界保护、无遗漏/重复、拼接和错误情况。 |
| testing/test/Launcher/Application/Translation/CMakeLists.txt | 修改 | 加载新测试目录；现有client测试需要时注入既有模型缓存路径用于真实本地长文验证，不下载模型。 |
| testing/CMakeLists.txt | 修改 | 登记新测试模块到现有脚本目标映射。 |
| testing/test/Launcher/Application/Translation/source/client_tests.cpp | 修改 | 同一计划、原接口串行调用、完整发布/后备、取消/期限、auto原样传递、诊断单次和必要真实本地调用。 |

现有协议、自定义HTTP、Local和窗口用例只选择有明确影响的用例复跑，不为本次重构这些测试或替身。

文档收尾同步产品需求、开发规范、架构、进度及决策中直接受影响的文字；历史翻译方案仅补后续指向。本稿不包含提交、推送、版本升级或发布。

## 8. 施工和验收

1. 完成Segmentation及纯文本测试，确认200上限、语义下取整与完整字节覆盖。
2. 在client和父CMake接入，使用原提供方接口验证顺序执行、失败后备、取消及一次完整发布。
3. 用已有本地模型验证长文路径，在线使用fake/本机测试边界，不读取用户凭据或调用真实付费接口。
4. Debug /W4 /WX、相关定向回归、git diff和文档同步后交付验收。

必须覆盖：

- 200上限及176句末示例，短文一段，原换行和空行，数字/缩写/网址/引号与Unicode边界。
- 原文范围无遗漏、无重复；全文直通与只有一个正文段但含首尾分隔内容分别正确。
- 纯空白/分隔内容没有可翻译正文时，返回既有输入错误，零次Execute。
- 每家收到相同片段计划，均使用原Validate/Execute，source=auto不被上层替换。
- 全文超过旧10000门禁可以进入分段，特殊文本仍遵守真实接口及token保护。
- 中间段失败不发布半篇；取消后没有后续段或后备；后备从第一段开始。
- 每家仅一条attempt，部分执行后Validate失败仍记为已执行。
- 120秒整轮不因段数或后备重置；诊断仍为一次完整请求。
- 无新增UI、进度、设置或分段专属提示；最终结果使用原身份和修订检查。

## 9. 审核状态

用户已明确批准核心5项范围施工。Astra子Agent已复核该最小架构可行；本稿保留放宽普通整文准入这一原需求接入，并撤掉整篇auto与其他附加策略。

完整文档已通过Astra子Agent交叉复核，并补清Validate/Execute共用deadline、空计划零执行及接缝选项的模块边界；未发现需要扩大文件范围的遗漏。实施与最后只读监督已完成，生产及测试范围保持本稿限定。

## 10. 实施结果（2026-10-26）

- 严格5项生产文件和5项测试文件落地，未修改Local/provider/校验协议/UI/Settings；文档按真实行为同步。
- 95个不同定向用例全部通过、无跳过：translation_segmentation 18、translation 65、translation_local 12。普通及诊断入口、同计划顺序/后备、取消/期限/身份、聚合上限及真实Local长文均有验证，未运行全量。
- 分段回归包含恰200字符后的分隔、中文紧邻网址、裸域名及URL尾标点、句末/弱边界、Unicode序列，以及外缘空白清理不能拆开Mark/VS/ZWJ基字符。只对新增及受影响路径复跑。
- 拼接空白译文使用已有EmptyResult允许原后备，未误判成致命用户输入错误。原文和旧完整结果不被部分译文覆盖。
- 正式Debug /W4 /WX最终构建成功：build/Debug/src/Launcher/Open-ST.exe。
- 测试去重清单：testing/testoutput/segmentation-verification-summary.json；构建日志：segmentation-debug-final.log；分段及整合日志均位于testing/testoutput/segmentation-*.log。
- 本轮普通沙箱定位到vcpkg创建标准输入句柄错误5，仅相关构建使用对应执行权限；普通定向CTest可在沙箱内完成。没有修改工具链、脚本或模型来绕过失败。
- 用户于2026-10-26初步验收后授权提交，提交继续排除.gitignore；推送及发布不在本次指令范围。容量验证不替代实际译文质量判断。
