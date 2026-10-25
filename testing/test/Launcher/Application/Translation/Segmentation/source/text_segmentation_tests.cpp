// 纯文本分段测试覆盖语义优先级、Unicode 保护、字节分区与原子有界拼接。
#include "text_segmentation.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
using open_st::translation_segmentation::Assemble;
using open_st::translation_segmentation::Error;
using open_st::translation_segmentation::JoinStyle;
using open_st::translation_segmentation::MAX_SEGMENT_CODEPOINTS;
using open_st::translation_segmentation::Piece;
using open_st::translation_segmentation::Plan;
using open_st::translation_segmentation::Split;

// 重复 UTF-8 样本构造确定长度，避免按字节误算测试字符数。
// 入参：text 为一个或多个标量；count 为重复次数。返回：拥有的测试字符串。
std::string Repeat(std::string_view text, std::size_t count)
{
    std::string result;
    for (std::size_t index = 0; index < count; ++index)
        result += text;
    return result;
}

// 收集实际提供方会收到的正文切片，并校验整个字节分区没有空洞或重叠。
// 入参：original 为原文；plan 为待验证计划。返回：正文字符串数组。
std::vector<std::string> Segments(std::string_view original, const Plan& plan)
{
    std::vector<std::string> result;
    std::size_t offset = 0;
    std::string reconstructed;
    for (const Piece& piece : plan.pieces)
    {
        EXPECT_EQ(piece.offset, offset);
        EXPECT_GT(piece.length, 0U);
        const std::string_view part = original.substr(piece.offset, piece.length);
        reconstructed += part;
        offset += piece.length;
        if (!piece.literal)
            result.emplace_back(part);
    }
    EXPECT_EQ(offset, original.size());
    EXPECT_EQ(reconstructed, original);
    EXPECT_EQ(result.size(), plan.segmentCount);
    EXPECT_EQ(plan.sourceBytes, original.size());
    return result;
}

// 固定 200 以 Unicode 标量计数，三字节汉字和四字节 emoji 均保持单段上限。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, fixed_limit_counts_scalars_and_short_text_stays_one_segment)
{
    EXPECT_EQ(MAX_SEGMENT_CODEPOINTS, 200U);
    for (const std::string_view unit : {std::string_view("汉"), std::string_view("😀")})
    {
        Plan plan;
        const std::string exact = Repeat(unit, 199) + "。";
        ASSERT_EQ(Split(exact, plan), Error::None);
        EXPECT_EQ(Segments(exact, plan), (std::vector<std::string>{exact}));
        const std::string longer = exact + std::string(unit);
        ASSERT_EQ(Split(longer, plan), Error::None);
        EXPECT_EQ(Segments(longer, plan), (std::vector<std::string>{exact, std::string(unit)}));
    }
}

// 176 标量完整句末优先于 200 附近词间空格，不向后扩张到下一句末。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, rounds_down_to_last_complete_sentence)
{
    const std::string first = Repeat("字", 175) + "。";
    const std::string second = "next sentence has several words and ends later.";
    const std::string text = first + second;
    Plan plan;
    ASSERT_EQ(Split(text, plan), Error::None);
    EXPECT_EQ(Segments(text, plan), (std::vector<std::string>{first, second}));
}

// 第 201 标量为分隔空白时，正文可以恰好占满 200；换行和 CRLF 不进入正文。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, accepts_separator_immediately_after_exact_limit)
{
    for (const std::string_view separator : {std::string_view(" "), std::string_view("\n"), std::string_view("\r\n")})
    {
        const std::string first(200, 'a');
        const std::string text = first + std::string(separator) + "b";
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        EXPECT_EQ(Segments(text, plan), (std::vector<std::string>{first, "b"}));
        ASSERT_EQ(plan.pieces.size(), 3U);
        EXPECT_EQ(text.substr(plan.pieces[1].offset, plan.pieces[1].length), separator);
    }
}

// 强边界、分号、从句、词空白依次退让，同级取上限内最后一处。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, chooses_last_boundary_within_highest_available_priority)
{
    const std::vector<std::string> prefixes = {Repeat("a", 50) + ";" + Repeat("b", 100) + ";",
                                               Repeat("a", 50) + "," + Repeat("b", 100) + ",",
                                               Repeat("a", 50) + " " + Repeat("b", 100)};
    for (const std::string& prefix : prefixes)
    {
        const std::string text = prefix + " " + Repeat("c", 80);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        const std::vector<std::string> segments = Segments(text, plan);
        ASSERT_EQ(segments.size(), 2U);
        EXPECT_EQ(segments.front(), prefix);
        EXPECT_EQ(segments.back(), Repeat("c", 80));
        EXPECT_EQ(plan.pieces[1].literal, true);
    }
    const std::string text = Repeat("a", 80) + ";" + Repeat("b", 80) + "," + Repeat("c", 80);
    Plan plan;
    ASSERT_EQ(Split(text, plan), Error::None);
    EXPECT_EQ(Segments(text, plan).front(), Repeat("a", 80) + ";");
}

