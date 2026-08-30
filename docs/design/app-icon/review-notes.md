# 图标方案检索与修订记录

日期：2026-09-08。状态：用户已定稿 A2，并完成应用接入。

用户选择原 A，并要求联网检查重复；随后要求框内加入 ST 元素。
检索 screenshot/scan 图标、蓝青四角图形及 Snipaste/ShareX 等应用标识。
结果存在通用四角框符号；不能仅根据共有四角元素判断整体高度相似。本次检索未确认与原 A
三蓝一青、空心圆角框组合完全相同的图标；这不构成全网唯一性或法律检索结论。
此前将两个同类通用图标表述为“明显相似、不宜定稿”过于武断，已更正。

检索参考：
- https://www.flaticon.com/free-icon/scan_6659942
- https://icons8.com/icon/111371/full-screen
- https://www.flaticon.com/free-icon/frame_7690983
- https://www.snipaste.com/index.html
- https://getsharex.com/brand-assets

用户已于本轮明确“落稿A2”。定稿为圆头矢量 S/T、蓝色 S 与青色 T，不依赖字体文件。
源图：../../../resources/icons/open-st.svg；透明PNG及九尺寸ICO位于同目录。
ICO 包含 16/20/24/32/40/48/64/128/256 px 的32位透明PNG帧；定稿预览见 final-preview.png。
正式资源按用户要求保留，废弃方案与临时对比稿已删除。

实现：Launcher 使用命名资源 OPEN_ST_ICON 将 ICO 嵌入 EXE。Application 独立加载大小图标，
Settings 借用图标并通过 WM_SETICON 设到窗口实例。图标在窗口关闭、托盘移除之后释放。
小图标使用系统小图标尺寸；当前不新增跨DPI动态重载机制。16 px 中ST主要作为辅助识别细节。
验证：Debug /W4 /WX 构建通过；从最终EXE加载九种尺寸并检查实际位图尺寸9/9通过。
Settings 模块回归16/16通过，git diff检查通过。
