#include <overlay_renderer.h>
#include <desktop_preview.h>
#include <display_color_state.h>
#include <selection_model.h>

#include "outside_mask_layout.h"
#include "overlay_settings.h"

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr float SELECTION_BORDER_WIDTH = 1.0F;
constexpr float HANDLE_VISUAL_HALF_SIZE = 4.0F;

// 将设置中的 sRGB 分量解码为线性光；HDR 界面白单独按本屏 SDR 白亮度缩放。
float UiChannel(std::uint32_t channel, bool hdr, float whiteScale) noexcept
{
    const float encoded = static_cast<float>(channel) / 255.0F;
    if (!hdr)
    {
        return encoded;
    }
    const float linear = encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
    return linear * whiteScale;
}

// 把 0xRRGGBB 设置颜色转换为目标交换链工作空间，不改变冻结图像像素。
D2D1_COLOR_F ColorFromRgb(std::uint32_t color, bool hdr, float whiteScale) noexcept
{
    return D2D1::ColorF(UiChannel((color >> 16U) & 0xFFU, hdr, whiteScale),
                        UiChannel((color >> 8U) & 0xFFU, hdr, whiteScale),
                        UiChannel(color & 0xFFU, hdr, whiteScale));
}

// 把虚拟桌面物理像素矩形平移为以冻结帧左上角为原点的客户区矩形。
D2D1_RECT_F ToClientRectangle(open_st::RectI rectangle, open_st::RectI frameBounds) noexcept
{
    return D2D1::RectF(static_cast<float>(rectangle.left - frameBounds.left),
                       static_cast<float>(rectangle.top - frameBounds.top),
                       static_cast<float>(rectangle.right - frameBounds.left),
                       static_cast<float>(rectangle.bottom - frameBounds.top));
}

// 用四个轴对齐填充矩形绘制 1px 内轮廓，避免整数坐标描边落在半像素产生模糊。
void FillOnePixelOutline(ID2D1DeviceContext* context, D2D1_RECT_F rectangle, ID2D1Brush* brush) noexcept
{
    const float horizontalEnd = std::min(rectangle.left + SELECTION_BORDER_WIDTH, rectangle.right);
    const float horizontalStart = std::max(rectangle.right - SELECTION_BORDER_WIDTH, rectangle.left);
    const float verticalEnd = std::min(rectangle.top + SELECTION_BORDER_WIDTH, rectangle.bottom);
    const float verticalStart = std::max(rectangle.bottom - SELECTION_BORDER_WIDTH, rectangle.top);
    context->FillRectangle(D2D1::RectF(rectangle.left, rectangle.top, rectangle.right, verticalEnd), brush);
    context->FillRectangle(D2D1::RectF(rectangle.left, verticalStart, rectangle.right, rectangle.bottom), brush);
    context->FillRectangle(D2D1::RectF(rectangle.left, rectangle.top, horizontalEnd, rectangle.bottom), brush);
    context->FillRectangle(D2D1::RectF(horizontalStart, rectangle.top, rectangle.right, rectangle.bottom), brush);
}

// 把失败操作名和 HRESULT 格式化为供上层日志诊断使用的宽字符串。
std::wstring FormatHResult(const wchar_t* operation, HRESULT result)
{
    std::wostringstream stream;
    stream << operation << L"失败，HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}
} // namespace

namespace open_st
{
// PImpl 把 DirectX COM 类型留在实现文件中，使公开头的编译依赖保持稳定。
struct OverlayRenderer::Impl final
{
    HWND window{};
    RectI frameBounds{};
    DXGI_FORMAT pixelFormat{DXGI_FORMAT_UNKNOWN};
    float uiWhiteScale{1.0F};
    OutputColorMetadata colorMetadata{};
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1Bitmap1> frameBitmap;
    ComPtr<ID2D1SolidColorBrush> dimBrush;
    ComPtr<ID2D1SolidColorBrush> selectionBrush;
    ComPtr<ID2D1SolidColorBrush> handleFillBrush;

