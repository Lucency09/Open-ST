// 文件职责：使用独立 D3D11、Direct2D 和 DirectComposition 资源呈现贴图，并保留原图供设备恢复。

#include "pin_renderer.h"
#include <pin_image.h>

#include <cmath>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace open_st
{
using Microsoft::WRL::ComPtr;

struct PinRenderer::Impl final
{
    HWND window{};
    std::shared_ptr<const PinImage> image;
    ComPtr<ID3D11Device> device;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> drawingDevice;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1Bitmap1> sourceBitmap;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    int width{};
    int height{};

    // 解除窗口合成树并按引用方向释放资源，保留重建所需窗口和不可变原图。
    // 入参：无。
    // 返回：无返回值；图形资源和已分配交换链尺寸清零。
    void ReleaseGraphics() noexcept
    {
        if (this->target)
            this->target->SetRoot(nullptr);
        if (this->composition)
            this->composition->Commit();
        if (this->context)
            this->context->SetTarget(nullptr);
        this->visual.Reset();
        this->effect.Reset();
        this->target.Reset();
        this->composition.Reset();
        this->sourceBitmap.Reset();
        this->targetBitmap.Reset();
        this->context.Reset();
        this->drawingDevice.Reset();
        this->factory.Reset();
        this->swapChain.Reset();
        this->device.Reset();
        this->width = 0;
        this->height = 0;
    }

    // 将交换链后缓冲区绑定为 Direct2D 绘制目标。
    // 入参：无；消费本对象已创建的交换链和设备上下文。
    // 返回：绑定成功为 S_OK，失败为对应图形接口的 HRESULT。
    HRESULT BindBuffer()
    {
        ComPtr<IDXGISurface> surface;
        HRESULT result = this->swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (FAILED(result))
            return result;
        const D2D1_BITMAP_PROPERTIES1 properties =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        result = this->context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &this->targetBitmap);
        if (SUCCEEDED(result))
            this->context->SetTarget(this->targetBitmap.Get());
        return result;
    }

    // 创建窗口独占的图形管线并上传第四字节固定为 255 的显示副本。
    // 入参：newWidth、newHeight 为首帧物理尺寸；原图及窗口须已经设置。
    // 返回：成功为 S_OK，失败保留 HRESULT 供外层清理及诊断。
    HRESULT Build(int newWidth, int newHeight)
    {
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &this->device, nullptr, nullptr);
        if (FAILED(result))
        {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &this->device, nullptr, nullptr);
        }
        if (FAILED(result))
            return result;
        ComPtr<IDXGIDevice> dxgiDevice;
        result = this->device.As(&dxgiDevice);
        if (FAILED(result))
            return result;
        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result))
            return result;
        ComPtr<IDXGIFactory2> dxgiFactory;
        result = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(result))
            return result;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(newWidth);
        description.Height = static_cast<UINT>(newHeight);
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        result =
            dxgiFactory->CreateSwapChainForComposition(this->device.Get(), &description, nullptr, &this->swapChain);
        if (FAILED(result))
            return result;
        result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, this->factory.GetAddressOf());
        if (FAILED(result))
            return result;
        result = this->factory->CreateDevice(dxgiDevice.Get(), &this->drawingDevice);
        if (FAILED(result))
            return result;
        result = this->drawingDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &this->context);
        if (FAILED(result))
            return result;
        this->context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        result = this->BindBuffer();
        if (FAILED(result))
            return result;
        const std::span<const std::uint8_t> original = this->image->Pixels();
        std::vector<std::uint8_t> opaque(original.begin(), original.end());
        for (std::size_t offset = 3; offset < opaque.size(); offset += 4U)
            opaque[offset] = 255;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        result = this->context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(this->image->Width()), static_cast<UINT>(this->image->Height())),
            opaque.data(), static_cast<UINT32>(this->image->Stride()), &properties, &this->sourceBitmap);
        if (FAILED(result))
            return result;
        result = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&this->composition));
        if (FAILED(result))
            return result;
        result = this->composition->CreateTargetForHwnd(this->window, TRUE, &this->target);
        if (FAILED(result))
            return result;
        result = this->composition->CreateVisual(&this->visual);
        if (FAILED(result))
            return result;
        result = this->visual->SetContent(this->swapChain.Get());
        if (FAILED(result))
            return result;
        result = this->composition->CreateEffectGroup(&this->effect);
        if (FAILED(result))
            return result;
        result = this->visual->SetEffect(this->effect.Get());
        if (FAILED(result))
            return result;
        result = this->target->SetRoot(this->visual.Get());
        if (FAILED(result))
            return result;
        this->width = newWidth;
        this->height = newHeight;
        return S_OK;
    }

    // 调整后缓冲区并提交当前原图和独立合成透明度。
    // 入参：newWidth、newHeight 为校验后的物理尺寸；opacity 为校验后的不透明度。
    // 返回：绘制、呈现和合成提交均成功时为 S_OK，否则返回首次失败 HRESULT。
    HRESULT Draw(int newWidth, int newHeight, float opacity)
    {
        HRESULT result = S_OK;
        if (newWidth != this->width || newHeight != this->height)
        {
            // 上一帧 EndDraw 已提交批次；解绑目标后不能再 Flush，否则 Direct2D 返回状态错误。
            this->context->SetTarget(nullptr);
            this->targetBitmap.Reset();
            result = this->swapChain->ResizeBuffers(0, static_cast<UINT>(newWidth), static_cast<UINT>(newHeight),
                                                    DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(result))
                return result;
            result = this->BindBuffer();
            if (FAILED(result))
                return result;
            this->width = newWidth;
            this->height = newHeight;
        }
        this->context->BeginDraw();
        this->context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        const D2D1_RECT_F destination = D2D1::RectF(0, 0, static_cast<float>(newWidth), static_cast<float>(newHeight));
        this->context->DrawBitmap(this->sourceBitmap.Get(), &destination, 1.0F, D2D1_INTERPOLATION_MODE_LINEAR);
        result = this->context->EndDraw();
        if (FAILED(result))
            return result;
        result = this->swapChain->Present(1, 0);
        if (FAILED(result))
            return result;
        result = this->effect->SetOpacity(opacity);
        if (FAILED(result))
            return result;
        return this->composition->Commit();
    }
};

