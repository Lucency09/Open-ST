// 纯标准库 UTF-8 分段与有界拼接；原文分区是唯一接缝依据。
#include "text_segmentation.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace open_st::translation_segmentation
{
namespace
{
struct Scalar
{
    char32_t value{};
    std::size_t offset{};
};

// Unicode 15.1.0 Mark 属性范围由 Unicode 数据库生成，包含组合、间距附加字符和变体选择符。
constexpr std::pair<char32_t, char32_t> MARK_RANGES[] = {
    {0x300, 0x36F},     {0x483, 0x489},     {0x591, 0x5BD},     {0x5BF, 0x5BF},     {0x5C1, 0x5C2},
    {0x5C4, 0x5C5},     {0x5C7, 0x5C7},     {0x610, 0x61A},     {0x64B, 0x65F},     {0x670, 0x670},
    {0x6D6, 0x6DC},     {0x6DF, 0x6E4},     {0x6E7, 0x6E8},     {0x6EA, 0x6ED},     {0x711, 0x711},
    {0x730, 0x74A},     {0x7A6, 0x7B0},     {0x7EB, 0x7F3},     {0x7FD, 0x7FD},     {0x816, 0x819},
    {0x81B, 0x823},     {0x825, 0x827},     {0x829, 0x82D},     {0x859, 0x85B},     {0x898, 0x89F},
    {0x8CA, 0x8E1},     {0x8E3, 0x903},     {0x93A, 0x93C},     {0x93E, 0x94F},     {0x951, 0x957},
    {0x962, 0x963},     {0x981, 0x983},     {0x9BC, 0x9BC},     {0x9BE, 0x9C4},     {0x9C7, 0x9C8},
    {0x9CB, 0x9CD},     {0x9D7, 0x9D7},     {0x9E2, 0x9E3},     {0x9FE, 0x9FE},     {0xA01, 0xA03},
    {0xA3C, 0xA3C},     {0xA3E, 0xA42},     {0xA47, 0xA48},     {0xA4B, 0xA4D},     {0xA51, 0xA51},
    {0xA70, 0xA71},     {0xA75, 0xA75},     {0xA81, 0xA83},     {0xABC, 0xABC},     {0xABE, 0xAC5},
    {0xAC7, 0xAC9},     {0xACB, 0xACD},     {0xAE2, 0xAE3},     {0xAFA, 0xAFF},     {0xB01, 0xB03},
    {0xB3C, 0xB3C},     {0xB3E, 0xB44},     {0xB47, 0xB48},     {0xB4B, 0xB4D},     {0xB55, 0xB57},
    {0xB62, 0xB63},     {0xB82, 0xB82},     {0xBBE, 0xBC2},     {0xBC6, 0xBC8},     {0xBCA, 0xBCD},
    {0xBD7, 0xBD7},     {0xC00, 0xC04},     {0xC3C, 0xC3C},     {0xC3E, 0xC44},     {0xC46, 0xC48},
    {0xC4A, 0xC4D},     {0xC55, 0xC56},     {0xC62, 0xC63},     {0xC81, 0xC83},     {0xCBC, 0xCBC},
    {0xCBE, 0xCC4},     {0xCC6, 0xCC8},     {0xCCA, 0xCCD},     {0xCD5, 0xCD6},     {0xCE2, 0xCE3},
    {0xCF3, 0xCF3},     {0xD00, 0xD03},     {0xD3B, 0xD3C},     {0xD3E, 0xD44},     {0xD46, 0xD48},
    {0xD4A, 0xD4D},     {0xD57, 0xD57},     {0xD62, 0xD63},     {0xD81, 0xD83},     {0xDCA, 0xDCA},
    {0xDCF, 0xDD4},     {0xDD6, 0xDD6},     {0xDD8, 0xDDF},     {0xDF2, 0xDF3},     {0xE31, 0xE31},
    {0xE34, 0xE3A},     {0xE47, 0xE4E},     {0xEB1, 0xEB1},     {0xEB4, 0xEBC},     {0xEC8, 0xECE},
    {0xF18, 0xF19},     {0xF35, 0xF35},     {0xF37, 0xF37},     {0xF39, 0xF39},     {0xF3E, 0xF3F},
    {0xF71, 0xF84},     {0xF86, 0xF87},     {0xF8D, 0xF97},     {0xF99, 0xFBC},     {0xFC6, 0xFC6},
    {0x102B, 0x103E},   {0x1056, 0x1059},   {0x105E, 0x1060},   {0x1062, 0x1064},   {0x1067, 0x106D},
    {0x1071, 0x1074},   {0x1082, 0x108D},   {0x108F, 0x108F},   {0x109A, 0x109D},   {0x135D, 0x135F},
    {0x1712, 0x1715},   {0x1732, 0x1734},   {0x1752, 0x1753},   {0x1772, 0x1773},   {0x17B4, 0x17D3},
    {0x17DD, 0x17DD},   {0x180B, 0x180D},   {0x180F, 0x180F},   {0x1885, 0x1886},   {0x18A9, 0x18A9},
    {0x1920, 0x192B},   {0x1930, 0x193B},   {0x1A17, 0x1A1B},   {0x1A55, 0x1A5E},   {0x1A60, 0x1A7C},
    {0x1A7F, 0x1A7F},   {0x1AB0, 0x1ACE},   {0x1B00, 0x1B04},   {0x1B34, 0x1B44},   {0x1B6B, 0x1B73},
    {0x1B80, 0x1B82},   {0x1BA1, 0x1BAD},   {0x1BE6, 0x1BF3},   {0x1C24, 0x1C37},   {0x1CD0, 0x1CD2},
    {0x1CD4, 0x1CE8},   {0x1CED, 0x1CED},   {0x1CF4, 0x1CF4},   {0x1CF7, 0x1CF9},   {0x1DC0, 0x1DFF},
    {0x20D0, 0x20F0},   {0x2CEF, 0x2CF1},   {0x2D7F, 0x2D7F},   {0x2DE0, 0x2DFF},   {0x302A, 0x302F},
    {0x3099, 0x309A},   {0xA66F, 0xA672},   {0xA674, 0xA67D},   {0xA69E, 0xA69F},   {0xA6F0, 0xA6F1},
    {0xA802, 0xA802},   {0xA806, 0xA806},   {0xA80B, 0xA80B},   {0xA823, 0xA827},   {0xA82C, 0xA82C},
    {0xA880, 0xA881},   {0xA8B4, 0xA8C5},   {0xA8E0, 0xA8F1},   {0xA8FF, 0xA8FF},   {0xA926, 0xA92D},
    {0xA947, 0xA953},   {0xA980, 0xA983},   {0xA9B3, 0xA9C0},   {0xA9E5, 0xA9E5},   {0xAA29, 0xAA36},
    {0xAA43, 0xAA43},   {0xAA4C, 0xAA4D},   {0xAA7B, 0xAA7D},   {0xAAB0, 0xAAB0},   {0xAAB2, 0xAAB4},
    {0xAAB7, 0xAAB8},   {0xAABE, 0xAABF},   {0xAAC1, 0xAAC1},   {0xAAEB, 0xAAEF},   {0xAAF5, 0xAAF6},
    {0xABE3, 0xABEA},   {0xABEC, 0xABED},   {0xFB1E, 0xFB1E},   {0xFE00, 0xFE0F},   {0xFE20, 0xFE2F},
    {0x101FD, 0x101FD}, {0x102E0, 0x102E0}, {0x10376, 0x1037A}, {0x10A01, 0x10A03}, {0x10A05, 0x10A06},
    {0x10A0C, 0x10A0F}, {0x10A38, 0x10A3A}, {0x10A3F, 0x10A3F}, {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27},
    {0x10EAB, 0x10EAC}, {0x10EFD, 0x10EFF}, {0x10F46, 0x10F50}, {0x10F82, 0x10F85}, {0x11000, 0x11002},
    {0x11038, 0x11046}, {0x11070, 0x11070}, {0x11073, 0x11074}, {0x1107F, 0x11082}, {0x110B0, 0x110BA},
    {0x110C2, 0x110C2}, {0x11100, 0x11102}, {0x11127, 0x11134}, {0x11145, 0x11146}, {0x11173, 0x11173},
    {0x11180, 0x11182}, {0x111B3, 0x111C0}, {0x111C9, 0x111CC}, {0x111CE, 0x111CF}, {0x1122C, 0x11237},
    {0x1123E, 0x1123E}, {0x11241, 0x11241}, {0x112DF, 0x112EA}, {0x11300, 0x11303}, {0x1133B, 0x1133C},
    {0x1133E, 0x11344}, {0x11347, 0x11348}, {0x1134B, 0x1134D}, {0x11357, 0x11357}, {0x11362, 0x11363},
    {0x11366, 0x1136C}, {0x11370, 0x11374}, {0x11435, 0x11446}, {0x1145E, 0x1145E}, {0x114B0, 0x114C3},
    {0x115AF, 0x115B5}, {0x115B8, 0x115C0}, {0x115DC, 0x115DD}, {0x11630, 0x11640}, {0x116AB, 0x116B7},
    {0x1171D, 0x1172B}, {0x1182C, 0x1183A}, {0x11930, 0x11935}, {0x11937, 0x11938}, {0x1193B, 0x1193E},
    {0x11940, 0x11940}, {0x11942, 0x11943}, {0x119D1, 0x119D7}, {0x119DA, 0x119E0}, {0x119E4, 0x119E4},
    {0x11A01, 0x11A0A}, {0x11A33, 0x11A39}, {0x11A3B, 0x11A3E}, {0x11A47, 0x11A47}, {0x11A51, 0x11A5B},
    {0x11A8A, 0x11A99}, {0x11C2F, 0x11C36}, {0x11C38, 0x11C3F}, {0x11C92, 0x11CA7}, {0x11CA9, 0x11CB6},
    {0x11D31, 0x11D36}, {0x11D3A, 0x11D3A}, {0x11D3C, 0x11D3D}, {0x11D3F, 0x11D45}, {0x11D47, 0x11D47},
    {0x11D8A, 0x11D8E}, {0x11D90, 0x11D91}, {0x11D93, 0x11D97}, {0x11EF3, 0x11EF6}, {0x11F00, 0x11F01},
    {0x11F03, 0x11F03}, {0x11F34, 0x11F3A}, {0x11F3E, 0x11F42}, {0x13440, 0x13440}, {0x13447, 0x13455},
    {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36}, {0x16F4F, 0x16F4F}, {0x16F51, 0x16F87}, {0x16F8F, 0x16F92},
    {0x16FE4, 0x16FE4}, {0x16FF0, 0x16FF1}, {0x1BC9D, 0x1BC9E}, {0x1CF00, 0x1CF2D}, {0x1CF30, 0x1CF46},
    {0x1D165, 0x1D169}, {0x1D16D, 0x1D172}, {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD},
    {0x1D242, 0x1D244}, {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84},
    {0x1DA9B, 0x1DA9F}, {0x1DAA1, 0x1DAAF}, {0x1E000, 0x1E006}, {0x1E008, 0x1E018}, {0x1E01B, 0x1E021},
    {0x1E023, 0x1E024}, {0x1E026, 0x1E02A}, {0x1E08F, 0x1E08F}, {0x1E130, 0x1E136}, {0x1E2AE, 0x1E2AE},
    {0x1E2EC, 0x1E2EF}, {0x1E4EC, 0x1E4EF}, {0x1E8D0, 0x1E8D6}, {0x1E944, 0x1E94A}, {0xE0100, 0xE01EF},
};

// 判定 Unicode Mark 属性，避免把附加字符与前面的基字符分开。
// 入参：value 为 Unicode 标量。返回：是否属于标记范围。
bool IsMark(char32_t value)
{
    std::size_t first = 0;
    std::size_t last = std::size(MARK_RANGES);
    while (first < last)
    {
        const std::size_t middle = first + (last - first) / 2;
        if (value < MARK_RANGES[middle].first)
            last = middle;
        else if (value > MARK_RANGES[middle].second)
            first = middle + 1;
        else
            return true;
    }
    return false;
}

// 统一原文分隔和译文外缘所用的 Unicode 空白定义。
// 入参：value 为 Unicode 标量。返回：是否为空白。
bool IsWhitespace(char32_t value)
{
    return (value >= U'\t' && value <= U'\r') || value == U' ' || value == 0x85 || value == 0xA0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200A) || value == 0x2028 || value == 0x2029 || value == 0x202F ||
           value == 0x205F || value == 0x3000;
}

// 解码严格 UTF-8，保存每个标量起始字节并在末尾追加边界哨兵。
// 入参：text 为借用字节；scalars 接收临时解码；stop 为取消信号。返回：解码或取消错误。
Error Decode(std::string_view text, std::vector<Scalar>& scalars, std::stop_token stop)
{
    for (std::size_t offset = 0; offset < text.size();)
    {
        if (stop.stop_requested())
            return Error::Cancelled;
        const std::size_t start = offset;
        const unsigned char lead = static_cast<unsigned char>(text[offset++]);
        std::uint32_t value = lead;
        std::size_t remaining = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80)
        {
        }
        else if (lead >= 0xC2 && lead <= 0xDF)
        {
            value = lead & 0x1FU;
            remaining = 1;
            minimum = 0x80;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            value = lead & 0x0FU;
            remaining = 2;
            minimum = 0x800;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            value = lead & 0x07U;
            remaining = 3;
            minimum = 0x10000;
        }
        else
            return Error::InvalidInput;
        if (remaining > text.size() - offset)
            return Error::InvalidInput;
        for (std::size_t index = 0; index < remaining; ++index)
        {
            const unsigned char next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xC0U) != 0x80U)
                return Error::InvalidInput;
            value = (value << 6U) | (next & 0x3FU);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return Error::InvalidInput;
        scalars.push_back({static_cast<char32_t>(value), start});
    }
    scalars.push_back({0, text.size()});
    return stop.stop_requested() ? Error::Cancelled : Error::None;
}

