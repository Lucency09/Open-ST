// 验证普通自定义HTTP表单回写协议JSON时保留原生类型与模式草稿，不链接或调用翻译网络。
#include "translation_custom_draft.h"
#include <gtest/gtest.h>
namespace
{
using Json = nlohmann::json;
// 提供覆盖嵌套正文和全部映射能力的合成配置。
// 入参：comparison为JSON原生比较值。
// 返回：独立测试条目，没有真实账号或服务端点。
Json Profile(Json comparison)
{
    return {{"id", "stable"},
            {"name", "Synthetic"},
            {"kind", "custom_http"},
            {"enabled", false},
            {"secrets", {{"api_key", ""}}},
            {"configuration",
             {{"method", "PATCH"},
              {"url", "https://example.invalid/translate"},
              {"headers", {{"Authorization", "Bearer {{secret.api_key}}"}}},
              {"query", {{"q", "{{text}}"}}},
              {"source_languages", {{"auto", "detect"}, {"zh-CN", "zh"}, {"en", "en"}, {"ja", "jp"}}},
              {"target_languages", {{"zh-CN", "zh"}, {"en", "en"}, {"ja", "jp"}}},
              {"body_mode", "json"},
              {"body", Json::array({{{"text", "{{text}}"}, {"nested", Json::array({true, 12, nullptr})}}})},
              {"response",
               {{"mode", "json"},
                {"array_pointer", ""},
                {"item_pointer", ""},
                {"separator", "\n\t"},
                {"success", {{"pointer", ""}, {"equals", comparison}}},
                {"error_pointer", ""},
                {"error_map",
                 {{"auth", "authentication"},
                  {"permission", "permission"},
                  {"quota", "quota"},
                  {"limit", "rate_limited"},
                  {"bad", "input"},
                  {"", "service"},
                  {"length", "context_limit"}}}}}}}};
}
// 编辑后未改输入必须保留空Pointer、真实换行分隔符和四种标量，数字不会变成字符串。
// 入参：无。
// 返回：用断言记录完整协议对象保持相同。
TEST(CustomTranslationDraftTest, round_trip_preserves_all_fields_and_native_scalar_types)
{
    for (const Json& value : Json::array({"1", 1, -2, 1.5, true, false, nullptr}))
    {
        const Json profile = Profile(value);
        open_st::CustomTranslationDraft draft(profile);
        Json result;
        ASSERT_TRUE(draft.Build(result));
        EXPECT_EQ(result, profile.at("configuration"));
        const auto& actual = result.at("response").at("success").at("equals");
        EXPECT_EQ(actual.is_string(), value.is_string());
        EXPECT_EQ(actual.is_number(), value.is_number());
        EXPECT_EQ(actual.is_boolean(), value.is_boolean());
        EXPECT_EQ(actual.is_null(), value.is_null());
        EXPECT_EQ(draft.secrets, profile.at("secrets"));
    }
}
// 正文模式切换只选择序列化来源，不丢弃非法JSON中间稿或表单和纯文本。
// 入参：无。
// 返回：活动模式有效时能提交，切回非法JSON时必须拒绝且不改输出。
TEST(CustomTranslationDraftTest, body_modes_preserve_hidden_drafts_and_reject_active_partial_json)
{
    open_st::CustomTranslationDraft draft(Profile(nullptr));
    draft.jsonBody = "{unfinished";
    draft.rawBody = "{{text}}\n{{source}}";
    draft.formBody = {{"value", "{{text}} &+%"}};
    Json output = {{"unchanged", true}};
    EXPECT_FALSE(draft.Build(output));
    EXPECT_EQ(output, (Json{{"unchanged", true}}));
    draft.bodyMode = "form";
    ASSERT_TRUE(draft.Build(output));
    EXPECT_EQ(output["body"], draft.formBody);
    draft.bodyMode = "raw";
    ASSERT_TRUE(draft.Build(output));
    EXPECT_EQ(output["body"], "{{text}}\n{{source}}");
    draft.bodyMode = "none";
    ASSERT_TRUE(draft.Build(output));
    EXPECT_TRUE(output["body"].is_null());
    draft.bodyMode = "json";
    EXPECT_FALSE(draft.Build(output));
    EXPECT_EQ(draft.jsonBody, "{unfinished");
    draft.jsonBody = R"([{"text":"{{text}}","nested":[true,null,5]}])";
    ASSERT_TRUE(draft.Build(output));
    EXPECT_TRUE(output["body"].is_array());
}
// 纯文本响应隐藏JSON映射但不清除其草稿；空成功路径与业务错误路径不得被当成未启用。
// 入参：无。
// 返回：文本模式只发布mode，切回JSON恢复原结构。
TEST(CustomTranslationDraftTest, response_modes_preserve_mapping_and_optional_empty_pointers)
{
    const Json profile = Profile("ok\nnext");
    open_st::CustomTranslationDraft draft(profile);
    draft.responseMode = "text";
    Json result;
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"], (Json{{"mode", "text"}}));
    draft.responseMode = "json";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"], profile["configuration"]["response"]);
    draft.useSuccess = false;
    draft.useErrors = false;
    draft.extraction = "single";
    draft.textPointer = "";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"], (Json{{"mode", "json"}, {"text_pointer", ""}}));
    draft.useSuccess = true;
    draft.useErrors = true;
    draft.extraction = "array";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"], profile["configuration"]["response"]);
}
// 标量模式保存独立字符串/数值/布尔草稿，输入JSON字符串不能冒充数值。
// 入参：无。
// 返回：各原生类别保持，非法数值阻止当前模式提交。
TEST(CustomTranslationDraftTest, scalar_editor_does_not_coerce_text_or_lose_other_type_drafts)
{
    open_st::CustomTranslationDraft draft(Profile("plain"));
    draft.successNumber = "42";
    draft.successBoolean = true;
    Json result;
    draft.scalarType = "number";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"]["success"]["equals"], 42);
    draft.scalarType = "boolean";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"]["success"]["equals"], true);
    draft.scalarType = "null";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_TRUE(result["response"]["success"]["equals"].is_null());
    draft.scalarType = "string";
    ASSERT_TRUE(draft.Build(result));
    EXPECT_EQ(result["response"]["success"]["equals"], "plain");
    draft.scalarType = "number";
    draft.successNumber = "\"42\"";
    EXPECT_FALSE(draft.Build(result));
    draft.useSuccess = false;
    EXPECT_TRUE(draft.Build(result));
}
} // namespace
