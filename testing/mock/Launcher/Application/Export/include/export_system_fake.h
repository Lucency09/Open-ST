#pragma once

#include "clipboard_api.h"
#include "file_write_api.h"
#include <algorithm>
#include <string>
#include <vector>

namespace open_st::testing
{
// 每个测试线程独立保存边界状态；所有回调均为内存替身，不访问真实剪贴板或文件。
class ExportSystemFake final
{
  public:
    // 在当前线程注册替身，测试按作用域串行使用。
    ExportSystemFake()
    {
        ExportSystemFake::current_ = this;
    }
    // 清除当前线程绑定。
    ~ExportSystemFake()
    {
        ExportSystemFake::current_ = nullptr;
    }
    // 禁止复制活动绑定。
    ExportSystemFake(const ExportSystemFake&) = delete;
    // 禁止赋值活动绑定。
    ExportSystemFake& operator=(const ExportSystemFake&) = delete;

    std::string failure;
    std::vector<std::string> calls;
    std::vector<std::uint8_t> memory;
    std::vector<std::uint8_t> writtenBytes;
    std::vector<DWORD> dispositions;
    bool existing{};
    bool cleanupFails{};
    bool deleteRequested{};
    UINT publishedFormat{};
    DWORD writeLimit{MAXDWORD};

    // 返回完全覆盖的剪贴板函数表，避免默认回调意外访问系统。
    ClipboardApi Clipboard() const
    {
        return {&IsWindowFake, &Allocate, &Lock, &Unlock, &Free, &Open, &Empty, &Set, &CloseClipboardFake};
    }
    // 返回完全覆盖的文件函数表。
    FileWriteApi File() const
    {
        return {&Create, &Write, &Flush, &CloseFile, &SetInformation};
    }
    // 提供不可解引用的窗口身份，仅供替身校验。
    static HWND Owner()
    {
        return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1U));
    }

  private:
    inline static thread_local ExportSystemFake* current_{};
    // 记录调用并在指定阶段注入 Win32 错误。
    static bool Succeed(const char* operation)
    {
        ExportSystemFake::current_->calls.emplace_back(operation);
        if (ExportSystemFake::current_->failure == operation)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        return true;
    }
    // 模拟有效 owner 检查。
    static BOOL WINAPI IsWindowFake(HWND)
    {
        return ExportSystemFake::Succeed("owner") ? TRUE : FALSE;
    }
    // 用向量承载全局内存，返回独立的虚构句柄。
    static HGLOBAL WINAPI Allocate(UINT, SIZE_T size)
    {
        if (!ExportSystemFake::Succeed("allocate"))
        {
            return nullptr;
        }
        ExportSystemFake::current_->memory.resize(size);
        return reinterpret_cast<HGLOBAL>(static_cast<std::uintptr_t>(2U));
    }
    // 提供可写缓冲区或模拟锁定失败。
    static LPVOID WINAPI Lock(HGLOBAL)
    {
        return ExportSystemFake::Succeed("lock") ? ExportSystemFake::current_->memory.data() : nullptr;
    }
    // 模拟最后一次解锁返回 FALSE 且错误码为零的正常 Win32 语义。
    static BOOL WINAPI Unlock(HGLOBAL)
    {
        if (ExportSystemFake::Succeed("unlock"))
        {
            SetLastError(ERROR_SUCCESS);
        }
        return FALSE;
    }
    // 记录本地所有权释放，保留字节供断言。
    static HGLOBAL WINAPI Free(HGLOBAL)
    {
        (void)ExportSystemFake::Succeed("free");
        return nullptr;
    }
    // 模拟有限次数的剪贴板打开操作。
    static BOOL WINAPI Open(HWND)
    {
        return ExportSystemFake::Succeed("open") ? TRUE : FALSE;
    }
    // 模拟清空剪贴板。
    static BOOL WINAPI Empty()
    {
        return ExportSystemFake::Succeed("empty") ? TRUE : FALSE;
    }
    // 成功时记录数据格式并返回传入的所有权句柄。
    static HANDLE WINAPI Set(UINT format, HANDLE memory)
    {
        ExportSystemFake::current_->publishedFormat = format;
        return ExportSystemFake::Succeed("set") ? memory : nullptr;
    }
    // 记录剪贴板会话关闭。
    static BOOL WINAPI CloseClipboardFake()
    {
        return ExportSystemFake::Succeed("clipboardClose") ? TRUE : FALSE;
    }
    // 模拟首次创建冲突及仅覆盖已存在文件的第二次打开。
    static HANDLE WINAPI Create(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
    {
        ExportSystemFake::current_->dispositions.push_back(disposition);
        if (!ExportSystemFake::Succeed("create"))
        {
            return INVALID_HANDLE_VALUE;
        }
        if (ExportSystemFake::current_->existing && disposition == CREATE_NEW)
        {
            SetLastError(ERROR_FILE_EXISTS);
            return INVALID_HANDLE_VALUE;
        }
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(3U));
    }
    // 支持短写、零写及失败注入，保存实际交付字节以核验循环偏移。
    static BOOL WINAPI Write(HANDLE, LPCVOID data, DWORD count, LPDWORD written, LPOVERLAPPED)
    {
        if (!ExportSystemFake::Succeed("write"))
        {
            return FALSE;
        }
        *written = (std::min)(count, ExportSystemFake::current_->writeLimit);
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        ExportSystemFake::current_->writtenBytes.insert(ExportSystemFake::current_->writtenBytes.end(), bytes,
                                                        bytes + *written);
        return TRUE;
    }
    // 模拟最终刷新失败。
    static BOOL WINAPI Flush(HANDLE)
    {
        return ExportSystemFake::Succeed("flush") ? TRUE : FALSE;
    }
    // 记录文件句柄关闭。
    static BOOL WINAPI CloseFile(HANDLE)
    {
        return ExportSystemFake::Succeed("fileClose") ? TRUE : FALSE;
    }
    // 捕获句柄删除标记，模拟清理失败且不删除任何磁盘路径。
    static BOOL WINAPI SetInformation(HANDLE, FILE_INFO_BY_HANDLE_CLASS type, LPVOID information, DWORD)
    {
        (void)ExportSystemFake::Succeed("cleanup");
        ExportSystemFake::current_->deleteRequested =
            type == FileDispositionInfo && static_cast<FILE_DISPOSITION_INFO*>(information)->DeleteFile != FALSE;
        if (ExportSystemFake::current_->cleanupFails)
        {
            SetLastError(ERROR_SHARING_VIOLATION);
            return FALSE;
        }
        return TRUE;
    }
};
} // namespace open_st::testing
