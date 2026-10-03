// 自定义HTTP子表单的独立草稿；切换显示模式不丢弃未提交字段。
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
namespace open_st
{
struct CustomTranslationDraft final
{
    std::string method, url, bodyMode, jsonBody, rawBody;
    nlohmann::json headers, query, sourceLanguages, targetLanguages, formBody, secrets;
    std::string responseMode, extraction, textPointer, arrayPointer, itemPointer, separator;
    bool useSuccess{}, successBoolean{}, useErrors{};
    std::string successPointer, scalarType, successText, successNumber, errorPointer;
    nlohmann::json errorMap;
    // 从已保存配置建立可独立切换模式的UI草稿，未出现的模式采用空输入。
    // 入参：profile为完整条目。
    // 返回：不修改原条目的独立草稿。
    explicit CustomTranslationDraft(const nlohmann::json& profile);
    // 只序列化当前正文/响应模式；隐藏的无效中间输入不进入候选。
    // 入参：configuration接收完整候选。
    // 返回：活动JSON/标量输入可解析时true。
    [[nodiscard]] bool Build(nlohmann::json& configuration) const;
};
// 解析JSON正文或带类型的比较值，不把字符串/数字/bool/null混为文本。
// 入参：value为原始文本，result为成功输出，body限制为对象/数组。
// 返回：完整且有界JSON为true，失败不替换输出。
bool ParseTranslationEditorJson(std::string_view value, nlohmann::json& result, bool body);
} // namespace open_st
