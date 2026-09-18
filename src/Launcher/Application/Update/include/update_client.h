// 声明更新任务及只读结果；不拥有窗口、设置文件或安装器执行权。
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace open_st
{
enum class UpdateDistribution
{
    Installed,
    Portable
};
enum class UpdatePhase
{
    Idle,
    Checking,
    Available,
    Current,
    LocalAhead,
    Downloading,
    Ready,
    Cancelled,
    Failed
};
enum class UpdateError
{
    None,
    Configuration,
    Network,
    Timeout,
    NotFound,
    RateLimited,
    InvalidRelease,
    MissingPackage,
    Integrity,
    Storage,
    Busy,
    Cancelled,
    Unavailable
};
struct UpdateSnapshot
{
    std::uint64_t revision{};
    UpdatePhase phase{UpdatePhase::Idle};
    UpdateError error{UpdateError::None};
    std::string currentVersion, latestVersion;
    std::wstring releasePage;
    std::uint64_t received{}, total{};
};
namespace update_detail
{
class UpdateTransport;
}
struct UpdateClientTestAccess;
// 生命周期和控制命令限构造线程；Snapshot 允许跨线程读取，notify 在工作线程调用。
class UpdateClient final
{
  public:
    // 建立独立更新会话，通知仅唤醒宿主，不在工作线程操作窗口。
    // 入参：cacheRoot 为 EXE 旁缓存目录；notify 为可从工作线程调用的通知。
    // 返回：未开始请求的对象。
    UpdateClient(std::filesystem::path cacheRoot, std::function<void()> notify);
    // 取消任务并回收异步资源，未交接文件随对象清理。
    // 入参：无。返回：无。
    ~UpdateClient();
    // 禁止复制请求与缓存所有权。
    // 入参：源任务。返回：不可调用。
    UpdateClient(const UpdateClient&) = delete;
    // 禁止复制赋值。
    // 入参：源任务。返回：不可调用。
    UpdateClient& operator=(const UpdateClient&) = delete;
    // 查询固定仓库的正式发行，不下载安装包。
    // 入参：currentVersion 为编译版本；distribution 为 Settings 提供的已校验模式。
    // 返回：已开始为 true，活动任务或分配失败为 false。
    bool Check(std::string currentVersion, UpdateDistribution distribution) noexcept;
    // 明确用户确认后才开始下载本次已查询的安装包。
    // 入参：无。返回：安装版可用结果被接受为 true。
    bool DownloadConfirmed() noexcept;
    // 撤销当前操作，后续回调不能发布可启动结果。
    // 入参：无。返回：无，不等待网络超时。
    void Cancel() noexcept;
    // 取得任务状态的独立副本，不暴露内部可变对象。
    // 入参：无。返回：当前快照，分配失败可抛异常。
    UpdateSnapshot Snapshot() const;
    // 借用受保护的已校验安装包，保护持续覆盖宿主系统启动调用。
    // 入参：launch 为同步窄启动回调，仅成功时返回 true。
    // 返回：启动成功为 true；失败须由用户重新发起更新，取消后不可启动。
    bool LaunchReady(const std::function<bool(const std::filesystem::path&)>& launch) noexcept;

  private:
    friend struct UpdateClientTestAccess;
    // 测试用传输注入，生产构造仍建立 WinHTTP 实现。
    // 入参：cacheRoot、notify 同生产接口；transport 为独占边界替身。返回：任务实例。
    UpdateClient(std::filesystem::path cacheRoot, std::function<void()> notify,
                 std::unique_ptr<update_detail::UpdateTransport> transport);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
