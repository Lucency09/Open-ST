// 实现当前用户启动项的严格识别、冲突保护和写入后核验。

#include <filesystem>
#include <startup_registration.h>
#include <string_view>
#include <utility>
#include <vector>

namespace open_st
{
namespace
{
struct RegistryKey
{
    HKEY value{};
    // 关闭本次查询或写入拥有的注册表句柄。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~RegistryKey()
    {
        if (this->value != nullptr)
            RegCloseKey(this->value);
    }
};
// 比较 Windows 路径文本，忽略大小写。
// 入参：left、right 为待比较的 Windows 路径文本。
// 返回：忽略大小写后两条路径文本相等时为 true，否则为 false。
bool EqualPath(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
} // namespace
// 创建当前用户单值启动项访问对象，保存程序路径与注册表位置。
// 入参：executablePath 为程序绝对路径；registryKey 为当前用户下的启动项键路径；valueName 为只允许操作的单个值名。
// 返回：无返回值。
StartupRegistration::StartupRegistration(std::wstring executablePath, std::wstring registryKey, std::wstring valueName)
    : executablePath_(std::move(executablePath)), registryKey_(std::move(registryKey)), valueName_(std::move(valueName))
{
}
// 识别当前用户启动项的归属，区分当前路径、旧路径、冲突和系统失败。
// 入参：无显式入参。
// 返回：启动项缺失、属于当前路径、属于旧路径、外部冲突或系统失败的状态及错误码。
StartupStatus StartupRegistration::Query() const
{
    RegistryKey key;
    LSTATUS error = RegOpenKeyExW(HKEY_CURRENT_USER, this->registryKey_.c_str(), 0, KEY_QUERY_VALUE, &key.value);
    if (error == ERROR_FILE_NOT_FOUND)
        return {StartupState::Missing, 0};
    if (error != ERROR_SUCCESS)
        return {StartupState::Failed, static_cast<DWORD>(error)};
    DWORD size{}, type{};
    error = RegQueryValueExW(key.value, this->valueName_.c_str(), nullptr, &type, nullptr, &size);
    if (error == ERROR_FILE_NOT_FOUND)
        return {StartupState::Missing, 0};
    if (error != ERROR_SUCCESS)
        return {StartupState::Failed, static_cast<DWORD>(error)};
    if (type != REG_SZ || size < sizeof(wchar_t) || size > 65536 || size % sizeof(wchar_t) != 0)
        return {StartupState::Conflict, ERROR_INVALID_DATA};
    std::vector<wchar_t> buffer(size / sizeof(wchar_t));
    error = RegQueryValueExW(key.value, this->valueName_.c_str(), nullptr, &type,
                             reinterpret_cast<BYTE*>(buffer.data()), &size);
    if (error != ERROR_SUCCESS)
        return {StartupState::Failed, static_cast<DWORD>(error)};
    if (type != REG_SZ || size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0 ||
        buffer[size / sizeof(wchar_t) - 1] != L'\0')
        return {StartupState::Conflict, ERROR_INVALID_DATA};
    const std::wstring command(buffer.data(), size / sizeof(wchar_t) - 1);
    constexpr std::wstring_view suffix = L"\" --startup";
    if (command.empty() || command.front() != L'"' || !command.ends_with(suffix))
        return {StartupState::Conflict, ERROR_INVALID_DATA};
    const std::wstring path = command.substr(1, command.size() - suffix.size() - 1);
    if (path.find_first_of(L"\"\r\n") != std::wstring::npos || path.find(L'\0') != std::wstring::npos ||
        !std::filesystem::path(path).is_absolute())
        return {StartupState::Conflict, ERROR_INVALID_DATA};
    if (EqualPath(path, this->executablePath_))
        return {StartupState::CurrentPath, 0};
    if (EqualPath(std::filesystem::path(path).filename().wstring(),
                  std::filesystem::path(this->executablePath_).filename().wstring()))
        return {StartupState::OtherPath, 0};
    return {StartupState::Conflict, ERROR_INVALID_DATA};
}
// 按启停意图更新本程序启动项，在授权修复旧路径后写入并读取核验。
// 入参：enabled 为期望启停状态；allowPathRepair 为是否允许将已识别的旧程序路径修复为当前路径。
// 返回：操作后的核验状态；拒绝修复或外部冲突时保留原状态，系统操作失败附带错误码。
StartupStatus StartupRegistration::Apply(bool enabled, bool allowPathRepair) const
{
    const std::wstring command = L"\"" + this->executablePath_ + L"\" --startup";
    if (enabled && (command.size() > 260 || !std::filesystem::path(this->executablePath_).is_absolute() ||
                    this->executablePath_.find_first_of(L"\"\r\n") != std::wstring::npos ||
                    this->executablePath_.find(L'\0') != std::wstring::npos))
        return {StartupState::Failed, ERROR_INVALID_PARAMETER};
    const StartupStatus before = this->Query();
    if (before.state == StartupState::Failed || before.state == StartupState::Conflict ||
        (enabled && before.state == StartupState::OtherPath && !allowPathRepair))
        return before;
    if (!enabled && before.state == StartupState::Missing)
        return before;
    RegistryKey key;
    LSTATUS error = RegCreateKeyExW(HKEY_CURRENT_USER, this->registryKey_.c_str(), 0, nullptr, 0,
                                    KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key.value, nullptr);
    if (error != ERROR_SUCCESS)
        return {StartupState::Failed, static_cast<DWORD>(error)};
    if (enabled)
        error = RegSetValueExW(key.value, this->valueName_.c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    else
        error = RegDeleteValueW(key.value, this->valueName_.c_str());
    if (error != ERROR_SUCCESS && !(error == ERROR_FILE_NOT_FOUND && !enabled))
        return {StartupState::Failed, static_cast<DWORD>(error)};
    const StartupStatus after = this->Query();
    if (after.state == (enabled ? StartupState::CurrentPath : StartupState::Missing))
        return after;
    return {StartupState::Failed, after.error != 0 ? after.error : ERROR_WRITE_FAULT};
}
} // namespace open_st
