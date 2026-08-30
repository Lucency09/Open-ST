#include <json_file.h>

#include <log.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
constexpr std::uintmax_t MAX_JSON_FILE_BYTES = 1024U * 1024U;

// 只在 Common 内传播阶段与系统错误，不携带 JSON 正文。
struct FileFailure final
{
    const char* stage;
    DWORD code;
};

// 持有 Win32 文件句柄，在异常和普通返回路径中均确保关闭。
class FileHandle final
{
  public:
    // 接管一个已打开的句柄，INVALID_HANDLE_VALUE 表示没有资源。
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    // 自动关闭尚未释放的文件句柄。
    ~FileHandle()
    {
        if (this->value_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(this->value_);
        }
    }
    // 禁止复制文件资源所有权。
    FileHandle(const FileHandle&) = delete;
    // 禁止复制赋值，避免重复关闭。
    FileHandle& operator=(const FileHandle&) = delete;
    // 借用底层句柄调用 Windows API。
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return this->value_;
    }
    // 显式关闭并报告失败，不让析构再次关闭同一资源。
    void Close()
    {
        const HANDLE value = std::exchange(this->value_, INVALID_HANDLE_VALUE);
        if (value != INVALID_HANDLE_VALUE && CloseHandle(value) == FALSE)
        {
            throw FileFailure{"close", GetLastError()};
        }
    }

  private:
    HANDLE value_;
};

struct FileSignature final
{
    DWORD volumeSerialNumber{};
    DWORD fileIndexHigh{};
    DWORD fileIndexLow{};
    DWORD fileSizeHigh{};
    DWORD fileSizeLow{};
    FILETIME lastWriteTime{};

    // 比较文件身份、长度与最后写入时间，用于缓存验证及外部变更复核。
    [[nodiscard]] bool operator==(const FileSignature& other) const noexcept
    {
        return this->volumeSerialNumber == other.volumeSerialNumber && this->fileIndexHigh == other.fileIndexHigh &&
               this->fileIndexLow == other.fileIndexLow && this->fileSizeHigh == other.fileSizeHigh &&
               this->fileSizeLow == other.fileSizeLow &&
               this->lastWriteTime.dwHighDateTime == other.lastWriteTime.dwHighDateTime &&
               this->lastWriteTime.dwLowDateTime == other.lastWriteTime.dwLowDateTime;
    }
};

enum class FileQueryStatus
{
    Present,
    Missing
};

// 规范化业务提供的路径，不在 Common 内选择业务目录。
std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

// 按 Windows 路径大小写规则比较规范化后的路径文本。
bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right) noexcept
{
    const std::wstring& leftText = left.native();
    const std::wstring& rightText = right.native();
    return CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()), rightText.c_str(),
                                static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
}

// 读取文件元数据；只将真正缺失视为 Missing，其他失败中止当前操作。
FileQueryStatus QueryFileSignature(const std::filesystem::path& path, FileSignature& signature)
{
    const HANDLE opened =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return FileQueryStatus::Missing;
        }
        throw FileFailure{"query_signature", error};
    }
    const FileHandle file(opened);
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file.Get(), &information) == FALSE)
    {
        throw FileFailure{"query_information", GetLastError()};
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        throw FileFailure{"target_is_directory", ERROR_DIRECTORY};
    }
    signature =
        FileSignature{information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow,
                      information.nFileSizeHigh,        information.nFileSizeLow,   information.ftLastWriteTime};
    return FileQueryStatus::Present;
}

// 把双 DWORD 长度转换为无符号字节数。
std::uintmax_t FileSize(const FileSignature& signature) noexcept
{
    ULARGE_INTEGER size{};
    size.HighPart = signature.fileSizeHigh;
    size.LowPart = signature.fileSizeLow;
    return size.QuadPart;
}

// 有界读取完整文件并确认读取期间未变化；格式解析由文件状态对象完成。
std::string ReadStableBytes(const std::filesystem::path& path, const FileSignature& beforeRead)
{
    const std::uintmax_t size = FileSize(beforeRead);
    if (size == 0 || size > MAX_JSON_FILE_BYTES)
    {
        throw FileFailure{"invalid_size", ERROR_FILE_INVALID};
    }
    const FileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE)
    {
        throw FileFailure{"open_read", GetLastError()};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    DWORD read{};
    if (ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) == FALSE)
    {
        throw FileFailure{"read", GetLastError()};
    }
    if (read != bytes.size())
    {
        throw FileFailure{"short_read", ERROR_HANDLE_EOF};
    }
    FileSignature afterRead{};
    if (QueryFileSignature(path, afterRead) != FileQueryStatus::Present || !(beforeRead == afterRead))
    {
        throw FileFailure{"changed_during_read", ERROR_RETRY};
    }
    return bytes;
}

