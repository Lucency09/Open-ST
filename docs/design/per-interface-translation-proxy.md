# 翻译接口独立代理配置迁移方案（已批准施工）

日期：2026-10-26。基线：35d1038，自动分段已本地提交，已发布版本为v1.2.0。用户于本轮确认方案并授权施工；实施前已澄清Common与Settings的职责。

## 1. 本次目标

- 删除设置窗口中的“翻译网络”整页。
- 在每个在线翻译接口的新增/编辑窗口中配置它自己的连接方式与代理地址。
- 支持明确直连；同一个提供方的多个接口条目也可使用不同代理。
- 正式翻译和“测试连接”使用同一条目的网络配置。
- 迁移已有全局代理配置，保留升级前的有效路由选择。

本稿只做这项配置和界面迁移，不新增模块、线程、第三方依赖或代理协议。Local、Segmentation及翻译调度算法无需修改。

## 2. 界面与行为

### 2.1 每接口编辑布局

在现有名称、类型、启用状态之后，供应商参数之前增加网络配置组，复用WindowRenderer现有select/edit与布局：

```text
接口名称 / 接口类型 / 启用

连接方式：[跟随系统 / 直连 / 自定义HTTP代理]
代理地址：[http://127.0.0.1:7897             ]
说明：代理地址仅在“自定义HTTP代理”时生效。

原有供应商参数、测试连接和确认/取消
```

- 新建在线接口默认“跟随系统”，地址为空，保持当前默认路由语义；用户可单独改为直连或自定义。
- 地址框保留可见和可编辑，非空值仍按现有HTTP代理格式校验；自定义模式必须填写有效地址。这样旧的错误地址可以在同一窗口直接修复，不会被隐藏或禁用控件挡住。
- 跟随系统/直连时，保留草稿中的有效地址供切换模式使用，但绝不把该地址用于请求。
- 新增窗口切换在线类型时，网络草稿作为本接口共同字段保留；切到本地类型时隐藏整组，切回在线仍保留本次草稿。
- 内置CTranslate2本地接口不联网，不显示也不保存network字段。它的引擎和配置结构继续复用。
- 子窗口“确定”只更新父设置草稿，主窗口“应用/确定”才保存；取消不污染父草稿。
- 代理草稿变化后，现有testedCandidate比较会标识旧测试结果；正在执行的测试继续使用它提交时的快照，原取消/关闭规则保持。

### 2.2 三种连接方式

| 配置值 | 用户选择 | 行为 |
| --- | --- | --- |
| system | 跟随系统 | 保留现有WinHTTP系统代理/PAC语义，系统规则可以选择代理或直连。 |
| direct | 直连 | 明确不使用系统HTTP代理，采用WINHTTP_ACCESS_TYPE_NO_PROXY。 |
| custom | 自定义HTTP代理 | 使用本条目地址，沿用现有HTTP代理格式及校验。 |

系统模式与明确直连不能混为一谈。WinHttpOpen分别提供AUTOMATIC_PROXY、NO_PROXY和NAMED_PROXY；非NAMED模式传入NO_PROXY_NAME/NO_PROXY_BYPASS。[微软API说明](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpopen)

本接口代理失败不会自行改为直连。若原有有序后备进入下一接口，则下一接口使用自己的连接方式。直连描述针对HTTP代理设置，不改变系统VPN或底层网络路由。

## 3. 数据结构与唯一来源

在线接口顶层新增network，和configuration、secrets并列，所有在线类型（含自定义HTTP）共用：

```json
{
  "id": "stable-id",
  "name": "我的接口",
  "enabled": true,
  "kind": "openai",
  "configuration": {"base_url": "https://example.com/v1", "model": "model-name"},
  "secrets": {"api_key": ""},
  "network": {
    "proxy_mode": "custom",
    "proxy_address": "http://127.0.0.1:7897"
  }
}
```

不把代理混进供应商configuration，以免改动Custom HTTP协议编辑器。线上条目要求network完整，Local保持原六字段。已有network即使非法也不能被默认值覆盖；错误交给领域校验并允许在编辑窗修复。

- 新默认资源中的各在线模板直接带network；不再包含两个全局代理键。
- 自定义HTTP沿用现有领域新建模板入口，为其补齐system/空地址；本次不迁移整套自定义模板或新增用户级默认代理。
- TranslationConfiguration移除proxyMode/proxyAddress。运行配置只从当前profile.network取得代理，不保留全局运行期回退。
- TranslationSettingKeys和ReadTranslationSettings删除旧全局代理读取；全局源/目标语言、本地档位目录仍沿用。
- 网络对象计入现有条目/列表预算；不因此放宽现有大小限制，超限保留数据并报告配置错误。

