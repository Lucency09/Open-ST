// 按冻结桌面的固定网格缓存块均值，原生 HDR 解码及跨屏拼接继续复用正式 SDR 输出路径。
#include <annotation_mosaic_source.h>
#include <color_conversion.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <selection_output_renderer.h>
#include <windows_util.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <list>
#include <map>
#include <set>
#include <tuple>

namespace open_st
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr int TILE_EDGE = 256;
constexpr std::size_t COLOR_BUDGET = 16U * 1024U * 1024U;
constexpr std::size_t BITMAP_BUDGET = 16U * 1024U * 1024U;
constexpr std::size_t MAX_TILES = 1024;
constexpr std::size_t MAX_BITMAPS = 512;

struct TileKey
{
    unsigned block{};
    int x{};
    int y{};
    // 按块档位和桌面网格位置排序，不受对象裁剪或窗口原点影响。
    // 入参：other 为另一 tile 标识。
    // 返回：严格顺序比较。
    bool operator<(const TileKey& other) const noexcept
    {
        return std::tie(this->block, this->x, this->y) < std::tie(other.block, other.x, other.y);
    }
};

struct ColorTile
{
    TileKey key;
    RectI bounds;
    unsigned columns{};
    unsigned rows{};
    std::vector<std::uint32_t> colors;
    // 估算自有颜色及管理项字节，包含各容器节点的保守余量。
    // 入参：无。
    // 返回：用于会话缓存上限核算的字节数。
    std::size_t Bytes() const noexcept
    {
        return this->colors.capacity() * sizeof(std::uint32_t) + 512U;
    }
};

struct BitmapTile
{
    ComPtr<ID2D1RenderTarget> target;
    std::shared_ptr<const ColorTile> colors;
    bool hdr{};
    float whiteScale{};
    ComPtr<ID2D1BitmapBrush> brush;
    std::size_t bytes{};
};

struct FrozenIdentity
{
    const std::uint8_t* pixels{};
    std::size_t size{};
    RectI bounds{};
    CapturedPixelFormat format{};
    CapturedColorSpace space{};
    CapturedColorSpace displaySpace{};
    bool converted{};
    float white{};
};

// 比较物理矩形，不依赖截屏状态或当前窗口几何。
// 入参：first、second 为物理半开矩形。
// 返回：四边完全相同为 true。
bool SameRect(RectI first, RectI second) noexcept
{
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}

// 计算马赛克对象与当前选区、冻结桌面的有限相交范围。
// 入参：object 为矩形马赛克；selection、desktop 为物理边界。
// 返回：覆盖其可见像素的半开整数范围，无交集返回空矩形。
RectI VisibleBounds(const AnnotationObject& object, RectI selection, RectI desktop)
{
    const double left = std::max({object.origin.x + std::min(0.0, object.extent.x), static_cast<double>(selection.left),
                                  static_cast<double>(desktop.left)});
    const double top = std::max({object.origin.y + std::min(0.0, object.extent.y), static_cast<double>(selection.top),
                                 static_cast<double>(desktop.top)});
    const double right = std::min({object.origin.x + std::max(0.0, object.extent.x),
                                   static_cast<double>(selection.right), static_cast<double>(desktop.right)});
    const double bottom = std::min({object.origin.y + std::max(0.0, object.extent.y),
                                    static_cast<double>(selection.bottom), static_cast<double>(desktop.bottom)});
    if (left >= right || top >= bottom)
        return {};
    return {static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)), static_cast<int>(std::ceil(right)),
            static_cast<int>(std::ceil(bottom))};
}
} // namespace

struct AnnotationMosaicSource::Impl
{
    const FrozenDesktopFrame* desktop{};
    RectI bounds{};
    DWORD thread{GetCurrentThreadId()};
    std::vector<FrozenIdentity> storage;
    SelectionOutputRenderer converter;
    std::list<std::shared_ptr<const ColorTile>> colorCache;
    std::size_t colorBytes{};
    std::list<BitmapTile> bitmaps;
    std::size_t bitmapBytes{};
    std::map<TileKey, std::shared_ptr<const ColorTile>> prepared;
    std::weak_ptr<const std::vector<AnnotationObject>> preparedSnapshot;
    const void* preparedIdentity{};
    RectI preparedSelection{};

