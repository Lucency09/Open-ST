// 声明业务 JSON 文件的共享句柄、原子编辑接口和命名绑定管理器。

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace open_st
{
// 编辑当前文档；空 optional 仅表示文件不存在，JSON null 仍是有值文档。
// 返回 false 或抛出异常均取消；不得重入同一文件、保留文档引用或执行外部副作用。
using JsonDocumentEditor = std::function<bool(std::optional<nlohmann::json>& document)>;

class JsonFileState;
struct JsonFileTestAccess;

// 文件操作入口；管理细节由 Common 隐藏，复制句柄仍操作同一文件。
class JsonFileHandle final
{
  public:
    // 创建暂未绑定文件状态的空句柄。
    // 入参：无。
    // 返回：构造函数无返回值；初始 IsValid() 为 false。
    JsonFileHandle() noexcept = default;

    // 判断 JSON 文件句柄是否关联有效的管理状态。
    // 入参：无。
    // 返回：已关联文件状态时为 true；空句柄为 false，不检查磁盘文件是否存在或可访问。
    [[nodiscard]] bool IsValid() const noexcept;
    // 读取当前磁盘 JSON 文档并向调用方提供独立副本。
    // 入参：document：输出参数，成功时接收完整 JSON 文档。
    // 返回：读取并解析成功时为 true；失败时为 false 且不改变 document，不用旧缓存冒充成功。
    [[nodiscard]] bool Read(nlohmann::json& document) const noexcept;

    // 以完整 JSON 文档替换目标内容，文件缺失时安全创建。
    // 入参：document：调用期间借用的完整替换文档。
    // 返回：提交成功时为 true；句柄无效或读写失败时为 false，不覆盖已有的损坏或不可访问文件。
    [[nodiscard]] bool Write(const nlohmann::json& document) const noexcept;

    // 在同一文件锁内编辑当前 JSON 文档并原子提交。
    // 入参：editor：同步编辑回调，接收 optional 文档；无值表示文件不存在；不得重入同文件、保存文档引用或执行外部副作用。
    // 返回：编辑接受且可提交时为 true；拒绝、异常、编辑后无文档或读写失败时为 false；相同内容也检查写入条件但不重写文件。
    [[nodiscard]] bool Write(const JsonDocumentEditor& editor) const noexcept;

  private:
    friend class JsonFileManager;

    // 建立共享文件状态的业务访问句柄。
    // 入参：state：与其他句柄共享的文件状态，转入当前句柄。
    // 返回：构造函数无返回值；当前句柄延长共享状态的存活时间。
    explicit JsonFileHandle(std::shared_ptr<JsonFileState> state) noexcept;

    std::shared_ptr<JsonFileState> state_;
};

// 进程内唯一的 JSON 文件状态管理器；卡名和路径全部由业务模块动态提供。
class JsonFileManager final
{
  public:
    // 禁止复制 JSON 文件管理器，保持进程中的文件绑定表唯一。
    // 入参：未命名的 const JsonFileManager 引用：拟复制的源管理器。
    // 返回：无；函数已删除，调用会导致编译错误。
    JsonFileManager(const JsonFileManager&) = delete;
    // 禁止复制 JSON 文件管理器，保持进程中的文件绑定表唯一。
    // 入参：未命名的 const JsonFileManager 引用：拟复制的源管理器。
    // 返回：无；函数已删除，调用会导致编译错误。
    JsonFileManager& operator=(const JsonFileManager&) = delete;
    // 释放管理器持有的文件绑定和共享状态引用。
    // 入参：无。
    // 返回：析构函数无返回值；仍被外部句柄共享的状态由共享所有权决定生命周期。
    ~JsonFileManager();

    // 取得供各业务模块复用的进程 JSON 文件管理器。
    // 入参：无。
    // 返回：唯一管理器的借用引用，调用方不得销毁该实例。
    [[nodiscard]] static JsonFileManager& Instance() noexcept;

    // 建立或复用业务卡名与规范化文件路径的唯一绑定。
    // 入参：cardName：非空业务文件标识；filePath：拟绑定的 JSON 文件路径。
    // 返回：成功返回共享文件句柄；参数非法、绑定冲突或资源失败返回无效句柄，本调用不读取文件。
    [[nodiscard]] JsonFileHandle GetFile(std::string_view cardName, const std::filesystem::path& filePath) noexcept;

  private:
    friend struct JsonFileTestAccess;
    class Impl;
    // 创建进程 JSON 文件管理器的内部绑定表。
    // 入参：无。
    // 返回：构造函数无返回值；由 Instance 创建，分配失败可抛出异常。
    JsonFileManager();
    // 为隔离测试释放指定业务文件的管理器绑定。
    // 入参：cardName：要解除的业务文件标识。
    // 返回：无绑定或成功移除时为 true；仍有外部句柄持有状态或内部失败时为 false。
    [[nodiscard]] bool ReleaseFile(std::string_view cardName) noexcept;

    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