// 保护 CRLF、附加字符、变体选择符、emoji 修饰符及 ZWJ 两侧。
// 入参：scalars 包含末尾哨兵；boundary 为右侧标量下标。返回：是否能安全切分。
bool IsSafe(const std::vector<Scalar>& scalars, std::size_t boundary)
{
    if (boundary == 0 || boundary + 1 == scalars.size())
        return true;
    const char32_t left = scalars[boundary - 1].value;
    const char32_t right = scalars[boundary].value;
    return !(left == U'\r' && right == U'\n') && left != 0x200D && right != 0x200D && !IsMark(right) &&
           !(right >= 0x1F3FB && right <= 0x1F3FF) && !(right >= 0xE0020 && right <= 0xE007F);
}

// 判断 ASCII 字母，避免受进程 locale 或有符号 char 影响。
// 入参：value 为标量。返回：是否为英文字母。
bool IsLetter(char32_t value)
{
    return (value >= U'a' && value <= U'z') || (value >= U'A' && value <= U'Z');
}

// 判断数字语法中需要保护的 ASCII 数字。
// 入参：value 为标量。返回：是否为十进制数字。
bool IsDigit(char32_t value)
{
    return value >= U'0' && value <= U'9';
}

// 判断尾随闭合符号，完整句末连同引号和括号一起保留。
// 入参：value 为标量。返回：是否为闭合符号。
bool IsClosing(char32_t value)
{
    return value == U'"' || value == U'\'' || value == U')' || value == U']' || value == U'}' || value == 0x2019 ||
           value == 0x201D || value == 0x3009 || value == 0x300B || value == 0x300D || value == 0x300F ||
           value == 0x3011 || value == 0xFF09 || value == 0xFF3D;
}

