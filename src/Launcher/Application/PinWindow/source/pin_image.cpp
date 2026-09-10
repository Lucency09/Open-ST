// 文件职责：校验并复制贴图 BGRX 图像，提供不依赖截图模块的只读原图快照。

#include <pin_image.h>

#include <cstring>
#include <limits>
#include <utility>

namespace open_st
{
// 接收已经校验的尺寸和紧密像素，建立图像自有存储。
// 入参：width、height 为原图物理像素尺寸；pixels 为完整紧密 BGRX 缓冲区。
// 返回：无返回值；转移缓冲区而不再复制像素。
PinImage::PinImage(int width, int height, std::vector<std::uint8_t> pixels) noexcept
    : width_(width), height_(height), pixels_(std::move(pixels))
{
}

// 将工厂局部图像转移到共享所有权容器。
// 入参：source 为本次创建的临时图像。
// 返回：无返回值；接管源对象的像素缓冲区。
PinImage::PinImage(PinImage&& source) noexcept = default;

// 释放图像持有的像素缓冲区。
// 入参：无。
// 返回：无返回值；标准容器回收其内存。
PinImage::~PinImage() = default;

// 拒绝非法尺寸、跨度及不足缓冲区，复制时保留第四字节并去掉每行填充。
// 入参：width、height 为物理像素尺寸；stride 为输入字节跨度；pixels 为借用输入；error 接收诊断。
// 返回：成功为独立不可变图像；校验或内存分配失败为空指针，输入数据不会被修改。
std::shared_ptr<const PinImage> PinImage::Create(int width, int height, std::size_t stride,
                                                 std::span<const std::uint8_t> pixels, std::wstring& error)
{
    error.clear();
    constexpr int MAX_DIMENSION = 16384;
    if (width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
    {
        error = L"贴图尺寸不在有效图形资源范围内。";
        return {};
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
    const std::size_t rowsBeforeLast = static_cast<std::size_t>(height - 1);
    if (stride < rowBytes ||
        (rowsBeforeLast != 0U && stride > ((std::numeric_limits<std::size_t>::max)() - rowBytes) / rowsBeforeLast) ||
        pixels.size() < stride * rowsBeforeLast + rowBytes)
    {
        error = L"贴图像素跨度或缓冲区长度无效。";
        return {};
    }
    try
    {
        std::vector<std::uint8_t> storage(rowBytes * static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row)
        {
            std::memcpy(storage.data() + static_cast<std::size_t>(row) * rowBytes,
                        pixels.data() + static_cast<std::size_t>(row) * stride, rowBytes);
        }
        return std::make_shared<const PinImage>(PinImage(width, height, std::move(storage)));
    }
    catch (...)
    {
        error = L"分配贴图原始像素失败。";
        return {};
    }
}

// 查询图像的原始宽度。
// 入参：无。
// 返回：物理像素宽度。
int PinImage::Width() const noexcept
{
    return this->width_;
}

// 查询图像的原始高度。
// 入参：无。
// 返回：物理像素高度。
int PinImage::Height() const noexcept
{
    return this->height_;
}

// 查询紧密 BGRX 存储的行跨度。
// 入参：无。
// 返回：原始宽度乘四的字节数。
std::size_t PinImage::Stride() const noexcept
{
    return static_cast<std::size_t>(this->width_) * 4U;
}

// 提供图像存活期间有效的只读原始像素。
// 入参：无。
// 返回：连续紧密 BGRX 视图，原始第四字节完整保留。
std::span<const std::uint8_t> PinImage::Pixels() const noexcept
{
    return this->pixels_;
}
} // namespace open_st
