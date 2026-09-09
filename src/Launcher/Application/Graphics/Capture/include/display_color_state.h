// 文件职责：声明冻结显示输出颜色状态的有效性检查，供截图会话发现 SDR 参考白变化。

#pragma once

#include <frozen_desktop_frame.h>

namespace open_st
{
// 检查冻结时保存的 SDR 参考白是否仍与系统一致。
// 入参：metadata：冻结 plane 保存的目标身份、参考白值及有效标记。
// 返回：未缓存参考白时为 true；已缓存且查询值一致时为 true，元数据矛盾、查询失败或亮度变化时为 false。
[[nodiscard]] bool IsCapturedOutputColorStateCurrent(const OutputColorMetadata& metadata) noexcept;
} // namespace open_st