// 判定连续句末标点，后续扩展必须整体满足固定上限。
// 入参：value 为标量。返回：是否为句末标点。
bool IsTerminal(char32_t value)
{
    return value == U'.' || value == U'!' || value == U'?' || value == 0x3002 || value == 0xFF01 || value == 0xFF1F;
}

// 将明显正文标点与 URL/邮件词元分开，避免无空格的中文句子被网址保护吞并。
// 入参：value 为 Unicode 标量。返回：是否为明显的词元分隔符。
bool IsTokenSeparator(char32_t value)
{
    return IsWhitespace(value) || value == U'"' || value == U'<' || value == U'>' || value == 0x3002 ||
           value == 0xFF01 || value == 0xFF1F || value == 0xFF1B || value == 0xFF0C || value == 0x3001 ||
           value == 0xFF1A || value == 0x2018 || value == 0x2019 || value == 0x201C || value == 0x201D ||
           value == 0x300C || value == 0x300D || value == 0x300E || value == 0x300F || value == 0xFF08 ||
           value == 0xFF09;
}

// 识别裸 ASCII 域名及其路径/查询，只接受合法点分标签和至少两字母的末级标签。
// 入参：token 为已剥离尾部句点和闭合符号的词元。返回：是否包含明显的裸域名。
bool IsDomainToken(std::string_view token)
{
    while (!token.empty() &&
           (token.front() == '(' || token.front() == '[' || token.front() == '{' || token.front() == '\''))
        token.remove_prefix(1);
    const std::string_view host = token.substr(0, token.find_first_of("/?:#"));
    std::size_t first = 0;
    std::size_t labels = 0;
    while (first < host.size())
    {
        const std::size_t dot = host.find('.', first);
        const std::size_t last = dot == std::string_view::npos ? host.size() : dot;
        if (last == first || host[first] == '-' || host[last - 1] == '-')
            return false;
        bool lettersOnly = true;
        for (std::size_t index = first; index < last; ++index)
        {
            const char32_t value = static_cast<unsigned char>(host[index]);
            if (!IsLetter(value) && !IsDigit(value) && value != U'-')
                return false;
            lettersOnly = lettersOnly && IsLetter(value);
        }
        ++labels;
        if (dot == std::string_view::npos)
            return labels >= 2 && last - first >= 2 && lettersOnly;
        first = last + 1;
    }
    return false;
}

