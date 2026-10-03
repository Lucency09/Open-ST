// 本地文本排布的私有边界；只组装已验证 UTF-8，不决定模型、网络或后备接口。
#pragma once
#include <functional>
#include <translation_local.h>

namespace open_st::translation_local::detail
{
// 所有阶段共用的取消和期限规则，用户取消始终优先。
// 入参：deadline 为同一项期限，stop 为整轮取消。返回：当前停止原因。
Error StopReason(std::chrono::steady_clock::time_point deadline, std::stop_token stop) noexcept;
// 仅按原有 CRLF/LF/CR 翻译非空内容行；缩进、空白行和分隔符原样保留。
// 入参：text 已通过整文分词预算；translate 同步翻译一行，必须共用同一模型与期限。
// 返回：全部成功时才发布完整正文，任何失败均无部分译文；最终文字与格式合计不超过 1 MiB。
Result TranslateLines(std::string_view text, std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                      const std::function<Result(std::string_view)>& translate);
} // namespace open_st::translation_local::detail
