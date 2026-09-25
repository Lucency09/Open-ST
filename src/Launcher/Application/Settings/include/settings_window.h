// 声明设置窗口及宿主注入的本地化、启动项应用和忙状态回调。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace open_st
{
// Settings 自有组合值，由 Application 适配到输入与注册模块。
struct SettingsHotkeyChord final
{
    UINT modifiers{};
    UINT key{};
};
// Application 拥有的维护任务快照，查询和显示只发生在 UI 线程。
struct SettingsMaintenanceStatus final
{
    bool running{};
    std::wstring text;
};
// 设置下拉框的自有选项值，领域模块通过 Application 适配。
struct SettingsOption final
{
    std::string value;
    std::wstring label;
};
// 由上级 Application 提供本地化查询及保存通知，Settings 不依赖同级本地化模块。
struct SettingsWindowCallbacks final
{
    // 可选借用图标，调用者保持有效至窗口关闭；Settings 不销毁句柄。
    HICON largeIcon{};
    HICON smallIcon{};
    // 按调用方请求的动态文本键查询当前语言文本。
    // 入参：std::string_view 为只在调用期间借用的界面文本键。
    // 返回：该键在当前语言下的宽字符串文案。
    std::function<std::wstring(std::string_view)> text;
    // 查询当前已生效语言，作为设置缺失时的选中项。
    // 入参：无。
    // 返回：宿主当前已生效的语言代码。
    std::function<std::string()> currentLanguage;
    // 每次展开下拉框时重新查询资源实际提供的语言代码。
    // 入参：无。
    // 返回：资源当前声明的可用语言代码列表。
    std::function<std::vector<std::string>()> availableLanguages;
    // 通知宿主设置窗口进入或退出确认、保存及系统应用的忙状态。
    // 入参：bool 表示是否忙碌；true 为进入，false 为退出。
    // 返回：无返回值；通知异常由窗口捕获，不影响交互恢复。
    std::function<void(bool)> busyChanged;
    // 在意图保存后应用当前用户的系统启动入口。
    // 入参：bool 为已保存的自启启用意图。
    // 返回：系统启动入口已按目标生效时为 true，否则为 false。
    std::function<bool(bool)> startupApplied;
    // 动态查询并本地化系统启动入口状态。
    // 入参：无。
    // 返回：当前启动入口状态的本地化宽字符串。
    std::function<std::wstring()> startupStatus;
    // 在语言持久化成功后通知宿主应用运行期语言。
    // 入参：std::string_view 为已保存的语言代码，只在本次调用期间有效。
    // 返回：运行期语言已生效时为 true，否则为 false。
    std::function<bool(std::string_view)> languageApplied;
    // 解析并校验业务允许的组合字符串。
    // 入参：string_view 为配置 token，仅本次调用借用。
    // 返回：合法组合；无效或不支持时为空。
    std::function<std::optional<SettingsHotkeyChord>(std::string_view)> hotkeyDecode;
    // 校验录入并生成规范配置值。
    // 入参：SettingsHotkeyChord 为录入的数值组合。
    // 返回：规范字符串；无效组合为空。
    std::function<std::optional<std::string>(SettingsHotkeyChord)> hotkeyEncode;
    // 生成组合显示文字，不修改配置或注册。
    // 入参：SettingsHotkeyChord 为待显示组合。
    // 返回：可显示的组合名称。
    std::function<std::wstring(SettingsHotkeyChord)> hotkeyFormat;
    // 保存前准备候选注册并保留旧注册。
    // 入参：string_view 为目标配置。
    // 返回：候选准备成功为 true；失败不改变活动注册。
    std::function<bool(std::string_view)> hotkeyPrepare;
    // 查询草稿组合是否尚未生效，或是否仍有旧注册需要释放。
    // 入参：string_view 为目标组合 token。
    // 返回：需要通过应用完成注册或重试为 true；空回调视为没有待应用状态。
    std::function<bool(std::string_view)> hotkeyNeedsApply;
    // 无异常地发布或撤销已准备候选；宿主实现不得抛出异常。
    // 入参：bool 为是否提交候选。
    // 返回：资源清理成功为 true；false 表示注册仍需清理，提交的新组合已生效。
    std::function<bool(bool)> hotkeyFinish;
    // 查询活动注册及清理状态。
    // 入参：无。
    // 返回：本地化状态文字。
    std::function<std::wstring()> hotkeyStatus;
    // 通知宿主录入开始或结束，用于暂停截图并截断排队消息。
    // 入参：bool 为是否正在录入。
    // 返回：无。
    std::function<void(bool)> hotkeyRecording;
    // 在注册事务结束后刷新宿主提示，不参与无异常激活步骤。
    // 入参：无。
    // 返回：无；刷新失败不撤销已保存结果。
    std::function<void()> hotkeyRefresh;
    // 校验直接输入的选框颜色，并在显式提交时提供大写规范值。
    // 入参：string_view 为用户原始 #RRGGBB 文本，不允许自动裁剪空白。
    // 返回：合法颜色的大写 token；非法或未完成输入为空。
    std::function<std::optional<std::string>(std::string_view)> normalizeBorderColor;
    // 校验默认保存格式的稳定配置 token。
    // 入参：string_view 为 jpeg 或 png 格式 token。
    // 返回：支持该格式时为 true。
    std::function<bool(std::string_view)> validImageFormat;
    // 校验整数 JPEG 质量，不夹取或替换用户值。
    // 入参：int64_t 为质量值，合法范围由宿主规则确定。
    // 返回：可用于 JPEG 编码时为 true。
    std::function<bool(std::int64_t)> validJpegQuality;
    // 只在当前构建提供 OCR 时开放对应设置页及默认恢复范围。
    bool ocrAvailable{};
    // 查询领域提供的 OCR 模型档位，不在 Settings 重复维护合法值。
    // 入参：无。
    // 返回：稳定配置值和当前语言显示名称。
    std::function<std::vector<SettingsOption>()> ocrModels;
    // 查询领域提供的 OCR 识别语言组合。
    // 入参：无。
    // 返回：稳定配置值和当前语言显示名称。
    std::function<std::vector<SettingsOption>()> ocrLanguages;
    // 提交前复核 OCR 参数组合，不加载模型或发起识别。
    // 入参：模型档位、识别语言配置值，仅本次调用借用。
    // 返回：领域允许的组合为 true。
    std::function<bool(std::string_view, std::string_view)> validOcrOptions;
    // 打开日志目录，不保存当前草稿；成功打开为 true，缺少回调时操作不可用。
    std::function<bool()> openLogDirectory;
    // 启动已确认的历史清理；true 表示任务已接收，不表示删除完成。
    std::function<bool()> clearHistoricalLogs;
    // 查询宿主维护任务的当前状态，不持有任务或窗口所有权。
    std::function<SettingsMaintenanceStatus()> maintenanceStatus;
};

// 管理进程内唯一的非模态设置窗口；关闭窗口不会结束应用消息循环。
class SettingsWindow final
{
  public:
    // 创建空窗口容器，实际 HWND 在 Show 时创建。
    // 入参：无显式入参。
    // 返回：无返回值。
    SettingsWindow();
    // 禁止复制窗口所有权。
    // 入参：未命名 SettingsWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    SettingsWindow(const SettingsWindow&) = delete;
    // 禁止复制赋值，避免重复销毁窗口资源。
    // 入参：未命名 SettingsWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    // 关闭窗口并释放其持有的回调和控件资源。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~SettingsWindow();

    // 在 UI 线程创建并显示设置窗口；窗口已存在时激活原窗口。
    // 入参：instance 为当前程序模块句柄；callbacks 为移交给窗口的宿主回调集合，捕获对象须活到窗口释放回调。
    // 返回：已有窗口激活或新建显示成功为 true；参数、布局、绑定或创建失败为 false。
    [[nodiscard]] bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks) noexcept;
    // 处理非模态窗口的 Tab、Enter 和 Escape 键盘导航。
    // 入参：message 为应用消息循环取得的待处理消息。
    // 返回：消息已被窗口键盘导航处理为 true，否则为 false。
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const noexcept;
    // 将本程序已注册组合转交仍有焦点的录入控件。
    // 入参：modifiers、key 为组合；time 为原消息时间。
    // 返回：控件接受消息为 true；无录入或过期消息为 false。
    [[nodiscard]] bool ProcessRecordedHotkey(UINT modifiers, UINT key, DWORD time) noexcept;
    // 空闲时同步关闭并释放回调；忙时仅请求延迟关闭，回调保留至后续空闲 Close 或销毁。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Close() noexcept;
    // 重新获取当前语言的布局、状态和字段错误文字。
    // 入参：无显式入参。
    // 返回：无返回值。
    void RefreshTexts() noexcept;
    // 取得当前设置窗口句柄，供宿主设置模态窗口的 owner。
    // 入参：无显式入参。
    // 返回：借用的当前设置窗口 HWND；未打开时为 nullptr，关闭后失效，不得由调用方销毁。
    [[nodiscard]] HWND NativeHandle() const noexcept;
    // 返回窗口是否仍然存在。
    // 入参：无显式入参。
    // 返回：实际设置窗口存在时为 true，否则为 false。
    [[nodiscard]] bool IsOpen() const noexcept;

  private:
    friend struct SettingsWindowTestAccess;
    class Impl;

    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
