// 验证组合规则、会话命令以及注册事务，不占用真实全局快捷键。
#include <gtest/gtest.h>
#include <hotkeys_test_access.h>

#include <array>
#include <string_view>

namespace open_st
{
namespace
{
constexpr HotkeyChord FIRST{MOD_CONTROL | MOD_ALT, 'Q'};
constexpr HotkeyChord SECOND{MOD_CONTROL | MOD_SHIFT, VK_F8};

class FakeBackend final : public HotkeyBackend
{
  public:
    std::array<HotkeyChord, 0xC000> held{};
    int registerCalls{};
    int unregisterCalls{};
    int lastId{};
    UINT lastModifiers{};
    bool failRegister{};
    bool failUnregister{};

    // 模拟注册及组合冲突，不访问当前桌面。
    // 入参：未使用窗口；id 和组合表示候选；error 返回可预测故障。
    // 返回：合法且无冲突的注册成功时为 true。
    bool Register(HWND, int id, UINT modifiers, UINT key, DWORD& error) noexcept override
    {
        ++this->registerCalls;
        this->lastId = id;
        this->lastModifiers = modifiers;
        const HotkeyChord chord{modifiers & ~static_cast<UINT>(MOD_NOREPEAT), key};
        if (this->failRegister)
        {
            error = ERROR_HOTKEY_ALREADY_REGISTERED;
            return false;
        }
        for (const HotkeyChord& entry : this->held)
        {
            if (entry == chord)
            {
                error = ERROR_HOTKEY_ALREADY_REGISTERED;
                return false;
            }
        }
        if (id < 1 || id >= static_cast<int>(this->held.size()))
        {
            error = ERROR_INVALID_PARAMETER;
            return false;
        }
        this->held[static_cast<std::size_t>(id)] = chord;
        error = ERROR_SUCCESS;
        return true;
    }

