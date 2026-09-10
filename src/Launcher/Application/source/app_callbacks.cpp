// 集中组装 App 注入子模块的回调，连接本地化、业务命令与跨线程截图门禁。

#include <app.h>
#include <pin_window_manager.h>
#include <settings_window.h>
#include <ui_text.h>
#include <vector>

namespace open_st
{
// 组装贴图与宿主之间的窄回调，保持兄弟模块互不依赖。
// 入参：无。
// 返回：文本同步查询，复制保存仅提交异步请求。
PinWindowCallbacks App::MakePinCallbacks()
{
    PinWindowCallbacks callbacks;
    // 查询贴图标题和菜单所需的当前语言文字。
    // 入参：key 为资源键。
    // 返回：本地化宽字符串。
    callbacks.text = [](std::string_view key) { return GetUiText(key); };
    // 将贴图输出意图投递到 App 消息队列。
    // 入参：id 为稳定图像标识；command 为复制或保存。
    // 返回：成功入队 true，不代表输出已经成功。
    callbacks.command = [this](PinId id, PinCommand command) { return this->PostPinCommand(id, command); };
    // 让主消息循环在 Manager 最外层调用退出后重新检查延迟退出条件。
    // 入参：无。
    // 返回：无返回值；不在管理器的同步回调内销毁 App。
    callbacks.stopped = [this]()
    {
        if (IsWindow(this->messageWindow_))
            PostMessageW(this->messageWindow_, WM_APP + 6, 0, 0);
    };
    return callbacks;
}

// 组装供设置和欢迎窗口调用的本地化及系统集成回调。
// 入参：无。
// 返回：包含文本、语言、启动项和忙状态通知的回调集合；其中捕获的 App 须存活到回调解除。
SettingsWindowCallbacks App::MakeSettingsCallbacks()
{
    SettingsWindowCallbacks callbacks;
    // 为子窗口提供指定键的当前语言文本。
    // 入参：key：布局或工具栏请求的本地化文本键。
    // 返回：GetUiText 返回的本地化宽字符串。
    callbacks.text = [](std::string_view key) { return GetUiText(key); };
    // 向设置窗口提供当前生效的语言代码。
    // 入参：无。
    // 返回：当前运行时语言代码，用于设置缺失时的默认选择。
    callbacks.currentLanguage = []() { return CurrentUiLanguageCode(); };
    // 向设置窗口提供当前可用的语言列表。
    // 入参：无。
    // 返回：文本资源可用时返回语言代码列表；不可用时返回空列表。
    callbacks.availableLanguages = []()
    { return IsUiTextAvailable() ? GetAvailableUiLanguages() : std::vector<std::string>{}; };
    // 在设置保存后切换运行语言并刷新已有界面。
    // 入参：language：已提交的语言代码。
    // 返回：SetUiLanguage 的应用结果；切换成功 true，失败 false，并执行界面刷新。
    callbacks.languageApplied = [this](std::string_view language)
    {
        const bool applied = SetUiLanguage(language);
        this->RefreshLocalizedUi();
        return applied;
    };
    callbacks.largeIcon = this->largeIcon_;
    callbacks.smallIcon = this->smallIcon_;
    // 把已保存的启动项开关应用到系统。
    // 入参：enabled：用户已提交的启动项启用状态。
    // 返回：ApplyStartup 的执行结果，成功 true，失败 false。
    callbacks.startupApplied = [this](bool enabled) { return this->ApplyStartup(enabled); };
    // 为设置页面查询当前启动项状态文字。
    // 入参：无显式入参；捕获 App 查询系统集成状态。
    // 返回：当前语言的启动状态宽字符串。
    callbacks.startupStatus = [this]() { return this->StartupStatusText(); };
    // 同步设置窗口忙状态以控制截图请求准入。
    // 入参：busy：设置窗口是否正在执行需阻止截图的操作。
    // 返回：无返回值；更新 settingsBusy_ 并刷新截图门禁。
    callbacks.busyChanged = [this](bool busy)
    {
        this->settingsBusy_ = busy;
        this->UpdateCaptureGate();
    };
    return callbacks;
}

// 创建供截图工具栏同步查询本地化文本的回调。
// 入参：无。
// 返回：文本查询函数；在 UI 线程调用，本地化服务须在调用期间可用。
std::function<std::wstring(std::string_view)> App::MakeToolbarTextResolver()
{
    // 查询工具栏指定文本键对应的当前语言文字。
    // 入参：key：工具栏请求的本地化文本键。
    // 返回：GetUiText 返回的本地化宽字符串。
    return [](std::string_view key) { return GetUiText(key); };
}

// 创建供截图工具栏投递业务命令的回调。
// 入参：无。
// 返回：UI 线程命令接收函数；捕获的 App 须存活到工具栏解除回调。
std::function<bool(CaptureToolbarCommand, std::uint64_t)> App::MakeToolbarCommandHandler()
{
    // 把工具栏命令转交 App 消息队列执行。
    // 入参：command：按钮稳定命令 ID；token：当前选区代次；捕获 App。
    // 返回：预订并投递成功 true；状态不允许或投递失败 false，不代表业务已执行成功。
    return [this](CaptureToolbarCommand command, std::uint64_t token)
    { return this->PostToolbarCommand(command, token); };
}

// 创建供单实例接收线程检查截图准入状态的回调。
// 入参：无。
// 返回：仅读取原子门禁的查询函数；App 须先停止并等待监听线程，再释放自身资源。
std::function<std::optional<LPARAM>()> App::MakeCaptureGateCallback()
{
    // 向实例接收线程提供当前截图请求是否可进入的判断。
    // 入参：无显式入参；捕获 this，原子读取应用截图门禁。
    // 返回：暂停时 nullopt；允许时返回当前代次，作为排队请求的校验令牌。
    return [this]() -> std::optional<LPARAM>
    {
        const std::uint64_t gate = this->captureGate_.load(std::memory_order_acquire);
        return (gate & 1) != 0 ? std::nullopt : std::optional<LPARAM>(static_cast<LPARAM>(gate));
    };
}
} // namespace open_st