旧持久化键translation.network.proxy_mode与translation.network.proxy_address仅出现在迁移逻辑和相关回归中。编辑元数据可使用profile.network.proxy_mode等名称，避免把新控件误当旧全局设置绑定。

## 4. 旧配置迁移

Settings仅决定配置字段如何迁移：在settings.cpp现有初始化JsonFileHandle::Write回调中修改内存候选文档，将字段搬移和旧键清理交给同一次Common事务提交。文件读取、JSON解析、文件锁、并发冲突检测、原子写入及读写错误均由Common现有实现负责；Settings不重复实现这些机制，Common的JSON文件管理代码无需修改。

| 旧状态 | 处理 |
| --- | --- |
| 在线条目缺network，旧全局代理有效 | 将旧mode/address复制到该条目，包括未启用条目，保留路由选择。 |
| 条目已有network | 原样保留，包括显式null或非法值；不从旧全局值覆盖。 |
| 旧mode或address键缺失 | 分别使用旧版既定缺省system、空地址；显式存在的值即使类型/内容错误也不能回退。 |
| 用户没有interfaces，但有旧全局代理键 | 先物化默认接口副本，再把旧代理赋给副本中的在线条目；不能被新模板已有的system覆盖。 |
| interfaces与旧代理都缺失 | 继续使用正常的新默认资源，不为此额外物化设置。 |
| 本地条目 | 保持原样，不添加network。 |
| 旧mode/地址非法 | 原始值移入对应network，由新领域校验报错；不改成system/direct。 |
| 结构不完整，无法安全判断迁移范围 | 保留原文件及旧键，报告配置问题，不猜测或重建用户列表。 |

全部可迁移条目处理完成后，删除两个旧全局键；接口ID、顺序、名称、启用状态、参数、密钥及其他无关设置保持。重复初始化不覆盖已有network，不再重复改变数据。

迁移只负责结构搬移，不在Settings复制Translation/Http的代理语法规则。现有非法network不参与默认修复，编辑窗口必须以错误状态展示并允许用户主动选择模式或修改地址。

### 4.1 迁移未完成时的配置默认值策略

以下属于Settings的业务默认值策略，不是文件读写安全机制；Common只报告事务成功或失败，不解释翻译代理字段。已有Settings初始化允许“默认资源可用但用户文件暂不可写”。需要阻止以下情况：旧全局custom仍未迁移，而读取层直接拿新默认interfaces中的system发出请求。

在settings.cpp增加私有的“遗留代理尚待承接”结构判断：旧全局键仍存在，并且无法从用户文档确认接口列表已完整承接网络配置；包括interfaces缺失、类型错误、条目结构无法识别，以及在线条目缺network。已有network的非法内容交给领域校验，不据此阻断编辑。遇到尚待承接状态：

- 包含translation.interfaces的Snapshot读取返回失败，禁止默认接口回退。
- GetJsonSetting读取translation.interfaces时也阻止同样的默认回退。
- EditFiles返回失败，暂不把旧用户文件与新模板交给编辑事务，避免“测试连接”从默认system草稿启动。

保留原文件和既有持久化失败提示，不改变Initialize的整体成功/失败契约。恢复配置文件可写性并重启后重试迁移；该异常期间设置编辑暂不可用，不增加新的修复界面或事务协议。

已有完整network的配置即使残留旧键，也不能被此判断错误阻断。非法旧值若已成功搬入network，属于领域校验/用户修复问题，设置编辑仍可进入。

## 5. 从编辑到请求的完整链路

### 5.1 编辑与测试连接

EditTranslationProfile和submitTranslationTest删除独立proxyMode/proxyAddress参数。测试只提交完整候选profile及原样本/档位参数，代理就是当前未保存草稿的一部分。

App仍把候选复制到唯一TranslationClient的诊断请求中，临时启用仅作用于该请求；测试不保存配置。testedCandidate包含network，所以网络修改自然标记测试结果过期，无需新增状态机。

### 5.2 正式请求

内置BuildRequest和BuildCustomRequest共用一个父Translation私有代理转换函数，将已校验profile.network映射为http::Proxy：

- 模式解析明确区分system/direct/custom，未知值返回配置错误，不能用else默认为直连。
- 只有custom传出地址，system/direct使用空地址。
- 代理合法性仍由Translation领域校验和Common/Http通用请求校验承担，不在UI重复解析。

共享函数声明放既有translation_protocol.h、实现在translation_protocol.cpp，不新增模块或源文件；两个构造入口都保留“输出完整请求”的契约。