    // 模拟释放及故障后仍持有的注册。
    // 入参：未使用窗口；id 为目标；error 返回失败原因。
    // 返回：成功清空记录为 true。
    bool Unregister(HWND, int id, DWORD& error) noexcept override
    {
        ++this->unregisterCalls;
        if (this->failUnregister)
        {
            error = ERROR_ACCESS_DENIED;
            return false;
        }
        this->held[static_cast<std::size_t>(id)] = {};
        error = ERROR_SUCCESS;
        return true;
    }
};

// 将组合编码为真实 WM_HOTKEY 消息参数。
// 入参：chord 为规范组合。
// 返回：低字修饰键与高字虚拟键组成的消息数据。
LPARAM MessageData(HotkeyChord chord)
{
    return MAKELPARAM(static_cast<WORD>(chord.modifiers), static_cast<WORD>(chord.key));
}

// 验证乱序修饰键的规范化及数字和功能键边界。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ChordTest, parses_and_normalizes_supported_chords)
{
    HotkeyChord chord{};
    ASSERT_TRUE(ParseHotkey("Shift+Alt+Ctrl+Q", chord));
    EXPECT_EQ(SerializeHotkey(chord), "Ctrl+Alt+Shift+Q");
    for (const std::string_view text : {"Alt+0", "Ctrl+B", "F1", "F11", "F13", "F24", "Shift+F8"})
    {
        EXPECT_TRUE(ParseHotkey(text, chord)) << text;
        EXPECT_FALSE(SerializeHotkey(chord).empty());
    }
}

// 验证未知、重复、多主键和产品排除范围，并确保失败不污染输出。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ChordTest, rejects_invalid_text_without_changing_output)
{
    for (const std::string_view text :
         {"",        "Q",       "Shift+Q", "Ctrl+C",    "Ctrl+S",      "Ctrl+Z",      "Ctrl+Y",   "Ctrl+Shift+Z",
          "F12",     "F0",      "F25",     "Win+Q",     "Ctrl",        "Ctrl+Ctrl+Q", "Ctrl+Q+S", "Ctrl++Q",
          "Ctrl+Q+", " Ctrl+Q", "Ctrl+Q ", "Ctrl+Home", "Alt+OEM_PLUS"})
    {
        HotkeyChord chord = FIRST;
        EXPECT_FALSE(ParseHotkey(text, chord)) << text;
        EXPECT_EQ(chord, FIRST) << text;
    }
}

// 验证数值入口与文本入口共享支持范围，禁止额外系统标记和纯修饰键。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ChordTest, rejects_unsupported_numeric_chords)
{
    EXPECT_FALSE(IsSupportedHotkey({MOD_WIN | MOD_CONTROL, 'Q'}));
    EXPECT_FALSE(IsSupportedHotkey({MOD_NOREPEAT | MOD_CONTROL, 'Q'}));
    EXPECT_FALSE(IsSupportedHotkey({MOD_CONTROL, VK_CONTROL}));
    EXPECT_FALSE(IsSupportedHotkey({MOD_CONTROL, VK_F12}));
    EXPECT_TRUE(IsSupportedHotkey({MOD_CONTROL | MOD_SHIFT, 'C'}));
    EXPECT_TRUE(SerializeHotkey({0, 'Q'}).empty());
}

// 验证复制保存禁用重复和 Alt/Shift，而 Esc 始终保持原分层取消语义。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(SessionKeyTest, preserves_existing_capture_commands)
{
    constexpr LPARAM repeated = static_cast<LPARAM>(1) << 30;
    EXPECT_EQ(MatchSessionKey(VK_RETURN, 0, 0), SessionKeyCommand::Copy);
    EXPECT_EQ(MatchSessionKey('C', 0, MOD_CONTROL), SessionKeyCommand::Copy);
    EXPECT_EQ(MatchSessionKey('S', 0, MOD_CONTROL), SessionKeyCommand::Save);
    EXPECT_EQ(MatchSessionKey(VK_RETURN, 0, MOD_CONTROL), SessionKeyCommand::None);
    EXPECT_EQ(MatchSessionKey('C', repeated, MOD_CONTROL), SessionKeyCommand::None);
    EXPECT_EQ(MatchSessionKey('S', 0, MOD_CONTROL | MOD_SHIFT), SessionKeyCommand::None);
    EXPECT_EQ(MatchSessionKey(VK_RETURN, 0, MOD_ALT), SessionKeyCommand::None);
    EXPECT_EQ(MatchSessionKey(VK_ESCAPE, repeated, MOD_CONTROL | MOD_ALT | MOD_SHIFT), SessionKeyCommand::Cancel);
}

// 验证标注撤销与两种重做组合，只消费首次按下并拒绝额外 Alt。
// 入参：无。返回：无，以断言报告映射及全局冲突保护。
TEST(SessionKeyTest, maps_annotation_history_without_global_shortcut_conflicts)
{
    constexpr LPARAM repeated = static_cast<LPARAM>(1) << 30;
    EXPECT_EQ(MatchSessionKey('Z', 0, MOD_CONTROL), SessionKeyCommand::Undo);
    EXPECT_EQ(MatchSessionKey('Y', 0, MOD_CONTROL), SessionKeyCommand::Redo);
    EXPECT_EQ(MatchSessionKey('Z', 0, MOD_CONTROL | MOD_SHIFT), SessionKeyCommand::Redo);
    EXPECT_EQ(MatchSessionKey('Z', repeated, MOD_CONTROL), SessionKeyCommand::None);
    EXPECT_EQ(MatchSessionKey('Z', 0, MOD_CONTROL | MOD_ALT), SessionKeyCommand::None);
    EXPECT_FALSE(IsSupportedHotkey({MOD_CONTROL, 'Z'}));
    EXPECT_FALSE(IsSupportedHotkey({MOD_CONTROL, 'Y'}));
    EXPECT_FALSE(IsSupportedHotkey({MOD_CONTROL | MOD_SHIFT, 'Z'}));
}

// 验证候选提交前不分派、提交后严格核对 ID 和完整组合。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, dispatches_only_committed_identity)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    EXPECT_FALSE(manager->Active().has_value());
    ASSERT_TRUE(manager->Prepare(FIRST));
    const int firstId = backend.lastId;
    EXPECT_NE(backend.lastModifiers & MOD_NOREPEAT, 0U);
    EXPECT_FALSE(manager->Matches(firstId, MessageData(FIRST)));
    ASSERT_TRUE(manager->Finish(true));
    EXPECT_EQ(manager->Active(), FIRST);
    EXPECT_TRUE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_FALSE(manager->Matches(firstId + 1, MessageData(FIRST)));
    EXPECT_FALSE(manager->Matches(firstId, MessageData(SECOND)));
    ASSERT_TRUE(manager->Prepare(SECOND));
    const int secondId = backend.lastId;
    EXPECT_GT(secondId, firstId);
    EXPECT_TRUE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_FALSE(manager->Matches(secondId, MessageData(SECOND)));
    ASSERT_TRUE(manager->Finish(true));
    EXPECT_FALSE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_TRUE(manager->Matches(secondId, MessageData(SECOND)));
}

// 验证同组合复用不消耗系统注册，嵌套准备失败不覆盖已准备事务。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, reuses_active_and_rejects_nested_prepare)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    ASSERT_TRUE(manager->Prepare(FIRST));
    EXPECT_FALSE(manager->Prepare(SECOND));
    EXPECT_EQ(manager->LastError(), ERROR_BUSY);
    EXPECT_TRUE(manager->Finish(false));
    EXPECT_EQ(backend.registerCalls, 1);
    EXPECT_EQ(backend.unregisterCalls, 0);
    EXPECT_EQ(manager->Active(), FIRST);
}

// 验证注册冲突和非法输入均不影响旧活动组合。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, failed_prepare_preserves_old_registration)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    backend.failRegister = true;
    EXPECT_FALSE(manager->Prepare(SECOND));
    EXPECT_EQ(manager->LastError(), ERROR_HOTKEY_ALREADY_REGISTERED);
    EXPECT_EQ(manager->Active(), FIRST);
    EXPECT_FALSE(manager->Prepare({0, 'Q'}));
    EXPECT_EQ(manager->Active(), FIRST);
    EXPECT_EQ(backend.unregisterCalls, 0);
}

