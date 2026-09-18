// 为文件租约、JSON 事务和日志测试提供真实独立进程，不启动产品窗口。

#include "logger.h"
#include <file_lease.h>
#include <filesystem>
#include <json_file.h>
#include <log.h>
#include <string_view>
#include <windows.h>

// 按测试协议持有资源，准备后通知父进程并等待测试释放事件。
// 入参：mode、path、ready、release 四个命令行参数。
// 返回：成功为零；初始化或等待失败非零。
int wmain(int argc, wchar_t** argv)
{
    if (argc != 5)
        return 1;
    const std::wstring_view mode(argv[1]);
    const std::filesystem::path path(argv[2]);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
    if (ready == nullptr || release == nullptr)
        return 2;
    // 发布已进入受保护区，并等待父测试明确释放。
    // 入参：捕获的 ready、release 为测试事件。
    // 返回：事件握手成功时为 true。
    const auto hold = [ready, release]()
    { return SetEvent(ready) != FALSE && WaitForSingleObject(release, 15000) == WAIT_OBJECT_0; };
    bool result = false;
    if (mode == L"json")
    {
        const auto file = open_st::JsonFileManager::Instance().GetFile("probe", path);
        // 在真实跨进程事务内暂停并修改最新计数。
        // 入参：document：受租约保护的最新 JSON。
        // 返回：等待成功且文档存在时提交。
        result = file.Write(
            [&hold](std::optional<nlohmann::json>& document)
            {
                if (!document.has_value() || !hold())
                    return false;
                (*document)["count"] = document->value("count", 0) + 1;
                return true;
            });
    }
    else if (mode == L"logger")
    {
        result = open_st::Logger::Initialize(path);
        if (result)
        {
            OPEN_ST_LOG_ERROR("process probe");
            result = hold();
            open_st::Logger::Shutdown();
        }
    }
    else
    {
        open_st::FileLease lease;
        result = lease.TryAcquire(path, mode == L"shared" ? open_st::FileLeaseMode::Shared
                                                          : open_st::FileLeaseMode::Exclusive) &&
                 hold();
    }
    CloseHandle(ready);
    CloseHandle(release);
    return result ? 0 : 3;
}
