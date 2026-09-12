// 文件职责：实现单屏 Desktop Duplication、原生格式协商、GPU 回读与捕获资源的 RAII 清理。

#include "output_capture.h"

#include "captured_plane_writer.h"
#include "display_color_info.h"
#include "output_capture_policy.h"

#include <log.h>
#include <windows_util.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT CAPTURE_TIMEOUT_MILLISECONDS = 500;

using open_st::FormatHResult;

// 在所有退出路径上归还已经 AcquireNextFrame 的 Desktop Duplication 帧。
class AcquiredFrameGuard final
{
  public:
    // 建立桌面复制帧归还守卫，确保成功获取的帧最终释放。
    // 入参：duplication：调用期间保持有效的借用桌面复制接口。
    // 返回：无返回值；初始状态尚未取得帧，获取成功后须调用 MarkAcquired。
    explicit AcquiredFrameGuard(IDXGIOutputDuplication* duplication) noexcept : duplication_(duplication) {}
    // 在离开捕获作用域时归还尚未显式释放的桌面复制帧。
    // 入参：无。
    // 返回：无返回值；析构完成对应资源清理。
    ~AcquiredFrameGuard()
    {
        if (this->acquired_ && this->duplication_ != nullptr)
        {
            (void)this->duplication_->ReleaseFrame();
        }
    }

    // 禁止复制 guard，避免同一 Desktop Duplication 帧被释放两次。
    // 入参：未命名 const AcquiredFrameGuard 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    AcquiredFrameGuard(const AcquiredFrameGuard&) = delete;
    // 禁止复制赋值，保持释放职责唯一。
    // 入参：未命名 const AcquiredFrameGuard 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    AcquiredFrameGuard& operator=(const AcquiredFrameGuard&) = delete;

    // 登记桌面复制帧已成功获取，使守卫负责后续归还。
    // 入参：无。
    // 返回：无返回值；acquired_ 被设置为 true，后续 Release 或析构将尝试归还帧。
    void MarkAcquired() noexcept
    {
        this->acquired_ = true;
    }

    // 显式归还已获取的桌面复制帧并报告系统释放错误。
    // 入参：errorMessage：输出参数，ReleaseFrame 失败时写入阶段和 HRESULT。
    // 返回：无需归还或成功归还时为 true；ReleaseFrame 失败时为 false，已获取标记均清除以避免重复归还。
    [[nodiscard]] bool Release(std::wstring& errorMessage)
    {
        if (!this->acquired_ || this->duplication_ == nullptr)
        {
            return true;
        }
        const HRESULT result = this->duplication_->ReleaseFrame();
        this->acquired_ = false;
        if (FAILED(result))
        {
            errorMessage = FormatHResult(L"释放桌面复制帧", result);
            return false;
        }
        return true;
    }

  private:
    IDXGIOutputDuplication* duplication_{};
    bool acquired_{};
};

// 在所有退出路径上解除 D3D11 staging texture 的 CPU 映射。
class MappedTextureGuard final
{
  public:
    // 接管一块已映射纹理的解除映射职责，确保提前退出时也执行 Unmap。
    // 入参：context：借用的 D3D11 立即上下文；texture：已成功 Map 且等待解除映射的借用纹理。
    // 返回：无返回值；守卫开始负责解除该纹理映射。
    MappedTextureGuard(ID3D11DeviceContext* context, ID3D11Texture2D* texture) noexcept
        : context_(context), texture_(texture)
    {
    }
    // 在离开像素回读作用域时解除尚未显式解除的纹理映射。
    // 入参：无。
    // 返回：无返回值；析构完成对应资源清理。
    ~MappedTextureGuard()
    {
        this->Unmap();
    }

    // 禁止复制 guard，避免同一纹理被重复 Unmap。
    // 入参：未命名 const MappedTextureGuard 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    MappedTextureGuard(const MappedTextureGuard&) = delete;
    // 禁止复制赋值，保持 Unmap 职责唯一。
    // 入参：未命名 const MappedTextureGuard 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    MappedTextureGuard& operator=(const MappedTextureGuard&) = delete;

    // 解除 staging texture 的 CPU 映射并清除守卫借用的资源指针。
    // 入参：无。
    // 返回：无返回值；有效映射被解除，context 和 texture 指针清空，重复调用无效果。
    void Unmap() noexcept
    {
        if (this->context_ != nullptr && this->texture_ != nullptr)
        {
            this->context_->Unmap(this->texture_, 0U);
            this->context_ = nullptr;
            this->texture_ = nullptr;
        }
    }

  private:
    ID3D11DeviceContext* context_{};
    ID3D11Texture2D* texture_{};
};