    // 计算固定网格中的 tile 边界，最后一个 tile 只保留桌面范围。
    // 入参：key 为块档位与 tile 坐标。
    // 返回：完整 tile 与桌面相交的物理矩形。
    RectI TileBounds(TileKey key) const noexcept
    {
        const std::int64_t left =
            static_cast<std::int64_t>(this->bounds.left) + static_cast<std::int64_t>(key.x) * TILE_EDGE;
        const std::int64_t top =
            static_cast<std::int64_t>(this->bounds.top) + static_cast<std::int64_t>(key.y) * TILE_EDGE;
        return {static_cast<int>(left), static_cast<int>(top),
                static_cast<int>(std::min<std::int64_t>(left + TILE_EDGE, this->bounds.right)),
                static_cast<int>(std::min<std::int64_t>(top + TILE_EDGE, this->bounds.bottom))};
    }

    // 从完整未标注 SDR tile 计算每一网格块平均值，跨输出与桌面空洞已由输出器统一处理。
    // 入参：key 为缺失 tile；tile 接收完整色样；error 接收转换失败。
    // 返回：成功为 true，不发布局部转换结果。
    bool ReadTile(TileKey key, std::shared_ptr<const ColorTile>& tile, std::wstring& error)
    {
        const RectI region = this->TileBounds(key);
        SdrSelectionFrame pixels;
        if (!this->converter.Render(*this->desktop, region, pixels, error))
            return false;
        auto next = std::make_shared<ColorTile>();
        next->key = key;
        next->bounds = region;
        next->columns = (static_cast<unsigned>(region.Width()) + key.block - 1U) / key.block;
        next->rows = (static_cast<unsigned>(region.Height()) + key.block - 1U) / key.block;
        next->colors.resize(static_cast<std::size_t>(next->columns) * next->rows);
        for (unsigned row = 0; row < next->rows; ++row)
        {
            for (unsigned column = 0; column < next->columns; ++column)
            {
                std::uint64_t red{}, green{}, blue{}, count{};
                const unsigned right = std::min((column + 1U) * key.block, static_cast<unsigned>(region.Width()));
                const unsigned bottom = std::min((row + 1U) * key.block, static_cast<unsigned>(region.Height()));
                for (unsigned y = row * key.block; y < bottom; ++y)
                {
                    for (unsigned x = column * key.block; x < right; ++x)
                    {
                        const std::size_t offset = static_cast<std::size_t>(y) * pixels.Stride() + x * 4U;
                        blue += pixels.Pixels()[offset];
                        green += pixels.Pixels()[offset + 1U];
                        red += pixels.Pixels()[offset + 2U];
                        ++count;
                    }
                }
                next->colors[static_cast<std::size_t>(row) * next->columns + column] =
                    static_cast<std::uint32_t>(((red + count / 2U) / count) << 16U |
                                               ((green + count / 2U) / count) << 8U | (blue + count / 2U) / count);
            }
        }
        if (next->Bytes() > COLOR_BUDGET)
        {
            error = L"单个马赛克 tile 超过颜色缓存预算。";
            return false;
        }
        while (!this->colorCache.empty() &&
               (this->colorCache.size() >= MAX_TILES || this->colorBytes + next->Bytes() > COLOR_BUDGET))
        {
            this->colorBytes -= this->colorCache.back()->Bytes();
            this->colorCache.pop_back();
        }
        tile = next;
        this->colorCache.push_front(next);
        this->colorBytes += next->Bytes();
        return true;
    }

