// 文件职责：实现 HDR 像素解码及 Windows 原生效果色调映射，验证效果属性并回读 SDR 输出。

#include <algorithm>
#include <array>
#include <cmath>
#include <color_conversion.h>
#include <cstring>
#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <limits>
#include <log.h>
#include <native_tone_mapper.h>
#include <sstream>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
// 检查图形调用 HRESULT，并在失败时补充具体效果阶段诊断。
// 入参：result：待检查的 HRESULT；stage：图形调用阶段的宽字符描述；error：输出参数，失败时写入阶段及 HRESULT。
// 返回：HRESULT 成功时为 true；失败时为 false 并写入 error。
bool Check(HRESULT result, const wchar_t* stage, std::wstring& error)
{
    if (SUCCEEDED(result))
    {
        return true;
    }
    std::wostringstream stream;
    stream << stage << L"失败，HRESULT=0x" << std::hex << static_cast<unsigned long>(result);
    error = stream.str();
    return false;
}

// 设置并回读原生图像效果属性，确认效果接受所请求的参数。
// 入参：effect：借用的 D2D 效果对象；property：效果属性索引；value：拟写入的属性值；error：输出参数，接收失败诊断。
//       模板参数 T：支持效果属性读写及相等比较的属性值类型。
// 返回：属性写入、读取均成功且回读值一致时为 true；任一步失败或值不一致时为 false。
template <typename T> bool SetChecked(ID2D1Effect* effect, UINT32 property, T value, std::wstring& error)
{
    T actual{};
    if (!Check(effect->SetValue(property, value), L"设置原生效果属性", error) ||
        !Check(effect->GetValue(property, &actual), L"读取原生效果属性", error))
    {
        return false;
    }
    if (actual != value)
    {
        error = L"原生效果未接受请求的属性值。";
        return false;
    }
    return true;
}

// 将借用 HDR 图像解码为紧凑 FP16/scRGB，并统计内容峰值用于原生色调映射。
// 入参：image：HDR 原始像素、尺寸和字节行跨度；linear：输出参数，接收 FP16 RGBA 通道；peakNits：输出参数，接收最大正 RGB 亮度，单位
// nit；error：输出参数，接收输入或颜色异常诊断。
// 返回：完成全部像素解码时为 true；尺寸、格式或通道非法时为 false，linear 可能包含尚未完成的数据。
bool Decode(const open_st::HdrImageView& image, std::vector<std::uint16_t>& linear, float& peakNits,
            std::wstring& error)
{
    const bool fp16 = image.format == open_st::HdrPixelFormat::Rgba16FloatScRgb;
    const std::size_t pixelBytes = fp16 ? 8U : 4U;
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * pixelBytes;
    if ((!fp16 && image.format != open_st::HdrPixelFormat::Rgb10A2ScRgb &&
         image.format != open_st::HdrPixelFormat::Rgb10A2Hdr10) ||
        image.width == 0 || image.height == 0 || image.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        image.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || image.stride < rowBytes ||
        (image.height > 1U &&
         image.stride > (std::numeric_limits<std::size_t>::max() - rowBytes) / (image.height - 1U)) ||
        image.pixels.size() < image.stride * (image.height - 1U) + rowBytes)
    {
        error = L"HDR 输入尺寸、格式或行跨度无效。";
        return false;
    }
    linear.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
    peakNits = 0.0F;
    if (fp16)
    {
        // binary16 的非负有限位模式按数值单调排列；直接扫描避免逐像素解码再编码的额外成本。
        std::uint16_t peakBits{};
        for (std::uint32_t y = 0U; y < image.height; ++y)
        {
            std::uint16_t* destination = linear.data() + static_cast<std::size_t>(y) * image.width * 4U;
            std::memcpy(destination, image.pixels.data() + static_cast<std::size_t>(y) * image.stride, rowBytes);
            for (std::uint32_t x = 0U; x < image.width; ++x)
            {
                const std::size_t offset = static_cast<std::size_t>(x) * 4U;
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                {
                    const std::uint16_t bits = destination[offset + channel];
                    if ((bits & 0x7C00U) == 0x7C00U)
                    {
                        error = L"HDR 图像包含非有限 RGB 分量。";
                        return false;
                    }
                    if ((bits & 0x8000U) == 0U && bits > peakBits)
                    {
                        peakBits = bits;
                    }
                }
                destination[offset + 3U] = 0x3C00U;
            }
        }
        peakNits = open_st::DecodeFloat16(peakBits) * 80.0F;
        return true;
    }
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const std::uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            std::uint32_t packed{};
            std::memcpy(&packed, row + static_cast<std::size_t>(x) * 4U, 4U);
            const open_st::LinearScRgb rgb = open_st::DecodeRgb10A2ToScRgb(
                packed, image.format == open_st::HdrPixelFormat::Rgb10A2Hdr10 ? open_st::Rgb10ColorSpace::Hdr10
                                                                              : open_st::Rgb10ColorSpace::ScRgb);
            if (!std::isfinite(rgb.red) || !std::isfinite(rgb.green) || !std::isfinite(rgb.blue))
            {
                error = L"HDR 图像包含非有限 RGB 分量。";
                return false;
            }
            peakNits = std::max(peakNits, std::max({rgb.red, rgb.green, rgb.blue}) * 80.0F);
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            linear[index] = open_st::EncodeFloat16(rgb.red);
            linear[index + 1U] = open_st::EncodeFloat16(rgb.green);
            linear[index + 2U] = open_st::EncodeFloat16(rgb.blue);
            linear[index + 3U] = open_st::EncodeFloat16(1.0F);
        }
    }
    return true;
}
} // namespace

