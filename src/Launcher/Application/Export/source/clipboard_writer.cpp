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
    // 绑定分配结果和对应释放函数。
    ClipboardMemory(HGLOBAL memory, const ClipboardApi& api) : memory_(memory), api_(api) {}
    // 释放所有仍属于本地的全局内存。
    ~ClipboardMemory()
    {
        if (this->memory_ != nullptr)
        {
            this->api_.free(this->memory_);
        }
    }
    // 禁止复制以避免重复释放。
    ClipboardMemory(const ClipboardMemory&) = delete;
    // 禁止赋值以避免覆盖现有所有权。
    ClipboardMemory& operator=(const ClipboardMemory&) = delete;
    // 发布成功后解除本地所有权。
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
    // 接管已经打开的剪贴板会话。
    explicit ClipboardSession(const ClipboardApi& api) : api_(api) {}
    // 关闭剪贴板但不影响已发布数据的系统所有权。
    ~ClipboardSession()
    {
        this->api_.close();
    }
    // 会话不能复制。
    ClipboardSession(const ClipboardSession&) = delete;
    // 会话不能赋值。
    ClipboardSession& operator=(const ClipboardSession&) = delete;

  private:
    const ClipboardApi& api_;
};

// 记录当前 Win32 错误，不包含截图内容。
bool ClipboardFailure(const wchar_t* operation, std::wstring& error)
{
    const DWORD code = GetLastError();
    error = std::wstring(operation) + L" Win32=" + std::to_wstring(code);
    return false;
}
} // namespace

// 反转行序生成兼容性更好的正高度 BI_RGB；保留 RGB 并清零保留字节。
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
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(image.width);
        header.biHeight = static_cast<LONG>(image.height);
        header.biPlanes = 1U;
        header.biBitCount = 32U;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(pixelBytes);
        std::memcpy(candidate.data(), &header, sizeof(header));
        for (std::uint32_t y = 0U; y < image.height; ++y)
        {
            const std::uint8_t* source = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
            std::uint8_t* destination =
                candidate.data() + sizeof(header) + static_cast<std::size_t>(image.height - 1U - y) * rowBytes;
            std::memcpy(destination, source, rowBytes);
            for (std::uint32_t x = 0U; x < image.width; ++x)
            {
                destination[static_cast<std::size_t>(x) * 4U + 3U] = 0U;
            }
        }
        dib = std::move(candidate);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = L"Insufficient memory to build clipboard image";
        return false;
    }
}

// 在清空剪贴板前完成图像构造、分配和填充；失败不进行无界重试。
bool CopyImageWithApi(HWND owner, const SdrImageView& image, const ClipboardApi& api, std::wstring& error)
{
    error.clear();
    if (owner == nullptr || api.isWindow(owner) == FALSE)
    {
        error = L"Clipboard owner is not a valid window";
        return false;
    }
    std::vector<std::uint8_t> dib;
    if (!BuildClipboardDib(image, dib, error))
    {
        return false;
    }
    const HGLOBAL allocated = api.allocate(GMEM_MOVEABLE, dib.size());
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
    std::memcpy(destination, dib.data(), dib.size());
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

// 产品调用使用默认系统函数表，测试通过私有入口替换边界。
bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring& error)
{
    return CopyImageWithApi(owner, image, ClipboardApi{}, error);
}
} // namespace open_st
