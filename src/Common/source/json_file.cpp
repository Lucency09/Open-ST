// 实现 JSON 文件懒加载、变更检测、串行编辑及原子替换写入。

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
    // 接管 Win32 文件句柄以便退出作用域时自动关闭。
    // 入参：value：已打开的文件句柄，INVALID_HANDLE_VALUE 表示无资源。
    // 返回：构造函数无返回值；非无效句柄的关闭职责归当前对象。
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    // 关闭仍由当前守卫拥有的文件句柄。
    // 入参：无。
    // 返回：析构函数无返回值；关闭错误不向外传播。
    ~FileHandle()
    {
        if (this->value_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(this->value_);
        }
    }
    // 禁止复制文件句柄关闭职责，避免重复清理。
    // 入参：未命名 const FileHandle 引用：拟复制的源守卫。
    // 返回：无；函数已删除，调用会导致编译错误。
    FileHandle(const FileHandle&) = delete;
    // 禁止复制文件句柄关闭职责，避免重复清理。
    // 入参：未命名 const FileHandle 引用：拟复制的源守卫。
    // 返回：无；函数已删除，调用会导致编译错误。
    FileHandle& operator=(const FileHandle&) = delete;
    // 借出当前文件句柄供同步调用 Windows 文件接口。
    // 入参：无。
    // 返回：当前 HANDLE，不转移所有权；未持有资源时为 INVALID_HANDLE_VALUE。
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return this->value_;
    }
    // 显式关闭文件并在关闭失败时报告所处阶段。
    // 入参：无。
    // 返回：无返回值；系统关闭失败时抛出 FileFailure，内部句柄始终先复位以避免再次关闭。
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

    // 比较两次文件查询是否代表同一身份和版本。
    // 入参：other：另一份文件身份、长度及最后写入时间签名。
    // 返回：卷、文件索引、字节数及写入时间均相同时为 true，否则为 false。
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

// 把业务路径转换为可用于绑定比较的规范化绝对路径。
// 入参：path：业务提供的文件路径，可为相对路径。
// 返回：绝对且消除词法冗余的路径值；解析路径失败时抛出文件系统异常，不解析符号链接身份。
std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

// 按 Windows 不区分大小写的文本规则比较规范化路径。
// 入参：left、right：待比较的规范化文件路径。
// 返回：路径文本按序数忽略大小写后相等时为 true，否则为 false，不检查磁盘文件身份。
bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right) noexcept
{
    const std::wstring& leftText = left.native();
    const std::wstring& rightText = right.native();
    return CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()), rightText.c_str(),
                                static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
}

// 查询磁盘文件身份及版本，区分真实缺失与访问故障。
// 入参：path：目标路径；signature：输出参数，成功时写入文件身份、字节数及写入时间。
// 返回：文件存在时为 Present；文件或父路径不存在时为 Missing 且保留 signature；其他系统错误或目录目标抛出 FileFailure。
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

// 从文件签名还原完整字节长度。
// 入参：signature：含高低 DWORD 长度的文件签名。
// 返回：合并后的无符号文件字节数。
std::uintmax_t FileSize(const FileSignature& signature) noexcept
{
    ULARGE_INTEGER size{};
    size.HighPart = signature.fileSizeHigh;
    size.LowPart = signature.fileSizeLow;
    return size.QuadPart;
}

// 读取完整 JSON 文件字节并检查读取期间文件未变化。
// 入参：path：目标文件路径；beforeRead：读取前已取得的文件签名。
// 返回：自有原始字节字符串；文件为空、超过 1 MiB、短读、访问失败或签名变化时抛出 FileFailure。
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

