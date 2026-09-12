// 声明组合键规则、截图会话映射及全局注册事务；业务准入和消息时间边界由宿主维护。
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace open_st
{
struct HotkeyChord
{
    UINT modifiers{};
    UINT key{};
    // 按修饰键和虚拟键比较两个规范组合。
    // 入参：other 为待比较组合。
    // 返回：两个值都相等时为 true。
    bool operator==(const HotkeyChord& other) const noexcept = default;
};

// 验证本阶段支持范围，排除系统保留和截图会话冲突组合。
// 入参：chord 为不含 MOD_NOREPEAT 的组合值。
// 返回：允许保存及尝试注册时为 true。
bool IsSupportedHotkey(HotkeyChord chord) noexcept;

// 解析与语言无关的严格 token 格式，允许调整修饰键顺序。
// 入参：text 为组合文本；chord 只在解析及业务校验成功时更新。
// 返回：合法组合为 true；失败时保持输出不变。
bool ParseHotkey(std::string_view text, HotkeyChord& chord) noexcept;

// 生成 Ctrl、Alt、Shift、主键顺序的持久化与默认显示文本。
// 入参：chord 为待格式化组合。
// 返回：合法组合的规范文本；不支持的组合返回空串。
std::string SerializeHotkey(HotkeyChord chord);

enum class SessionKeyCommand
{
    None,
    Copy,
    Save,
    Cancel
};

// 保留截图窗口原有固定键语义，Esc 不受重复和修饰键限制。
// 入参：key、keyData 来自 WM_KEYDOWN；modifiers 为当前 MOD_* 位。
// 返回：固定截图命令；未匹配或复制保存重复按键返回 None。
SessionKeyCommand MatchSessionKey(WPARAM key, LPARAM keyData, UINT modifiers) noexcept;

class HotkeyBackend;
class HotkeyManagerTestAccess;

class HotkeyManager
{
  public:
    // 为宿主窗口创建唯一注册所有者，构造阶段不注册组合。
    // 入参：window 为借用的消息接收窗口，必须活到管理器清理之后。
    // 返回：无；内部状态分配失败抛出标准分配异常。
    explicit HotkeyManager(HWND window);
    // 尽力清理所有注册；宿主正常退出应先显式 Shutdown 以检查失败。
    // 入参：无。
    // 返回：无。
    ~HotkeyManager();
    // 禁止复制唯一注册所有者。
    // 入参：源管理器。
    // 返回：不可调用。
    HotkeyManager(const HotkeyManager&) = delete;
    // 禁止复制赋值，避免多个管理器回收同一注册。
    // 入参：源管理器。
    // 返回：不可调用。
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // 在保留当前活动注册的同时准备候选，预先完成后续切换所需分配。
    // 入参：chord 为经过支持范围校验的目标；未完成事务时拒绝再次准备。
    // 返回：候选可提交时为 true，失败保留活动注册并更新 LastError。
    bool Prepare(HotkeyChord chord) noexcept;
    // 无异常发布已准备注册或撤销候选，并尝试清理失去活动身份的注册。
    // 入参：commit 表示配置已保存；true 时先发布新活动状态再清理旧注册。
    // 返回：全部待清理注册释放成功为 true；false 不撤销已提交的新状态。
    bool Finish(bool commit) noexcept;
    // 查询当前唯一允许分派的注册组合。
    // 入参：无。
    // 返回：活动组合，未注册或已关闭时为空。
    std::optional<HotkeyChord> Active() const noexcept;
    // 校验 WM_HOTKEY 的活动 ID 与完整组合，拒绝候选及退役注册消息。
    // 入参：id、keyData 来自 WM_HOTKEY；时间与暂停边界由宿主另行校验。
    // 返回：匹配当前活动身份时为 true。
    bool Matches(WPARAM id, LPARAM keyData) const noexcept;
    // 重试释放不可分派的退役注册，不改变当前活动或候选身份。
    // 入参：无。
    // 返回：没有清理欠账时为 true，失败保留所有权供下次重试。
    bool RetryCleanup() noexcept;
    // 禁止后续注册并取消全部分派，统一回收活动、候选和退役注册。
    // 入参：无；允许重复调用以重试清理。
    // 返回：全部释放成功为 true，否则保留所有权并返回 false。
    bool Shutdown() noexcept;
    // 读取最近一次操作的 Win32 错误，成功操作返回 ERROR_SUCCESS。
    // 入参：无。
    // 返回：错误码。
    DWORD LastError() const noexcept;

  private:
    friend class HotkeyManagerTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
