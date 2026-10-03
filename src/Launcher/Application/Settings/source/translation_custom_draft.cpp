// 将普通表单草稿适配回既有HTTP配置schema，业务合法性仍由领域回调校验。
#include "translation_custom_draft.h"
#include <stdexcept>
namespace open_st
{
// 限制编辑器解析资源。
// 入参：value为原始JSON文本，result为成功输出，body要求对象或数组。
// 返回：成功状态，失败不提交旧解析对象。
bool ParseTranslationEditorJson(std::string_view value, nlohmann::json& result, bool body)
{
    try
    {
        if (value.size() > 262144)
            return false;
        // 逐事件限制编辑器JSON嵌套深度，不改变数据内容。
        // 入参：depth为当前深度，事件和值在此仅按解析器契约借用。
        // 返回：允许时true，超过保护上限抛错使本次解析失败。
        const auto limit = [](int depth, nlohmann::json::parse_event_t, nlohmann::json&)
        {
            if (depth > 64)
                throw std::runtime_error("Translation editor nesting limit");
            return true;
        };
        nlohmann::json candidate = nlohmann::json::parse(value, limit);
        if (body && !candidate.is_object() && !candidate.is_array())
            return false;
        result = std::move(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
// 拆出UI模式草稿，保留空Pointer、真实分隔符和标量类别。
// 入参：profile为完整候选。
// 返回：没有任何持久化操作的新草稿。
CustomTranslationDraft::CustomTranslationDraft(const nlohmann::json& profile)
{
    const nlohmann::json& config = profile.at("configuration");
    this->method = config.value("method", "POST");
    this->url = config.value("url", "");
    this->headers = config.value("headers", nlohmann::json::object());
    this->query = config.value("query", nlohmann::json::object());
    this->sourceLanguages = config.value("source_languages", nlohmann::json::object());
    this->targetLanguages = config.value("target_languages", nlohmann::json::object());
    this->secrets = profile.value("secrets", nlohmann::json::object());
    this->bodyMode = config.value("body_mode", "none");
    this->jsonBody = "{}";
    this->formBody = nlohmann::json::object();
    if (config.contains("body"))
    {
        if (this->bodyMode == "json")
            this->jsonBody = config.at("body").dump(2);
        else if (this->bodyMode == "form")
            this->formBody = config.at("body");
        else if (this->bodyMode == "raw")
            this->rawBody = config.at("body").get<std::string>();
    }
    const nlohmann::json response = config.value("response", nlohmann::json::object());
    this->responseMode = response.value("mode", "text");
    this->extraction = response.contains("array_pointer") ? "array" : "single";
    this->textPointer = response.value("text_pointer", "");
    this->arrayPointer = response.value("array_pointer", "");
    this->itemPointer = response.value("item_pointer", "");
    this->separator = response.value("separator", "\n");
    this->useSuccess = response.contains("success");
    this->scalarType = "string";
    if (this->useSuccess)
    {
        const nlohmann::json& success = response.at("success");
        this->successPointer = success.at("pointer").get<std::string>();
        const nlohmann::json& value = success.at("equals");
        if (value.is_string())
            this->successText = value.get<std::string>();
        else if (value.is_boolean())
        {
            this->scalarType = "boolean";
            this->successBoolean = value.get<bool>();
        }
        else if (value.is_null())
            this->scalarType = "null";
        else
        {
            this->scalarType = "number";
            this->successNumber = value.dump();
        }
    }
    this->useErrors = response.contains("error_pointer");
    this->errorPointer = response.value("error_pointer", "");
    this->errorMap = response.value("error_map", nlohmann::json::object());
}
// 只输出当前模式并保持JSON原生类型，隐藏草稿不参与提交。
// 入参：configuration接收成功装配的完整协议对象。
// 返回：活动输入可解析为true，无效时保留输出原值。
bool CustomTranslationDraft::Build(nlohmann::json& configuration) const
{
    nlohmann::json body;
    if (this->bodyMode == "json")
    {
        if (!ParseTranslationEditorJson(this->jsonBody, body, true))
            return false;
    }
    else if (this->bodyMode == "form")
        body = this->formBody;
    else if (this->bodyMode == "raw")
        body = this->rawBody;
    else if (this->bodyMode != "none")
        return false;
    nlohmann::json response{{"mode", this->responseMode}};
    if (this->responseMode == "json")
    {
        if (this->extraction == "array")
        {
            response["array_pointer"] = this->arrayPointer;
            response["item_pointer"] = this->itemPointer;
            response["separator"] = this->separator;
        }
        else if (this->extraction == "single")
            response["text_pointer"] = this->textPointer;
        else
            return false;
        if (this->useSuccess)
        {
            nlohmann::json value;
            if (this->scalarType == "string")
                value = this->successText;
            else if (this->scalarType == "number")
            {
                if (!ParseTranslationEditorJson(this->successNumber, value, false) || !value.is_number())
                    return false;
            }
            else if (this->scalarType == "boolean")
                value = this->successBoolean;
            else if (this->scalarType != "null")
                return false;
            response["success"] = {{"pointer", this->successPointer}, {"equals", value}};
        }
        if (this->useErrors)
        {
            response["error_pointer"] = this->errorPointer;
            response["error_map"] = this->errorMap;
        }
    }
    else if (this->responseMode != "text")
        return false;
    configuration = {{"method", this->method},
                     {"url", this->url},
                     {"headers", this->headers},
                     {"query", this->query},
                     {"source_languages", this->sourceLanguages},
                     {"target_languages", this->targetLanguages},
                     {"body_mode", this->bodyMode},
                     {"body", std::move(body)},
                     {"response", std::move(response)}};
    return true;
}
} // namespace open_st