// 混合 CRLF、空行和缩进在分段处成为 literal，首尾分隔也只使用一次。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, preserves_hard_line_separators_and_single_body_surroundings)
{
    const std::string first = Repeat("字", 150);
    const std::string second = Repeat("词", 100);
    const std::string text = "\r\n\t" + first + "\r\n\r\n　\t" + second + "\r \n";
    Plan plan;
    ASSERT_EQ(Split(text, plan), Error::None);
    EXPECT_EQ(Segments(text, plan), (std::vector<std::string>{first, second}));
    std::string output;
    const std::vector<std::string> translations = {"  FIRST \n", "\t SECOND  "};
    ASSERT_EQ(Assemble(text, plan, translations, JoinStyle::Spaced, 1000, output), Error::None);
    EXPECT_EQ(output, "\r\n\tFIRST\r\n\r\n　\tSECOND\r \n");
    ASSERT_EQ(Split("\n hello \r\n", plan), Error::None);
    const std::vector<std::string> one = {" translated "};
    ASSERT_EQ(Assemble("\n hello \r\n", plan, one, JoinStyle::Compact, 1000, output), Error::None);
    EXPECT_EQ(output, "\n translated \r\n");
}

// 小数、版本、数字分组、时间和缩写不能成为比前一完整句更靠后的强边界。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, protects_numeric_syntax_and_common_abbreviations)
{
    const std::string first = Repeat("字", 120) + "。";
    const std::vector<std::string> samples = {
        "value 3.14 and version 1.2.0 are retained ", "Mr. Smith and Mrs. Smith with Ms. Jones ",
        "Dr. Smith meets Prof. Jones e.g. here ",     "i.e. keep etc. and U.S. abbreviations ",
        "the value is 1,234,567 at 12:30 ",           "visit https://example.com/a:b,c?d=x! then ",
        "contact user.name@example.com for help "};
    for (const std::string& sample : samples)
    {
        const std::string text = first + sample + Repeat("tail ", 30);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None) << sample;
        EXPECT_EQ(Segments(text, plan).front(), first) << sample;
    }
}

// 没有空白的超长网址、邮件、数字或单词不因内部标点而被硬切。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, returns_error_for_unbreakable_tokens)
{
    const std::vector<std::string> samples = {Repeat("x", 201), "https://example.com/" + Repeat("a;b,c:d?e!f", 30),
                                              Repeat("a", 101) + "@example." + Repeat("b", 101),
                                              Repeat("123,", 60) + "123", Repeat("1.", 110) + "1"};
    for (const std::string& sample : samples)
    {
        Plan plan;
        plan.sourceBytes = 17;
        EXPECT_EQ(Split(sample, plan), Error::NoBoundary);
        EXPECT_EQ(plan.sourceBytes, 17U);
        EXPECT_TRUE(plan.pieces.empty());
    }
}

// 无空格中英混排的网址和邮件不吞并词元外的中文句末，仍选择 176 处完整句子。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, url_protection_preserves_surrounding_chinese_sentence_boundaries)
{
    const std::string first = Repeat("字", 175) + "。";
    for (const std::string_view token :
         {std::string_view("https://example.com/long-path"), std::string_view("long.user.name@example.com")})
    {
        const std::string text = first + std::string(token) + "。" + Repeat("尾", 80);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        const std::vector<std::string> segments = Segments(text, plan);
        ASSERT_EQ(segments.size(), 2U);
        EXPECT_EQ(segments.front(), first);
    }
}

// 裸域名的路径与查询标点不能切开词元，ASCII 字内句点也不作为句末。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, protects_bare_domain_queries_and_ascii_internal_periods)
{
    const std::string first = Repeat("字", 175) + "。";
    for (const std::string_view token :
         {std::string_view("example.com/path?q=1"), std::string_view("sub.example.org/a:b,c?d=e!f"),
          std::string_view("foo.bar?query=value")})
    {
        const std::string text = first + std::string(token) + " " + Repeat("tail ", 20);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        EXPECT_EQ(Segments(text, plan).front(), first);
    }
}

// URL 后正常英文句点仍是强边界，尾随括号随句末保留，不扩大保护到下一句。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, preserves_sentence_period_after_domains_and_urls)
{
    for (const std::string_view token :
         {std::string_view("example.com."), std::string_view("https://example.com/path."),
          std::string_view("(example.com/path.)")})
    {
        const std::string first = Repeat("字", 150) + std::string(token);
        const std::string text = first + " " + Repeat("tail ", 30);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        EXPECT_EQ(Segments(text, plan).front(), first);
    }
}