// 在目标同目录生成进程、线程和序列组合的临时文件名。
std::filesystem::path TemporaryPath(const std::filesystem::path& path)
{
    static std::atomic<std::uint64_t> sequence{};
    std::wstring name = path.filename().native();
    name.append(L".tmp.").append(std::to_wstring(GetCurrentProcessId()));
    name.append(L".").append(std::to_wstring(GetCurrentThreadId()));
    name.append(L".").append(std::to_wstring(++sequence));
    return path.parent_path() / name;
}

// 持有临时文件名，成功移动后源路径已不存在；失败时清理残留，不触碰目标文件。
class TemporaryPathGuard final
{
  public:
    // 借用本次操作栈上的稳定路径。
    explicit TemporaryPathGuard(const std::filesystem::path& path) noexcept : path_(path) {}
    // 清理尚未移动的临时文件；清理失败只记录系统错误，不遮蔽原失败。
    ~TemporaryPathGuard()
    {
        if (this->active_ && DeleteFileW(this->path_.c_str()) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            {
                OPEN_ST_LOG_WARNING("JSON temporary cleanup failed. win32_error=", error);
            }
        }
    }
    // 禁止复制临时文件清理职责。
    TemporaryPathGuard(const TemporaryPathGuard&) = delete;
    // 禁止复制赋值临时文件清理职责。
    TemporaryPathGuard& operator=(const TemporaryPathGuard&) = delete;
    // 移动成功后立即解除旧路径清理职责，不再触碰可能被外部重新创建的同名文件。
    void Dismiss() noexcept
    {
        this->active_ = false;
    }

  private:
    const std::filesystem::path& path_;
    bool active_{true};
};

// 保留原有权限预检：已有文件可替换，父目录能创建自动删除的零字节临时文件。
void CheckWritableExistingFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        throw FileFailure{"write_attributes", GetLastError()};
    }
    if ((attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        throw FileFailure{"read_only_or_directory", ERROR_ACCESS_DENIED};
    }
    const FileHandle target(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (target.Get() == INVALID_HANDLE_VALUE)
    {
        throw FileFailure{"replace_permission", GetLastError()};
    }
    const std::filesystem::path probePath = TemporaryPath(path);
    FileHandle probe(CreateFileW(probePath.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (probe.Get() == INVALID_HANDLE_VALUE)
    {
        throw FileFailure{"directory_write_permission", GetLastError()};
    }
    probe.Close();
}

// 在同目录写入并刷盘后移动；缺失创建不使用覆盖标志，提交成功后不再做可能失败的分配。
void WriteBytesAtomically(const std::filesystem::path& path, std::string_view bytes, bool replaceExisting,
                          const FileSignature& baseline, FileSignature& writtenSignature)
{
    const std::filesystem::path temporaryPath = TemporaryPath(path);
    const HANDLE opened =
        CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE)
    {
        throw FileFailure{"create_temporary", GetLastError()};
    }
    // 两个构造均不抛异常；逆序析构保证先关闭文件，再清理路径，并保留原始失败。
    TemporaryPathGuard cleanup(temporaryPath);
    FileHandle file(opened);
    DWORD written{};
    if (WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) == FALSE)
    {
        throw FileFailure{"write_temporary", GetLastError()};
    }
    if (written != bytes.size())
    {
        throw FileFailure{"short_write", ERROR_WRITE_FAULT};
    }
    if (FlushFileBuffers(file.Get()) == FALSE)
    {
        throw FileFailure{"flush_temporary", GetLastError()};
    }
    file.Close();
    if (QueryFileSignature(temporaryPath, writtenSignature) != FileQueryStatus::Present)
    {
        throw FileFailure{"temporary_disappeared", ERROR_FILE_NOT_FOUND};
    }
    FileSignature current{};
    const FileQueryStatus status = QueryFileSignature(path, current);
    if ((replaceExisting && (status != FileQueryStatus::Present || !(current == baseline))) ||
        (!replaceExisting && status != FileQueryStatus::Missing))
    {
        throw FileFailure{"external_change_before_commit", ERROR_RETRY};
    }
    const DWORD flags =
        replaceExisting ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH : MOVEFILE_WRITE_THROUGH;
    if (MoveFileExW(temporaryPath.c_str(), path.c_str(), flags) == FALSE)
    {
        throw FileFailure{"commit", GetLastError()};
    }
    cleanup.Dismiss();
}
} // namespace

