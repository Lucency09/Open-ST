// 定义识别边界与自有紧凑像素；引擎仅被唯一工作线程访问。
#pragma once
#include <atomic>
#include <ocr_client.h>
#include <vector>

namespace open_st::ocr_detail
{
constexpr std::size_t MAX_PIXELS = 32000000;
constexpr std::uint32_t MAX_DIMENSION = 16384;
constexpr std::size_t MAX_TEXT_UNITS = 1000000;
struct Image
{
    std::uint32_t width{}, height{};
    std::vector<std::byte> pixels;
};
struct Recognition
{
    OcrError error{OcrError::None};
    std::wstring text;
    double loadMilliseconds{}, preprocessMilliseconds{}, recognizeMilliseconds{};
};
// 检查完整像素视图，不访问指针指向内容。
// 入参：image 为物理尺寸、跨度及借用缓冲区。
// 返回：尺寸、像素预算、跨度和最后一行均可安全访问时 true。
bool ValidImage(OcrImageView image) noexcept;
class Engine
{
  public:
    // 经基类销毁引擎并释放缓存。
    // 入参：无。
    // 返回：无。
    virtual ~Engine() = default;
    // 在唯一工作线程执行任务，每个阶段检查取消，不发布部分识别正文。
    // 入参：root 为资源根；image 为自有图；options 为已验证选项；cancel 为合作取消；phase 为阶段通知。
    // 返回：完整结果或分类失败；异常由客户端转换为 Unavailable。
    virtual Recognition Recognize(const std::filesystem::path& root, const Image& image, const OcrOptions& options,
                                  const std::atomic_bool& cancel, const std::function<void(OcrPhase)>& phase) = 0;
};
// 建立实际 Tesseract 引擎外壳，不加载模型。
// 入参：无。
// 返回：独占识别引擎。
std::unique_ptr<Engine> CreateEngine();
} // namespace open_st::ocr_detail
