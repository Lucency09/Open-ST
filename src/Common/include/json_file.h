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
    // 创建无效句柄，可在取得实际文件入口后赋值。
    JsonFileHandle() noexcept = default;

    // 只检查入口有效性，不检查文件是否存在或可访问。
    [[nodiscard]] bool IsValid() const noexcept;
    // 返回当前有效 JSON 的独立副本；失败不改变输出，也不返回旧缓存冒充成功。
    [[nodiscard]] bool Read(nlohmann::json& document) const noexcept;

    // 明确整份替换文档；缺失则安全创建，损坏或不可访问的已有文件不覆盖。
    [[nodiscard]] bool Write(const nlohmann::json& document) const noexcept;

    // 在同文件锁内执行一次编辑并提交；编辑后空 optional 不是删除请求，而是取消。
    // 同内容也检查当前写入条件，但不重写目标 JSON；成功不保证未来操作仍成功。
    [[nodiscard]] bool Write(const JsonDocumentEditor& editor) const noexcept;

  private:
    friend class JsonFileManager;

    // 由管理器创建指向其内部文件状态的入口。
    explicit JsonFileHandle(std::shared_ptr<JsonFileState> state) noexcept;

    std::shared_ptr<JsonFileState> state_;
};

// 进程内唯一的 JSON 文件状态管理器；卡名和路径全部由业务模块动态提供。
class JsonFileManager final
{
  public:
    // 禁止复制进程内管理器。
    JsonFileManager(const JsonFileManager&) = delete;
    // 禁止复制赋值，保持文件绑定唯一。
    JsonFileManager& operator=(const JsonFileManager&) = delete;
    // 进程退出时释放管理器持有的文件状态。
    ~JsonFileManager();

    // 获取进程内唯一实例。
    [[nodiscard]] static JsonFileManager& Instance() noexcept;

    // 只建立或复用标识与路径绑定，不读盘；绑定冲突或参数无效时返回无效句柄。
    [[nodiscard]] JsonFileHandle GetFile(std::string_view cardName, const std::filesystem::path& filePath) noexcept;

  private:
    friend struct JsonFileTestAccess;
    class Impl;
    // 创建内部绑定表；只由 Instance 调用。
    JsonFileManager();
    // 仅供隔离测试在没有外部句柄时解除绑定，正常业务不管理缓存生命周期。
    [[nodiscard]] bool ReleaseFile(std::string_view cardName) noexcept;

    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
