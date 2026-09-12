// 将 SDR 图像转换为 DIB 并发布到系统剪贴板，管理内存与会话所有权。

#include "clipboard_api.h"
#include "image_encoder.h"

#include <cstring>
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
    SetLastError(ERROR_SUCCESS);
    if (api.unlock(allocated) == FALSE && GetLastError() != ERROR_SUCCESS)
    {
        return ClipboardFailure(L"Unlock clipboard memory", error);
    }
    if (api.open(owner) == FALSE)
    {
        return ClipboardFailure(L"Open clipboard", error);
    }
    const ClipboardSession session(api);
    if (api.empty() == FALSE)
    {
        return ClipboardFailure(L"Empty clipboard", error);
    }
    if (api.set(CF_DIB, allocated) == nullptr)
    {
        return ClipboardFailure(L"Publish clipboard image", error);
    }
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
} // namespace open_st