// 生成与目标同目录的写入临时文件候选路径。
// 入参：path：最终目标文件路径。
// 返回：附带进程 ID、线程 ID 和递增序号的路径值；不创建文件，实际创建仍须独占检查。
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
    // 登记本次原子写入临时文件的失败清理职责。
    // 入参：path：借用的临时路径对象，必须比守卫存活更久。
    // 返回：构造函数无返回值；析构前若未 Dismiss 则尝试删除该路径。
    explicit TemporaryPathGuard(const std::filesystem::path& path) noexcept : path_(path) {}
    // 清理本次写入尚未成功移走的临时文件。
    // 入参：无。
    // 返回：析构函数无返回值；不存在视为已清理，其余删除失败仅写诊断，不遮蔽原错误。
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
    // 禁止复制临时路径删除职责，避免重复清理。
    // 入参：未命名 const TemporaryPathGuard 引用：拟复制的源守卫。
    // 返回：无；函数已删除，调用会导致编译错误。
    TemporaryPathGuard(const TemporaryPathGuard&) = delete;
    // 禁止复制临时路径删除职责，避免重复清理。
    // 入参：未命名 const TemporaryPathGuard 引用：拟复制的源守卫。
    // 返回：无；函数已删除，调用会导致编译错误。
    TemporaryPathGuard& operator=(const TemporaryPathGuard&) = delete;
    // 在临时文件成功移动后撤销旧路径的删除职责。
    // 入参：无。
    // 返回：无返回值；后续析构不再访问该路径，避免删除外部后来创建的同名文件。
    void Dismiss() noexcept
    {
        this->active_ = false;
    }

  private:
    const std::filesystem::path& path_;
    bool active_{true};
};

// 检查已有 JSON 文件可替换且父目录允许建立临时文件。
// 入参：path：已经存在的目标文件路径。
// 返回：无返回值；属性、替换权限、临时创建或关闭失败时抛出 FileFailure，探测文件按关闭删除。
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

// 通过同目录临时文件写入并刷新后替换目标，避免发布半份 JSON。
// 入参：path：最终路径；bytes：完整序列化字节，长度已由调用方限制；replaceExisting：是否替换已有目标；baseline：替换前签名；writtenSignature：输出参数，接收已写临时文件签名。
// 返回：无返回值；写入、刷新、基线复核或移动失败时抛出 FileFailure，未移动的临时文件由守卫清理；失败时 writtenSignature 可能已写入。
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
    // 为一份业务 JSON 文件建立共享状态及同步边界。
    // 入参：cardName：转入状态的业务标识；path：转入状态的规范化文件路径。
    // 返回：构造函数无返回值；只建立内存状态，不访问磁盘。
    JsonFileState(std::string cardName, std::filesystem::path path)
        : cardName_(std::move(cardName)), path_(std::move(path))
    {
    }
    // 提供已绑定路径供管理器检查名称和路径冲突。
    // 入参：无。
    // 返回：状态持有的文件路径只读引用，借用生命周期不超过当前共享状态。
    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return this->path_;
    }
    // 读取当前磁盘 JSON 文档并向调用方提供独立副本。
    // 入参：document：输出参数，成功时接收完整 JSON 文档。
    // 返回：读取并解析成功时为 true；失败时为 false 且不改变 document，不用旧缓存冒充成功。
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
    // 以完整 JSON 文档替换目标内容，文件缺失时安全创建。
    // 入参：document：调用期间借用的完整替换文档。
    // 返回：提交成功时为 true；句柄无效或读写失败时为 false，不覆盖已有的损坏或不可访问文件。
    bool Write(const nlohmann::json& document) noexcept
    {
        try
        {
            // 把整份替换文档写入通用编辑候选，复用文件锁和提交规则。
            // 入参：current：输入输出 optional 文档；捕获的 document 为调用方明确提供的完整替换值。
            // 返回：始终返回 true 接受替换；JSON 复制分配异常由外层写入流程处理。
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
    // 在同一文件锁内编辑当前 JSON 文档并原子提交。
    // 入参：editor：同步编辑回调，接收 optional 文档；无值表示文件不存在；不得重入同文件、保存文档引用或执行外部副作用。
    // 返回：编辑接受且可提交时为 true；拒绝、异常、编辑后无文档或读写失败时为 false；相同内容也检查写入条件但不重写文件。
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

    // 对连续重复的 JSON 读写故障去重并记录诊断。
    // 入参：writing：true 表示写故障，false 表示读故障；stage：具有稳定生命周期的阶段名；code：对应系统或解析错误码；调用方须持有文件锁。
    // 返回：无返回值；同操作连续同阶段同错误码不重复记录，不阻止下一次文件重试。
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

    // 核对当前磁盘版本并在变化时刷新已解析 JSON 快照。
    // 入参：无；调用方须持有当前文件锁。
    // 返回：目标缺失为 Missing，有效且已缓存或重新解析成功为 Present；访问、稳定性或解析失败抛出异常，不把旧缓存当作本次成功。
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

    // 在提交前确认目标仍符合本次编辑开始时的磁盘状态。
    // 入参：existed：编辑开始时文件是否存在；baseline：编辑前签名；调用方须持有文件锁。
    // 返回：无返回值；存在性或签名变化时抛出 FileFailure，外部变化不会自动重放业务编辑。
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

    // 校验完整候选文档并提交文件及对应缓存版本。
    // 入参：document：本次编辑后的完整 JSON；existed：编辑前文件是否存在；baseline：编辑前文件签名；调用方须持有文件锁。
    // 返回：内容不变且可写或新内容提交成功时为 true；其他失败抛出异常，由上层转换为 false；提交成功后发布相同语义的缓存快照。
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

// 建立共享文件状态的业务访问句柄。
// 入参：state：与其他句柄共享的文件状态，转入当前句柄。
// 返回：构造函数无返回值；当前句柄延长共享状态的存活时间。
JsonFileHandle::JsonFileHandle(std::shared_ptr<JsonFileState> state) noexcept : state_(std::move(state)) {}

// 判断 JSON 文件句柄是否关联有效的管理状态。
// 入参：无。
// 返回：已关联文件状态时为 true；空句柄为 false，不检查磁盘文件是否存在或可访问。
bool JsonFileHandle::IsValid() const noexcept
{
    return this->state_ != nullptr;
}

// 读取当前磁盘 JSON 文档并向调用方提供独立副本。
// 入参：document：输出参数，成功时接收完整 JSON 文档。
// 返回：读取并解析成功时为 true；失败时为 false 且不改变 document，不用旧缓存冒充成功。
bool JsonFileHandle::Read(nlohmann::json& document) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON read rejected: invalid handle.");
        return false;
    }
    return this->state_->Read(document);
}