// 句末标点组和闭合引号整体保留；超过上限时回到此前安全句末。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, keeps_terminal_runs_and_closing_quotes_with_preceding_body)
{
    const std::string first = Repeat("字", 170) + "？！\"）";
    const std::string text = first + Repeat("词", 80);
    Plan plan;
    ASSERT_EQ(Split(text, plan), Error::None);
    EXPECT_EQ(Segments(text, plan).front(), first);
    const std::string fallback = Repeat("字", 149) + "。";
    const std::string overflow = fallback + Repeat("词", 49) + "！\"" + Repeat("尾", 40);
    ASSERT_EQ(Split(overflow, plan), Error::None);
    EXPECT_EQ(Segments(overflow, plan).front(), fallback);
}

// 组合附加字符、变体选择符与 ZWJ 序列不能被句末候选拆开，只能向前回退。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, protects_combining_variation_and_zwj_boundaries)
{
    const std::string fallback = Repeat("字", 149) + "。";
    const std::vector<std::string> clusters = {"!\xCC\x81", "!️", "!‍😀", "!🏽", "!\xE0\xA4\xBE"};
    for (const std::string& cluster : clusters)
    {
        const std::string text = fallback + Repeat("词", 49) + cluster + Repeat("尾", 40);
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        EXPECT_EQ(Segments(text, plan).front(), fallback);
    }
    const std::string crlf = Repeat("字", 199) + "\r\n" + Repeat("尾", 10);
    Plan plan;
    ASSERT_EQ(Split(crlf, plan), Error::None);
    EXPECT_EQ(Segments(crlf, plan).front(), Repeat("字", 199));
    ASSERT_EQ(plan.pieces.size(), 3U);
    EXPECT_EQ(crlf.substr(plan.pieces[1].offset, plan.pieces[1].length), "\r\n");
    const std::string edgeCluster = " \xCC\x81hello";
    ASSERT_EQ(Split(edgeCluster, plan), Error::None);
    EXPECT_EQ(Segments(edgeCluster, plan), (std::vector<std::string>{edgeCluster}));
}

// 纯 Unicode 空白形成零正文，空输入没有范围，均不伪造可翻译段。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, whitespace_only_has_no_translation_segments)
{
    for (const std::string_view text : {std::string_view(), std::string_view(" \t\r\n　\xC2\xA0")})
    {
        Plan plan;
        ASSERT_EQ(Split(text, plan), Error::None);
        EXPECT_TRUE(Segments(text, plan).empty());
        EXPECT_EQ(plan.segmentCount, 0U);
    }
}

// 人工接缝区分中日韩紧接与英文空格，已有空格及标点不重复或删改。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, joins_artificial_seams_without_changing_punctuation)
{
    const std::string text = Repeat("字", 199) + "。" + Repeat("词", 30);
    Plan plan;
    ASSERT_EQ(Split(text, plan), Error::None);
    const std::vector<std::string> translations = {"first!", "second?"};
    std::string output;
    ASSERT_EQ(Assemble(text, plan, translations, JoinStyle::Compact, 100, output), Error::None);
    EXPECT_EQ(output, "first!second?");
    ASSERT_EQ(Assemble(text, plan, translations, JoinStyle::Spaced, 100, output), Error::None);
    EXPECT_EQ(output, "first! second?");
    const std::vector<std::string> spaced = {"first! ", " second?"};
    ASSERT_EQ(Assemble(text, plan, spaced, JoinStyle::Spaced, 100, output), Error::None);
    EXPECT_EQ(output, "first!  second?");
    const std::vector<std::string> punctuation = {"first!", ",second?"};
    ASSERT_EQ(Assemble(text, plan, punctuation, JoinStyle::Spaced, 100, output), Error::None);
    EXPECT_EQ(output, "first!,second?");
}

// 单段全文不处理译文外缘或内部空白；输出预算恰好包含全部字节才成功。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, keeps_single_segment_translation_and_checks_byte_budget)
{
    Plan plan;
    ASSERT_EQ(Split("hello", plan), Error::None);
    const std::vector<std::string> translations = {" 译  文 \n"};
    std::string output = "old";
    EXPECT_EQ(Assemble("hello", plan, translations, JoinStyle::Spaced, translations[0].size() - 1, output),
              Error::OutputTooLarge);
    EXPECT_EQ(output, "old");
    ASSERT_EQ(Assemble("hello", plan, translations, JoinStyle::Spaced, translations[0].size(), output), Error::None);
    EXPECT_EQ(output, translations[0]);
    const std::string text = Repeat("字", 199) + "。尾";
    ASSERT_EQ(Split(text, plan), Error::None);
    const std::vector<std::string> pieces = {"a", "b"};
    output = "old";
    EXPECT_EQ(Assemble(text, plan, pieces, JoinStyle::Spaced, 2, output), Error::OutputTooLarge);
    EXPECT_EQ(output, "old");
}

