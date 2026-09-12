// 文件职责：实现应用层诊断文本的 UTF-16 到 UTF-8 转换，并隔离转换异常。

#include "diagnostic_text.h"

#include <limits>
#include <windows.h>

namespace open_st
{
// 将 Win32 诊断文本转换为日志使用的 UTF-8 字符串，并隔离所有分配异常。
// 入参：value：借用的 UTF-16 诊断文本。
// 返回：转换成功时返回 UTF-8 文本；空文本、长度过大、无效编码或异常时返回空字符串。
std::string WideToUtf8(std::wstring_view value) noexcept
{
    try
    {
        if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return {};
        }

        const int sourceLength = static_cast<int>(value.size());
        const int requiredBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
                                                      nullptr, 0, nullptr, nullptr);
        if (requiredBytes <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(requiredBytes), '\0');
        const int convertedBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
                                                       result.data(), requiredBytes, nullptr, nullptr);
        return convertedBytes == requiredBytes ? result : std::string{};
    }
    catch (...)
    {
        return {};
    }
}
} // namespace open_st