// 以完整 JSON 文档替换目标内容，文件缺失时安全创建。
// 入参：document：调用期间借用的完整替换文档。
// 返回：提交成功时为 true；句柄无效或读写失败时为 false，不覆盖已有的损坏或不可访问文件。
bool JsonFileHandle::Write(const nlohmann::json& document) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON write rejected: invalid handle.");
        return false;
    }
    return this->state_->Write(document);
}

// 在同一文件锁内编辑当前 JSON 文档并原子提交。
// 入参：editor：同步编辑回调，接收 optional 文档；无值表示文件不存在；不得重入同文件、保存文档引用或执行外部副作用。
// 返回：编辑接受且可提交时为 true；拒绝、异常、编辑后无文档或读写失败时为 false；相同内容也检查写入条件但不重写文件。
bool JsonFileHandle::Write(const JsonDocumentEditor& editor) const noexcept
{
    if (this->state_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("JSON edit rejected: invalid handle.");
        return false;
    }
    return this->state_->Write(editor);
}

// 创建进程 JSON 文件管理器的内部绑定表。
// 入参：无。
// 返回：构造函数无返回值；由 Instance 创建，分配失败可抛出异常。
JsonFileManager::JsonFileManager() : impl_(std::make_unique<Impl>()) {}

// 释放管理器持有的文件绑定和共享状态引用。
// 入参：无。
// 返回：析构函数无返回值；仍被外部句柄共享的状态由共享所有权决定生命周期。
JsonFileManager::~JsonFileManager() = default;

// 取得供各业务模块复用的进程 JSON 文件管理器。
// 入参：无。
// 返回：唯一管理器的借用引用，调用方不得销毁该实例。
JsonFileManager& JsonFileManager::Instance() noexcept
{
    static JsonFileManager manager;
    return manager;
}

// 建立或复用业务卡名与规范化文件路径的唯一绑定。
// 入参：cardName：非空业务文件标识；filePath：拟绑定的 JSON 文件路径。
// 返回：成功返回共享文件句柄；参数非法、绑定冲突或资源失败返回无效句柄，本调用不读取文件。
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

// 为隔离测试释放指定业务文件的管理器绑定。
// 入参：cardName：要解除的业务文件标识。
// 返回：无绑定或成功移除时为 true；仍有外部句柄持有状态或内部失败时为 false。
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