// 预标记明显 URL 和邮件的词元，避免在其内部使用标点边界。
// 入参：original 为原文；scalars 为解码；protectedToken 接收标记；stop 为取消信号。
// 返回：完成或取消错误。
Error ProtectTokens(std::string_view original, const std::vector<Scalar>& scalars, std::vector<bool>& protectedToken,
                    std::stop_token stop)
{
    const std::size_t count = scalars.size() - 1;
    protectedToken.assign(count, false);
    for (std::size_t first = 0; first < count;)
    {
        if (stop.stop_requested())
            return Error::Cancelled;
        if (IsTokenSeparator(scalars[first].value))
        {
            ++first;
            continue;
        }
        std::size_t last = first + 1;
        while (last < count && !IsTokenSeparator(scalars[last].value))
            ++last;
        // 正文中的尾随句点/闭合符号不属于受保护网址内部，仍可形成完整句末。
        std::size_t protectedLast = last;
        while (protectedLast > first &&
               (scalars[protectedLast - 1].value == U'.' || IsClosing(scalars[protectedLast - 1].value)))
            --protectedLast;
        const std::string_view token =
            original.substr(scalars[first].offset, scalars[protectedLast].offset - scalars[first].offset);
        if (token.find("://") != std::string_view::npos || token.find("www.") != std::string_view::npos ||
            token.find('@') != std::string_view::npos || IsDomainToken(token))
            std::fill(protectedToken.begin() + static_cast<std::ptrdiff_t>(first),
                      protectedToken.begin() + static_cast<std::ptrdiff_t>(protectedLast), true);
        first = last;
    }
    return Error::None;
}