namespace open_st
{
// DirectX 对象全部留在 HDR 内部；每张图的位图由同步转换栈拥有。
class NativeToneMapper::Impl final
{
  public:
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Effect> tone;
    ComPtr<ID2D1Effect> white;
    ComPtr<ID2D1Effect> color;
    ComPtr<ID2D1ColorContext> sourceColor;
    ComPtr<ID2D1ColorContext> destinationColor;
    DWORD threadId{};
    bool initialized{};
    bool attempted{};
    bool warpAttempted{};
    std::wstring initializationError;

    // 断开效果图对单张图像的引用并释放图像资源，降低转换后的驻留内存。
    // 入参：无。
    // 返回：无返回值；输入、输出图像引用与 D2D 图像缓存被释放，设备和可复用效果仍保留。
    void ReleaseImages() noexcept
    {
        if (this->tone)
        {
            this->tone->SetInput(0, nullptr);
        }
        if (this->context)
        {
            this->context->SetTarget(nullptr);
        }
        if (this->device)
        {
            this->device->ClearResources(0U);
        }
        if (this->d3dContext)
        {
            this->d3dContext->ClearState();
            this->d3dContext->Flush();
        }
    }

    // 按指定驱动类型创建并验证原生色调映射所需的 D3D/D2D 设备和效果。
    // 入参：driver：D3D 硬件或 WARP 驱动类型；error：输出参数，接收设备或效果初始化失败原因。
    // 返回：设备及所需效果均可用时为 true；初始化或属性校验失败时为 false。
    bool InitializeDriver(D3D_DRIVER_TYPE driver, std::wstring& error)
    {
        this->color.Reset();
        this->white.Reset();
        this->tone.Reset();
        this->sourceColor.Reset();
        this->destinationColor.Reset();
        this->context.Reset();
        this->device.Reset();
        this->factory.Reset();
        this->d3dContext.Reset();
        this->d3dDevice.Reset();
        D3D_FEATURE_LEVEL level{};
        if (!Check(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0U,
                                     D3D11_SDK_VERSION, this->d3dDevice.GetAddressOf(), &level,
                                     this->d3dContext.GetAddressOf()),
                   L"创建 HDR D3D 设备", error))
        {
            return false;
        }
        ComPtr<IDXGIDevice> dxgi;
        const D2D1_FACTORY_OPTIONS options{};
        if (!Check(this->d3dDevice.As(&dxgi), L"获取 HDR DXGI 设备", error) ||
            !Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                     reinterpret_cast<void**>(this->factory.GetAddressOf())),
                   L"创建 HDR D2D 工厂", error) ||
            !Check(this->factory->CreateDevice(dxgi.Get(), this->device.GetAddressOf()), L"创建 HDR D2D 设备", error) ||
            !Check(this->device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, this->context.GetAddressOf()),
                   L"创建 HDR D2D 上下文", error))
        {
            return false;
        }
        this->context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        this->context->SetDpi(96.0F, 96.0F);
        if (!this->context->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT))
        {
            error = L"HDR 设备不支持 FP16 效果精度。";
            return false;
        }
        if (!Check(this->context->CreateEffect(CLSID_D2D1HdrToneMap, this->tone.GetAddressOf()), L"创建 HDR Tone Map",
                   error) ||
            !Check(this->context->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, this->white.GetAddressOf()),
                   L"创建 White Level", error) ||
            !Check(this->context->CreateEffect(CLSID_D2D1ColorManagement, this->color.GetAddressOf()),
                   L"创建 Color Management", error) ||
            !Check(this->context->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0U,
                                                     this->sourceColor.GetAddressOf()),
                   L"创建 scRGB 色彩上下文", error) ||
            !Check(this->context->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0U,
                                                     this->destinationColor.GetAddressOf()),
                   L"创建 sRGB 色彩上下文", error))
        {
            return false;
        }
        if (!SetChecked(this->tone.Get(), D2D1_HDRTONEMAP_PROP_DISPLAY_MODE, D2D1_HDRTONEMAP_DISPLAY_MODE_SDR, error) ||
            !SetChecked(this->tone.Get(), D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, 203.0F, error) ||
            !SetChecked(this->white.Get(), D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, 80.0F, error) ||
            !SetChecked(this->white.Get(), D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, 203.0F, error) ||
            !SetChecked(this->color.Get(), D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST,
                        error) ||
            !SetChecked(this->color.Get(), D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE,
                        D2D1_COLORMANAGEMENT_ALPHA_MODE_PREMULTIPLIED, error) ||
            !Check(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, this->sourceColor.Get()),
                   L"设置源色彩空间", error) ||
            !Check(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,
                                         this->destinationColor.Get()),
                   L"设置目标色彩空间", error))
        {
            return false;
        }
        for (ID2D1Effect* effect : {this->tone.Get(), this->white.Get(), this->color.Get()})
        {
            if (!SetChecked(effect, static_cast<UINT32>(D2D1_PROPERTY_PRECISION), D2D1_BUFFER_PRECISION_16BPC_FLOAT,
                            error))
            {
                return false;
            }
        }
        this->white->SetInputEffect(0U, this->tone.Get());
        this->color->SetInputEffect(0U, this->white.Get());
        return true;
    }

    // 在首次使用线程建立并缓存色调映射设备，硬件失败时尝试一次 WARP。
    // 入参：error：输出参数，初始化失败或跨线程调用时接收诊断。
    // 返回：设备可用时为 true；跨线程调用或初始化失败时为 false，失败结果被缓存。
    bool Initialize(std::wstring& error)
    {
        if (this->threadId != 0U && this->threadId != GetCurrentThreadId())
        {
            error = L"HDR 转换器必须在创建设备的同一线程使用。";
            return false;
        }
        if (this->initialized)
        {
            return true;
        }
        if (this->attempted)
        {
            error = this->initializationError;
            return false;
        }
        this->threadId = GetCurrentThreadId();
        this->attempted = true;
        this->initialized = this->InitializeDriver(D3D_DRIVER_TYPE_HARDWARE, error);
        if (!this->initialized)
        {
            OPEN_ST_LOG_WARNING("HDR hardware initialization failed; trying WARP once.");
            this->warpAttempted = true;
            this->initialized = this->InitializeDriver(D3D_DRIVER_TYPE_WARP, error);
        }
        if (!this->initialized)
        {
            this->initializationError = error;
        }
        else
        {
            error.clear();
        }
        return this->initialized;
    }
};