// 分配尚未绑定窗口的图形资源容器。
// 入参：无。
// 返回：无返回值；内存分配失败向调用方传播异常。
PinRenderer::PinRenderer() : impl_(std::make_unique<Impl>()) {}

// 在宿主窗口销毁前解除合成树并释放图像引用。
// 入参：无。
// 返回：无返回值。
PinRenderer::~PinRenderer()
{
    this->Reset();
}

// 校验窗口与原图并完成隐藏窗口的第一帧呈现。
// 入参：window 为有效窗口；image 为不可变原图；error 接收日志诊断。
// 返回：首帧完整提交为 true，失败为 false 且对象回到未初始化状态。
bool PinRenderer::Initialize(HWND window, std::shared_ptr<const PinImage> image, std::wstring& error)
{
    this->Reset();
    error.clear();
    if (!IsWindow(window) || !image)
    {
        error = L"贴图渲染窗口或原图无效。";
        return false;
    }
    this->impl_->window = window;
    this->impl_->image = std::move(image);
    try
    {
        HRESULT result = this->impl_->Build(this->impl_->image->Width(), this->impl_->image->Height());
        if (SUCCEEDED(result))
            result = this->impl_->Draw(this->impl_->width, this->impl_->height, 1.0F);
        if (SUCCEEDED(result))
            result = this->impl_->composition->WaitForCommitCompletion();
        if (SUCCEEDED(result))
            return true;
        error = L"创建贴图首帧失败，HRESULT=" + std::to_wstring(static_cast<unsigned long>(result));
    }
    catch (...)
    {
        error = L"创建贴图显示资源时发生异常。";
    }
    this->Reset();
    return false;
}

// 绘制指定尺寸和透明度，设备失效时仅尝试重建一次。
// 入参：width、height 为正物理尺寸且不超过 16384；opacity 为有限的 0～1；error 接收诊断。
// 返回：提交成功为 true；无效输入或图形错误为 false。
bool PinRenderer::Render(int width, int height, float opacity, std::wstring& error)
{
    error.clear();
    if (!this->impl_->image || !this->impl_->context || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        !std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F)
    {
        error = L"贴图渲染状态、尺寸或透明度无效。";
        return false;
    }
    try
    {
        HRESULT result = this->impl_->Draw(width, height, opacity);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
            result == D2DERR_RECREATE_TARGET)
        {
            this->impl_->ReleaseGraphics();
            result = this->impl_->Build(width, height);
            if (SUCCEEDED(result))
                result = this->impl_->Draw(width, height, opacity);
        }
        if (SUCCEEDED(result))
            return true;
        error = L"呈现贴图失败，HRESULT=" + std::to_wstring(static_cast<unsigned long>(result));
    }
    catch (...)
    {
        error = L"恢复贴图显示资源时发生异常。";
    }
    this->impl_->ReleaseGraphics();
    return false;
}

// 回收全部图形资源并移除原图及窗口关联。
// 入参：无。
// 返回：无返回值；重复调用安全。
void PinRenderer::Reset() noexcept
{
    this->impl_->ReleaseGraphics();
    this->impl_->image.reset();
    this->impl_->window = nullptr;
}
} // namespace open_st