// 排除常见缩写与带句点首字母串；字母间的句点也不视为句末。
// 入参：scalars 为全文；index 为句点下标。返回：是否为需要保护的句点。
bool IsAbbreviation(const std::vector<Scalar>& scalars, std::size_t index)
{
    const std::size_t count = scalars.size() - 1;
    if (index + 1 < count && (IsLetter(scalars[index + 1].value) || IsDigit(scalars[index + 1].value)))
        return true;
    std::size_t first = index;
    while (first > 0 && (IsLetter(scalars[first - 1].value) || scalars[first - 1].value == U'.'))
        --first;
    std::string token;
    for (std::size_t cursor = first; cursor <= index; ++cursor)
    {
        const char32_t value = scalars[cursor].value;
        token += static_cast<char>(value >= U'A' && value <= U'Z' ? value + (U'a' - U'A') : value);
    }
    constexpr std::string_view abbreviations[] = {"mr.", "mrs.", "ms.", "dr.", "prof.", "e.g.", "i.e.", "etc."};
    for (const std::string_view abbreviation : abbreviations)
        if (token == abbreviation)
            return true;
    bool initials = token.size() >= 2 && token.size() % 2 == 0;
    for (std::size_t cursor = 0; initials && cursor < token.size(); cursor += 2)
        initials = IsLetter(static_cast<char32_t>(token[cursor])) && token[cursor + 1] == '.';
    return initials;
}

