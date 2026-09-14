# 贴图层级问题验证报告（2026-09-14）

范围：仅确认两项反复失败的测试是否对应真实产品问题，不修产品、不修改原测试、不放宽原断言。
本次验证跨日，原始产物目录沿用开始日期：testing/testoutput/pin-order-verification-20260913/。

## 1. 结论

1. **存在真实冗余层级请求。** 在贴图相对顺序、置顶位和上层采样点命中均正确时，90 次正式 RepairOrder 调用，
   每次都向上层贴图发出 1 条 WM_WINDOWPOSCHANGING。该请求未改变始终位于其前面的 cloaked 系统辅助窗口。
2. **本次没有复现模态结束后仍有实际遮挡或顺序错误。** 24 个前置条件成立的受控场景全部恢复相对顺序、
   两个采样点命中和置顶位，其中 18 个确实建立了可见窗口遮挡，6 个实际取消了下层贴图的置顶位。
3. **两项原测试的共同红灯来自“前驱必须为 nullptr”。** 这一绝对 Z 序条件不能代替业务层级与可见性判断。
   因而当前同时存在测试判据问题和生产无效调整问题，不能简单归为全部测试误报。

本报告不证明没有肉眼可见闪烁，不承诺所有 Windows 环境，也没有测得程序自主持续循环或相应 CPU 占用。

## 2. 原测试复现

在正常桌面权限下保持原二进制、原筛选和断言，连续运行三轮；每轮两项都失败，共 6 次失败。

- repeated_correct_order_emits_no_position_requests：在 pin_window_tests.cpp:234 的 ASSERT 失败。
  此时计数器尚未创建、后面的 10 次 RepairOrder 未执行，原失败本身不能证明冗余次数。
- modal_exit_repairs_external_window_interleaving：在 :271 与 :280 的 EXPECT 失败，均要求 upperWindow 的前驱为空。
  该轮日志未显示其他恢复断言失败。原干扰 STATIC 窗口仅位于 (0,0)、大小 1×1，未与贴图区域建立实际遮挡。

日志：original-1.log、original-2.log、original-3.log。

## 3. 失败前驱的即时身份

本轮原测试和独立探针中的前驱均为 `0x90888`，另进行了正常桌面权限下的只读查询：

| 属性 | 本次值 |
| --- | --- |
| 类名 | ThumbnailDeviceHelperWnd |
| 进程 | C:\WINDOWS\Explorer.EXE |
| PID / TID | 13384 / 15004 |
| IsWindow | true |
| IsWindowVisible | true |
| DWMWA_CLOAKED | 1，查询成功 |
| 矩形 | (0,0)–(1,1) |
| 置顶位 | WS_EX_TOPMOST 存在 |
| owner | 无 |

这是 Explorer 的 cloaked 辅助窗口，不会绘制为本次贴图区域的遮挡物。
`IsWindowVisible` 单独不足以判断实际呈现；本轮同时检查了 DWM cloaked、矩形和采样点命中。
此前输入法窗口只是另一次观察中的候选解释，不能用于解释本轮固定前驱；本次即时身份记录取代该猜测。
窗口句柄可能复用，本报告不追认所有历史同数值句柄的归属。

完整查询记录：predecessor-live.json。

## 4. 独立探针方法

- 探针只链接已构建的原 PinWindow 与 Common 静态库；通过既有测试友元调用正式 RepairOrder，不复制或替换修复算法。
- 原源码、测试和工程 CMake 均未修改。新增探针源文件、独立 CMake 和输出只在忽略的测试产物目录。
- 用当前线程 WH_CALLWNDPROC 只观察两张自有贴图的 WM_WINDOWPOSCHANGING，记录目标、插入位置及 flags，不改消息。
- 每次调用分别记录前后 Z 序、置顶位、可见/cloaked 属性和窗口命中，不把原测试的失败前提作为探针主体的执行门槛。
- 命中采用物理坐标与 WindowFromPhysicalPoint；DWM 坐标失败时进行明确的逻辑到物理转换，坐标不可用则不能判读。
- 可见干扰窗口采用自定义普通窗口类，避免 STATIC 文本控件在命中测试中的特殊处理。
- 跨进程干扰由探针自己的子进程建立，通过有界 ready event 等待就绪，按物理矩形创建；不操作其他用户程序。
- BeginModal、SetWindowPos、DPI 设置的前提及每类干扰的实际命中被检查。只有干扰前提成立才计入恢复结果。
- 所有自有窗口和子进程已回收，结束后 pin_order_probe 进程数量为 0。