// 读取 DXGI 扩展颜色能力，用于确定原生像素转换和兼容状态。
// 入参：output：借用的 DXGI 显示输出对象。
// 返回：已读取的颜色空间、位深及亮度元数据；扩展接口不可用或读取失败时对应字段保持未知或零。
open_st::OutputColorMetadata ReadOutputColorMetadata(IDXGIOutput* output) noexcept
{
    open_st::OutputColorMetadata metadata{};
    ComPtr<IDXGIOutput6> output6;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output6.GetAddressOf()))))
    {
        DXGI_OUTPUT_DESC1 description{};
        if (SUCCEEDED(output6->GetDesc1(&description)))
        {
            metadata.displayColorSpace = open_st::MapCapturedColorSpace(description.ColorSpace);
            metadata.bitsPerColor = description.BitsPerColor;
            metadata.minimumLuminance = description.MinLuminance;
            metadata.maximumLuminance = description.MaxLuminance;
            metadata.maximumFullFrameLuminance = description.MaxFullFrameLuminance;
        }
    }
    return metadata;
}

// 优先建立高色深桌面复制会话，仅在明确兼容条件下使用旧接口。
// 入参：output：借用的显示输出；device：该适配器的 D3D11
// 设备；duplication：输出参数，接收复制会话；usedLegacyFallback：输出参数，标识是否采用旧接口；errorMessage：输出参数，失败时接收诊断。
// 返回：复制会话建立成功时为 true；接口或创建失败且无法受控回退时为 false，并写入诊断。
bool CreateDuplication(IDXGIOutput* output, ID3D11Device* device, ComPtr<IDXGIOutputDuplication>& duplication,
                       bool& usedLegacyFallback, std::wstring& errorMessage)
{
    usedLegacyFallback = false;
    ComPtr<IDXGIOutput5> output5;
    const HRESULT queryResult = output->QueryInterface(IID_PPV_ARGS(output5.GetAddressOf()));
    if (SUCCEEDED(queryResult))
    {
        const std::span<const DXGI_FORMAT> supportedFormats = open_st::PreferredDuplicationFormats();
        const HRESULT duplicateResult =
            output5->DuplicateOutput1(device, 0U, static_cast<UINT>(supportedFormats.size()), supportedFormats.data(),
                                      duplication.GetAddressOf());
        if (SUCCEEDED(duplicateResult))
        {
            return true;
        }
        if (!open_st::ShouldUseLegacyDuplication(queryResult, duplicateResult))
        {
            errorMessage = FormatHResult(L"创建高色深桌面复制会话", duplicateResult);
            return false;
        }
    }
    else if (!open_st::ShouldUseLegacyDuplication(queryResult, E_FAIL))
    {
        errorMessage = FormatHResult(L"获取 IDXGIOutput5", queryResult);
        return false;
    }

    duplication.Reset();
    ComPtr<IDXGIOutput1> output1;
    HRESULT result = output->QueryInterface(IID_PPV_ARGS(output1.GetAddressOf()));
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取 IDXGIOutput1", result);
        return false;
    }
    result = output1->DuplicateOutput(device, duplication.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建兼容桌面复制会话", result);
        return false;
    }
    usedLegacyFallback = true;
    OPEN_ST_LOG_WARNING("High-color desktop duplication is unavailable; using the legacy SDR-compatible path.");
    return true;
}
} // namespace