刚完成的分段逻辑已经把当前profile传给每次Execute；当前整篇不可变interfaces快照自然包含每个network，所以不修改translation_client.cpp或Segmentation。后备到另一接口时按该接口network构造新的请求，不能复用上一接口代理。

TextSession删除旧的两个全局字段比较；现有interfaces深比较已覆盖各条目network，设置应用继续使在途请求失效，不自动重发。

### 5.3 Common/Http的必要改动

ProxyMode增加Direct，默认值仍为System；IsValidRequest认可Direct并要求其实际请求地址为空。传输明确映射到WinHTTP的NO_PROXY，保留System和Custom原行为、TLS、超时、取消及响应预算。

Update等其他调用者没有被迁移到接口配置，继续消费Proxy默认System。此次仅补真正直连的传输能力，不改更新策略、不支持新代理协议或代理认证。

## 6. 删除旧页面与清理

- setting_windows.json删除translationNetwork页面。
- translation_settings.h中的全局字符串键只保留源/目标语言；translation_settings.cpp移除旧代理控件绑定、读取、校验集合及传参。
- settings_window.cpp清理旧网络页的功能开关分支；恢复默认仍通过现有接口列表事务处理，因此每接口代理随对应默认条目恢复。
- 复用既有代理标签，新增direct三语选项和接口级说明，删除不再被引用的旧页文案。
- Local编辑器不生成无效代理控件；CustomTranslationEditor只修改configuration/secrets，继续保留候选中的network，无需重构。

## 7. 文件级范围

本次涉及配置持久化、编辑、测试回调及实际传输四条既有通路。预计19个既有生产/资源文件，全部直接对应迁移或直连；没有新增产品文件、模块、CMake改动。

| 编号 | 文件（仓库相对路径） | 必要原因 |
| --- | --- | --- |
| P01 | src/Common/Http/include/http_transport.h | 增加Direct枚举，默认仍System。 |
| P02 | src/Common/Http/source/http_request.cpp | Direct合法性及无代理地址约束。 |
| P03 | src/Common/Http/source/http_transport.cpp | 显式NO_PROXY映射，保留原System/Custom。 |
| P04 | src/Launcher/Application/Translation/include/translation_client.h | 移除全局代理成员；保留原客户端/提供方执行接口。 |
| P05 | src/Launcher/Application/Translation/source/translation_options.cpp | 每接口network结构/校验、新建模板、移除旧全局读取与键目录。 |
| P06 | src/Launcher/Application/Translation/private/translation_protocol.h | 声明两个协议构造共用的窄代理转换函数。 |
| P07 | src/Launcher/Application/Translation/source/translation_protocol.cpp | 实现共同转换，内置请求使用当前profile代理。 |
| P08 | src/Launcher/Application/Translation/source/translation_custom.cpp | 自定义请求使用同一转换，移除全局代理取值。 |
| P09 | src/Launcher/Application/Settings/source/settings.cpp | 通过Common现有事务搬移字段、清理旧键；迁移未完成时阻止改变代理路径的默认值回退。 |
| P10 | src/Launcher/Application/Settings/private/translation_settings.h | 删除旧键与EditTranslationProfile代理参数。 |
| P11 | src/Launcher/Application/Settings/source/translation_settings.cpp | 清理父页代理绑定/读取，改为只传完整profile。 |
| P12 | src/Launcher/Application/Settings/source/translation_profile_dialog.cpp | 网络草稿、控件、校验、候选序列化与测试快照。 |
| P13 | src/Launcher/Application/Settings/include/settings_window.h | 简化测试回调签名，代理属于profile。 |
| P14 | src/Launcher/Application/Settings/source/settings_window.cpp | 清理已删除网络页的分支及必要调用适配。 |
| P15 | src/Launcher/Application/source/app_callbacks.cpp | 测试请求只消费完整候选，删全局代理传参/赋值。 |
| P16 | src/Launcher/Application/source/text_session.cpp | 删除全局代理比较，继续通过interfaces判断配置变化。 |
| P17 | resources/default_settings.json | 在线模板写入network，删除全局代理默认键。 |
| P18 | resources/setting_windows.json | 删除翻译网络页。 |
| P19 | resources/ui_text.json | 三语直连/接口网络文案，清理失效页文本。 |

Local、Segmentation、translation_provider.h/.cpp、translation_client.cpp、Common/WindowRenderer、Update及安装发布代码均不改。

### 7.1 必要测试文件（9项）