    // 按设备目标、不可变颜色 tile 和显示颜色参数复用有界位图画刷。
    // 入参：target 为绘制目标；tile 为块色样；hdr、whiteScale 为目标颜色域；brush、error 接收结果。
    // 返回：完整可用画刷为 true，上传失败不产生替代颜色。
    bool Brush(ID2D1RenderTarget* target, const std::shared_ptr<const ColorTile>& tile, bool hdr, float whiteScale,
               ComPtr<ID2D1BitmapBrush>& brush, std::wstring& error)
    {
        for (auto entry = this->bitmaps.begin(); entry != this->bitmaps.end(); ++entry)
        {
            if (entry->target.Get() == target && entry->colors == tile && entry->hdr == hdr &&
                entry->whiteScale == whiteScale)
            {
                brush = entry->brush;
                this->bitmaps.splice(this->bitmaps.begin(), this->bitmaps, entry);
                return true;
            }
        }
        std::vector<std::uint8_t> bgra;
        std::vector<std::uint16_t> linear;
        if (hdr)
            linear.resize(tile->colors.size() * 4U);
        else
            bgra.resize(tile->colors.size() * 4U);
        for (std::size_t index = 0; index < tile->colors.size(); ++index)
        {
            const std::uint32_t rgb = tile->colors[index];
            if (hdr)
            {
                linear[index * 4U] =
                    EncodeFloat16(SrgbToLinear(static_cast<float>((rgb >> 16U) & 255U) / 255.0F) * whiteScale);
                linear[index * 4U + 1U] =
                    EncodeFloat16(SrgbToLinear(static_cast<float>((rgb >> 8U) & 255U) / 255.0F) * whiteScale);
                linear[index * 4U + 2U] =
                    EncodeFloat16(SrgbToLinear(static_cast<float>(rgb & 255U) / 255.0F) * whiteScale);
                linear[index * 4U + 3U] = EncodeFloat16(1.0F);
            }
            else
            {
                bgra[index * 4U] = static_cast<std::uint8_t>(rgb & 255U);
                bgra[index * 4U + 1U] = static_cast<std::uint8_t>((rgb >> 8U) & 255U);
                bgra[index * 4U + 2U] = static_cast<std::uint8_t>((rgb >> 16U) & 255U);
                bgra[index * 4U + 3U] = 255;
            }
        }
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_BITMAP_PROPERTIES properties =
            D2D1::BitmapProperties(D2D1::PixelFormat(hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED),
                                   96, 96);
        HRESULT result = target->CreateBitmap(D2D1::SizeU(tile->columns, tile->rows),
                                              hdr ? static_cast<const void*>(linear.data()) : bgra.data(),
                                              tile->columns * (hdr ? 8U : 4U), properties, bitmap.GetAddressOf());
        const D2D1_BITMAP_BRUSH_PROPERTIES bitmapProperties = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        if (SUCCEEDED(result))
            result = target->CreateBitmapBrush(bitmap.Get(), &bitmapProperties, nullptr, brush.GetAddressOf());
        if (FAILED(result))
        {
            error = FormatHResult(L"创建马赛克块色样画刷", result);
            return false;
        }
        const std::size_t bytes = tile->colors.size() * (hdr ? 8U : 4U) + 1024U;
        while (!this->bitmaps.empty() &&
               (this->bitmaps.size() >= MAX_BITMAPS || this->bitmapBytes + bytes > BITMAP_BUDGET))
        {
            this->bitmapBytes -= this->bitmaps.back().bytes;
            this->bitmaps.pop_back();
        }
        BitmapTile entry;
        entry.target = target;
        entry.colors = tile;
        entry.hdr = hdr;
        entry.whiteScale = whiteScale;
        entry.brush = brush;
        entry.bytes = bytes;
        this->bitmaps.push_front(std::move(entry));
        this->bitmapBytes += bytes;
        return true;
    }
};

// 绑定本次不可变冻结桌面，不预先复制其全部像素。
// 入参：desktop 为调用方保持存活的不可变帧。
// 返回：无。
AnnotationMosaicSource::AnnotationMosaicSource(const FrozenDesktopFrame& desktop) : impl_(std::make_unique<Impl>())
{
    this->impl_->desktop = &desktop;
    this->impl_->bounds = desktop.Bounds();
    for (const CapturedOutputPlane& plane : desktop.Outputs())
        this->impl_->storage.push_back({plane.Pixels().data(), plane.Pixels().size(), plane.Bounds(), plane.Format(),
                                        plane.PixelColorSpace(), plane.ColorMetadata().displayColorSpace,
                                        plane.ColorMetadata().systemConvertedToSdr,
                                        plane.ColorMetadata().sdrWhiteLevelNits});
}
// 释放全部会话自有缓存与转换资源。
// 入参：无。
// 返回：无。
AnnotationMosaicSource::~AnnotationMosaicSource() = default;

// 在目标结束前释放其位图缓存，避免保留已关闭窗口或离屏整张图像的 COM 目标。
// 入参：target 为即将释放的目标。
// 返回：无；颜色来源缓存仍可复用。
void AnnotationMosaicSource::ReleaseTarget(ID2D1RenderTarget* target) noexcept
{
    for (auto entry = this->impl_->bitmaps.begin(); entry != this->impl_->bitmaps.end();)
    {
        if (entry->target.Get() == target)
        {
            this->impl_->bitmapBytes -= entry->bytes;
            entry = this->impl_->bitmaps.erase(entry);
        }
        else
            ++entry;
    }
}