namespace open_st
{
// 捕获单显示输出并回读原生像素，形成不依赖 DXGI 资源的冻结 plane。
// 入参：output：借用的显示输出；device：对应适配器的 D3D11
// 设备；context：该设备的立即上下文；capturedOutput：输出参数，接收自有原生
// plane；errorMessage：输出参数，接收失败诊断。
// 返回：捕获、映射、复制及资源归还成功时为 true；失败为 false，capturedOutput 保持无效。
bool CaptureSingleOutput(IDXGIOutput* output, ID3D11Device* device, ID3D11DeviceContext* context,
                         CapturedOutputPlane& capturedOutput, std::wstring& errorMessage)
{
    capturedOutput = {};
    errorMessage.clear();
    if (output == nullptr || device == nullptr || context == nullptr)
    {
        errorMessage = L"桌面捕获输出或 D3D11 设备无效。";
        return false;
    }

    DXGI_OUTPUT_DESC outputDescription{};
    HRESULT result = output->GetDesc(&outputDescription);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"读取显示输出信息", result);
        return false;
    }

    OutputColorMetadata metadata = ReadOutputColorMetadata(output);
    ReadDisplayColorInfo(output, outputDescription, metadata);
    ComPtr<IDXGIOutputDuplication> duplication;
    bool usedLegacyFallback{};
    if (!CreateDuplication(output, device, duplication, usedLegacyFallback, errorMessage))
    {
        return false;
    }
    metadata.usedLegacyDuplication = usedLegacyFallback;

    DXGI_OUTDUPL_FRAME_INFO frameInformation{};
    ComPtr<IDXGIResource> desktopResource;
    AcquiredFrameGuard frameGuard(duplication.Get());
    bool acquiredDesktopImage = false;
    unsigned int pointerOnlyFrameCount = 0;
    // 每个输出共用一个等待预算，鼠标更新不重置截止时间，也不消耗固定重试次数。
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(CAPTURE_TIMEOUT_MILLISECONDS);
    while (true)
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            break;
        }
        const UINT remainingMilliseconds =
            static_cast<UINT>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
        frameInformation = {};
        desktopResource.Reset();
        result =
            duplication->AcquireNextFrame(remainingMilliseconds, &frameInformation, desktopResource.GetAddressOf());
        if (result == DXGI_ERROR_WAIT_TIMEOUT)
        {
            break;
        }
        if (FAILED(result))
        {
            errorMessage = FormatHResult(L"获取桌面复制帧", result);
            return false;
        }
        frameGuard.MarkAcquired();
        if (frameInformation.LastPresentTime.QuadPart != 0)
        {
            acquiredDesktopImage = true;
            break;
        }

        ++pointerOnlyFrameCount;
        desktopResource.Reset();
        if (!frameGuard.Release(errorMessage))
        {
            return false;
        }
    }
    if (!acquiredDesktopImage)
    {
        std::wostringstream message;
        message << L"等待桌面复制图像帧超时，显示输出=" << outputDescription.DeviceName << L"，等待预算="
                << CAPTURE_TIMEOUT_MILLISECONDS << L"ms，已跳过仅指针更新帧=" << pointerOnlyFrameCount << L"。";
        errorMessage = message.str();
        return false;
    }

    ComPtr<ID3D11Texture2D> desktopTexture;
    result = desktopResource.As(&desktopTexture);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"获取桌面 D3D11 纹理", result);
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    desktopTexture->GetDesc(&textureDescription);
    CapturedPixelFormat format{};
    if (!TryMapCapturedPixelFormat(textureDescription.Format, format))
    {
        std::wostringstream message;
        message << L"桌面复制返回了不支持的像素格式，DXGI_FORMAT="
                << static_cast<unsigned int>(textureDescription.Format) << L"。";
        errorMessage = message.str();
        return false;
    }
    metadata.systemConvertedToSdr = WasSystemConvertedToSdr(format, metadata.displayColorSpace);

    D3D11_TEXTURE2D_DESC stagingDescription = textureDescription;
    stagingDescription.BindFlags = 0U;
    stagingDescription.MiscFlags = 0U;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stagingTexture;
    result = device->CreateTexture2D(&stagingDescription, nullptr, stagingTexture.GetAddressOf());
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"创建桌面读取纹理", result);
        return false;
    }

    context->CopyResource(stagingTexture.Get(), desktopTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(stagingTexture.Get(), 0U, D3D11_MAP_READ, 0U, &mapped);
    if (FAILED(result))
    {
        errorMessage = FormatHResult(L"映射桌面读取纹理", result);
        return false;
    }
    MappedTextureGuard mappedGuard(context, stagingTexture.Get());

    DXGI_OUTDUPL_DESC duplicationDescription{};
    duplication->GetDesc(&duplicationDescription);
    CapturedSurfaceRotation rotation{};
    if (!TryMapCapturedRotation(duplicationDescription.Rotation, rotation))
    {
        errorMessage = L"显示输出使用了未知旋转模式。";
        return false;
    }

    const RectI bounds{outputDescription.DesktopCoordinates.left, outputDescription.DesktopCoordinates.top,
                       outputDescription.DesktopCoordinates.right, outputDescription.DesktopCoordinates.bottom};
    const MappedCaptureSurface surface{static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch,
                                       static_cast<int>(textureDescription.Width),
                                       static_cast<int>(textureDescription.Height), format};
    const CapturedColorSpace pixelColorSpace = ResolveCapturedPixelColorSpace(format, metadata.displayColorSpace);
    const bool copied =
        BuildCapturedOutputPlane(surface, bounds, rotation, pixelColorSpace, metadata, capturedOutput, errorMessage);
    mappedGuard.Unmap();
    if (!frameGuard.Release(errorMessage))
    {
        capturedOutput = {};
        return false;
    }
    return copied;
}
} // namespace open_st