// 创建HDR 原生色调映射器的内部资源容器，延迟到首次初始化时建立图形设备。
// 入参：无。
// 返回：无返回值；构造后的对象尚未建立图形设备，内存分配失败可抛出异常。
NativeToneMapper::NativeToneMapper() : impl_(std::make_unique<Impl>()) {}
// 释放色调映射效果、设备及仍被效果引用的单张图像。
// 入参：无。
// 返回：无返回值；析构完成对应资源清理。
NativeToneMapper::~NativeToneMapper()
{
    this->ReleaseImageResources();
}
// 释放本次图像转换使用的大块资源，同时保留可复用转换设备。
// 入参：无。
// 返回：无返回值；效果图不再引用单张图像，图像缓存被释放，可再次执行转换。
void NativeToneMapper::ReleaseImageResources() noexcept
{
    this->impl_->ReleaseImages();
}
// 建立并预热 HDR 色调映射资源，避免第一次选区输出承担全部初始化成本。
// 入参：errorMessage：输出参数，失败时接收初始化或预热原因。
// 返回：设备及微小图像预热成功时为 true；受控回退后仍失败或分配异常时为 false。
bool NativeToneMapper::Prepare(std::wstring& errorMessage)
try
{
    const std::array<std::uint8_t, 8> black{};
    std::vector<std::uint8_t> result;
    const HdrImageView image{1U, 1U, 8U, black, HdrPixelFormat::Rgba16FloatScRgb};
    if (this->Convert(image, result, errorMessage))
    {
        return true;
    }
    if (this->impl_->initialized && !this->impl_->warpAttempted && this->impl_->threadId == GetCurrentThreadId())
    {
        // 设备创建成功不代表首次着色器编译/执行成功；完整预热也属于一次初始化降级边界。
        this->ReleaseImageResources();
        this->impl_->warpAttempted = true;
        OPEN_ST_LOG_WARNING("HDR hardware warm-up failed; trying WARP once.");
        this->impl_->initialized = false;
        this->impl_->initialized = this->impl_->InitializeDriver(D3D_DRIVER_TYPE_WARP, errorMessage);
        if (!this->impl_->initialized)
        {
            this->impl_->initializationError = errorMessage;
            return false;
        }
        return this->Convert(image, result, errorMessage);
    }
    return false;
}
catch (const std::exception&)
{
    this->ReleaseImageResources();
    this->impl_->initialized = false;
    this->impl_->initializationError.clear();
    // 低内存异常路径不再分配诊断字符串；调用方记录预热失败，纯 SDR 输出仍可用。
    errorMessage.clear();
    return false;
}

