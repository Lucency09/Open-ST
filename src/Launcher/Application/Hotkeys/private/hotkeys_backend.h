// 声明全局快捷键系统边界，供产品 Win32 后端及镜像测试替身实现。
#pragma once

#include <windows.h>

namespace open_st
{
class HotkeyBackend
{
  public:
    // 释放后端接口，注册所有权仍属于管理器。
    // 入参：无。
    // 返回：无。
    virtual ~HotkeyBackend() = default;
    // 尝试注册指定窗口、ID 和组合。
    // 入参：window、id 指定所有者；modifiers、key 为系统组合；error 输出失败码。
    // 返回：系统注册成功为 true。
    virtual bool Register(HWND window, int id, UINT modifiers, UINT key, DWORD& error) noexcept = 0;
    // 尝试释放指定注册，失败时保留管理器的所有权记录。
    // 入参：window、id 指定注册；error 输出失败码。
    // 返回：成功释放为 true。
    virtual bool Unregister(HWND window, int id, DWORD& error) noexcept = 0;
};
} // namespace open_st