// 验证来源属于同一个且仍有效的冻结帧，阻止跨会话误用已缓存像素。
// 入参：desktop 为拟输出帧。
// 返回：绑定与冻结存储身份都一致为 true。
bool AnnotationMosaicSource::IsFor(const FrozenDesktopFrame& desktop) const noexcept
{
    if (&desktop != this->impl_->desktop || !desktop.IsValid() || !SameRect(desktop.Bounds(), this->impl_->bounds) ||
        desktop.Outputs().size() != this->impl_->storage.size())
        return false;
    for (std::size_t index = 0; index < this->impl_->storage.size(); ++index)
    {
        const CapturedOutputPlane& plane = desktop.Outputs()[index];
        const FrozenIdentity& saved = this->impl_->storage[index];
        if (plane.Pixels().data() != saved.pixels || plane.Pixels().size() != saved.size ||
            !SameRect(plane.Bounds(), saved.bounds) || plane.Format() != saved.format ||
            plane.PixelColorSpace() != saved.space || plane.ColorMetadata().displayColorSpace != saved.displaySpace ||
            plane.ColorMetadata().systemConvertedToSdr != saved.converted ||
            plane.ColorMetadata().sdrWhiteLevelNits != saved.white)
            return false;
    }
    return true;
}

// 准备候选所需 tile，完整成功后才一次替换当前可绘制集合。
// 入参：annotations 为候选文档；selection 为目标裁剪；error 为诊断。
// 返回：完整成功为 true，失败保持旧候选。
bool AnnotationMosaicSource::Prepare(const AnnotationSnapshot& annotations, RectI selection, std::wstring& error)
try
{
    error.clear();
    if (GetCurrentThreadId() != this->impl_->thread || !this->IsFor(*this->impl_->desktop) || selection.IsEmpty())
    {
        error = L"马赛克冻结来源、线程或裁剪无效。";
        return false;
    }
    if (annotations.get() == this->impl_->preparedIdentity &&
        !this->impl_->preparedSnapshot.owner_before(annotations) &&
        !annotations.owner_before(this->impl_->preparedSnapshot) && SameRect(selection, this->impl_->preparedSelection))
        return true;
    std::set<TileKey> required;
    if (annotations)
    {
        for (const AnnotationObject& object : *annotations)
        {
            if (object.kind != AnnotationKind::Mosaic)
                continue;
            if (!IsValidAnnotation(object))
            {
                error = L"马赛克候选几何或块档位无效。";
                return false;
            }
            const RectI visible = VisibleBounds(object, selection, this->impl_->bounds);
            if (visible.IsEmpty())
                continue;
            const unsigned block = std::get<AnnotationMosaic>(object.payload).blockSize;
            const int left =
                static_cast<int>((static_cast<std::int64_t>(visible.left) - this->impl_->bounds.left) / TILE_EDGE);
            const int top =
                static_cast<int>((static_cast<std::int64_t>(visible.top) - this->impl_->bounds.top) / TILE_EDGE);
            const int right =
                static_cast<int>((static_cast<std::int64_t>(visible.right) - 1 - this->impl_->bounds.left) / TILE_EDGE);
            const int bottom =
                static_cast<int>((static_cast<std::int64_t>(visible.bottom) - 1 - this->impl_->bounds.top) / TILE_EDGE);
            if (static_cast<std::uint64_t>(right - left + 1) * static_cast<std::uint64_t>(bottom - top + 1) > MAX_TILES)
            {
                error = L"马赛克候选超过局部缓存预算。";
                return false;
            }
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x)
                {
                    required.insert({block, x, y});
                    if (required.size() > MAX_TILES)
                    {
                        error = L"马赛克候选超过局部缓存预算。";
                        return false;
                    }
                }
        }
    }
    std::map<TileKey, std::shared_ptr<const ColorTile>> next;
    std::size_t bytes{};
    for (const TileKey key : required)
    {
        const RectI bounds = this->impl_->TileBounds(key);
        const std::size_t expected = ((static_cast<unsigned>(bounds.Width()) + key.block - 1U) / key.block) *
                                         ((static_cast<unsigned>(bounds.Height()) + key.block - 1U) / key.block) * 4U +
                                     512U;
        if (bytes + expected > COLOR_BUDGET)
        {
            error = L"马赛克候选超过颜色缓存预算。";
            return false;
        }
        std::shared_ptr<const ColorTile> tile;
        for (auto found = this->impl_->colorCache.begin(); found != this->impl_->colorCache.end(); ++found)
        {
            if (!((*found)->key < key) && !(key < (*found)->key))
            {
                tile = *found;
                this->impl_->colorCache.splice(this->impl_->colorCache.begin(), this->impl_->colorCache, found);
                break;
            }
        }
        if (!tile && !this->impl_->ReadTile(key, tile, error))
            return false;
        bytes += tile->Bytes();
        if (bytes > COLOR_BUDGET)
        {
            error = L"马赛克候选超过颜色缓存预算。";
            return false;
        }
        next.emplace(key, std::move(tile));
    }
    this->impl_->prepared = std::move(next);
    this->impl_->preparedSnapshot = annotations;
    this->impl_->preparedIdentity = annotations.get();
    this->impl_->preparedSelection = selection;
    return true;
}
catch (const std::exception&)
{
    error = L"无法准备马赛克来源缓存。";
    return false;
}