namespace open_st
{
// 每份文件状态仅由 Common 管理，锁、快照、失败签名与版本均不进入业务接口。
class JsonFileState final
{
  public:
    // 绑定业务提供的名称与路径，不进行磁盘访问。
    JsonFileState(std::string cardName, std::filesystem::path path)
        : cardName_(std::move(cardName)), path_(std::move(path))
    {
    }
    // 借用绑定路径，供管理器检查标识冲突。
    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return this->path_;
    }
    // 串行读取并在成功复制后一次性交换输出，任何失败均保留调用者原值。
    bool Read(nlohmann::json& document) noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            try
            {
                if (this->RefreshLocked() == FileQueryStatus::Missing)
                {
                    throw FileFailure{"missing", ERROR_FILE_NOT_FOUND};
                }
                nlohmann::json result = *this->snapshot_;
                document.swap(result);
                this->readFailure_ = {};
                return true;
            }
            catch (const FileFailure& failure)
            {
                this->ReportFailureLocked(false, failure.stage, failure.code);
            }
            catch (const nlohmann::json::exception& error)
            {
                this->ReportFailureLocked(false, "json_exception", static_cast<DWORD>(error.id));
            }
            catch (...)
            {
                this->ReportFailureLocked(false, "unexpected_exception", 0);
            }
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("JSON read could not acquire operation resources. card=", this->cardName_);
        }
        return false;
    }
    // 对完整目标文档使用同一锁内编辑与提交路径，不另建一套写入规则。
    bool Write(const nlohmann::json& document) noexcept
    {
        try
        {
            // 完整替换是调用方明确提供的业务意图，不进行隐式字段合并。
            const JsonDocumentEditor editor = [&document](std::optional<nlohmann::json>& current)
            {
                current = document;
                return true;
            };
            return this->Write(editor);
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("JSON replacement preparation failed. card=", this->cardName_);
            return false;
        }
    }
    // 同文件锁覆盖读取、唯一一次业务编辑及提交；正常线程竞争只等待，不因旧基线失败。
    bool Write(const JsonDocumentEditor& editor) noexcept
    {
        try
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            try
            {
                if (!editor)
                {
                    throw FileFailure{"empty_editor", ERROR_INVALID_PARAMETER};
                }
                const bool existed = this->RefreshLocked() == FileQueryStatus::Present;
                const FileSignature baseline = this->validSignature_;
                std::optional<nlohmann::json> candidate;
                if (existed)
                {
                    candidate = *this->snapshot_;
                }
                if (!editor(candidate) || !candidate.has_value())
                {
                    this->ReportFailureLocked(true, "cancelled_by_editor", ERROR_CANCELLED);
                    return false;
                }
                const bool succeeded = this->CommitLocked(*candidate, existed, baseline);
                this->writeFailure_ = {};
                return succeeded;
            }
            catch (const FileFailure& failure)
            {
                this->ReportFailureLocked(true, failure.stage, failure.code);
            }
            catch (const nlohmann::json::exception& error)
            {
                // 不记录 what()，其中可能包含业务 JSON 片段。
                this->ReportFailureLocked(true, "json_exception", static_cast<DWORD>(error.id));
            }
            catch (...)
            {
                this->ReportFailureLocked(true, "unexpected_exception", 0);
            }
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("JSON write could not acquire operation resources. card=", this->cardName_);
        }
        return false;
    }

  private:
    struct FailureRecord final
    {
        const char* stage{};
        DWORD code{};
    };

    // 按读写操作分别去重连续同原因失败，成功后恢复下一次诊断；不影响文件重试。
    void ReportFailureLocked(bool writing, const char* stage, DWORD code) noexcept
    {
        FailureRecord& previous = writing ? this->writeFailure_ : this->readFailure_;
        if (previous.stage != nullptr && std::string_view(previous.stage) == stage && previous.code == code)
        {
            return;
        }
        previous = FailureRecord{stage, code};
        OPEN_ST_LOG_WARNING("JSON operation failed. operation=", writing ? "write" : "read", " card=", this->cardName_,
                            " stage=", stage, " error=", code);
    }

    // 按请求核对磁盘版本；失败保留旧快照，但不把它作为本次成功结果。
    FileQueryStatus RefreshLocked()
    {
        FileSignature signature{};
        if (QueryFileSignature(this->path_, signature) == FileQueryStatus::Missing)
        {
            return FileQueryStatus::Missing;
        }
        if (this->snapshot_ != nullptr && this->hasValidSignature_ && this->validSignature_ == signature)
        {
            return FileQueryStatus::Present;
        }
        if (this->hasInvalidSignature_ && this->invalidSignature_ == signature)
        {
            throw FileFailure{"invalid_json", ERROR_FILE_INVALID};
        }
        try
        {
            const std::string bytes = ReadStableBytes(this->path_, signature);
            nlohmann::json parsed = nlohmann::json::parse(bytes, nullptr, false);
            if (parsed.is_discarded())
            {
                throw FileFailure{"invalid_json", ERROR_FILE_INVALID};
            }
            this->snapshot_ = std::make_shared<const nlohmann::json>(std::move(parsed));
        }
        catch (const FileFailure& failure)
        {
            if (failure.code == ERROR_FILE_INVALID)
            {
                this->invalidSignature_ = signature;
                this->hasInvalidSignature_ = true;
            }
            throw;
        }
        this->validSignature_ = signature;
        this->hasValidSignature_ = true;
        this->hasInvalidSignature_ = false;
        ++this->revision_;
        return FileQueryStatus::Present;
    }

    // 复核目标仍符合写入开始时的磁盘状态；外部变化只取消本次操作，不自动重放编辑。
    void VerifyBaselineLocked(bool existed, const FileSignature& baseline)
    {
        FileSignature current{};
        const FileQueryStatus status = QueryFileSignature(this->path_, current);
        if ((existed && (status != FileQueryStatus::Present || !(current == baseline))) ||
            (!existed && status != FileQueryStatus::Missing))
        {
            throw FileFailure{"external_change", ERROR_RETRY};
        }
    }

    // 先准备序列化和快照，再检查权限及基线；提交完成后只执行不抛出的状态发布。
    bool CommitLocked(const nlohmann::json& document, bool existed, const FileSignature& baseline)
    {
        std::string serialized = document.dump(2);
        serialized.push_back('\n');
        if (serialized.size() > MAX_JSON_FILE_BYTES || document.is_discarded())
        {
            throw FileFailure{"invalid_write_document_or_size", ERROR_FILE_INVALID};
        }
        // 以实际 JSON 字节比较，区分 1 与 1.0；同内容不得重写用户的缩进或时间戳。
        const bool unchanged = existed && serialized == this->snapshot_->dump(2) + "\n";
        if (existed)
        {
            CheckWritableExistingFile(this->path_);
        }
        this->VerifyBaselineLocked(existed, baseline);
        if (unchanged)
        {
            return true;
        }
        // 与磁盘 JSON 表示一致，避免非有限浮点序列化为 null 后缓存仍持有非有限数。
        const std::shared_ptr<const nlohmann::json> published =
            std::make_shared<const nlohmann::json>(nlohmann::json::parse(serialized));
        std::error_code directoryError;
        std::filesystem::create_directories(this->path_.parent_path(), directoryError);
        if (directoryError)
        {
            throw FileFailure{"create_parent_directory", static_cast<DWORD>(directoryError.value())};
        }
        this->VerifyBaselineLocked(existed, baseline);
        FileSignature written{};
        WriteBytesAtomically(this->path_, serialized, existed, baseline, written);
        this->snapshot_ = published;
        this->validSignature_ = written;
        this->hasValidSignature_ = true;
        this->hasInvalidSignature_ = false;
        ++this->revision_;
        return true;
    }

    std::string cardName_;
    std::filesystem::path path_;
    std::mutex mutex_;
    std::shared_ptr<const nlohmann::json> snapshot_;
    FileSignature validSignature_{};
    FileSignature invalidSignature_{};
    bool hasValidSignature_{};
    bool hasInvalidSignature_{};
    std::uint64_t revision_{};
    FailureRecord readFailure_;
    FailureRecord writeFailure_;
};