    // 从 D2D context 解绑并释放交换链 back buffer 对应的目标位图。
    void ClearTarget() noexcept
    {
        // ResizeBuffers 前必须先从 D2D context 解绑并释放引用交换链 back buffer 的 target bitmap。
        if (this->d2dContext)
        {
            this->d2dContext->SetTarget(nullptr);
        }
        this->targetBitmap.Reset();
    }

    // 从当前交换链 back buffer 创建 D2D 呈现目标，失败时返回可诊断错误。
    bool CreateTargetBitmap(std::wstring& errorMessage)
    {
        ComPtr<IDXGISurface> surface;
        HRESULT result = this->swapChain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()));
        if (FAILED(result))
        {
            errorMessage = FormatHResult(L"获取覆盖窗口交换链表面", result);
            return false;
        }

        // 交换链、客户区和冻结帧都以物理像素表示。固定 96 DPI 并配合 PIXELS unit mode，
        // 避免在高 DPI 显示器上把客户区像素再次当成 DIP 缩放。
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(this->pixelFormat, D2D1_ALPHA_MODE_IGNORE), 96.0F, 96.0F);
        result = this->d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                               this->targetBitmap.GetAddressOf());
        if (FAILED(result))
        {
            errorMessage = FormatHResult(L"创建 Direct2D 呈现目标", result);
            return false;
        }

        this->d2dContext->SetTarget(this->targetBitmap.Get());
        return true;
    }
};

// 创建空的 PImpl 容器，图形设备和窗口资源由 Initialize 延迟建立。
OverlayRenderer::OverlayRenderer() : impl_(std::make_unique<Impl>()) {}

// 销毁 PImpl，并由 ComPtr 自动释放仍持有的全部图形资源。
OverlayRenderer::~OverlayRenderer() = default;

