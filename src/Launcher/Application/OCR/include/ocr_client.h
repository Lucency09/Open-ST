// 声明离线识别任务的像素输入、独占生命周期和不可变结果；不依赖窗口或其他业务模块。
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace open_st
{
struct OcrImageView
{
    std::uint32_t width{}, height{};
    std::size_t stride{};
    std::span<const std::byte> pixels;
};
struct OcrOptions
{
    std::string model{"fast"};
    std::string language{"chi_sim+eng+jpn"};
};
// 发布模型档位供表单直接消费，顺序与默认值保持一致。
// 入参：无。
// 返回：进程生命周期有效的只读列表。
std::span<const std::string_view> OcrModelChoices() noexcept;
// 发布可识别语言组合，界面语言不改变此领域列表。
// 入参：无。
// 返回：进程生命周期有效的只读列表。
std::span<const std::string_view> OcrLanguageChoices() noexcept;
// 验证领域参数，不接受任意配置或模型路径。
// 入参：options 为模型和语言字符串。
// 返回：两项均属于白名单时 true。
bool AreOcrOptionsValid(const OcrOptions& options) noexcept;
enum class OcrPhase
{
    Idle,
    Preparing,
    Loading,
    Recognizing,
    Succeeded,
    Empty,
    Failed,
    Cancelled
};
enum class OcrError
{
    None,
    Busy,
    InvalidInput,
    InvalidOptions,
    ModelMissing,
    ModelIntegrity,
    ModelLoad,
    Recognition,
    ResultTooLarge,
    Unavailable,
    Cancelled
};
struct OcrSnapshot
{
    std::uint64_t requestId{}, revision{};
    OcrPhase phase{OcrPhase::Idle};
    OcrError error{OcrError::None};
    bool busy{};
    std::shared_ptr<const std::wstring> text;
    double loadMilliseconds{}, preprocessMilliseconds{}, recognizeMilliseconds{};
};
namespace ocr_detail
{
class Engine;
}
struct OcrClientTestAccess;

// 控制命令限宿主线程；快照可跨线程读取。通知仅用于投递编号，不允许从通知重入或销毁对象。
class OcrClient final
{
  public:
    // 建立延迟加载客户端，模型固定从 EXE 根目录的 resources/ocr 读取。
    // 入参：root 为绝对程序目录；notify 为工作线程安全通知，可为空，宿主也可轮询快照。
    // 返回：未加载模型、未创建工作线程的实例；分配失败抛异常。
    OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify);
    // 回收唯一工作线程；正常宿主先异步 RequestShutdown 并等到 ShutdownComplete 再析构。
    // 入参：无。
    // 返回：无；兜底取消并等待未完成的不可中断引擎阶段，不 detach。
    ~OcrClient();
    // 禁止复制线程与模型的所有权。
    // 入参：未命名源客户端。
    // 返回：不可调用。
    OcrClient(const OcrClient&) = delete;
    // 禁止复制赋值任务所有权。
    // 入参：未命名源客户端。
    // 返回：不可调用。
    OcrClient& operator=(const OcrClient&) = delete;
    // 复制一份有界 BGRX 输入并提交唯一任务；像素单位为物理像素，X 通道忽略。
    // 入参：image 为本次同步借用；options 为档位和语言；requestId 输出单调编号；error 输出拒绝原因。
    // 返回：接受为 true；忙、停止、非法或分配失败为 false，不排队、不保留调用方图像。
    bool Submit(OcrImageView image, const OcrOptions& options, std::uint64_t& requestId, OcrError& error) noexcept;
    // 立即撤销当前结果的发布资格，不在 UI 回调中等待识别结束。
    // 入参：无。
    // 返回：无；Snapshot.busy 在后台真正结束前仍为 true。
    void Cancel() noexcept;
    // 禁止后续请求并唤醒工作线程释放引擎；宿主继续处理消息直至完成。
    // 入参：无。
    // 返回：无；不等待不可中断的初始化阶段。
    void RequestShutdown() noexcept;
    // 查询后台引擎是否已经释放且不再执行通知。
    // 入参：无。
    // 返回：未启线程或停止收尾已完成为 true，调用 RequestShutdown 后用于异步退出。
    [[nodiscard]] bool ShutdownComplete() const noexcept;
    // 取得独立状态与共享不可变正文，轮询失败通知也不会丢失完成状态。
    // 入参：无。
    // 返回：当前快照；取消后不返回过期正文。
    [[nodiscard]] OcrSnapshot Snapshot() const;

  private:
    friend struct OcrClientTestAccess;
    // 注入识别边界以测试真实单工作线程和取消状态，产品构造始终使用本地引擎。
    // 入参：root 为资源根；notify 为通知；engine 为独占测试引擎。
    // 返回：未启动客户端。
    OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify,
              std::unique_ptr<ocr_detail::Engine> engine);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
