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
    ~RegistryKey()
    {
        if (this->value != nullptr)
            RegCloseKey(this->value);
    }
};
// 比较 Windows 路径文本，忽略大小写。
bool EqualPath(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
} // namespace
// 保存注入参数，不在构造期间写注册表。
StartupRegistration::StartupRegistration(std::wstring executablePath, std::wstring registryKey, std::wstring valueName)
    : executablePath_(std::move(executablePath)), registryKey_(std::move(registryKey)), valueName_(std::move(valueName))
{
}
// 严格识别带引号绝对路径和固定开关，其他值均保留为冲突。
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
// 不覆盖外部冲突；成功写入后再次读取，避免把系统错误报告为已生效。
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
