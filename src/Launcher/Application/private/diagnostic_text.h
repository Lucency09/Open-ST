// 文件职责：声明应用层显示与诊断文本的 UTF 编码转换辅助函数。

#pragma once

#include <string>
#include <string_view>

namespace open_st
{
// 将 Win32 诊断文本转换为日志使用的 UTF-8 字符串。
// 入参：value：借用的 UTF-16 诊断文本。
// 返回：转换成功时返回 UTF-8 文本；空文本、长度过大、无效编码或分配失败时返回空字符串。
[[nodiscard]] std::string WideToUtf8(std::wstring_view value) noexcept;
// 将业务UTF-8文字转换为系统控件使用的UTF-16，不记录内容。
// 入参：value为完整UTF-8。返回：合法文本；无效、过长或异常返回空。
[[nodiscard]] std::wstring Utf8ToWide(std::string_view value) noexcept;
} // namespace open_st