## 5. 实验矩阵和结果

### 自然顺序下的冗余请求

| 环境 | 运行数 | 每次调用数 | 位置请求 | 顺序/上层命中 |
| --- | --- | --- | --- | --- |
| 原测试几何：32×24，两图同在 (30,30)，DPI unaware | 3 | 10 | 每次 1 条，共 30 | 全部正确 |
| 部分重叠大图，DPI unaware | 3 | 10 | 每次 1 条，共 30 | 全部正确 |
| 部分重叠大图，Per-Monitor V2 | 3 | 10 | 每次 1 条，共 30 | 全部正确 |

90 条请求全部针对上层贴图，flags 为 531（0x213），属于含 Z 序调整意图的位置请求。
本报告记录的是请求，不把 WM_WINDOWPOSCHANGING 等同于成功移动或实际重绘。
每次结束后的前驱仍是上述 cloaked Explorer 窗口，说明当前生产“前驱非空就修复”的条件始终不能被这类请求消除。

### 模态结束恢复

以下四类分别在大图 DPI unaware 和 Per-Monitor V2 环境各运行三次：

| 场景 | 事前验证 | 次数 | 事后结果 |
| --- | --- | --- | --- |
| 同进程可见窗插入两图之间 | 下层独露点命中干扰窗，上层点命中上层贴图 | 6 | 相对顺序和两点命中恢复 |
| 同进程可见窗盖到最上方 | 两点均命中干扰窗 | 6 | 相对顺序和两点命中恢复 |
| 下层贴图取消置顶 | WS_EX_TOPMOST 确实被清除 | 6 | 置顶位、顺序和两点命中恢复 |
| 自有子进程可见置顶窗覆盖 | 无 owner、不同 PID，两个点均命中干扰窗 | 6 | 本机观测到顺序和两点命中恢复 |

24 次前置全部成立，24 次后置全部满足，每次 EndModal 产生 2 条位置请求。
跨进程置顶测试是本机兼容性观察，不把“必须压过所有其他程序置顶窗”增加为产品需求。
两个点分别用于检查下层独露区和上层区；命中并不等于全区域逐像素或无闪烁的视觉验收。

日志：original-*.jsonl、unaware-*.jsonl、pmv2-*.jsonl。结构化汇总见 summary.json，汇总脚本为 summarize.py。

## 6. 代码与需求对照

- RepairOrder 的 alreadyPlaced 同时检查置顶位和原生前驱；对于业务顶部窗口，expectedPredecessor 被设为 nullptr。
- 因此前驱即使没有实际呈现，仍使生产每次调用进入 SetWindowPos 分支。重入保护仍在，不能据此说存在无限递归。
- 原贴图设计将本程序贴图的相对顺序与普通桌面置顶作为目标，没有要求压过所有不可见系统辅助窗口。
- Windows 的 HWND_TOPMOST 语义是高于非置顶窗口，不能简单改写成任何环境下 GW_HWNDPREV 必须为空。

## 7. 原验证边界与后续决定

以上数据来自未修改产品和原测试的验证阶段，当时原两项仍失败；不将历史失败结果改写为通过。
证据只确认位置请求冗余，未确认闪烁、卡顿或层级恢复功能故障；24 个有效恢复场景全部通过。

用户随后确认实际使用无明显问题，并批准先纠正测试和记录、后置生产优化，见 D-066 及
[缩减后的方案范围](design/pin-order-remediation-v0.3.md)。因此不再把原两项测试失败列为两个功能 bug。
测试纠正保留第一项原名及零请求断言，让已知冗余如实失败；第二项通过实际遮挡、相对顺序、置顶和命中验证功能。
不因生产优化后置而禁用或跳过原用例，不把删除绝对空前驱断言表述为冗余已消除。最新回归结果见实现进度。

子 Agent 独立解析了全部 9 份 JSONL，重新核对 PID、可见性、cloaked、前后命中及事件计数，结果与本报告一致。

参考：

- [原贴图设计](design/pin-window-v0.2.md)。
- [SetWindowPos 官方说明](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)。
- [GetWindow 官方说明](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow)。
- [本地诊断汇总](../testing/testoutput/pin-order-verification-20260913/summary.json)。
- [即时前驱元数据](../testing/testoutput/pin-order-verification-20260913/predecessor-live.json)。
