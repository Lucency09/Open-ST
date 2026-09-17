// 把 DirectWrite 完整文字布局的字形运行转换为可共享的对象局部填充几何。
#include "annotation_text_geometry.h"
#include <d2d1helper.h>
#include <dwrite.h>
#include <exception>
#include <limits>

namespace open_st
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT32 MAX_TEXT_SEGMENTS = 262144U;

class OutlineRenderer final : public IDWriteTextRenderer
{
  public:
    ID2D1Factory* factory{};
    std::vector<ComPtr<ID2D1Geometry>> outlines;
    UINT32 segments{};

    // 为同步文字布局借出接口，不将栈内 renderer 所有权交给 DirectWrite。
    // 入参：iid 为所需接口；object 接收借用指针。
    // 返回：支持的同步绘制接口为 S_OK，否则 E_NOINTERFACE。
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer))
        {
            *object = static_cast<IDWriteTextRenderer*>(this);
            this->AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    // 同步借用接口不转移栈对象所有权。
    // 入参：无。
    // 返回：固定借用引用计数。
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return 1;
    }
    // 同步绘制返回后由作用域回收 renderer，不通过 COM 删除栈对象。
    // 入参：无。
    // 返回：固定借用引用计数。
    ULONG STDMETHODCALLTYPE Release() override
    {
        return 1;
    }
    // 禁用像素对齐以保存对象物理坐标，并使各输出共享同一轮廓。
    // 入参：未命名上下文未使用；disabled 接收标志。
    // 返回：参数有效为 S_OK。
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        if (disabled == nullptr)
            return E_POINTER;
        *disabled = TRUE;
        return S_OK;
    }
    // 字形局部几何采用单位变换，最终平移由统一标注绘制路径执行。
    // 入参：未命名上下文未使用；transform 接收单位矩阵。
    // 返回：参数有效为 S_OK。
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        if (transform == nullptr)
            return E_POINTER;
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    // 布局字号以物理像素解释，不随屏幕 DPI 改变对象内容。
    // 入参：未命名上下文未使用；pixels 接收单位比例。
    // 返回：参数有效为 S_OK。
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override
    {
        if (pixels == nullptr)
            return E_POINTER;
        *pixels = 1.0F;
        return S_OK;
    }
    // 提取真实字形运行的轮廓，并用布局基线定位；空白字符不会生成外接矩形。
    // 入参：x、y 为局部基线；run 为字形运行；其他参数仅为 DirectWrite 绘制上下文。
    // 返回：成功为 S_OK，任何字形或预算失败使整段文字失败。
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
    try
    {
        if (run == nullptr || run->fontFace == nullptr)
            return E_INVALIDARG;
        ComPtr<ID2D1PathGeometry> path;
        HRESULT result = this->factory->CreatePathGeometry(path.GetAddressOf());
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(result))
            result = path->Open(sink.GetAddressOf());
        if (FAILED(result))
            return result;
        result = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                   run->glyphOffsets, run->glyphCount, run->isSideways,
                                                   (run->bidiLevel & 1U) != 0U, sink.Get());
        const HRESULT closed = sink->Close();
        if (FAILED(result) || FAILED(closed))
            return FAILED(result) ? result : closed;
        UINT32 count{};
        result = path->GetSegmentCount(&count);
        if (FAILED(result) || count > MAX_TEXT_SEGMENTS - this->segments)
            return FAILED(result) ? result : E_OUTOFMEMORY;
        this->segments += count;
        if (count == 0)
            return S_OK;
        ComPtr<ID2D1TransformedGeometry> translated;
        result = this->factory->CreateTransformedGeometry(path.Get(), D2D1::Matrix3x2F::Translation(x, y),
                                                          translated.GetAddressOf());
        if (SUCCEEDED(result))
            this->outlines.push_back(translated);
        return result;
    }
    catch (const std::exception&)
    {
        return E_OUTOFMEMORY;
    }
    // 当前统一字体不设置下划线，接口保留空操作。
    // 入参：全部参数为 DirectWrite 可选装饰信息。
    // 返回：S_OK。
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
    {
        return S_OK;
    }
    // 当前统一字体不设置删除线，接口保留空操作。
    // 入参：全部参数为 DirectWrite 可选装饰信息。
    // 返回：S_OK。
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
    {
        return S_OK;
    }
    // 本模块不注入内联对象，遇到未支持对象时拒绝而非静默丢失内容。
    // 入参：全部参数为 DirectWrite 内联对象信息。
    // 返回：E_NOTIMPL。
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                               IUnknown*) override
    {
        return E_NOTIMPL;
    }
};
} // namespace

// 按固定字体和显式换行生成对象局部文字轮廓，不做自动软换行。
// 入参：factory 为实际 D2D 工厂；text 为已校验文字；geometry 接收完整填充轮廓。
// 返回：完整布局及字形提取成功为 S_OK；错误或路径超预算不发布部分几何。
HRESULT CreateAnnotationTextGeometry(ID2D1Factory* factory, const AnnotationText& text, ComPtr<ID2D1Geometry>& geometry)
try
{
    thread_local ComPtr<IDWriteFactory> writeFactory;
    if (!writeFactory)
    {
        const HRESULT created = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                    reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));
        if (FAILED(created))
            return created;
    }
    ComPtr<IDWriteTextFormat> format;
    HRESULT result =
        writeFactory->CreateTextFormat(reinterpret_cast<const wchar_t*>(ANNOTATION_FONT_FAMILY.data()), nullptr,
                                       DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       static_cast<float>(text.fontSize), L"", format.GetAddressOf());
    if (SUCCEEDED(result))
        result = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(result))
        result = writeFactory->CreateTextLayout(reinterpret_cast<const wchar_t*>(text.text->data()),
                                                static_cast<UINT32>(text.text->size()), format.Get(), 1000000.0F,
                                                1000000.0F, layout.GetAddressOf());
    if (FAILED(result))
        return result;
    OutlineRenderer renderer;
    renderer.factory = factory;
    result = layout->Draw(nullptr, &renderer, 0.0F, 0.0F);
    if (FAILED(result))
        return result;
    std::vector<ID2D1Geometry*> outlines;
    outlines.reserve(renderer.outlines.size());
    for (const ComPtr<ID2D1Geometry>& outline : renderer.outlines)
        outlines.push_back(outline.Get());
    if (outlines.empty())
    {
        ComPtr<ID2D1PathGeometry> empty;
        result = factory->CreatePathGeometry(empty.GetAddressOf());
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(result))
            result = empty->Open(sink.GetAddressOf());
        if (SUCCEEDED(result))
            result = sink->Close();
        if (SUCCEEDED(result))
            geometry = empty;
        return result;
    }
    ComPtr<ID2D1GeometryGroup> group;
    result = factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, outlines.data(), static_cast<UINT32>(outlines.size()),
                                          group.GetAddressOf());
    if (SUCCEEDED(result))
        geometry = group;
    return result;
}
catch (const std::exception&)
{
    return E_OUTOFMEMORY;
}
} // namespace open_st
