// 本地语言识别适配：只读取系统返回的首选语言，不按目标或候选列表猜测方向。
#pragma once
#include <functional>
#include <translation_local.h>

namespace open_st::translation_local::detail
{
using LanguageDetector = std::function<std::wstring(std::wstring_view)>;
// 对全文副本做 NFC 后调用离线 ELS；注入边界仅供确定性验证，不改变原文。
// 入参：text 为有界 UTF-8，deadline/stop 沿用整轮预算，detector 为空时使用系统服务。
// 返回：检测出的受支持源语言或不适用/取消/超时/非法输入。
Result DetectSourceLanguage(std::string_view text, std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                            const LanguageDetector& detector = {});
} // namespace open_st::translation_local::detail
