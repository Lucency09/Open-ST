// 为镜像测试提供后端注入及注册 ID 耗尽边界入口，不进入公开接口集。
#pragma once

#include <hotkeys.h>
#include <hotkeys_backend.h>

namespace open_st
{
class HotkeyManagerTestAccess
{
  public:
    // 创建借用测试替身的管理器，不触碰真实系统注册。
    // 入参：window 为模拟窗口；backend 的生命周期必须覆盖管理器。
    // 返回：唯一管理器对象。
    static std::unique_ptr<HotkeyManager> Create(HWND window, HotkeyBackend& backend);
    // 设置下一次分配的 ID，用于可重复覆盖耗尽边界。
    // 入参：manager 为隔离管理器；nextId 为下一次尝试分配的 ID。
    // 返回：无。
    static void SetNextId(HotkeyManager& manager, int nextId) noexcept;
};
} // namespace open_st
