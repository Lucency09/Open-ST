#include <gtest/gtest.h>
#include <single_instance.h>
#include <startup_registration.h>
#include <thread>

namespace open_st
{
namespace
{
// 为每个用例生成不与产品系统入口重叠的名称。
std::wstring UniqueName()
{
    static unsigned sequence{};
    return L"Open-ST-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
}
class StartupRegistrationTest : public testing::Test
{
  protected:
    std::wstring key_ = L"Software\\Open-ST-Tests\\" + UniqueName();
    // 仅清理本用例创建的专属测试键，不访问真实 Run 项。
    void TearDown() override
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, this->key_.c_str());
    }
};
// 验证启用、查询、重复禁用及带空格路径的引号命令契约。
TEST_F(StartupRegistrationTest, enable_query_disable)
{
    StartupRegistration registration(L"C:\\Open ST\\Open-ST.exe", this->key_);
    EXPECT_EQ(registration.Query().state, StartupState::Missing);
    ASSERT_EQ(registration.Apply(true).state, StartupState::CurrentPath);
    EXPECT_EQ(registration.Query().state, StartupState::CurrentPath);
    EXPECT_EQ(registration.Apply(false).state, StartupState::Missing);
    EXPECT_EQ(registration.Apply(false).state, StartupState::Missing);
}
// 验证旧路径必须获得明确修复授权后才会被新路径覆盖。
TEST_F(StartupRegistrationTest, moved_path_requires_confirmation)
{
    StartupRegistration oldPath(L"C:\\Old\\Open-ST.exe", this->key_);
    StartupRegistration newPath(L"C:\\New\\Open-ST.exe", this->key_);
    ASSERT_EQ(oldPath.Apply(true).state, StartupState::CurrentPath);
    EXPECT_EQ(newPath.Query().state, StartupState::OtherPath);
    EXPECT_EQ(newPath.Apply(true).state, StartupState::OtherPath);
    EXPECT_EQ(oldPath.Query().state, StartupState::CurrentPath);
    EXPECT_EQ(newPath.Apply(true, true).state, StartupState::CurrentPath);
}
// 验证外部不属于本程序的值不会被启用、禁用或修复覆盖。
TEST_F(StartupRegistrationTest, unrelated_value_is_preserved)
{
    StartupRegistration other(L"C:\\Other.exe", this->key_);
    StartupRegistration registration(L"C:\\Open-ST.exe", this->key_);
    ASSERT_EQ(other.Apply(true).state, StartupState::CurrentPath);
    EXPECT_EQ(registration.Apply(true, true).state, StartupState::Conflict);
    EXPECT_EQ(registration.Apply(false).state, StartupState::Conflict);
    EXPECT_EQ(other.Query().state, StartupState::CurrentPath);
}
// 验证超长、相对或包含引号的启动路径不会创建入口。
TEST_F(StartupRegistrationTest, invalid_command_is_rejected)
{
    EXPECT_EQ(StartupRegistration(L"Open-ST.exe", this->key_).Apply(true).state, StartupState::Failed);
    EXPECT_EQ(StartupRegistration(L"C:\\bad\"name.exe", this->key_).Apply(true).state, StartupState::Failed);
    EXPECT_EQ(StartupRegistration(L"C:\\" + std::wstring(260, L'x') + L".exe", this->key_).Apply(true).state,
              StartupState::Failed);
    EXPECT_EQ(StartupRegistration(L"C:\\Open-ST.exe", this->key_).Query().state, StartupState::Missing);
}
// 验证 Run 命令允许恰好 260 字符，超过一个字符时不截断或覆盖旧值。
TEST_F(StartupRegistrationTest, run_command_length_boundary)
{
    StartupRegistration allowed(L"C:\\" + std::wstring(241, L'x') + L".exe", this->key_);
    ASSERT_EQ(allowed.Apply(true).state, StartupState::CurrentPath);
    StartupRegistration tooLong(L"C:\\" + std::wstring(242, L'x') + L".exe", this->key_);
    EXPECT_EQ(tooLong.Apply(true, true).state, StartupState::Failed);
    EXPECT_EQ(allowed.Query().state, StartupState::CurrentPath);
}
// 验证外部写入的非字符串值会被报告为冲突而不会被删除。
TEST_F(StartupRegistrationTest, wrong_registry_type_is_preserved)
{
    HKEY key{};
    ASSERT_EQ(
        RegCreateKeyExW(HKEY_CURRENT_USER, this->key_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr),
        ERROR_SUCCESS);
    const DWORD value = 42;
    const LSTATUS result =
        RegSetValueExW(key, L"Open-ST", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    ASSERT_EQ(result, ERROR_SUCCESS);
    StartupRegistration registration(L"C:\\Open-ST.exe", this->key_);
    EXPECT_EQ(registration.Query().state, StartupState::Conflict);
    EXPECT_EQ(registration.Apply(false).state, StartupState::Conflict);
}
// 验证未知和互斥参数不改变上次解析结果。
TEST(LaunchCommandTest, fixed_commands_only)
{
    LaunchCommand command = LaunchCommand::Normal;
    EXPECT_TRUE(ParseLaunchCommand({}, command));
    EXPECT_TRUE(ParseLaunchCommand({L"--capture"}, command));
    EXPECT_EQ(command, LaunchCommand::Capture);
    EXPECT_FALSE(ParseLaunchCommand({L"--unknown"}, command));
    EXPECT_EQ(command, LaunchCommand::Capture);
    EXPECT_FALSE(ParseLaunchCommand({L"--capture", L"--startup"}, command));
    EXPECT_TRUE(ParseLaunchCommand({L"--startup"}, command));
    EXPECT_EQ(command, LaunchCommand::Startup);
}
// 验证同用户同名只产生一个主实例，尚未开始监听时会有限超时。
TEST(SingleInstanceTest, duplicate_does_not_become_primary)
{
    const std::wstring name = UniqueName();
    SingleInstance primary(name), secondary(name);
    ASSERT_EQ(primary.Acquire().result, InstanceResult::Primary);
    ASSERT_EQ(secondary.Acquire().result, InstanceResult::Forwarded);
    const ULONGLONG before = GetTickCount64();
    EXPECT_EQ(secondary.Forward(LaunchCommand::Normal, 50).result, InstanceResult::Failed);
    EXPECT_LT(GetTickCount64() - before, 1000u);
}
// 验证固定命令经管道传输后只异步投递到目标窗口，停止不需要客户端连接。
TEST(SingleInstanceTest, pipe_forwards_command_and_stops)
{
    const std::wstring name = UniqueName();
    SingleInstance primary(name), secondary(name);
    ASSERT_EQ(primary.Acquire().result, InstanceResult::Primary);
    ASSERT_EQ(secondary.Acquire().result, InstanceResult::Forwarded);
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    constexpr UINT messageId = WM_APP + 43;
    EXPECT_TRUE(primary.StartListening(window, messageId));
    EXPECT_EQ(secondary.Forward(LaunchCommand::Capture).result, InstanceResult::Forwarded);
    MSG message{};
    EXPECT_TRUE(PeekMessageW(&message, window, messageId, messageId, PM_REMOVE));
    EXPECT_EQ(message.wParam, static_cast<WPARAM>(LaunchCommand::Capture));
    primary.Stop();
    DestroyWindow(window);
}
// 验证暂停门禁在接收线程拒绝截图且不投递消息，普通启动仍可到达。
TEST(SingleInstanceTest, capture_gate_rejects_paused_request)
{
    const std::wstring name = UniqueName();
    SingleInstance primary(name), secondary(name);
    ASSERT_EQ(primary.Acquire().result, InstanceResult::Primary);
    ASSERT_EQ(secondary.Acquire().result, InstanceResult::Forwarded);
    primary.SetCaptureGate([]() -> std::optional<LPARAM> { return std::nullopt; });
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    constexpr UINT messageId = WM_APP + 46;
    EXPECT_TRUE(primary.StartListening(window, messageId));
    const InstanceStatus result = secondary.Forward(LaunchCommand::Capture);
    EXPECT_EQ(result.result, InstanceResult::Failed);
    EXPECT_EQ(result.error, ERROR_BUSY);
    MSG message{};
    EXPECT_FALSE(PeekMessageW(&message, window, messageId, messageId, PM_REMOVE));
    EXPECT_EQ(secondary.Forward(LaunchCommand::Normal).result, InstanceResult::Forwarded);
    EXPECT_TRUE(PeekMessageW(&message, window, messageId, messageId, PM_REMOVE));
    EXPECT_EQ(message.lParam, 0);
    primary.Stop();
    DestroyWindow(window);
}
// 验证允许截图携带接收时的代次，供 UI 丢弃已过期请求。
TEST(SingleInstanceTest, capture_gate_passes_generation_token)
{
    const std::wstring name = UniqueName();
    SingleInstance primary(name), secondary(name);
    ASSERT_EQ(primary.Acquire().result, InstanceResult::Primary);
    ASSERT_EQ(secondary.Acquire().result, InstanceResult::Forwarded);
    primary.SetCaptureGate([]() -> std::optional<LPARAM> { return 42; });
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    constexpr UINT messageId = WM_APP + 47;
    EXPECT_TRUE(primary.StartListening(window, messageId));
    EXPECT_EQ(secondary.Forward(LaunchCommand::Capture).result, InstanceResult::Forwarded);
    MSG message{};
    EXPECT_TRUE(PeekMessageW(&message, window, messageId, messageId, PM_REMOVE));
    EXPECT_EQ(message.wParam, static_cast<WPARAM>(LaunchCommand::Capture));
    EXPECT_EQ(message.lParam, 42);
    primary.Stop();
    DestroyWindow(window);
}
// 验证管道延迟创建时次实例重试可以成功，而不会错误启动第二个实例。
TEST(SingleInstanceTest, startup_readiness_retry)
{
    const std::wstring name = UniqueName();
    SingleInstance primary(name), secondary(name);
    ASSERT_EQ(primary.Acquire().result, InstanceResult::Primary);
    ASSERT_EQ(secondary.Acquire().result, InstanceResult::Forwarded);
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    InstanceStatus result;
    std::thread client([&]() { result = secondary.Forward(LaunchCommand::Normal, 2000); });
    Sleep(40);
    EXPECT_TRUE(primary.StartListening(window, WM_APP + 44));
    client.join();
    EXPECT_EQ(result.result, InstanceResult::Forwarded);
    primary.Stop();
    DestroyWindow(window);
}
// 仅由父用例通过环境变量激活子进程接收端；普通测试枚举必须跳过。
TEST(SingleInstanceProcessTest, child_receiver)
{
    wchar_t name[256]{};
    if (GetEnvironmentVariableW(L"OPEN_ST_TEST_INSTANCE_NAME", name, 256) == 0)
        GTEST_SKIP() << "Only activated by the cross-process parent test.";
    SingleInstance instance(name);
    ASSERT_EQ(instance.Acquire().result, InstanceResult::Primary);
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    ASSERT_TRUE(instance.StartListening(window, WM_APP + 45));
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(L"Local\\") + name + L".ready").c_str());
    ASSERT_NE(ready, nullptr);
    SetEvent(ready);
    CloseHandle(ready);
    MSG message{};
    bool received = false;
    const ULONGLONG deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline)
    {
        if (PeekMessageW(&message, window, WM_APP + 45, WM_APP + 45, PM_REMOVE))
        {
            received = true;
            break;
        }
        Sleep(10);
    }
    EXPECT_TRUE(received);
    EXPECT_EQ(message.wParam, static_cast<WPARAM>(LaunchCommand::Capture));
    // 等待父进程完成协议，避免依赖固定休眠来保证管道回复已被读取。
    HANDLE done = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(L"Local\\") + name + L".done").c_str());
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(WaitForSingleObject(done, 3000), WAIT_OBJECT_0);
    CloseHandle(done);
    instance.Stop();
    DestroyWindow(window);
}
// 验证独立进程共享用户互斥体并能通过受限管道转发命令。
TEST(SingleInstanceProcessTest, forwards_between_processes)
{
    const std::wstring name = UniqueName();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\" + name + L".ready").c_str());
    ASSERT_NE(ready, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\" + name + L".done").c_str());
    ASSERT_NE(done, nullptr);
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" --gtest_filter=SingleInstanceProcessTest.child_receiver";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    SetEnvironmentVariableW(L"OPEN_ST_TEST_INSTANCE_NAME", name.c_str());
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(L"OPEN_ST_TEST_INSTANCE_NAME", nullptr);
    if (!created)
    {
        CloseHandle(ready);
        CloseHandle(done);
        FAIL() << "CreateProcess failed: " << GetLastError();
    }
    const DWORD wait = WaitForSingleObject(ready, 3000);
    EXPECT_EQ(wait, WAIT_OBJECT_0);
    if (wait == WAIT_OBJECT_0)
    {
        SingleInstance client(name);
        EXPECT_EQ(client.Acquire().result, InstanceResult::Forwarded);
        EXPECT_EQ(client.Forward(LaunchCommand::Capture).result, InstanceResult::Forwarded);
    }
    SetEvent(done);
    EXPECT_EQ(WaitForSingleObject(process.hProcess, 6000), WAIT_OBJECT_0);
    DWORD exitCode{};
    EXPECT_TRUE(GetExitCodeProcess(process.hProcess, &exitCode));
    EXPECT_EQ(exitCode, 0u);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(ready);
    CloseHandle(done);
}
} // namespace
} // namespace open_st
