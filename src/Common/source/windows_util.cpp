// 实现共享 Windows 路径查询与 HRESULT 诊断，业务路径仍由调用模块决定。

#include <windows_util.h>

#include <array>
#include <sstream>
#include <string_view>

namespace open_st
{
// 查询当前可执行文件的父目录，不改变调用方的资源和数据定位规则。
// 入参：无。
// 返回：可执行文件父目录；系统查询失败或缓冲区不足时为空，分配异常向调用方传播。
std::filesystem::path GetExecutableDirectory()
{
    std::array<wchar_t, 32768> pathBuffer{};
    const DWORD length = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length >= static_cast<DWORD>(pathBuffer.size()))
    {
        return {};
    }
    return std::filesystem::path(std::wstring_view(pathBuffer.data(), length)).parent_path();
}

// 组合失败操作名称和 HRESULT，供调用方定位底层系统故障。
// 入参：operation：非空指针，指向失败操作的宽字符名称；result：该操作返回的 HRESULT。
// 返回：包含原操作名称及十六进制 HRESULT 的诊断字符串。
std::wstring FormatHResult(const wchar_t* operation, HRESULT result)
{
    std::wostringstream stream;
    stream << operation << L"失败，HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}
} // namespace open_st
