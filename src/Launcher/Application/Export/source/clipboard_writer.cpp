// 将 SDR 图像转换为 DIB 并发布到系统剪贴板，管理内存与会话所有权。

#include "clipboard_api.h"
#include "image_encoder.h"

#include <cstring>
#include <limits>
#include <new>

namespace open_st
{
namespace
{
// 持有尚未发布的全局内存；只有 SetClipboardData 成功才能转移所有权。
class ClipboardMemory final
{
  public:
    // 接管尚未发布的剪贴板全局内存，确保失败路径释放分配结果。
    // 入参：memory：由全局内存接口分配的 HGLOBAL，可为空；api：在守卫存活期间保持有效的借用剪贴板接口表。
    // 返回：无返回值；在调用 Release 前，本对象负责释放非空 memory。
    ClipboardMemory(HGLOBAL memory, const ClipboardApi& api) : memory_(memory), api_(api) {}
    // 释放仍由本地拥有的剪贴板全局内存，避免发布失败后泄漏。
    // 入参：无。
    // 返回：无返回值；已通过 Release 转交系统的内存不会再次释放。
    ~ClipboardMemory()
    {
        if (this->memory_ != nullptr)
        {
            this->api_.free(this->memory_);
        }
    }
    // 禁止复制全局内存的管理职责，避免重复释放或关闭。
    // 入参：未命名 const ClipboardMemory 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    ClipboardMemory(const ClipboardMemory&) = delete;
    // 禁止复制全局内存的管理职责，避免重复释放或关闭。
    // 入参：未命名 const ClipboardMemory 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    ClipboardMemory& operator=(const ClipboardMemory&) = delete;
    // 在剪贴板发布成功后撤销本地内存释放职责。
    // 入参：无。
    // 返回：无返回值；仅清除本地句柄，已发布的内存由系统继续持有。
    void Release() noexcept
    {
        this->memory_ = nullptr;
    }

  private:
    HGLOBAL memory_{};
    const ClipboardApi& api_;
};

// 成功打开剪贴板后，在所有返回路径关闭会话。
class ClipboardSession final
{
  public:
    // 接管已经打开的剪贴板会话的关闭职责。
    // 入参：api：在会话存活期间保持有效的借用剪贴板接口表，调用方须已成功打开剪贴板。
    // 返回：无返回值；析构时通过 api.close 关闭会话。
    explicit ClipboardSession(const ClipboardApi& api) : api_(api) {}
    // 结束已打开的剪贴板访问会话，解除本进程对剪贴板的访问占用。
    // 入参：无。
    // 返回：无返回值；关闭会话，已发布数据仍归系统所有。
    ~ClipboardSession()
    {
        this->api_.close();
    }
    // 禁止复制剪贴板会话的管理职责，避免重复释放或关闭。
    // 入参：未命名 const ClipboardSession 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    ClipboardSession(const ClipboardSession&) = delete;
    // 禁止复制剪贴板会话的管理职责，避免重复释放或关闭。
    // 入参：未命名 const ClipboardSession 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    ClipboardSession& operator=(const ClipboardSession&) = delete;

