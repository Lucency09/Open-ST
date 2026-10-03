// 应用唯一翻译客户端及设置测试的发布资格；界面只借用，不另建工作线程。
#pragma once
#include <cstdint>
#include <memory>
#include <windows.h>
#ifdef OPEN_ST_HAS_TRANSLATION
#include <translation_client.h>
#endif
namespace open_st
{
inline constexpr UINT_PTR TranslationPollTimer = 142;
struct AppTranslationState
{
#ifdef OPEN_ST_HAS_TRANSLATION
    std::unique_ptr<TranslationClient> client;
#endif
    std::uint64_t probeRequest{};
};
} // namespace open_st