// 真实首尾换行 literal 只清理安全外缘，不删除 Mark/VS 的空白基字符或拆开 ZWJ 序列。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, literal_edge_trimming_preserves_unicode_clusters)
{
    Plan plan;
    const std::string prefixSource = "\nhello";
    ASSERT_EQ(Split(prefixSource, plan), Error::None);
    const std::vector<std::string> leadingClusters = {" \xCC\x81hi", " \xEF\xB8\x8Fhi", " \xE2\x80\x8Dhi"};
    std::string output;
    for (const std::string& cluster : leadingClusters)
    {
        const std::vector<std::string> translated = {"  " + cluster};
        ASSERT_EQ(Assemble(prefixSource, plan, translated, JoinStyle::Compact, 100, output), Error::None);
        EXPECT_EQ(output, "\n" + cluster);
    }

    const std::string suffixSource = "hello\r\n";
    ASSERT_EQ(Split(suffixSource, plan), Error::None);
    const std::vector<std::string> trailingClusters = {"hi \xCC\x81", "hi \xEF\xB8\x8F", "hi\xE2\x80\x8D "};
    for (const std::string& cluster : trailingClusters)
    {
        const std::vector<std::string> translated = {cluster + "\t "};
        ASSERT_EQ(Assemble(suffixSource, plan, translated, JoinStyle::Compact, 100, output), Error::None);
        EXPECT_EQ(output, cluster + "\r\n");
    }

    const std::string bothSource = "\nhello\r\n";
    ASSERT_EQ(Split(bothSource, plan), Error::None);
    const std::string cluster = " \xCC\x81hi\xE2\x80\x8D ";
    const std::vector<std::string> translated = {"\t " + cluster + "\t "};
    ASSERT_EQ(Assemble(bothSource, plan, translated, JoinStyle::Spaced, 100, output), Error::None);
    EXPECT_EQ(output, "\n" + cluster + "\r\n");
}

// 非法 UTF-8、断裂分区、数量不符和空译文保持旧输出，不发布部分结果。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, rejects_invalid_utf8_plans_and_translations_atomically)
{
    const std::vector<std::string> invalid = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE4\xB8"};
    Plan plan;
    plan.sourceBytes = 17;
    for (const std::string& text : invalid)
    {
        EXPECT_EQ(Split(text, plan), Error::InvalidInput);
        EXPECT_EQ(plan.sourceBytes, 17U);
    }
    ASSERT_EQ(Split("字", plan), Error::None);
    std::string output = "old";
    const std::vector<std::string> valid = {"translated"};
    const std::vector<std::string> empty = {""};
    EXPECT_EQ(Assemble("字", plan, {}, JoinStyle::Compact, 100, output), Error::InvalidPlan);
    EXPECT_EQ(Assemble("字", plan, empty, JoinStyle::Compact, 100, output), Error::InvalidInput);
    EXPECT_EQ(Assemble("字", plan, invalid, JoinStyle::Compact, 100, output), Error::InvalidPlan);
    for (const std::string& text : invalid)
    {
        const std::vector<std::string> one = {text};
        EXPECT_EQ(Assemble("字", plan, one, JoinStyle::Compact, 100, output), Error::InvalidInput);
    }
    Plan broken = plan;
    broken.pieces[0].offset = 1;
    EXPECT_EQ(Assemble("字", broken, valid, JoinStyle::Compact, 100, output), Error::InvalidPlan);
    broken = plan;
    broken.pieces[0].length = 1;
    EXPECT_EQ(Assemble("字", broken, valid, JoinStyle::Compact, 100, output), Error::InvalidPlan);
    broken = plan;
    broken.pieces[0].literal = true;
    broken.segmentCount = 0;
    EXPECT_EQ(Assemble("字", broken, {}, JoinStyle::Compact, 100, output), Error::InvalidPlan);
    EXPECT_EQ(output, "old");
}

// 已取消调用不解析原文、不发布计划或译文，保留调用方已有状态。
// 入参：无。返回：测试断言结果。
TEST(TextSegmentation, cancellation_preserves_existing_outputs)
{
    Plan plan;
    ASSERT_EQ(Split("hello", plan), Error::None);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(Split("replacement", plan, stop.get_token()), Error::Cancelled);
    EXPECT_EQ(plan.sourceBytes, 5U);
    const std::vector<std::string> translated = {"translated"};
    std::string output = "old";
    EXPECT_EQ(Assemble("hello", plan, translated, JoinStyle::Compact, 100, output, stop.get_token()), Error::Cancelled);
    EXPECT_EQ(output, "old");
}
} // namespace