// 验证保存失败撤销候选，旧 ID 继续可用且撤销 ID 永不复用。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, rollback_preserves_active_and_never_reuses_ids)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    const int firstId = backend.lastId;
    ASSERT_TRUE(manager->Prepare(SECOND));
    const int cancelledId = backend.lastId;
    ASSERT_TRUE(manager->Finish(false));
    EXPECT_TRUE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_FALSE(manager->Matches(cancelledId, MessageData(SECOND)));
    ASSERT_TRUE(manager->Prepare(SECOND));
    EXPECT_GT(backend.lastId, cancelledId);
    EXPECT_TRUE(manager->Finish(false));
}

// 验证提交后旧注册清理失败只保留回收欠账，不回滚新状态或分派旧消息。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, commit_survives_old_unregister_failure)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    const int firstId = backend.lastId;
    ASSERT_TRUE(manager->Prepare(SECOND));
    backend.failUnregister = true;
    EXPECT_FALSE(manager->Finish(true));
    EXPECT_EQ(manager->LastError(), ERROR_ACCESS_DENIED);
    EXPECT_EQ(manager->Active(), SECOND);
    EXPECT_FALSE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_FALSE(manager->RetryCleanup());
    backend.failUnregister = false;
    EXPECT_TRUE(manager->RetryCleanup());
    EXPECT_EQ(manager->LastError(), ERROR_SUCCESS);
    EXPECT_EQ(manager->Active(), SECOND);
}

// 验证撤销失败的组合保持不可分派，再次应用相同组合可复用仍持有的注册。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, reuses_retired_candidate_after_rollback_failure)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    ASSERT_TRUE(manager->Prepare(SECOND));
    const int secondId = backend.lastId;
    backend.failUnregister = true;
    EXPECT_FALSE(manager->Finish(false));
    EXPECT_FALSE(manager->Matches(secondId, MessageData(SECOND)));
    EXPECT_EQ(manager->Active(), FIRST);
    const int callsBefore = backend.registerCalls;
    ASSERT_TRUE(manager->Prepare(SECOND));
    EXPECT_EQ(backend.registerCalls, callsBefore);
    backend.failUnregister = false;
    EXPECT_TRUE(manager->Finish(true));
    EXPECT_TRUE(manager->Matches(secondId, MessageData(SECOND)));
}

// 验证 ID 上限后明确失败并保持旧注册，同组合复用不受耗尽影响。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, reports_id_exhaustion_without_losing_active)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    HotkeyManagerTestAccess::SetNextId(*manager, 0xBFFF);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    EXPECT_EQ(backend.lastId, 0xBFFF);
    EXPECT_FALSE(manager->Prepare(SECOND));
    EXPECT_EQ(manager->Active(), FIRST);
    ASSERT_TRUE(manager->Prepare(FIRST));
    EXPECT_TRUE(manager->Finish(true));
}

// 验证关闭先撤销所有分派，即使系统释放失败也可重试且不得重新注册。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, shutdown_disables_dispatch_and_retries_cleanup)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    ASSERT_TRUE(manager->Finish(true));
    const int firstId = backend.lastId;
    ASSERT_TRUE(manager->Prepare(SECOND));
    backend.failUnregister = true;
    EXPECT_FALSE(manager->Shutdown());
    EXPECT_FALSE(manager->Active().has_value());
    EXPECT_FALSE(manager->Matches(firstId, MessageData(FIRST)));
    EXPECT_FALSE(manager->Prepare(FIRST));
    backend.failUnregister = false;
    EXPECT_TRUE(manager->Shutdown());
    EXPECT_TRUE(manager->Shutdown());
}

// 验证析构统一释放活动与尚未完成的候选注册。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, destructor_reclaims_active_and_candidate)
{
    FakeBackend backend;
    {
        std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
        ASSERT_TRUE(manager->Prepare(FIRST));
        ASSERT_TRUE(manager->Finish(true));
        ASSERT_TRUE(manager->Prepare(SECOND));
    }
    EXPECT_EQ(backend.unregisterCalls, 2);
}

// 验证清理重试只释放退役记录，不释放正在准备但尚未提交的候选。
// 入参：无；使用隔离后端或纯值构造边界。
// 返回：无；通过断言报告实际行为与预期的差异。
TEST(ManagerTest, cleanup_preserves_prepared_candidate)
{
    FakeBackend backend;
    std::unique_ptr<HotkeyManager> manager = HotkeyManagerTestAccess::Create(nullptr, backend);
    ASSERT_TRUE(manager->Prepare(FIRST));
    const int candidateId = backend.lastId;
    EXPECT_TRUE(manager->RetryCleanup());
    EXPECT_EQ(backend.unregisterCalls, 0);
    EXPECT_FALSE(manager->Active().has_value());
    EXPECT_TRUE(manager->Finish(true));
    EXPECT_TRUE(manager->Matches(candidateId, MessageData(FIRST)));
}
} // namespace
} // namespace open_st
