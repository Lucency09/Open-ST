// 文件职责：实现标准库 UTF-16 正文校验、换行规范和字号／块大小档位规则。
#include <algorithm>
#include <annotation.h>

namespace open_st
{
namespace
{
// 检查原始 UTF-16 上限和代理项配对，禁止 NUL 而不修改输入。
// 入参：text：原始代码单元序列。
// 返回：长度、NUL 和代理项均合法为 true。
bool ValidUtf16(std::u16string_view text) noexcept
{
    if (text.size() > ANNOTATION_MAX_TEXT_UNITS)
        return false;
    for (std::size_t index = 0U; index < text.size(); ++index)
    {
        const char16_t unit = text[index];
        if (unit == 0U)
            return false;
        if (unit >= 0xD800U && unit <= 0xDBFFU)
        {
            if (++index >= text.size() || text[index] < 0xDC00U || text[index] > 0xDFFFU)
                return false;
        }
        else if (unit >= 0xDC00U && unit <= 0xDFFFU)
            return false;
    }
    return true;
}
} // namespace

// 将验证完成的原文逐字符规范为 LF，保留所有其他字符与空白。
// 入参：source：原始 UTF-16；normalized：成功接收共享正文。
// 返回：完成为 true；非法或分配失败不改 normalized。
bool NormalizeAnnotationText(std::u16string_view source, std::shared_ptr<const std::u16string>& normalized) noexcept
try
{
    if (!ValidUtf16(source))
        return false;
    std::u16string text;
    text.reserve(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        if (source[index] == u'\r')
        {
            text.push_back(u'\n');
            if (index + 1U < source.size() && source[index + 1U] == u'\n')
                ++index;
        }
        else
            text.push_back(source[index]);
    }
    normalized = std::make_shared<const std::u16string>(std::move(text));
    return true;
}
catch (...)
{
    return false;
}

// 使用 Unicode 空白集合判断是否缺少可提交正文，不 trim 实际内容。
// 入参：text：UTF-16 正文。
// 返回：全为空白或空串为 true。
bool IsBlankAnnotationText(std::u16string_view text) noexcept
{
    for (char16_t unit : text)
    {
        if (!((unit >= 0x0009U && unit <= 0x000DU) || unit == 0x0020U || unit == 0x0085U || unit == 0x00A0U ||
              unit == 0x1680U || (unit >= 0x2000U && unit <= 0x200AU) || unit == 0x2028U || unit == 0x2029U ||
              unit == 0x202FU || unit == 0x205FU || unit == 0x3000U))
            return false;
    }
    return true;
}

// 检查保存正文已经完成统一换行且仍满足 UTF-16 输入边界。
// 入参：text：拟保存的正文。
// 返回：有效且不含 CR 为 true。
bool IsNormalizedAnnotationText(std::u16string_view text) noexcept
{
    return ValidUtf16(text) && text.find(u'\r') == std::u16string_view::npos;
}

// 校验首版固定字号，不接受隐式舍入或非有限浮点值。
// 入参：fontSize：物理像素字号。
// 返回：允许档位为 true。
bool IsValidAnnotationFontSize(double fontSize) noexcept
{
    return std::find(ANNOTATION_FONT_SIZES.begin(), ANNOTATION_FONT_SIZES.end(), fontSize) !=
           ANNOTATION_FONT_SIZES.end();
}

// 校验马赛克固定物理块大小。
// 入参：blockSize：块边长。
// 返回：允许档位为 true。
bool IsValidAnnotationBlockSize(unsigned blockSize) noexcept
{
    return std::find(ANNOTATION_MOSAIC_BLOCK_SIZES.begin(), ANNOTATION_MOSAIC_BLOCK_SIZES.end(), blockSize) !=
           ANNOTATION_MOSAIC_BLOCK_SIZES.end();
}
} // namespace open_st