| 文件 | 验证内容 |
| --- | --- |
| testing/test/Common/Http/source/http_tests.cpp | Direct校验/回环请求，默认System及既有Custom回归。 |
| testing/test/Launcher/Application/Settings/source/settings_tests.cpp | 迁移、已有network保留、非法值、缺interfaces、失败读门禁与幂等。 |
| testing/test/Launcher/Application/Settings/source/settings_window_tests.cpp | 页面移除、草稿/恢复默认/应用及旧键不再绑定。 |
| testing/test/Launcher/Application/Settings/source/translation_profile_dialog_tests.cpp | 模式/地址、切换类型保留草稿、本地隐藏、未保存代理测试及取消不写回。 |
| testing/test/Launcher/Application/Translation/source/protocol_tests.cpp | 内置接口分别生成System/Direct/Custom请求。 |
| testing/test/Launcher/Application/Translation/source/custom_tests.cpp | 自定义HTTP同样使用接口network及非法数据拒绝。 |
| testing/test/Launcher/Application/Translation/source/client_tests.cpp | 不同接口不同路由、分段/后备/快照一致，迁移旧全局代理断言。 |
| testing/test/Launcher/Application/source/hotkey_integration_tests.cpp | 适配真实App测试回调签名，验证候选网络快照。 |
| testing/test/Launcher/Application/source/text_translation_interaction_tests.cpp | 接口代理变更使旧结果过期，提交快照使用新字段。 |

沿用现有测试目标，预计不改测试CMake。其他测试夹具通过默认资源创建条目时自动得到新network；若发现直接构造旧条目/旧回调的额外必要调用方，先明确证据再调整范围。

### 7.2 文档同步

获准施工后同步产品需求、开发规范、架构、实现进度和决策中直接相关内容；历史翻译方案只增加后续指向，不改写原审核记录。当前仅新增本稿。

## 8. 分步实施与验收

1. 补Common/Http Direct及对应定向测试。
2. 完成接口数据、迁移、正式请求与测试请求适配；结构/签名变化的必要调用方在同一步迁移，避免留两套生产代理入口。
3. 移除网络页，完成每接口编辑、草稿/测试/恢复默认及三语说明。
4. Debug /W4 /WX和相关定向测试、diff与文件范围核验，交付实际页面验收。

必须覆盖：

- 接口A自定义代理、接口B直连、接口C跟随系统；顺序后备及所有分段都使用当前接口自己的设置。
- 同一种供应商的两个条目使用不同代理，不能因复用provider实例沿用上一条的路由。
- 自定义代理不可达时，不在本条目悄悄改走直连。
- 编辑未保存时测试使用当前草稿，修改代理标记旧测试结果；取消保留父草稿和原文件。
- 旧custom/system迁移保持模式；已有新network优先；非法旧值不被默认值修复；重复迁移不覆盖独立配置。
- 旧代理存在但interfaces缺失时正确物化；覆盖interfaces类型错误、条目结构无法识别及在线缺network；模拟写入失败后正式翻译、默认读取和设置测试入口不能获得错误的默认System/Direct。
- 本地条目不含network，纯本地不发网络；恢复默认、两个文本窗口的配置失效与旧快捷键流程正常。
- Direct回环传输、枚举映射及默认System分别核对；不修改机器系统代理/PAC，不用真实付费接口验收。

测试限定Http、Translation、Settings及明确受影响的Application用例；Common变动不等于全量回归。构建同时覆盖必要调用方，保留Update默认System的断言/核对。此方案不含提交、推送或发布。

## 9. 审核记录

Astra子Agent已只读核对数据流、公共传输、旧配置迁移与编辑事务，支持profile顶层network、三模式、Local零网络及回调参数清理；提出的缺interfaces物化、迁移失败默认回退、text_session旧成员引用和共享转换函数均已纳入。

完整文档已通过Astra最终交叉复核；最终意见要求将interfaces类型错误、条目结构无法识别纳入迁移失败门禁，已补充且不增加文件范围。用户已批准施工，并要求明确文件读写安全由Common负责；本稿已据此修订。实施过程只操作仓库及隔离测试配置。

## 10. 实施结果（2026-10-26）

按上述19个生产/资源及9个测试文件范围完成，Debug /W4 /WX通过，268项定向测试通过、1项人工欢迎窗口跳过。施工监督发现的多条非法network无法逐条修复问题已改为允许父草稿暂存修复，最终应用仍校验全表；窗口测试发现的说明节点缺id已修复并复跑通过。Common JSON实现、Local及Segmentation未修改。
实际页面视觉验收因Computer Use启动授权超时尚未完成；真实窗口自动化不等同于完整视觉验收。详情见[实现进度](../implementation_progress.md)。本轮未提交、推送或发布。