// 为一个候选标点分配优先级，排除网址、邮件、数字、版本与时间内部边界。
// 入参：scalars 为全文；protectedToken 为词元标记；index 为候选下标。返回：0 表示不可用，1 最强。
int Priority(const std::vector<Scalar>& scalars, const std::vector<bool>& protectedToken, std::size_t index)
{
    const char32_t value = scalars[index].value;
    if (value == U'\r' || value == U'\n' || value == 0x2028 || value == 0x2029)
        return 1;
    if (protectedToken[index])
        return 0;
    const bool numeric = index > 0 && index + 2 < scalars.size() && IsDigit(scalars[index - 1].value) &&
                         IsDigit(scalars[index + 1].value);
    if (IsTerminal(value))
        return value == U'.' && (numeric || IsAbbreviation(scalars, index)) ? 0 : 1;
    if (value == U';' || value == 0xFF1B)
        return 2;
    if (value == U',' || value == U':' || value == 0xFF0C || value == 0x3001 || value == 0xFF1A)
        return numeric ? 0 : 3;
    return 0;
}

// 记录连续字节范围，所有权留在计划内，正文与分隔内容均不能遗漏。
// 入参：plan 为临时计划；scalars 为全文；first/last 为标量半开区间；literal 表示原样分隔。
// 返回：无。
void AddPiece(Plan& plan, const std::vector<Scalar>& scalars, std::size_t first, std::size_t last, bool literal)
{
    if (first == last)
        return;
    plan.pieces.push_back({scalars[first].offset, scalars[last].offset - scalars[first].offset, literal});
    if (!literal)
        ++plan.segmentCount;
}