// 将准备好的 SDR 块均值映射到当前目标颜色域，并填充实际可见对象轮廓。
// 入参：target、object、visible 为本次绘制；origin、selection 为目标空间；hdr、whiteScale 为颜色域；error 为诊断。
// 返回：全部 tile 绘制成功为 true，缺少来源或图形失败为 false。
bool AnnotationMosaicSource::Draw(ID2D1RenderTarget* target, const AnnotationObject& object, ID2D1Geometry* visible,
                                  AnnotationPoint origin, RectI selection, bool hdr, float whiteScale,
                                  std::wstring& error)
try
{
    if (GetCurrentThreadId() != this->impl_->thread || !this->IsFor(*this->impl_->desktop) || target == nullptr ||
        visible == nullptr || !IsValidAnnotation(object) || object.kind != AnnotationKind::Mosaic ||
        !std::isfinite(whiteScale) || whiteScale <= 0)
    {
        error = L"马赛克绘制上下文无效。";
        return false;
    }
    const RectI bounds = VisibleBounds(object, selection, this->impl_->bounds);
    if (bounds.IsEmpty())
        return true;
    const unsigned block = std::get<AnnotationMosaic>(object.payload).blockSize;
    const int left = static_cast<int>((static_cast<std::int64_t>(bounds.left) - this->impl_->bounds.left) / TILE_EDGE);
    const int top = static_cast<int>((static_cast<std::int64_t>(bounds.top) - this->impl_->bounds.top) / TILE_EDGE);
    const int right =
        static_cast<int>((static_cast<std::int64_t>(bounds.right) - 1 - this->impl_->bounds.left) / TILE_EDGE);
    const int bottom =
        static_cast<int>((static_cast<std::int64_t>(bounds.bottom) - 1 - this->impl_->bounds.top) / TILE_EDGE);
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x)
        {
            const auto found = this->impl_->prepared.find({block, x, y});
            if (found == this->impl_->prepared.end())
            {
                error = L"马赛克候选尚未完整准备。";
                return false;
            }
            ComPtr<ID2D1BitmapBrush> brush;
            if (!this->impl_->Brush(target, found->second, hdr, whiteScale, brush, error))
                return false;
            const RectI tile = found->second->bounds;
            brush->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(block), static_cast<float>(block)) *
                                D2D1::Matrix3x2F::Translation(static_cast<float>(tile.left - origin.x),
                                                              static_cast<float>(tile.top - origin.y)));
            target->PushAxisAlignedClip(
                D2D1::RectF(static_cast<float>(tile.left - origin.x), static_cast<float>(tile.top - origin.y),
                            static_cast<float>(tile.right - origin.x), static_cast<float>(tile.bottom - origin.y)),
                D2D1_ANTIALIAS_MODE_ALIASED);
            target->FillGeometry(visible, brush.Get());
            target->PopAxisAlignedClip();
        }
    return true;
}
catch (const std::exception&)
{
    error = L"无法分配马赛克绘制资源。";
    return false;
}
} // namespace open_st
