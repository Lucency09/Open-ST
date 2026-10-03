// 测试直接读取正式默认资源，避免维护另一套默认接口与语言配置。
#pragma once
#include <fstream>
#include <stdexcept>
#include <translation_client.h>

namespace open_st
{
// 读取源码树中的默认配置；不访问运行目录或用户设置。
// 入参：无。返回：默认设置对象；资源不可读时抛异常。
inline nlohmann::json TestDefaultSettings()
{
    std::ifstream stream(std::filesystem::path(OPEN_ST_TEST_SOURCE_ROOT) / "resources/default_settings.json");
    if (!stream)
        throw std::runtime_error("default settings fixture unavailable");
    return nlohmann::json::parse(stream).at("settings");
}
// 从正式资源创建测试条目，协议样例按需覆盖字段。
// 入参：kind 为类型。返回：新身份的空凭据条目。
inline nlohmann::json TestTranslationProfile(std::string_view kind)
{
    return CreateTranslationProfile(kind, TestDefaultSettings().at("translation.interfaces"));
}
// 用默认资源初始化请求语言和网络配置，具体用例再覆盖其关注的内容。
// 入参：无。返回：无原文的请求。
inline TranslationRequest TestTranslationRequest()
{
    const nlohmann::json values = TestDefaultSettings();
    const TranslationSettings settings = ReadTranslationSettings(
        [&values](std::string_view key) -> std::optional<nlohmann::json>
        {
            const auto found = values.find(key);
            return found == values.end() ? std::nullopt : std::optional<nlohmann::json>(*found);
        });
    TranslationRequest request;
    request.options = settings.options;
    request.configuration = settings.configuration;
    return request;
}
} // namespace open_st
