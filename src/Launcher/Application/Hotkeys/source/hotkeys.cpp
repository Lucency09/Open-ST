// 实现快捷键组合规则、固定会话映射及保留旧注册的候选事务。

#include "hotkeys_backend.h"
#include "hotkeys_test_access.h"
#include <atomic>
#include <hotkeys.h>
#include <vector>

namespace open_st
{
namespace
{
std::atomic<int> nextRegistrationId{1};

// 分配进程内永不复用的注册身份，到上限后保持耗尽状态。
// 入参：无。
// 返回：有效 ID；耗尽为零。
int AllocateRegistrationId() noexcept
{
    int candidate = nextRegistrationId.load(std::memory_order_relaxed);
    while (candidate <= 0xBFFF)
    {
        if (nextRegistrationId.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
            return candidate;
    }
    return 0;
}

class Win32HotkeyBackend final : public HotkeyBackend
{
  public:
    // 注册系统组合并返回原始错误，不持有业务配置。
    // 入参：window、id 为所有者；modifiers、key 为组合；error 输出失败码。
    // 返回：系统调用成功为 true。
    bool Register(HWND window, int id, UINT modifiers, UINT key, DWORD& error) noexcept override
    {
        const bool success = RegisterHotKey(window, id, modifiers, key) != FALSE;
        error = success ? ERROR_SUCCESS : GetLastError();
        return success;
    }
    // 注销系统组合，失败由管理器保留所有权。
    // 入参：window、id 为注册身份；error 输出错误。
    // 返回：释放成功为 true。
    bool Unregister(HWND window, int id, DWORD& error) noexcept override
    {
        const bool success = UnregisterHotKey(window, id) != FALSE;
        error = success ? ERROR_SUCCESS : GetLastError();
        return success;
    }
};
} // namespace

// 限制主键和修饰键范围，排除系统保留及截图命令冲突。
// 入参：chord 为不含注册选项的组合。
// 返回：本阶段支持该组合时为 true。
bool IsSupportedHotkey(HotkeyChord chord) noexcept
{
    if ((chord.modifiers & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) != 0)
        return false;
    if (chord.modifiers == MOD_CONTROL && (chord.key == 'C' || chord.key == 'S'))
        return false;
    if (chord.key >= VK_F1 && chord.key <= VK_F24)
        return chord.key != VK_F12;
    return ((chord.key >= 'A' && chord.key <= 'Z') || (chord.key >= '0' && chord.key <= '9')) &&
           (chord.modifiers & (MOD_CONTROL | MOD_ALT)) != 0;
}

// 解析固定 ASCII token，失败不改变调用方输出。
// 入参：text 为配置字符串；chord 接收成功结果。
// 返回：语法与业务支持范围均通过时为 true。
bool ParseHotkey(std::string_view text, HotkeyChord& chord) noexcept
{
    HotkeyChord candidate;
    while (!text.empty())
    {
        const std::size_t separator = text.find('+');
        const std::string_view token = text.substr(0, separator);
        UINT modifier = 0;
        if (token == "Ctrl")
            modifier = MOD_CONTROL;
        else if (token == "Alt")
            modifier = MOD_ALT;
        else if (token == "Shift")
            modifier = MOD_SHIFT;
        if (modifier != 0)
        {
            if (candidate.key != 0 || (candidate.modifiers & modifier) != 0)
                return false;
            candidate.modifiers |= modifier;
        }
        else
        {
            if (candidate.key != 0 || separator != std::string_view::npos)
                return false;
            if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9')))
                candidate.key = static_cast<UINT>(token[0]);
            else if (token.size() >= 2 && token.size() <= 3 && token[0] == 'F' && token[1] != '0')
            {
                UINT number = 0;
                for (std::size_t index = 1; index < token.size(); ++index)
                {
                    if (token[index] < '0' || token[index] > '9')
                        return false;
                    number = number * 10 + static_cast<UINT>(token[index] - '0');
                }
                if (number == 0 || number > 24)
                    return false;
                candidate.key = VK_F1 + number - 1;
            }
            else
                return false;
        }
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
        if (text.empty())
            return false;
    }
    if (!IsSupportedHotkey(candidate))
        return false;
    chord = candidate;
    return true;
}

// 生成可持久化的规范顺序，显示文字可由宿主另行格式化。
// 入参：chord 为业务组合。
// 返回：规范 token；非法组合为空串。
std::string SerializeHotkey(HotkeyChord chord)
{
    if (!IsSupportedHotkey(chord))
        return {};
    std::string value;
    if ((chord.modifiers & MOD_CONTROL) != 0)
        value += "Ctrl+";
    if ((chord.modifiers & MOD_ALT) != 0)
        value += "Alt+";
    if ((chord.modifiers & MOD_SHIFT) != 0)
        value += "Shift+";
    if (chord.key >= VK_F1 && chord.key <= VK_F24)
        value += "F" + std::to_string(chord.key - VK_F1 + 1);
    else
        value += static_cast<char>(chord.key);
    return value;
}

// 保留旧截图按键行为，取消不受修饰或重复限制。
// 入参：key、keyData 为 WM_KEYDOWN；modifiers 为当前修饰状态。
// 返回：对应局部命令，未匹配返回 None。
SessionKeyCommand MatchSessionKey(WPARAM key, LPARAM keyData, UINT modifiers) noexcept
{
    if (key == VK_ESCAPE)
        return SessionKeyCommand::Cancel;
    if ((keyData & (static_cast<LPARAM>(1) << 30)) != 0 || (modifiers & (MOD_ALT | MOD_SHIFT)) != 0)
        return SessionKeyCommand::None;
    const bool control = (modifiers & MOD_CONTROL) != 0;
    if ((key == VK_RETURN && !control) || (key == 'C' && control))
        return SessionKeyCommand::Copy;
    return key == 'S' && control ? SessionKeyCommand::Save : SessionKeyCommand::None;
}

struct HotkeyManager::Impl
{
    struct Registration
    {
        int id{};
        HotkeyChord chord;
    };
    HWND window{};
    Win32HotkeyBackend native;
    HotkeyBackend* backend = &this->native;
    std::vector<Registration> registrations;
    int nextId = 1;
    int active{};
    int candidate{};
    bool prepared{};
    bool closed{};
    DWORD error{};

