# Windows 原生 HDR Tone Map 离屏输出验证报告

验证日期：2026-09-07（前期探针）；2026-09-08 产品接入补测见第9节。
验证对象：Direct2D `HDR Tone Map`、`White Level Adjustment` 与 `Color Management` 组合效果
结论：接口、性能及真实 HDR 桌面中的 SDR 参考白语义验证通过；Windows 原生效果链保留为
Open-ST 已选 HDR→SDR 输出方向。现已按第9节接入正式产品并通过受控输出测试；跨系统画质与
候选参数的用户视觉验收仍待完成。第1至8节保留前期探针语境，不能作为“尚未接入产品”的当前状态。

## 1. 验证目的

Open-ST 的冻结桌面按显示输出保留 BGRA8、RGB10A2 或 FP16/scRGB 原始数据。普通 PNG、JPEG
和 Windows 剪贴板最终需要 SDR/sRGB BGRA8 图像，因此必须评估能否复用 Windows 原生效果完成
一次显式 HDR→SDR 转换，避免自行维护色调映射曲线。

本次验证分为首次合成离屏验证和一次修订性真实桌面补测，回答以下问题：

1. 目标机器能否创建并离屏运行全部原生效果；
2. scRGB、色调映射、白电平和 sRGB 编码各阶段的数值行为；
3. 黑位、灰阶、普通桌面白、高光、饱和色和负色域分量的输出情况；
4. `InputMaxLuminance` 与固定 SDR 输出亮度参数是否提供足够且可预测的控制；
5. GPU 完成并回读 CPU 后的冷启动和热运行耗时；
6. 首次报告将 80 nit 视作普通 HDR 桌面中的 SDR 白是否成立；
7. 该路线能否继续作为 Open-ST 首版复制和保存的默认转换方向。

首次验证只使用程序生成的合成像素。修订补测在 HDR 输出上显示由程序绘制的已知 UINT8/sRGB 色块，
使用产品现有 `DesktopCapturer` 捕获后只读取色块中心的数值，不保存桌面图像、原始帧或像素文件，也不修改
系统 HDR 设置。补测完成后临时代码和构建产物均删除。

## 2. 依据与效果链契约

微软文档建议按“颜色管理到 scRGB → HDR Tone Map → White Level Adjustment → 颜色管理到 sRGB”
组织效果链。`InputMaxLuminance` 表示内容最高亮度，`OutputMaxLuminance` 表示输出目标支持的最高亮度。
scRGB 的 `1.0` 对应 80 nit 名义参考白，但它是绝对亮度编码尺度，不能直接等同于 HDR 桌面中普通 SDR
窗口的白色。Windows 会按每个输出的 `DISPLAYCONFIG_SDR_WHITE_LEVEL` 将 SDR 内容提升到用户设置的参考白；
FP16/scRGB 捕获中的预期 SDR 白为 `sdrWhiteLevelNits / 80`。