  private:
    const ClipboardApi& api_;
};

// 记录剪贴板操作刚产生的 Win32 错误，供调用方结束失败流程。
// 入参：operation：失败操作的宽字符名称；error：输出参数，接收操作名称及当前 GetLastError 错误码。
// 返回：始终返回 false，使调用方可直接返回同一失败结果。
bool ClipboardFailure(const wchar_t* operation, std::wstring& error)
{
    const DWORD code = GetLastError();
    error = std::wstring(operation) + L" Win32=" + std::to_wstring(code);
    return false;
}

// 发布已经填充的全局内存，复用所有格式的解锁与剪贴板会话流程。
// 入参：owner 为有效窗口；allocated 为仍由调用方管理的已锁内存；format 为格式；api 为接口；error 接收诊断。
// 返回：成功转交系统为 true，失败为 false，调用方须释放未转交内存。
bool PublishClipboardMemory(HWND owner, HGLOBAL allocated, UINT format, const ClipboardApi& api, std::wstring& error)
{
    SetLastError(ERROR_SUCCESS);
    if (api.unlock(allocated) == FALSE && GetLastError() != ERROR_SUCCESS)
        return ClipboardFailure(L"Unlock clipboard memory", error);
    if (api.open(owner) == FALSE)
        return ClipboardFailure(L"Open clipboard", error);
    const ClipboardSession session(api);
    if (api.empty() == FALSE)
        return ClipboardFailure(L"Empty clipboard", error);
    if (api.set(format, allocated) == nullptr)
        return ClipboardFailure(L"Publish clipboard data", error);
    return true;
}

// 将已校验的图像直接写入完整 DIB 缓冲区，不分配临时像素副本。
// 入参：image 为已通过 ValidateSdrImage 的视图；buffer 至少容纳位图头与紧凑像素。
// 返回：无返回值；保留 RGB，翻转行序并清零 DIB 保留字节。
void FillClipboardDib(const SdrImageView& image, std::uint8_t* buffer) noexcept
{
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(image.width);
    header.biHeight = static_cast<LONG>(image.height);
    header.biPlanes = 1U;
    header.biBitCount = 32U;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(rowBytes * image.height);
    std::memcpy(buffer, &header, sizeof(header));
    for (std::uint32_t y = 0U; y < image.height; ++y)
    {
        const std::uint8_t* source = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
        std::uint8_t* destination =
            buffer + sizeof(header) + static_cast<std::size_t>(image.height - 1U - y) * rowBytes;
        std::memcpy(destination, source, rowBytes);
        for (std::uint32_t x = 0U; x < image.width; ++x)
        {
            destination[static_cast<std::size_t>(x) * 4U + 3U] = 0U;
        }
    }
}
} // namespace

// 把 SDR 图像转换为可发布到剪贴板的底向上 32 位 BI_RGB DIB。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；dib：输出参数，成功时接收位图头及紧凑像素字节；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：构建完成时为 true；校验或分配失败时为 false 且不修改 dib；DIB 保留 RGB 并把保留字节清零。
bool BuildClipboardDib(const SdrImageView& image, std::vector<std::uint8_t>& dib, std::wstring& error)
{
    if (!ValidateSdrImage(image, error))
    {
        return false;
    }
    try
    {
        const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
        const std::size_t pixelBytes = rowBytes * image.height;
        std::vector<std::uint8_t> candidate(sizeof(BITMAPINFOHEADER) + pixelBytes);
        FillClipboardDib(image, candidate.data());
        dib = std::move(candidate);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = L"Insufficient memory to build clipboard image";
        return false;
    }
}

// 通过注入的系统接口将完整 SDR 图像发布为 CF_DIB，并转交全局内存所有权。
// 入参：owner：有效的剪贴板所有者窗口；image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；api：调用期间有效的剪贴板系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：成功发布时为 true；准备或系统调用失败时为 false，并释放尚未移交的本地内存；清空剪贴板之后的失败无法恢复旧内容。
bool CopyImageWithApi(HWND owner, const SdrImageView& image, const ClipboardApi& api, std::wstring& error)
{
    error.clear();
    if (owner == nullptr || api.isWindow(owner) == FALSE)
    {
        error = L"Clipboard owner is not a valid window";
        return false;
    }
    if (!ValidateSdrImage(image, error))
    {
        return false;
    }
    const std::size_t dibBytes = sizeof(BITMAPINFOHEADER) + static_cast<std::size_t>(image.width) * 4U * image.height;
    const HGLOBAL allocated = api.allocate(GMEM_MOVEABLE, dibBytes);
    if (allocated == nullptr)
    {
        return ClipboardFailure(L"Allocate clipboard memory", error);
    }
    ClipboardMemory memory(allocated, api);
    void* const destination = api.lock(allocated);
    if (destination == nullptr)
    {
        return ClipboardFailure(L"Lock clipboard memory", error);
    }
    FillClipboardDib(image, static_cast<std::uint8_t*>(destination));
    if (!PublishClipboardMemory(owner, allocated, CF_DIB, api, error))
        return false;
    memory.Release();
    return true;
}

// 将 SDR 图像复制到系统剪贴板，供其他应用按不透明 CF_DIB 粘贴。
// 入参：owner：有效的剪贴板所有者窗口；image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride
// 为行字节跨度，第四字节不表示透明度；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：成功发布时为 true，失败时为 false；成功后图像内存归系统，发布阶段失败可能已清空旧剪贴板。
bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring& error)
{
    return CopyImageWithApi(owner, image, ClipboardApi{}, error);
}

// 校验 Unicode 文本并通过共享发布流程交给系统剪贴板。
// 入参：owner 为有效窗口；text 为完整 UTF-16 文本；api 为借用接口；error 接收不含正文的诊断。
// 返回：成功为 true；准备失败不清空剪贴板，发布失败释放仍由本地拥有的内存。
bool CopyTextWithApi(HWND owner, std::wstring_view text, const ClipboardApi& api, std::wstring& error)
{
    error.clear();
    if (owner == nullptr || api.isWindow(owner) == FALSE)
    {
        error = L"Clipboard owner is not a valid window";
        return false;
    }
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        text.find(L'\0') != std::wstring_view::npos ||
        (!text.empty() && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                              nullptr, 0, nullptr, nullptr) == 0))
    {
        error = L"Clipboard text is not valid UTF-16";
        return false;
    }
    try
    {
        std::wstring normalized;
        normalized.reserve(text.size());
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const wchar_t character = text[index];
            if (character == L'\r' || character == L'\n')
            {
                normalized.append(L"\r\n");
                if (character == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n')
                    ++index;
            }
            else
                normalized.push_back(character);
        }
        if (normalized.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) - 1U)
        {
            error = L"Clipboard text is too large";
            return false;
        }
        const std::size_t bytes = (normalized.size() + 1U) * sizeof(wchar_t);
        const HGLOBAL allocated = api.allocate(GMEM_MOVEABLE, bytes);
        if (allocated == nullptr)
            return ClipboardFailure(L"Allocate clipboard memory", error);
        ClipboardMemory memory(allocated, api);
        void* const destination = api.lock(allocated);
        if (destination == nullptr)
            return ClipboardFailure(L"Lock clipboard memory", error);
        std::memcpy(destination, normalized.c_str(), bytes);
        if (!PublishClipboardMemory(owner, allocated, CF_UNICODETEXT, api, error))
            return false;
        memory.Release();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = L"Insufficient memory to copy clipboard text";
        return false;
    }
}

// 将 Unicode 文本发布到系统剪贴板，不附带图像或业务格式。
// 入参：owner 为有效窗口；text 为完整 UTF-16 文本；error 接收不含正文的诊断。
// 返回：成功为 true，失败为 false；成功后内存归系统所有。
bool CopyTextToClipboard(HWND owner, std::wstring_view text, std::wstring& error)
{
    return CopyTextWithApi(owner, text, ClipboardApi{}, error);
}
} // namespace open_st
