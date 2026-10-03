// 声明截图会话和OCR结果的生命周期协调，不向核心传入App或窗口对象。
#pragma once
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
namespace open_st
{
class SdrSelectionFrame;
class TranslationClient;
inline constexpr UINT TextWakeMessage = WM_APP + 141;
inline constexpr UINT_PTR TextPollTimer = 141;
class CaptureTextSession final
{
  public:
    // 建立延迟加载的协调器。
    // 入参：owner为消息窗口；icon为借用图标；read一次读取完整配置，失败为空；translator借用宿主唯一客户端。返回：未识别对象。
    CaptureTextSession(HWND owner, HICON icon, std::function<std::optional<nlohmann::json>()> read = {},
                       TranslationClient* translator = nullptr);
    // 回收会话；正常退出须先等后台结束。
    // 入参：无。返回：无。
    ~CaptureTextSession();
    // 接收本次正式输出及固定设置。
    // 入参：frame为借用图像；model/language为OCR参数，不自动发起翻译。返回：无。
    void Begin(const SdrSelectionFrame& frame, const std::string& model, const std::string& language);
    // 提供结果窗口给宿主协调层级，不暴露窗口实现。
    // 入参：无。返回：存活结果窗口的借用句柄或空。
    HWND ResultWindowHandle() const noexcept;
    // 激活已有结果，防止再次生成图像。
    // 入参：无。返回：已有窗口为true，不提交识别或翻译。
    bool ActivateExisting() noexcept;
    // 已保存的接口或语言设置变化立即使在途请求及旧译文失效，不自动重发。
    // 入参：无。返回：无。
    void ConfigurationChanged() noexcept;
    // 关闭当前结果并取消请求，后台对象继续存活。
    // 入参：无。返回：无。
    void EndCapture() noexcept;
    // 查询并分派新快照，补收丢失通知。
    // 入参：无。返回：无。
    void Poll() noexcept;
    // 转交非模态窗口输入。
    // 入参：message为主循环消息。返回：消费为true。
    bool Process(MSG& message);
    // 转交系统注册的编辑组合。
    // 入参：modifiers/key为已核验热键。返回：消费为true。
    bool RegisteredHotkey(UINT modifiers, UINT key) noexcept;
    // 开始取消退出，UI继续分派消息。
    // 入参：无。返回：无。
    void Shutdown() noexcept;
    // 查询后台释放完成。
    // 入参：无。返回：可以析构为true。
    bool ShutdownComplete() const noexcept;
    // 将宿主模态准入状态同步到结果窗口。
    // 入参：paused为暂停标志。返回：无。
    void Pause(bool paused) noexcept;

  private:
    // 仅响应结果窗明确操作，固定当前草稿和配置；OCR未完成时拒绝，不排队。
    // 入参：无。返回：无；拒绝原因在现有窗口显示。
    void Translate() noexcept;
    // 取消本次翻译发布资格，不等待后台。
    // 入参：无。返回：无。
    void CancelTranslation() noexcept;
    // 从当前配置刷新持续传输提示，不读取正文或访问网络。
    // 入参：无。返回：无，配置错误留给领域提交校验。
    void RefreshNetworkWarning();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