// 在固定上限中按优先级选择最靠后的安全语义边界。
// 入参：scalars 为全文；protectedToken 为词元保护；first 为正文起点；end 为剩余正文末尾。
// 返回：正文结束下标，无安全边界时返回 first。
std::size_t FindBoundary(const std::vector<Scalar>& scalars, const std::vector<bool>& protectedToken, std::size_t first,
                         std::size_t end)
{
    const std::size_t limit = first + MAX_SEGMENT_CODEPOINTS;
    std::size_t selected = first;
    int selectedPriority = 5;
    // limit 处的空白不属于正文，可形成恰好 200 标量的正文范围。
    for (std::size_t index = first; index <= limit; ++index)
    {
        const char32_t value = scalars[index].value;
        const bool whitespace = IsWhitespace(value);
        int priority = Priority(scalars, protectedToken, index);
        if (priority == 0)
            priority = whitespace ? 4 : 0;
        if (priority == 0 || priority > selectedPriority)
            continue;
        std::size_t boundary = whitespace ? index : index + 1;
        if (!whitespace)
        {
            while (boundary < end && (IsTerminal(scalars[boundary].value) || IsClosing(scalars[boundary].value)))
                ++boundary;
        }
        if (boundary <= first || boundary > limit || !IsSafe(scalars, boundary))
            continue;
        // 空白运行的首个位置才是边界，缩进全部由后续 literal 拥有。
        while (boundary > first && IsWhitespace(scalars[boundary - 1].value))
            --boundary;
        if (boundary > first && IsSafe(scalars, boundary) && (priority < selectedPriority || boundary > selected))
        {
            selected = boundary;
            selectedPriority = priority;
        }
    }
    return selected;
}

// 在追加之前检查剩余预算，避免整数溢出及超预算临时输出。
// 入参：output 为临时结果；text 为待追加字节；maxBytes 为总预算。返回：追加是否成功。
bool Append(std::string& output, std::string_view text, std::size_t maxBytes)
{
    if (text.size() > maxBytes - output.size())
        return false;
    output.append(text);
    return true;
}

// 仅人工接缝补空格，已有空白、闭合标点及开括号之后不补。
// 入参：left/right 为接缝两侧标量。返回：是否需要插入 ASCII 空格。
bool NeedsSpace(char32_t left, char32_t right)
{
    if (IsWhitespace(left) || IsWhitespace(right) || IsClosing(right) || IsTerminal(right) || right == U',' ||
        right == U';' || right == U':' || right == 0xFF0C || right == 0xFF1B || right == 0xFF1A || right == 0x3001)
        return false;
    return left != U'(' && left != U'[' && left != U'{' && left != 0x2018 && left != 0x201C && left != 0x300C &&
           left != 0x300E && left != 0xFF08;
}
} // namespace

// 在同一原文快照上生成完整分区，失败时保留调用方已有计划。
// 入参：original 为 UTF-8 原文；plan 为成功输出；stop 为协作取消。返回：完整规划结果。
Error Split(std::string_view original, Plan& plan, std::stop_token stop)
{
    if (stop.stop_requested())
        return Error::Cancelled;
    std::vector<Scalar> scalars;
    const Error decoded = Decode(original, scalars, stop);
    if (decoded != Error::None)
        return decoded;
    std::vector<bool> protectedToken;
    const Error protectedResult = ProtectTokens(original, scalars, protectedToken, stop);
    if (protectedResult != Error::None)
        return protectedResult;
    Plan candidate;
    candidate.sourceBytes = original.size();
    const std::size_t count = scalars.size() - 1;
    std::size_t end = count;
    while (end > 0 && IsWhitespace(scalars[end - 1].value))
        --end;
    // 外缘空白也是可能的组合序列基字符，无法剥离时交给正文一起处理。
    while (end < count && !IsSafe(scalars, end))
        ++end;
    std::size_t first = 0;
    while (first < count)
    {
        if (stop.stop_requested())
            return Error::Cancelled;
        std::size_t body = first;
        while (body < count && IsWhitespace(scalars[body].value))
            ++body;
        while (body > first && !IsSafe(scalars, body))
            --body;
        AddPiece(candidate, scalars, first, body, true);
        if (body == count)
            break;
        const std::size_t boundary =
            end - body <= MAX_SEGMENT_CODEPOINTS ? end : FindBoundary(scalars, protectedToken, body, end);
        if (boundary == body)
            return Error::NoBoundary;
        AddPiece(candidate, scalars, body, boundary, false);
        first = boundary;
    }
    if (stop.stop_requested())
        return Error::Cancelled;
    plan = std::move(candidate);
    return Error::None;
}

