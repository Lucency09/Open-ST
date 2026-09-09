// 提供导出系统边界的线程局部替身，记录调用并模拟剪贴板与文件写入故障。

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
    // 入参：无显式入参。
    // 返回：无返回值。
    ExportSystemFake()
    {
        ExportSystemFake::current_ = this;
    }
    // 清除当前线程绑定。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~ExportSystemFake()
    {
        ExportSystemFake::current_ = nullptr;
    }
    // 禁止复制活动绑定。
    // 入参：未命名 ExportSystemFake 引用为被禁止复制或赋值的当前线程替身绑定来源。
    // 返回：无可调用实现；该操作已删除，尝试调用会导致编译错误。
    ExportSystemFake(const ExportSystemFake&) = delete;
    // 禁止赋值活动绑定。
    // 入参：未命名 ExportSystemFake 引用为被禁止复制或赋值的当前线程替身绑定来源。
    // 返回：无可调用实现；该操作已删除，尝试调用会导致编译错误。
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

    // 构造导出剪贴板测试的完整系统函数替身表。
    // 入参：无显式入参。
    // 返回：全部指向当前线程替身的剪贴板 API 函数表。
    ClipboardApi Clipboard() const
    {
        return {&IsWindowFake, &Allocate, &Lock, &Unlock, &Free, &Open, &Empty, &Set, &CloseClipboardFake};
    }
    // 构造导出文件写入测试的完整系统函数替身表。
    // 入参：无显式入参。
    // 返回：全部指向当前线程替身的文件写入 API 函数表。
    FileWriteApi File() const
    {
        return {&Create, &Write, &Flush, &CloseFile, &SetInformation};
    }
    // 提供导出测试使用的稳定虚构窗口身份。
    // 入参：无显式入参。
    // 返回：数值为 1 的虚构窗口身份；不是实际窗口，不可解引用或交给真实系统 API。
    static HWND Owner()
    {
        return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1U));
    }

  private:
    inline static thread_local ExportSystemFake* current_{};
    // 记录调用并在指定阶段注入 Win32 错误。
    // 入参：operation 为记录到调用轨迹并与 failure 比较的系统操作名称。
    // 返回：operation 与故障注入点不同为 true；相同时为 false 并设置 ERROR_ACCESS_DENIED。
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
    // 入参：未命名 HWND 为调用方传入的虚构宿主身份，本替身不访问实际窗口。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI IsWindowFake(HWND)
    {
        return ExportSystemFake::Succeed("owner") ? TRUE : FALSE;
    }
    // 用向量承载全局内存，返回独立的虚构句柄。
    // 入参：未命名 UINT 为全局内存分配标志，本替身忽略；size 为要分配的缓冲区字节数。
    // 返回：成功时为数值 2 的虚构全局内存句柄；注入分配失败时为 nullptr。
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
    // 入参：未命名 HGLOBAL 为虚构全局内存身份；实际字节保存在当前线程替身的 memory 中。
    // 返回：替身向量的可写存储地址；注入锁定失败时为 nullptr。
    static LPVOID WINAPI Lock(HGLOBAL)
    {
        return ExportSystemFake::Succeed("lock") ? ExportSystemFake::current_->memory.data() : nullptr;
    }
    // 模拟最后一次解锁返回 FALSE 且错误码为零的正常 Win32 语义。
    // 入参：未命名 HGLOBAL 为虚构全局内存身份；实际字节保存在当前线程替身的 memory 中。
    // 返回：总为 FALSE；成功时 LastError 为 ERROR_SUCCESS，注入失败时保留错误码。
    static BOOL WINAPI Unlock(HGLOBAL)
    {
        if (ExportSystemFake::Succeed("unlock"))
        {
            SetLastError(ERROR_SUCCESS);
        }
        return FALSE;
    }
    // 记录本地所有权释放，保留字节供断言。
    // 入参：未命名 HGLOBAL 为虚构全局内存身份；实际字节保存在当前线程替身的 memory 中。
    // 返回：固定为 nullptr，模拟内存释放成功；测试字节缓冲区仍保留。
    static HGLOBAL WINAPI Free(HGLOBAL)
    {
        (void)ExportSystemFake::Succeed("free");
        return nullptr;
    }
    // 模拟有限次数的剪贴板打开操作。
    // 入参：未命名 HWND 为调用方传入的虚构宿主身份，本替身不访问实际窗口。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI Open(HWND)
    {
        return ExportSystemFake::Succeed("open") ? TRUE : FALSE;
    }
    // 模拟清空剪贴板。
    // 入参：无显式入参。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI Empty()
    {
        return ExportSystemFake::Succeed("empty") ? TRUE : FALSE;
    }
    // 模拟剪贴板数据发布并记录请求格式，以便验证格式和所有权交接。
    // 入参：format 为发布的剪贴板格式编号；memory 为待模拟移交所有权的虚构内存句柄。
    // 返回：注入成功时返回传入的 memory；注入失败时为 nullptr。
    static HANDLE WINAPI Set(UINT format, HANDLE memory)
    {
        ExportSystemFake::current_->publishedFormat = format;
        return ExportSystemFake::Succeed("set") ? memory : nullptr;
    }
    // 记录剪贴板会话关闭。
    // 入参：无显式入参。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI CloseClipboardFake()
    {
        return ExportSystemFake::Succeed("clipboardClose") ? TRUE : FALSE;
    }
    // 模拟首次创建冲突及仅覆盖已存在文件的第二次打开。
    // 入参：disposition 为创建方式；其余未命名参数依次为路径、访问权限、共享方式、安全属性、文件属性和模板句柄，本替身不使用这些值。
    // 返回：成功时为数值 3 的虚构文件句柄；注入失败或 CREATE_NEW 冲突时为 INVALID_HANDLE_VALUE。
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
    // 入参：data 为本次写入字节的起始地址；count 为请求字节数；written 输出实际字节数；未命名 HANDLE、LPOVERLAPPED 分别为忽略的文件句柄和重叠操作。
    // 返回：写入模拟成功时为 TRUE 并填写 written；注入失败时为 FALSE。
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
    // 模拟文件数据刷新并支持在最终落盘阶段注入失败。
    // 入参：未命名 HANDLE 为被模拟操作的虚构文件句柄。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI Flush(HANDLE)
    {
        return ExportSystemFake::Succeed("flush") ? TRUE : FALSE;
    }
    // 记录文件句柄关闭。
    // 入参：未命名 HANDLE 为被模拟操作的虚构文件句柄。
    // 返回：对应故障注入点未触发时为 TRUE，触发时为 FALSE。
    static BOOL WINAPI CloseFile(HANDLE)
    {
        return ExportSystemFake::Succeed("fileClose") ? TRUE : FALSE;
    }
    // 捕获句柄删除标记，模拟清理失败且不删除任何磁盘路径。
    // 入参：type 为文件信息类别；information 为借用信息结构指针；未命名 HANDLE 和 DWORD 分别为忽略的文件句柄及结构字节数。
    // 返回：cleanupFails 为 false 时返回 TRUE；否则返回 FALSE 并设置共享冲突错误。
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