// 通过 Windows 原生效果将借用 HDR 图像转换为紧凑 SDR/sRGB 输出。
// 入参：image：只在调用期间借用的 HDR 图像视图；bgra：输出参数，接收紧凑 BGRA8 字节且 alpha 为 255；errorMessage：输出参数，接收失败诊断。
// 返回：完成解码、原生映射和 GPU 回读时为 true；失败为 false，bgra 清空，不发布部分图像。
bool NativeToneMapper::Convert(const HdrImageView& image, std::vector<std::uint8_t>& bgra, std::wstring& errorMessage)
try
{
    bgra.clear();
    errorMessage.clear();
    std::vector<std::uint16_t> linear;
    float peakNits{};
    if (!Decode(image, linear, peakNits, errorMessage) || !this->impl_->Initialize(errorMessage))
    {
        return false;
    }
    // 守卫先创建后绑定位图，保证提前返回和异常都断开 context/effect 的图像引用。
    struct ImageGuard final
    {
        NativeToneMapper& mapper;
        // 在转换退出或异常展开时断开效果图对本次图像的引用。
        // 入参：无。
        // 返回：无返回值；析构完成对应资源清理。
        ~ImageGuard()
        {
            this->mapper.ReleaseImageResources();
        }
    } guard{*this};
    const float inputPeak = std::max(80.0F, peakNits);
    OPEN_ST_LOG_DEBUG("HDR content peak nits=", peakNits, ", effect input peak nits=", inputPeak);
    if (!SetChecked(this->impl_->tone.Get(), D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE, inputPeak, errorMessage))
    {
        return false;
    }
    const D2D1_SIZE_U size = D2D1::SizeU(image.width, image.height);
    const D2D1_BITMAP_PROPERTIES1 inputProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0F, 96.0F);
    const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    const D2D1_BITMAP_PROPERTIES1 readProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    ComPtr<ID2D1Bitmap1> input;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1Bitmap1> readback;
    if (!Check(this->impl_->context->CreateBitmap(size, linear.data(), image.width * 8U, &inputProperties,
                                                  input.GetAddressOf()),
               L"上传 HDR 输入", errorMessage) ||
        !Check(this->impl_->context->CreateBitmap(size, nullptr, 0U, &targetProperties, target.GetAddressOf()),
               L"创建 SDR 输出目标", errorMessage) ||
        !Check(this->impl_->context->CreateBitmap(size, nullptr, 0U, &readProperties, readback.GetAddressOf()),
               L"创建 SDR 回读位图", errorMessage))
    {
        return false;
    }
    this->impl_->tone->SetInput(0U, input.Get());
    this->impl_->context->SetTarget(target.Get());
    this->impl_->context->BeginDraw();
    this->impl_->context->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F));
    this->impl_->context->DrawImage(
        this->impl_->color.Get(), D2D1::Point2F(0.0F, 0.0F),
        D2D1::RectF(0.0F, 0.0F, static_cast<float>(image.width), static_cast<float>(image.height)),
        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
    if (!Check(this->impl_->context->EndDraw(), L"执行 HDR 到 SDR 效果链", errorMessage))
    {
        return false;
    }
    this->impl_->context->SetTarget(nullptr);
    if (!Check(readback->CopyFromBitmap(nullptr, target.Get(), nullptr), L"复制 SDR 回读位图", errorMessage))
    {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4U;
    std::vector<std::uint8_t> candidate(stride * image.height);
    D2D1_MAPPED_RECT mapped{};
    if (!Check(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped), L"映射 SDR 回读数据", errorMessage))
    {
        return false;
    }
    for (std::uint32_t y = 0U; y < image.height; ++y)
    {
        std::memcpy(candidate.data() + static_cast<std::size_t>(y) * stride,
                    mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, stride);
    }
    const HRESULT unmap = readback->Unmap();
    if (!Check(unmap, L"解除 SDR 回读映射", errorMessage))
    {
        return false;
    }
    for (std::size_t alpha = 3U; alpha < candidate.size(); alpha += 4U)
    {
        candidate[alpha] = 255U;
    }
    bgra = std::move(candidate);
    return true;
}
catch (const std::exception&)
{
    this->ReleaseImageResources();
    bgra.clear();
    errorMessage = L"HDR 转换资源分配失败。";
    return false;
}
} // namespace open_st
