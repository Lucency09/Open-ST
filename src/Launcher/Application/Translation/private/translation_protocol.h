// 声明供应商协议、模板和有界 JSON 的纯逻辑，不包含重试或窗口状态。
#pragma once
#include <http_transport.h>
#include <translation_client.h>

namespace open_st::translation_detail
{
// 验证 UTF-8、拒绝 NUL 并统计码点。
// 入参：text 为 UTF-8；count 接收码点数。
// 返回：合法为 true。
bool CountText(std::string_view text, std::size_t& count) noexcept;
// 严格转换 UTF-8 到 UTF-16。
// 入参：text 为 UTF-8。
// 返回：完整字符串，非法为空。
std::wstring Wide(std::string_view text);
// 表单或 URL 查询值编码，输入不得预编码。
// 入参：value 为原始 UTF-8。
// 返回：百分号编码字符串。
std::string Encode(std::string_view value);
// 验证 JSON 最大深度、节点及字符串预算，避免递归解析输入失控。
// 入参：value 为现有树，maxBytes 为序列化字节预算。
// 返回：合法为 true。
bool BoundedJson(const nlohmann::json& value, std::size_t maxBytes);
// 有界解析，回调在超过深度/节点时中止，不生成部分 JSON。
// 入参：text 为有界 UTF-8 JSON 文本。
// 返回：完整树；无效时抛异常。
nlohmann::json ParseJson(std::string_view text);
// 发布自定义 HTTP 默认配置。
// 入参：无。
// 返回：可编辑的模板示例。
nlohmann::json DefaultCustomConfiguration();
// 验证自定义模板/能力/映射及秘密名称，无需真实凭据。
// 入参：profile 为完整条目。
// 返回：合法为 None。
TranslationError ValidateCustom(const nlohmann::json& profile);
// 构造一次内置或自定义请求，不执行网络；返回后才准入传输。
// 入参：input 为整轮输入；profile 为单项；salt 为签名盐；output 接收请求。
// 返回：成功为 None；动态预算不足为 InputTooLarge。
TranslationError BuildRequest(const TranslationRequest& input, const nlohmann::json& profile, std::string_view salt,
                              http::Request& output);
// 单次内置响应解析，业务分类优先于 HTTP 兜底。
// 入参：provider 为类型，status 为状态，body 为正文，output 接收完整译文。
// 返回：成功为 None，错误不覆盖 output。
TranslationError ParseResponse(std::string_view provider, unsigned status, std::string_view body,
                               TranslationResult& output);
// 自定义模板请求展开，字段和值在正确编码阶段限额。
// 入参：input 为整轮输入；profile 为单项；output 接收请求。
// 返回：成功为 None。
TranslationError BuildCustomRequest(const TranslationRequest& input, const nlohmann::json& profile,
                                    http::Request& output);
// 自定义响应映射，绝不返回部分数组内容。
// 入参：profile 为条目；response 为元数据；body 为正文；output 接收结果。
// 返回：领域分类。
TranslationError ParseCustomResponse(const nlohmann::json& profile, const http::Result& response, std::string_view body,
                                     TranslationResult& output);
// HTTP 最终状态分类，只在业务错误不能识别时调用。
// 入参：status 为状态码。
// 返回：成功为 None，未知 400/422 为 InvalidInput。
TranslationError HttpStatusError(unsigned status) noexcept;
// 检查动态编码后的请求预算与公共合法性。
// 入参：request 为构造完成的请求。
// 返回：成功为 None。
TranslationError RequestBudget(const http::Request& request);
// 校验可发布译文非空、UTF-8 和总字节预算。
// 入参：text 为候选译文。
// 返回：合法为 None。
TranslationError ValidateResultText(std::string_view text) noexcept;
} // namespace open_st::translation_detail