class JsonFileManager::Impl final
{
  public:
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<JsonFileState>> files;
};

// 只由管理器建立文件入口，不执行文件访问。
JsonFileHandle::JsonFileHandle(std::shared_ptr<JsonFileState> state) noexcept : state_(std::move(state)) {}

// 无效句柄仅表示获取入口失败，不代表特定磁盘错误。
bool JsonFileHandle::IsValid() const noexcept
{
    return this->state_ != nullptr;
}

// 将读取交给内部文件状态；无效入口不修改输出。
bool JsonFileHandle::Read(nlohmann::json& document) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON read rejected: invalid handle.");
        return false;
    }
    return this->state_->Read(document);
}

// 明确整份写入，具体内容由业务提供。
bool JsonFileHandle::Write(const nlohmann::json& document) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON write rejected: invalid handle.");
        return false;
    }
    return this->state_->Write(document);
}

// 将业务编辑交给受同步保护的文件状态执行。
bool JsonFileHandle::Write(const JsonDocumentEditor& editor) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON edit rejected: invalid handle.");
        return false;
    }
    return this->state_->Write(editor);
}

// 创建唯一绑定表，不进行业务文件访问。
JsonFileManager::JsonFileManager() : impl_(std::make_unique<Impl>()) {}

// 进程退出时释放强持有的全部文件状态。
JsonFileManager::~JsonFileManager() = default;