// 为覆盖窗口创建 D3D/D2D/交换链资源，并一次性上传不可变桌面帧。
bool OverlayRenderer::Initialize(HWND window, const OutputPreviewFrame& frame,
                                 std::optional<std::string_view> configuredBorderColor,
                                 std::wstring& errorMessage)
{
    this->Reset();
    errorMessage.clear();
    const bool hdr = frame.pixelFormat == CapturedPixelFormat::Rgba16FloatScRgb;
    const std::size_t bytesPerPixel = hdr ? 8U : 4U;
    if (window == nullptr || frame.bounds.IsEmpty() ||
        (!hdr && frame.pixelFormat != CapturedPixelFormat::Bgra8Unorm) ||
        frame.stride != static_cast<std::size_t>(frame.bounds.Width()) * bytesPerPixel ||
        frame.pixels.size() != static_cast<std::size_t>(frame.stride) * frame.bounds.Height() ||
        !std::isfinite(frame.uiWhiteScale) || frame.uiWhiteScale <= 0.0F)
    {
        errorMessage = L"覆盖渲染器收到了无效的窗口或桌面帧。";
        return false;
    }

    this->impl_->window = window;
    this->impl_->frameBounds = frame.bounds;
    this->impl_->pixelFormat = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    this->impl_->uiWhiteScale = frame.uiWhiteScale;
    this->impl_->colorMetadata = frame.colorMetadata;
    constexpr std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedLevel{};
    const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    // 优先使用硬件设备；硬件初始化失败时用 WARP 软件光栅器保证覆盖窗口仍有可用后端。
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, featureLevels.data(),
                                       static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                                       this->impl_->d3dDevice.GetAddressOf(), &selectedLevel,
                                       this->impl_->d3dContext.GetAddressOf());
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, deviceFlags, featureLevels.data(),
                                   static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                                   this->impl_->d3dDevice.GetAddressOf(), &selectedLevel,
                                   this->impl_->d3dContext.GetAddressOf());
    }
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建覆盖窗口 D3D11 设备", result);
        this->Reset();
        return false;
    }

    // 同一底层 D3D11 device 依次派生 DXGI device、D2D device 和 D2D context，
    // 确保 Direct2D 能直接把内容画到 DXGI 交换链表面。
    ComPtr<IDXGIDevice> dxgiDevice;
    result = this->impl_->d3dDevice.As(&dxgiDevice);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取覆盖窗口 DXGI 设备", result);
        this->Reset();
        return false;
    }

    D2D1_FACTORY_OPTIONS factoryOptions{};
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &factoryOptions,
                               reinterpret_cast<void**>(this->impl_->d2dFactory.GetAddressOf()));
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建 Direct2D 1.1 工厂", result);
        this->Reset();
        return false;
    }

    result = this->impl_->d2dFactory->CreateDevice(dxgiDevice.Get(), this->impl_->d2dDevice.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建 Direct2D 设备", result);
        this->Reset();
        return false;
    }

    result = this->impl_->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                         this->impl_->d2dContext.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建 Direct2D 设备上下文", result);
        this->Reset();
        return false;
    }

    // 截图几何统一采用虚拟桌面物理像素。Direct2D 默认使用 DIP，若沿用窗口 DPI，
    // 高 DPI 或混合 DPI 环境会让交换链目标与冻结帧使用不同单位。
    this->impl_->d2dContext->SetDpi(96.0F, 96.0F);
    this->impl_->d2dContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    this->impl_->d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if (!this->impl_->d2dContext->IsDxgiFormatSupported(this->impl_->pixelFormat) ||
        (hdr && !this->impl_->d2dContext->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT)))
    {
        errorMessage = L"Direct2D 不支持当前输出所需的原生像素格式或 FP16 精度。";
        this->Reset();
        return false;
    }
    D2D1_RENDERING_CONTROLS renderingControls{};
    this->impl_->d2dContext->GetRenderingControls(&renderingControls);
    renderingControls.bufferPrecision = hdr ? D2D1_BUFFER_PRECISION_16BPC_FLOAT : D2D1_BUFFER_PRECISION_8BPC_UNORM;
    this->impl_->d2dContext->SetRenderingControls(renderingControls);

    ComPtr<IDXGIAdapter> adapter;
    result = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取覆盖窗口显示适配器", result);
        this->Reset();
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    result = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取 DXGI 1.2 工厂", result);
        this->Reset();
        return false;
    }

    // 双缓冲 flip model 由 DWM 负责合成；窗口本身不需要透明 alpha，因此使用 IGNORE。
    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Format = this->impl_->pixelFormat;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> baseSwapChain;
    result = factory->CreateSwapChainForHwnd(this->impl_->d3dDevice.Get(), window, &swapChainDescription, nullptr,
                                             nullptr, baseSwapChain.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建覆盖窗口交换链", result);
        this->Reset();
        return false;
    }
    // 截图工具不应响应 DXGI 默认的 Alt+Enter 全屏切换。
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    result = baseSwapChain.As(&this->impl_->swapChain);
    const DXGI_COLOR_SPACE_TYPE colorSpace = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceSupport{};
    if (SUCCEEDED(result))
    {
        result = this->impl_->swapChain->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport);
    }
    if (SUCCEEDED(result) && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0U)
    {
        result = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(result))
    {
        result = this->impl_->swapChain->SetColorSpace1(colorSpace);
    }
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"确认并设置覆盖窗口交换链颜色空间", result);
        this->Reset();
        return false;
    }

    // 工厂创建可能晚于捕获；再核对 HWND 实际关联的输出，封住捕获与初始化之间的配置变化窗口。
    if (frame.colorMetadata.deviceName[0] != L'\0')
    {
        ComPtr<IDXGIOutput> containingOutput;
        DXGI_OUTPUT_DESC outputDescription{};
        result = this->impl_->swapChain->GetContainingOutput(containingOutput.GetAddressOf());
        if (SUCCEEDED(result))
        {
            result = containingOutput->GetDesc(&outputDescription);
        }
        const RECT& desktop = outputDescription.DesktopCoordinates;
        if (FAILED(result) || std::wcscmp(outputDescription.DeviceName, frame.colorMetadata.deviceName.data()) != 0 ||
            desktop.left != frame.bounds.left || desktop.top != frame.bounds.top ||
            desktop.right != frame.bounds.right || desktop.bottom != frame.bounds.bottom)
        {
            errorMessage = L"覆盖交换链关联的显示输出与冻结帧不一致，请重新截图。";
            this->Reset();
            return false;
        }
        if (frame.colorMetadata.displayColorSpace != CapturedColorSpace::Unknown)
        {
            ComPtr<IDXGIOutput6> colorOutput;
            DXGI_OUTPUT_DESC1 colorDescription{};
            result = containingOutput.As(&colorOutput);
            if (SUCCEEDED(result))
            {
                result = colorOutput->GetDesc1(&colorDescription);
            }
            DXGI_COLOR_SPACE_TYPE expectedColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            if (frame.colorMetadata.displayColorSpace == CapturedColorSpace::Hdr10)
            {
                expectedColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            else if (frame.colorMetadata.displayColorSpace == CapturedColorSpace::ScRgb)
            {
                expectedColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            }
            if (FAILED(result) || colorDescription.ColorSpace != expectedColorSpace)
            {
                errorMessage = L"显示输出的高级颜色模式在捕获后发生变化，请重新截图。";
                this->Reset();
                return false;
            }
        }
    }
    this->impl_->dxgiFactory = factory;

    if (!this->impl_->CreateTargetBitmap(errorMessage))
    {
        this->Reset();
        return false;
    }

    // 冻结帧只在 Initialize 时从 CPU 上传一次；之后每次 WM_PAINT 都复用同一个 D2D 位图。
    const D2D1_BITMAP_PROPERTIES1 frameProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(this->impl_->pixelFormat, D2D1_ALPHA_MODE_IGNORE), 96.0F, 96.0F);
    result = this->impl_->d2dContext->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(frame.bounds.Width()), static_cast<UINT32>(frame.bounds.Height())),
        frame.pixels.data(), frame.stride, &frameProperties, this->impl_->frameBitmap.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"上传冻结桌面帧", result);
        this->Reset();
        return false;
    }

    result = this->impl_->d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),
                                                            this->impl_->dimBrush.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建截图遮罩画刷", result);
        this->Reset();
        return false;
    }
    // 画刷颜色保持不透明黑，透明度单独设置，明确要求 SourceOver 合成得到“变暗”而非覆盖黑色。
    this->impl_->dimBrush->SetOpacity(0.48F);

    result = this->impl_->d2dContext->CreateSolidColorBrush(
        ColorFromRgb(ResolveSelectionBorderColor(configuredBorderColor), hdr, frame.uiWhiteScale),
                                                            this->impl_->selectionBrush.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建选区边框画刷", result);
        this->Reset();
        return false;
    }

    result = this->impl_->d2dContext->CreateSolidColorBrush(ColorFromRgb(0xFFFFFFU, hdr, frame.uiWhiteScale),
                                                            this->impl_->handleFillBrush.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建选区控制点画刷", result);
        this->Reset();
        return false;
    }

    return true;
}