// 按经验证的分区拼接完整结果，仅成功时交换到调用方输出。
// 入参：original/plan 为原文及计划；translations 为正文译文；style 为接缝；maxBytes 为预算；
// output 接收结果；stop 为协作取消。返回：校验、预算或取消错误，失败无部分输出。
Error Assemble(std::string_view original, const Plan& plan, std::span<const std::string> translations, JoinStyle style,
               std::size_t maxBytes, std::string& output, std::stop_token stop)
{
    if (stop.stop_requested())
        return Error::Cancelled;
    if (plan.sourceBytes != original.size() || plan.segmentCount != translations.size() ||
        (style != JoinStyle::Compact && style != JoinStyle::Spaced))
        return Error::InvalidPlan;
    std::vector<Scalar> source;
    const Error decoded = Decode(original, source, stop);
    if (decoded != Error::None)
        return decoded;
    std::size_t offset = 0;
    std::size_t scalarIndex = 0;
    std::size_t segmentCount = 0;
    for (const Piece& piece : plan.pieces)
    {
        if (stop.stop_requested())
            return Error::Cancelled;
        if (piece.offset != offset || piece.length == 0 || piece.length > original.size() - offset)
            return Error::InvalidPlan;
        const std::size_t first = scalarIndex;
        offset += piece.length;
        while (scalarIndex + 1 < source.size() && source[scalarIndex].offset < offset)
        {
            if (piece.literal && !IsWhitespace(source[scalarIndex].value))
                return Error::InvalidPlan;
            ++scalarIndex;
        }
        if (source[scalarIndex].offset != offset || !IsSafe(source, scalarIndex) ||
            (!piece.literal && scalarIndex - first > MAX_SEGMENT_CODEPOINTS))
            return Error::InvalidPlan;
        if (!piece.literal)
            ++segmentCount;
    }
    if (offset != original.size() || segmentCount != plan.segmentCount)
        return Error::InvalidPlan;
    std::string candidate;
    std::size_t translationIndex = 0;
    char32_t previous = 0;
    for (std::size_t index = 0; index < plan.pieces.size(); ++index)
    {
        if (stop.stop_requested())
            return Error::Cancelled;
        const Piece& piece = plan.pieces[index];
        if (piece.literal)
        {
            if (!Append(candidate, original.substr(piece.offset, piece.length), maxBytes))
                return Error::OutputTooLarge;
            continue;
        }
        const std::string_view translation = translations[translationIndex++];
        std::vector<Scalar> translated;
        const Error translatedResult = Decode(translation, translated, stop);
        if (translatedResult != Error::None)
            return translatedResult;
        std::size_t first = 0;
        std::size_t last = translated.size() - 1;
        if (index > 0 && plan.pieces[index - 1].literal)
        {
            while (first < last && IsWhitespace(translated[first].value))
                ++first;
            // 空白也能作为附加字符或 ZWJ 序列的基字符，清理不能拆开该序列。
            while (first > 0 && !IsSafe(translated, first))
                --first;
        }
        if (index + 1 < plan.pieces.size() && plan.pieces[index + 1].literal)
        {
            while (last > first && IsWhitespace(translated[last - 1].value))
                --last;
            while (last + 1 < translated.size() && !IsSafe(translated, last))
                ++last;
        }
        if (first == last)
            return Error::InvalidInput;
        if (style == JoinStyle::Spaced && index > 0 && !plan.pieces[index - 1].literal &&
            NeedsSpace(previous, translated[first].value) && !Append(candidate, " ", maxBytes))
            return Error::OutputTooLarge;
        if (!Append(candidate,
                    translation.substr(translated[first].offset, translated[last].offset - translated[first].offset),
                    maxBytes))
            return Error::OutputTooLarge;
        previous = translated[last - 1].value;
    }
    if (stop.stop_requested())
        return Error::Cancelled;
    output = std::move(candidate);
    return Error::None;
}
} // namespace open_st::translation_segmentation