// 获取进程内单例，所有句柄通过同一张绑定表建立。
JsonFileManager& JsonFileManager::Instance() noexcept
{
    static JsonFileManager manager;
    return manager;
}

// 懒建立标识与路径绑定，复用已有状态并拒绝不一致的别名。
JsonFileHandle JsonFileManager::GetFile(std::string_view cardName, const std::filesystem::path& filePath) noexcept
{
    try
    {
        if (cardName.empty() || filePath.empty())
        {
            OPEN_ST_LOG_WARNING("JSON handle acquisition rejected: empty name or path.");
            return {};
        }
        const std::filesystem::path normalizedPath = NormalizePath(filePath);
        const std::string ownedName(cardName);
        const std::scoped_lock<std::mutex> lock(this->impl_->mutex);
        const std::unordered_map<std::string, std::shared_ptr<JsonFileState>>::const_iterator existing =
            this->impl_->files.find(ownedName);
        if (existing != this->impl_->files.end())
        {
            if (!PathsEqual(existing->second->Path(), normalizedPath))
            {
                OPEN_ST_LOG_ERROR("JSON name requested with a different path. card=", ownedName);
                return {};
            }
            return JsonFileHandle(existing->second);
        }
        for (const std::pair<const std::string, std::shared_ptr<JsonFileState>>& entry : this->impl_->files)
        {
            if (PathsEqual(entry.second->Path(), normalizedPath))
            {
                OPEN_ST_LOG_ERROR("JSON path requested with a different name. card=", ownedName);
                return {};
            }
        }
        const std::shared_ptr<JsonFileState> state = std::make_shared<JsonFileState>(ownedName, normalizedPath);
        this->impl_->files.emplace(ownedName, state);
        return JsonFileHandle(state);
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        OPEN_ST_LOG_ERROR("JSON path binding failed. card=", cardName, " error=", error.code().value());
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("JSON handle acquisition failed. card=", cardName);
    }
    return {};
}

// 测试私有清理入口，拒绝释放仍有外部使用者的文件。
bool JsonFileManager::ReleaseFile(std::string_view cardName) noexcept
{
    try
    {
        const std::scoped_lock<std::mutex> lock(this->impl_->mutex);
        const std::unordered_map<std::string, std::shared_ptr<JsonFileState>>::iterator entry =
            this->impl_->files.find(std::string(cardName));
        if (entry == this->impl_->files.end())
        {
            return true;
        }
        if (entry->second.use_count() != 1)
        {
            return false;
        }
        this->impl_->files.erase(entry);
        return true;
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("JSON test binding cleanup failed. card=", cardName);
        return false;
    }
}
} // namespace open_st