- [HDR Tone Map effect](https://learn.microsoft.com/en-us/windows/win32/direct2d/hdr-tone-map-effect)
- [White Level Adjustment effect](https://learn.microsoft.com/en-us/windows/win32/direct2d/white-level-adjustment-effect)
- [Color Management effect](https://learn.microsoft.com/en-us/windows/win32/direct2d/color-management)
- [DirectX Advanced Color 与 SDR 参考白](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [微软 Advanced Color Images 样例](https://github.com/microsoft/Windows-universal-samples/tree/main/Samples/D2DAdvancedColorImages)

探针输入已经是线性 scRGB，因此不重复执行 PQ 解码或 HDR10 色域转换。测试链为：

```text
R32G32B32A32_FLOAT scRGB
  → D2D HDR Tone Map（DisplayMode = SDR）
  → D2D White Level Adjustment（InputWhiteLevel = 80 nit）
  → D2D Color Management（scRGB → sRGB，BEST）
  → B8G8R8A8_UNORM
  → GPU 同步、CPU 回读
```

Direct2D 上下文和三个效果均显式请求 `32BPC_FLOAT` 中间精度，并回读检查实际效果属性仍为该值。
输入与目标均为不透明预乘 alpha。终端 BGRA8 只发生一次 sRGB 编码和量化。

## 3. 测试环境

| 项目 | 实际值 |
|---|---|
| 操作系统 | Microsoft Windows `10.0.26200.9168` |
| GPU | NVIDIA GeForce RTX 5090 D |
| NVIDIA 驱动 | `610.88` |
| D3D 驱动类型 | Hardware；未回退 WARP |
| D3D feature level | `0xB000`（11.0） |
| 编译器 | MSVC 19.44（Visual Studio 2022 17.14.19） |
| 编译规则 | C++20、`/W4 /WX /permissive- /utf-8` |
| 仓库基准提交 | `1b3d32e2eabe76241eb4945f6d4cbe582130a8fb` |

工作区已有未提交的文档和忽略规则变更。本次验证没有覆盖或回退这些已有改动。

## 4. 测试程序与执行方式

本次使用一个临时独立可执行程序，未注册成 gTest 或 CTest case。程序在
`testing/test/Launcher/Application/Graphics` 下施工，输出只进入 `testing/testoutput/Debug`；完成本报告后，
源码、CMake 入口、EXE、OBJ、CSV 和 PNG 均删除，不作为项目长期测试基础设施保留。

测试程序执行以下步骤：

1. 创建硬件 D3D11 设备、D2D 设备与 FP32 上下文；
2. 创建 HDR Tone Map、White Level Adjustment 和 Color Management；
3. 分别执行仅颜色管理、白电平加颜色管理、仅色调映射、色调映射加白电平、完整浮点链和完整 BGRA8 链；
4. 使用 16 个已知像素覆盖黑色、线性灰阶、80/160/260/400/1000 nit 灰白、RGB/CMY、负 scRGB
   色域分量和暖色高光；
5. 对同一合成内容比较 `InputMaxLuminance = 260/1000/4000 nit`，并比较
   `OutputMaxLuminance = 80/203/260 nit`；
6. 对 NaN 输入执行前置拒绝，避免非有限值进入 GPU 效果和峰值统计；
7. 生成 1920×1080 色块样张，并对 3840×2160 渐变执行三次预热、十次正式测量；
8. 每次正式测量包含参数设置、输出数组分配、GPU 上传、效果绘制、GPU 完成、映射和 CPU 行复制；
   不把日志输出计入耗时。

内容统计同时计算 `max(R,G,B) × 80 nit` 通道峰值和 Rec.709 亮度峰值，二者不会混作显示器峰值。
峰值参数敏感性矩阵特意让传入属性与实际 1000 nit 合成内容不同，用于观察属性是否影响结果；它不代表
该组合具有正确 HDR 元数据。

### 4.1 修订性真实桌面补测

为验证首次报告对“普通桌面白”的解释，补测临时向既有真实捕获集成程序加入一个受控 case，不增加永久
gTest，也不修改产品源码或 CMake。case 执行以下步骤：

1. 先使用产品 `DesktopCapturer` 枚举真实输出，只选择同时满足 FP16、scRGB 和已取得 SDR 白元数据的
   HDR 输出；若没有满足条件的输出则失败，不使用 BGRA8 降级结果代替；
2. 在该输出中央显示一个无边框、置顶且不激活的普通 UINT8/GDI 窗口，依次绘制黑、sRGB 64/128/192
   灰、白、红、绿、蓝八个不透明色块；
3. 等待 300 ms 让 DWM 完成合成，再通过产品的 `DuplicateOutput1` 路径捕获一次完整冻结帧；
4. 每个色块只读取中心 9×9 像素的 FP16/scRGB 中位数，避免边缘混色、光标或单像素异常；
5. 将捕获值与 `sRGB 分段线性化结果 × (sdrWhiteLevelNits / 80)` 比较；
6. 在正常交互桌面先通过 CTest 一次，再直接运行同一 case 四次记录完整数值并检查重复稳定性。

受限沙箱内首次执行在创建高色深桌面复制会话时返回 `0x80070005 (ACCESS_DENIED)`。这与项目既有真实
捕获测试的环境边界一致；随后在正常交互桌面执行通过，未降低断言或把功能失败改成跳过。补测不重新执行
首次探针已经完成的 D2D 合成曲线和性能矩阵，而是补齐首次结论中缺失的真实 DWM 捕获语义证据。

## 5. 测试结果

### 5.1 接口和精度能力

硬件 D3D11、D2D FP32 上下文、三个原生效果、scRGB/sRGB 颜色上下文、FP32 输入/目标/回读和
BGRA8 目标全部创建成功。直接 MSVC 构建和项目 CMake 显式目标均在 `/W4 /WX` 下通过。

第一次脱离 Visual Studio 环境直接调用已有 Ninja 构建树时，因未加载 Windows SDK include 路径而报告
`C1034: d2d1_1.h: 不包括路径集`。在与项目脚本一致的 `vcvars64.bat` 环境中重跑后成功；该失败与
HDR API 或源码无关。

### 5.2 基础颜色管理

仅执行 scRGB→sRGB Color Management 时，结果与 IEC sRGB 传递函数一致，误差小于探针设置的
`0.015` 浮点容差：

| 线性输入 | D2D sRGB 浮点输出 | 说明 |
|---:|---:|---|
| 0.18 | 0.461341 | 与标准分段传递函数相符 |
| 0.50 | 0.735363 | 与标准分段传递函数相符 |
| 1.00 | 1.000000 | 参考白保持为 1 |

这说明 80 nit 名义白的数值变化不是 sRGB 编码或 BGRA 通道顺序错误。

### 5.3 名义参考白、桌面 SDR 白与高光

以下是完整 BGRA8 链在实际 1000 nit 合成内容下的灰阶结果。数值已换算为 8 位通道：

| 输入亮度 | 80 nit 目标 | 203 nit 目标 | 260 nit 目标 |
|---:|---:|---:|---:|
| 80 nit 名义参考白 | 194 | 192 | 172 |
| 160 nit | 230 | 230 | 220 |
| 260 nit | 241 | 240 | 235 |
| 400 nit | 246 | 246 | 243 |
| 1000 nit | 252 | 252 | 251 |

灰阶保持单调，高光没有在进入色调映射前截断。首次报告把 80 nit 行解释为普通桌面 SDR 白，并据此认为
普通窗口和文字背景会明显变灰；真实桌面补测证明这个前提错误。本机 `DISPLAY1` 的 Windows SDR 白为
260 nit，普通 UINT8/sRGB 白在 FP16/scRGB 捕获中是 `3.25`，因此应使用表中的 260 nit 行评价普通桌面白：
203 nit 目标下输出为 240/255，同时 400 nit 与 1000 nit 分别为 246 和 252，仍保留高光层次。

80 nit 仍是合法的绝对 HDR 亮度样本和 scRGB 编码基准。目标设为 203 nit 时，其各阶段结果为：

| 阶段 | R=G=B 浮点值 |
|---|---:|
| 原始 scRGB | 1.000000 |
| 仅 scRGB→sRGB | 1.000000 |
| 仅白电平调整后再转 sRGB | 0.660731 |
| 仅 HDR Tone Map，仍为线性空间 | 1.343070 |
| Tone Map + White Level，线性空间 | 0.529287 |
| 完整链 sRGB 浮点 | 0.754330 |
| 完整链 BGRA8 | 192/255（0.752941） |

因此 80 nit 样本的变化来自色调映射和白电平组合，不是颜色管理或最终量化造成；但它不能代表本机当前
HDR 桌面的普通 SDR 白。

### 5.4 黑位、峰值属性与色域

- 完整浮点链的黑色被抬到约 `0.00134` sRGB；量化到 BGRA8 后仍为 `0`，最终黑位通过。
- 全部灰阶在三个目标参数下均保持单调，alpha 始终保持不透明。
- 非有限输入在 CPU 分析阶段被明确拒绝，未进入 D2D 效果。
- 对同一像素数据分别设置 `InputMaxLuminance = 260/1000/4000 nit`，逐像素输出没有可测差异。
  本机 Windows/驱动组合下该属性没有提供预期的可控性，不能据此承诺跨系统复现同一曲线。
- 超出 sRGB 色域的强 RGB/CMY 与负 scRGB 分量最终会在一个或多个 8 位通道达到 0 或 255。
  这属于终端 SDR/sRGB 色域和量化边界，不因单个通道达到 255 就单独判失败；但 16 个样本中有
  8～9 个出现终端通道边界。该计数包含合法黑色和纯色，不能单独证明可见裁剪或色相失真；相关画质
  仍需在产品接入阶段用受控对比图和真实桌面视觉复核。

203 nit 目标的代表性输出如下：

| 输入样本 | 最终 sRGB 8 位 RGB |
|---|---|
| 黑色 | 0, 0, 0 |
| 80 nit 名义白 | 192, 192, 192 |
| 260 nit／本机桌面 SDR 白 | 240, 240, 240 |
| 1000 nit 白 | 252, 252, 252 |
| 260 nit 红 | 255, 7, 4 |
| 260 nit 绿 | 46, 255, 34 |
| 260 nit 蓝 | 0, 8, 255 |
| 负绿色域样本 | 255, 0, 102 |
| 暖色高光 | 255, 165, 74 |

### 5.5 性能

| 场景 | 分辨率 | 次数 | 中位数 | 最大值 |
|---|---:|---:|---:|---:|
| 冷启动：设备、效果、资源、完整转换和回读 | 1920×1080 | 1 | 185.567 ms | 185.567 ms |
| 热运行：复用设备和位图，完整转换和回读 | 3840×2160 | 10 | 12.910 ms | 13.315 ms |

冷启动结果说明不能在截图完成操作时同步新建设备和效果。热运行证明，在当前高端 GPU 上复用资源后，
4K 离屏转换与回读的耗时可以接受。这些数字不包含真实桌面捕获、跨屏裁切拼接、WIC 编码、文件 I/O
或剪贴板发布，也不能代表其他 GPU。

### 5.6 真实 DWM 合成与捕获补测

补测输出为 `\\.\DISPLAY1`，产品取得 `sdrWhiteLevelNits = 260`，因此普通 SDR 白在 FP16/scRGB 捕获中的
预期值为 `260 / 80 = 3.25`。受控色块的实测结果如下：

| UINT8/sRGB 输入 | 预期线性 scRGB | 捕获中位数 R,G,B |
|---|---:|---|
| 黑 `0,0,0` | 0 | `0, 0, 0` |
| 灰 `64,64,64` | 0.166626 | `0.166504, 0.166504, 0.166504` |
| 灰 `128,128,128` | 0.701547 | `0.701172, 0.701172, 0.701172` |
| 灰 `192,192,192` | 1.713120 | `1.712890, 1.712890, 1.712890` |
| 白 `255,255,255` | 3.250000 | `3.250000, 3.250000, 3.250000` |
| 红 `255,0,0` | 主通道 3.250000 | `3.250000, 0, 0` |
| 绿 `0,255,0` | 主通道 3.250000 | `0, 3.250000, 0` |
| 蓝 `0,0,255` | 主通道 3.250000 | `0, 0, 3.250000` |

三个灰阶的绝对误差分别约为 `0.000122`、`0.000375` 和 `0.000230`，符合 binary16 量化精度。直接运行
case 的首次记录及随后三次重复记录逐项完全一致；单次 case 用时为 698～726 ms，其中包含两次真实全桌面
捕获、300 ms DWM 等待、窗口创建与销毁和 ROI 采样，不能与 4K 纯转换热运行的 12.91 ms 混用。

该结果证明当前 Windows 配置下，产品取得的 FP16/scRGB 桌面帧已经包含 DWM 对普通 SDR 表面的参考白
缩放。下游 HDR→SDR 输出不得把捕获值再次乘以 `3.25`，也不能把 scRGB `1.0` 当作当前桌面普通白。
本次只验证已知 UINT8/sRGB 表面及捕获语义，没有在同一帧额外创建自有 FP16 HDR 交换链，也不替代用户
对真实 HDR 应用、高光、综合色彩和跨显示器结果的视觉验收。

## 6. 验收与回归

| 检查项 | 结果 |
|---|---|
| 独立探针直接 MSVC `/W4 /WX` 构建 | 通过 |
| 独立探针项目 CMake 显式目标 `/W4 /WX` 构建 | 通过 |
| 原生效果创建、逐阶段绘制和 CPU 回读 | 通过 |
| IEC sRGB 独立公式检查 | 通过 |
| 黑位量化、灰阶单调、alpha、不有限值拒绝 | 通过 |
| 真实 HDR 桌面 SDR 白缩放与 FP16 捕获 | 通过；260 nit 对应并实测 scRGB 3.25 |
| 受控 sRGB 灰阶和 RGB 色块重复稳定性 | 通过；四次记录逐项一致 |
| Graphics 既有自动测试 | 28/28 通过 |
| 清理后既有真实桌面捕获回归 | 1/1 通过 |
| 产品 Debug `/W4 /WX` 构建 | 清理临时探针后通过 |
| 仓库 diff 与临时文件清理 | 通过；结果见第 8 节 |

## 7. 结论与后续建议

首次报告关于默认画质不合格的结论不成立，因为它把 scRGB `1.0`／80 nit 编码基准误认为本机 HDR 桌面
的普通 SDR 白。修订补测证明普通 UINT8/sRGB 白经 DWM 合成后在产品捕获中为 scRGB `3.25`／260 nit；
使用首次探针同一张 1000 nit 合成表重新评价，203 nit 目标下普通桌面白输出 240/255，400 nit 和 1000 nit
高光输出 246 和 252。该分配没有首次报告所述的“普通窗口明显发灰”证据，并为更亮高光保留了码值。

修订后的阶段结论为：

1. 按用户选择，保留 `HDR Tone Map + White Level Adjustment + Color Management` 作为 Open-ST 首版
   HDR→SDR 输出技术方向；不再依据 80 nit 行否决 Windows 原生路线；
2. 捕获得到的 FP16/scRGB 普通 SDR 内容已经按逐显示器 `SDRWhiteLevel` 缩放，输出链不得再次乘参考白比例；
3. 保留“冻结帧是唯一输出源、只执行一次显式 HDR→SDR、最终生成 SDR/sRGB BGRA8”的架构约束；
4. 203 nit 组合目前是有实测依据的候选值，但本报告不把它升级为已经完成跨系统验证的永久参数；离线
   PNG/剪贴板的 White Level 参数方向仍须在产品接入阶段通过受控输出和视觉对比确认；
5. `InputMaxLuminance` 在本机 260/1000/4000 nit 间无可测响应、饱和色会触及 sRGB 边界，仍属于原生
   黑盒曲线的跨系统与综合色彩风险，不能因路线被选定而删除这些记录；
6. 设备和效果应在截图会话或图形服务中复用，避免约 186 ms 冷启动落在复制或保存动作上；
7. 本次数值补测纠正了捕获语义，不替代真实 HDR 应用、跨屏区域及最终 PNG/剪贴板结果的用户视觉验收。

## 8. 最终清理与验收记录

首次验证和修订补测完成后已删除以下内容：

- 临时 `native_hdr_probe.cpp` 与早期 `smoke.cpp`；
- `open_st_native_hdr_probe` CMake 目标；
- 探针 EXE、OBJ、CSV、PNG 和独立输出目录；
- CMake 构建树中该临时目标的残留目录和可执行文件；
- 临时受控 GDI 色块窗口、binary16 采样辅助函数和补测 gTest case；
- 重新配置产生的临时 case 注册及测试可执行文件中的补测代码。

删除后重新运行 `scripts/test.ps1 graphics`，项目重新配置并执行既有 Graphics 测试，结果为 28/28
通过；在正常交互桌面运行既有 `captures_virtual_desktop` 捕获回归，结果为 1/1 通过。随后运行
`scripts/build.ps1`，Debug 产品构建通过 `/W4 /WX`。

重新配置后的 CTest 清单和 `open_st_capture_tests.exe --gtest_list_tests` 均不再包含补测 case。
`testing/test/Launcher/Application/Graphics/Capture/source/desktop_capturer_tests.cpp` 的工作树和 HEAD 内容哈希
均为 `df05477dc7eb77dac6c18db2b136fcaf3d5156e8`，`git diff` 为空。当前沙箱只读挂载 `.git`，无法刷新
索引时间戳，因此 `git status` 仍可能暂时把该文件显示为修改；内容哈希、测试重新配置及构建共同确认没有
留下测试源码变更。最终工作区只保留本报告以及此前已经存在并继续更新的决策、进度和忽略规则变更。

## 9. 2026-09-08 产品接入与自动验证

本次已把颜色解码、NativeToneMapper 和原生效果链落入 Graphics/HDR 独立目标，并接入 Graphics 冻结选区
裁切以及 Application/Export 复制与保存。正式链为 HDR Tone Map SDR/203nit → White Level 80/203nit →
scRGB/sRGB Color Management BEST，所有效果FP16精度显式设置并回读。内容峰值与传给效果的下限80nit
参数分别记录；不重复乘 Windows SDR白比例，负分量保留，非有限RGB拒绝，消费者按不透明BGRX解释。

设备和效果复用，UI消息预热微小输入；硬件设备/效果构建或完整预热失败最多尝试一次WARP。每次转换断开
输入和target引用、ClearResources，并释放局部上传/目标/CPU_READ位图。FP16直接复制位模式和扫描指数，
避免逐像素decode/encode造成Debug约600ms的额外开销，输出像素与既有转换测试保持一致。

### 9.1 输出验证

正常桌面权限下全量171/171通过，无跳过。其中HDR 13项、Export 12项、Graphics 47项、Application 12项。
新增混合SDR/HDR测试按同一HDR ROI独立转换逐字节对照，并验证相邻SDR含第四字节精确保留。复制测试用
fake系统边界，不修改实际剪贴板。PNG RGB无损、JPEG尺寸与误差、sRGB元数据、文件失败清理均通过。

1000nit合成灰阶测试的黑位、单调性、260nit桌面白范围及高光区分通过。另一个10000nit合成输入发现：

| 输入 | 黑BGRA | 白BGRA | 等价FP16对照 |
|---|---|---|---|
| RGB10 scRGB，白1.0/80nit | 0,0,0,255 | 169,169,169,255 | 整张输出逐字节一致 |
| RGB10 HDR10，白码1023/10000nit | 3,3,3,255 | 255,255,255,255 | 整张输出逐字节一致 |

PQ零输入与矩阵转换仍为零，偏置出现在原生链。格式测试因此验证与等价FP16一致，不通过任意放宽黑位
阈值掩盖问题；独立1000nit黑位断言继续保留。10000nit场景黑位3/255作为已知限制，不代表真实内容视觉通过。

### 9.2 本次Debug性能与内存

最终全量测试中，3840×2160 FP16灰阶输入的完整Convert首次196.044ms；三次热转换63.0371、64.3137、
65.0455ms。该数据含CPU验证/紧凑复制、GPU上传/原生效果、回读和图像释放，与早期探针的纯转换数据
不可直接比较，也不代表Release性能或快捷键到冻结遮罩耗时。

| 采样时刻 | Working set字节 | Private usage字节 |
|---|---:|---:|
| 已分配测试输入，尚未创建mapper | 77983744 | 68427776 |
| 完成四次转换，保留输入/输出 | 187797504 | 303972352 |
| ReleaseImageResources并释放测试输入/输出，保留设备效果 | 88256512 | 204230656 |

最终工作集约84.2MiB、私有提交约194.8MiB，80MiB目标未达成；CPU进程计数不等同显存，也不能据此证明
没有驱动缓存。本次没有清空进程working set伪造内存达标，真实产品4K导出后回托盘内存仍须补测。

### 9.3 尚待人工阶段验收

Computer Use应用启动授权等待超时，因此未进行实际对话框与焦点检查；自动测试不替代用户对快捷复制、
保存、取消与失败后重试的体验确认。HDR原屏观感、跨屏拼缝、跨驱动颜色和既有跨屏掉帧事项继续保留待验收。
本次没有新增第三方依赖、临时截图、独立报告或探针产品目标。
