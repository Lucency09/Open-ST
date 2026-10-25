// 固定 Unicode 标量上限的翻译分段计划与有界拼接；不拥有提供方或设置状态。
#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace open_st::translation_segmentation
{
inline constexpr std::size_t MAX_SEGMENT_CODEPOINTS = 200;

enum class Error
{
    None,
    InvalidInput,
    NoBoundary,
    InvalidPlan,
    OutputTooLarge,
    Cancelled
};

enum class JoinStyle
{
    Compact,
    Spaced
};

// 原文的 UTF-8 字节范围；literal 为原样保留的分隔内容，其余范围逐项对应译文。
struct Piece
{
    std::size_t offset{};
    std::size_t length{};
    bool literal{};
};

// pieces 必须连续覆盖 sourceBytes，每个非 literal 范围最多包含 200 个 Unicode 标量。
struct Plan
{
    std::vector<Piece> pieces;
    std::size_t segmentCount{};
    std::size_t sourceBytes{};
};

// 按固定上限及语义边界生成完整分区，纯空白形成零正文计划。
// 入参：original 为合法 UTF-8；plan 接收成功结果；stop 为协作取消信号。
// 返回：错误分类；失败不改变 plan，不发布部分计划。
Error Split(std::string_view original, Plan& plan, std::stop_token stop = {});

// 以原始分隔内容和目标接缝规则拼接全部译文，并校验计划与输出预算。
// 入参：original 为原文；plan 为其完整分区；translations 逐项对应正文；style 为人工接缝规则；
// maxBytes 为最终 UTF-8 字节上限；output 接收完整结果；stop 为协作取消信号。
// 返回：错误分类；失败不改变 output，literal 对应侧之外的译文空白和标点保持原样。
Error Assemble(std::string_view original, const Plan& plan, std::span<const std::string> translations, JoinStyle style,
               std::size_t maxBytes, std::string& output, std::stop_token stop = {});
} // namespace open_st::translation_segmentation