// 调整交换链缓冲区与客户区尺寸，同时保留设备和已上传的冻结帧位图。
bool OverlayRenderer::Resize(unsigned int width, unsigned int height, std::wstring& errorMessage)
{
    if (!this->impl_->swapChain || width == 0 || height == 0)
    {
        return true;
    }

    // 先解除 back buffer 的所有 D2D 引用，再让 DXGI 重新分配缓冲区。
    this->impl_->ClearTarget();
    const HRESULT result = this->impl_->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"调整覆盖窗口交换链", result);
        return false;
    }
    return this->impl_->CreateTargetBitmap(errorMessage);
}

// 按冻结帧、选区外暗层、选区边框与控制点的顺序绘制并呈现一帧。
bool OverlayRenderer::Render(SelectionSnapshot snapshot, std::wstring& errorMessage)
{
    return this->DrawFrame(snapshot, true, errorMessage);
}

// 共用实际 D2D 绘制路径；集成测试可在交换链翻转前读取原生精度像素。
bool OverlayRenderer::DrawFrame(SelectionSnapshot snapshot, bool present, std::wstring& errorMessage)
{
    errorMessage.clear();
    if (!this->impl_->d2dContext || !this->impl_->frameBitmap || !this->impl_->targetBitmap ||
        !this->impl_->dimBrush || !this->impl_->selectionBrush || !this->impl_->handleFillBrush)
    {
        errorMessage = L"覆盖渲染器尚未初始化。";
        return false;
    }
    if (!this->impl_->dxgiFactory->IsCurrent() ||
        !IsCapturedOutputColorStateCurrent(this->impl_->colorMetadata))
    {
        errorMessage = L"显示设备或高级颜色配置已变化，当前冻结截图会话必须重新建立。";
        return false;
    }

    RECT clientRectangle{};
    if (GetClientRect(this->impl_->window, &clientRectangle) == FALSE || clientRectangle.right <= 0 ||
        clientRectangle.bottom <= 0)
    {
        errorMessage = L"覆盖窗口客户区尺寸无效。";
        return false;
    }

    // 每屏窗口必须覆盖原物理像素矩形；不通过缩放掩盖过期布局，否则截图内容会失真。
    if (clientRectangle.right != this->impl_->frameBounds.Width() ||
        clientRectangle.bottom != this->impl_->frameBounds.Height())
    {
        errorMessage = L"覆盖窗口尺寸与冻结输出不一致，当前截图会话必须重新建立。";
        return false;
    }

    // 这里的数值与 unit mode 都是物理像素，源和目标严格 1:1，不进行色调映射或白亮度再缩放。
    const D2D1_RECT_F targetRectangle =
        D2D1::RectF(0.0F, 0.0F, static_cast<float>(clientRectangle.right), static_cast<float>(clientRectangle.bottom));
    const D2D1_RECT_F sourceRectangle =
        D2D1::RectF(0.0F, 0.0F, static_cast<float>(this->impl_->frameBitmap->GetPixelSize().width),
                    static_cast<float>(this->impl_->frameBitmap->GetPixelSize().height));

    // Clear 只是确定交换链初值；冻结帧覆盖全客户区，随后只在纯几何布局给出的选区外条带合成暗层。
    this->impl_->d2dContext->BeginDraw();
    this->impl_->d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    this->impl_->d2dContext->DrawBitmap(this->impl_->frameBitmap.Get(), targetRectangle, 1.0F,
                                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, sourceRectangle);

    const OutsideMaskLayout maskLayout =
        BuildOutsideMaskLayout(this->impl_->frameBounds, snapshot.hasSelection, snapshot.rectangle);
    for (std::size_t index = 0U; index < maskLayout.count; ++index)
    {
        const D2D1_RECT_F maskRectangle =
            ToClientRectangle(maskLayout.rectangles[index], this->impl_->frameBounds);
        this->impl_->d2dContext->FillRectangle(maskRectangle, this->impl_->dimBrush.Get());
    }

    if (snapshot.hasSelection)
    {
        // 保留完整全局选区轮廓，由呈现目标裁剪；不能先取本屏交集，否则拼缝会出现伪边框。
        const D2D1_RECT_F selectionRectangle =
            ToClientRectangle(snapshot.rectangle, this->impl_->frameBounds);
        FillOnePixelOutline(this->impl_->d2dContext.Get(), selectionRectangle, this->impl_->selectionBrush.Get());

        if (snapshot.showHandles)
        {
            for (const SelectionHandlePosition& handle : snapshot.handles)
            {
                const float centerX = static_cast<float>(handle.center.x - this->impl_->frameBounds.left);
                const float centerY = static_cast<float>(handle.center.y - this->impl_->frameBounds.top);
                const D2D1_RECT_F handleRectangle =
                    D2D1::RectF(centerX - HANDLE_VISUAL_HALF_SIZE, centerY - HANDLE_VISUAL_HALF_SIZE,
                                centerX + HANDLE_VISUAL_HALF_SIZE, centerY + HANDLE_VISUAL_HALF_SIZE);
                this->impl_->d2dContext->FillRectangle(handleRectangle, this->impl_->handleFillBrush.Get());
                FillOnePixelOutline(this->impl_->d2dContext.Get(), handleRectangle, this->impl_->selectionBrush.Get());
            }
        }
    }

    // EndDraw 提交 D2D 命令并报告设备状态；成功后 Present(1) 与桌面刷新同步呈现。
    HRESULT result = this->impl_->d2dContext->EndDraw();
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"绘制截图覆盖窗口", result);
        return false;
    }

    if (!present)
    {
        return true;
    }
    result = this->impl_->swapChain->Present(1, 0);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"呈现截图覆盖窗口", result);
        return false;
    }
    return true;
}

