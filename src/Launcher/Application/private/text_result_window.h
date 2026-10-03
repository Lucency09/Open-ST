// 声明文字结果的唯一原文草稿、显示译文及公共窗口绑定。
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <windows.h>
namespace open_st
{
class WindowRenderer;
struct TextResultCallbacks
{
    std::function<void()> close, changed, translate, cancelTranslation;
};
class TextResultWindow final
{
  public:
    // 建立空结果窗口。入参：无。返回：未显示对象。
    TextResultWindow();
    // 释放公共窗口。入参：无。返回：无。
    ~TextResultWindow();
    // 创建非模态表单并固定初始语言，不读取配置文件。
    // 入参：owner/icon为宿主资源；callbacks为窄通知；source/target为语言。返回：成功为true。
    bool Show(HWND owner, HICON icon, TextResultCallbacks callbacks, std::string source = "auto",
              std::string target = "zh-CN");
    // 更新通用状态。入参：key为文本键。返回：无。
    void Status(std::string_view key);
    // 更新带参数或逐项诊断的安全状态。入参：text不含原文和凭据。返回：无。
    void StatusText(std::wstring text);
    // 首次填充本地识别原文，保持单一草稿。入参：text为正文。返回：无。
    void SetSource(std::wstring_view text);
    // 接受已经过会话核验的译文。入参：text为UTF8，origin为安全来源名。返回：无。
    void SetTranslation(std::string text, std::wstring origin);
    // 配置应用后使现有来源戳过期。入参：无。返回：无。
    void ConfigurationChanged();
    // 根据实际任务快照刷新按钮，不存忙状态镜像。入参：busy为当前是否忙。返回：无。
    void TranslationActivity(bool busy);
    // 显示配置链是否含明文HTTP。入参：insecure。返回：无。
    void SetNetworkWarning(bool insecure);
    // 读取唯一草稿。入参：无。返回：只读引用。
    const std::string& SourceText() const noexcept;
    // 读取用户的临时源语言。入参：无。返回：只读引用。
    const std::string& SourceLanguage() const noexcept;
    // 读取临时目标语言。入参：无。返回：只读引用。
    const std::string& TargetLanguage() const noexcept;
    // 查询包含文字、语言、配置变化的单调修订。入参：无。返回：修订号。
    std::uint64_t Revision() const noexcept;
    // 查询是否已有可编辑OCR结果。入参：无。返回：就绪为true。
    bool SourceReady() const noexcept;
    // 请求延迟关闭。入参：无。返回：无。
    void Close() noexcept;
    // 激活现有窗口。入参：无。返回：无。
    void Activate() noexcept;
    // 查询原生存活状态。入参：无。返回：存活为true。
    bool IsOpen() const noexcept;
    // 复用公共表单输入。入参：message。返回：消费为true。
    bool Process(MSG& message);
    // 分派已核验的全局编辑热键。入参：modifiers/key。返回：消费为true。
    bool RegisteredHotkey(UINT modifiers, UINT key) noexcept;
    // 参与宿主模态输入门禁。入参：paused。返回：无。
    void Pause(bool paused) noexcept;

  private:
    // 统一失效当前显示译文，通知宿主取消任务。入参：无。返回：无。
    void Changed();
    // 复制指定UTF8草稿。入参：text。返回：无，失败保留内容。
    void Copy(const std::string& text);
    std::unique_ptr<WindowRenderer> renderer_;
    TextResultCallbacks callbacks_;
    std::string source_, target_, draft_, translated_;
    std::wstring origin_;
    std::uint64_t revision_{}, translatedRevision_{};
    bool sourceReady_{}, insecure_{}, closing_{};
};
} // namespace open_st
