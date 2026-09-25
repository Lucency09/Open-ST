// 声明模型白名单验证与已验证内存读取，避免校验后按路径重新加载。
#pragma once
#include <atomic>
#include <ocr_client.h>
#include <vector>

namespace open_st::ocr_detail
{
struct ModelBytes
{
    std::string language;
    std::vector<char> bytes;
};
// 读取本次语言组合，校验编译期固定大小和 SHA-256 后交付同一份字节。
// 入参：root 为绝对程序根；options 为固定选择；cancel 为取消；models 输出完整候选。
// 返回：None 为完整成功，其余为分类失败且 models 为空。
OcrError LoadModels(const std::filesystem::path& root, const OcrOptions& options, const std::atomic_bool& cancel,
                    std::vector<ModelBytes>& models);
} // namespace open_st::ocr_detail