// 从尚未翻转的后缓冲复制到 staging 纹理，验证 D2D 上传及绘制没有截断 HDR 数值。
bool OverlayRenderer::ReadbackFrame(OutputPreviewFrame& frame, std::wstring& errorMessage)
{
    frame = {};
    if (!this->impl_->swapChain)
    {
        errorMessage = L"读取覆盖后缓冲前必须初始化渲染器。";
        return false;
    }
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = this->impl_->swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取测试后缓冲", result);
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0U;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0U;
    ComPtr<ID3D11Texture2D> staging;
    result = this->impl_->d3dDevice->CreateTexture2D(&description, nullptr, staging.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建测试回读纹理", result);
        return false;
    }
    const bool hdr = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    frame.bounds = this->impl_->frameBounds;
    frame.pixelFormat = hdr ? CapturedPixelFormat::Rgba16FloatScRgb : CapturedPixelFormat::Bgra8Unorm;
    frame.stride = description.Width * (hdr ? 8U : 4U);
    frame.uiWhiteScale = this->impl_->uiWhiteScale;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * description.Height);
    this->impl_->d3dContext->CopyResource(staging.Get(), backBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = this->impl_->d3dContext->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped);
    if (FAILED(result))
    {
        frame = {};
        errorMessage = FormatHResult(L"映射测试回读纹理", result);
        return false;
    }
    for (UINT row = 0U; row < description.Height; ++row)
    {
        std::memcpy(frame.pixels.data() + static_cast<std::size_t>(row) * frame.stride,
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                    frame.stride);
    }
    this->impl_->d3dContext->Unmap(staging.Get(), 0U);
    return true;
}

// 按依赖逆序释放画刷、位图、D2D、交换链和 D3D 资源，恢复未初始化状态。
void OverlayRenderer::Reset() noexcept
{
    if (!this->impl_)
    {
        return;
    }

    // 按资源依赖的逆序释放：画刷/位图 → D2D target/context/device → swap chain → D3D device。
    this->impl_->handleFillBrush.Reset();
    this->impl_->selectionBrush.Reset();
    this->impl_->dimBrush.Reset();
    this->impl_->frameBitmap.Reset();
    this->impl_->ClearTarget();
    this->impl_->d2dContext.Reset();
    this->impl_->d2dDevice.Reset();
    this->impl_->d2dFactory.Reset();
    this->impl_->swapChain.Reset();
    this->impl_->dxgiFactory.Reset();
    this->impl_->d3dContext.Reset();
    this->impl_->d3dDevice.Reset();
    this->impl_->window = nullptr;
    this->impl_->frameBounds = {};
    this->impl_->pixelFormat = DXGI_FORMAT_UNKNOWN;
    this->impl_->uiWhiteScale = 1.0F;
    this->impl_->colorMetadata = {};
}
} // namespace open_st