    // 回收全部非活动且非候选注册，失败记录保留至重试。
    // 入参：无。
    // 返回：没有待清理失败为 true；活动与候选永不被此函数释放。
    bool Cleanup() noexcept
    {
        this->error = ERROR_SUCCESS;
        for (std::size_t index = 0; index < this->registrations.size();)
        {
            const int id = this->registrations[index].id;
            if (id == this->active || (this->prepared && id == this->candidate))
            {
                ++index;
                continue;
            }
            DWORD failure = ERROR_SUCCESS;
            if (this->backend->Unregister(this->window, id, failure))
                this->registrations.erase(this->registrations.begin() + static_cast<std::ptrdiff_t>(index));
            else
            {
                if (this->error == ERROR_SUCCESS)
                    this->error = failure == ERROR_SUCCESS ? ERROR_GEN_FAILURE : failure;
                ++index;
            }
        }
        return this->error == ERROR_SUCCESS;
    }
};

// 构造空管理器，分配不发生于保存后的切换路径。
// 入参：window 为借用消息窗口。
// 返回：无；分配失败抛出异常。
HotkeyManager::HotkeyManager(HWND window) : impl_(std::make_unique<Impl>())
{
    this->impl_->window = window;
}

// 统一释放全部系统注册；失败时系统在线程退出时最终回收。
// 入参：无。
// 返回：无。
HotkeyManager::~HotkeyManager()
{
    (void)this->Shutdown();
}

// 准备候选并提前完成存储分配，旧活动组合始终保留。
// 入参：chord 为目标组合。
// 返回：准备成功为 true；失败保留旧状态和原错误码。
bool HotkeyManager::Prepare(HotkeyChord chord) noexcept
{
    Impl& state = *this->impl_;
    if (state.closed || !IsSupportedHotkey(chord))
    {
        state.error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (state.prepared)
    {
        state.error = ERROR_BUSY;
        return false;
    }
    for (const Impl::Registration& registration : state.registrations)
    {
        if (registration.chord == chord)
        {
            state.candidate = registration.id;
            state.prepared = true;
            state.error = ERROR_SUCCESS;
            return true;
        }
    }
    if (state.backend != &state.native && state.nextId > 0xBFFF)
    {
        state.error = ERROR_NO_SYSTEM_RESOURCES;
        return false;
    }
    try
    {
        state.registrations.reserve(state.registrations.size() + 1);
    }
    catch (...)
    {
        state.error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    const int id = state.backend == &state.native ? AllocateRegistrationId() : state.nextId++;
    if (id == 0)
    {
        state.error = ERROR_NO_SYSTEM_RESOURCES;
        return false;
    }
    if (!state.backend->Register(state.window, id, chord.modifiers | MOD_NOREPEAT, chord.key, state.error))
        return false;
    state.registrations.push_back({id, chord});
    state.candidate = id;
    state.prepared = true;
    state.error = ERROR_SUCCESS;
    return true;
}

// 无分配、无消息泵地发布候选，再回收退役注册。
// 入参：commit 为是否已保存配置。
// 返回：回收成功为 true，失败不撤销已激活的新身份。
bool HotkeyManager::Finish(bool commit) noexcept
{
    if (this->impl_->prepared && commit)
        this->impl_->active = this->impl_->candidate;
    this->impl_->prepared = false;
    this->impl_->candidate = 0;
    return this->impl_->Cleanup();
}

// 返回活动组合的值副本。
// 入参：无。
// 返回：当前组合，尚未注册或已关闭为空。
std::optional<HotkeyChord> HotkeyManager::Active() const noexcept
{
    for (const Impl::Registration& registration : this->impl_->registrations)
        if (registration.id == this->impl_->active)
            return registration.chord;
    return std::nullopt;
}

// 只匹配活动身份及完整键值，候选和退役注册均不分派。
// 入参：id、keyData 为 WM_HOTKEY 参数。
// 返回：身份和组合一致为 true。
bool HotkeyManager::Matches(WPARAM id, LPARAM keyData) const noexcept
{
    const std::optional<HotkeyChord> active = this->Active();
    return active && id == static_cast<WPARAM>(this->impl_->active) && LOWORD(keyData) == active->modifiers &&
           HIWORD(keyData) == active->key;
}

// 重试释放待清理注册，保留活动与候选身份。
// 入参：无。
// 返回：没有清理欠账为 true。
bool HotkeyManager::RetryCleanup() noexcept
{
    return this->impl_->Cleanup();
}

// 停止全部分派并释放系统资源，可在失败后反复调用。
// 入参：无。
// 返回：全部注册已释放为 true。
bool HotkeyManager::Shutdown() noexcept
{
    this->impl_->closed = true;
    this->impl_->active = 0;
    this->impl_->candidate = 0;
    this->impl_->prepared = false;
    return this->impl_->Cleanup();
}

// 提供最近一次系统或参数错误。
// 入参：无。
// 返回：原始 Win32 错误码，成功为零。
DWORD HotkeyManager::LastError() const noexcept
{
    return this->impl_->error;
}

// 先创建无注册的管理器，再注入借用替身，避免裸 owning new。
// 入参：window 为模拟窗口；backend 必须活到管理器析构后。
// 返回：独占管理器，不进行真实系统注册。
std::unique_ptr<HotkeyManager> HotkeyManagerTestAccess::Create(HWND window, HotkeyBackend& backend)
{
    std::unique_ptr<HotkeyManager> manager = std::make_unique<HotkeyManager>(window);
    manager->impl_->backend = &backend;
    return manager;
}

// 注入 ID 上限边界供确定性测试。
// 入参：manager 为隔离实例；nextId 为下一次 ID。
// 返回：无。
void HotkeyManagerTestAccess::SetNextId(HotkeyManager& manager, int nextId) noexcept
{
    manager.impl_->nextId = nextId;
}
} // namespace open_st
