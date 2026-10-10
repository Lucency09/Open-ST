// 文件职责：仅在测试中比较 adaptive GPU 候选与 CPU 参考，不接入生产色调映射。
#include <gtest/gtest.h>
#include <color_conversion.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
constexpr char shaderSource[] = R"(
StructuredBuffer<float4> source : register(t0);
RWStructuredBuffer<float4> target : register(u0);
cbuffer Parameters : register(b0) { float white; uint count; float2 padding; };
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    float3 rgb = source[id.x].rgb * (80.0 / white);
    float m = max(rgb.r, max(rgb.g, rgb.b));
    float A = .875, B = .995, K = .6, P = max(2.0, 4000.0 / white);
    float d = (B-A) / log(P), y = A*m;
    if (m > K && m < 1) {
        float t = (m-K)/(1-K);
        y = A*m + (1-K)*(A-d)*t*t*(1-t);
    } else if (m >= 1 && m <= P) y = A+(B-A)*log(m)/log(P);
    else if (m > P) y = B+(1-B)*(1-exp(-(m-P)*d/(P*(1-B))));
    target[id.x] = float4(m > 0 ? rgb*(y/m) : rgb*A, 1);
})";

// 对图形调用失败抛出阶段诊断，交由测试报告。
// 入参：result 为 HRESULT；stage 为调用阶段。
// 返回：无；失败时抛出异常，不降级到 WARP。
void Require(HRESULT result, const char* stage)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(stage) + ": " + std::to_string(result));
}

// 对已解码原始 FP16 输入执行一次参考白缩放和独立 CPU 曲线。
// 入参：input 为 raw scRGB（1=80 nit）；white 为参考白 nit。
// 返回：线性 RGB 候选，alpha 固定为 1，负通道不预裁剪。
Pixel CpuCurve(Pixel input, float white)
{
    for (std::size_t c = 0; c < 3; ++c)
        input[c] *= 80.0F / white;
    const float m = std::max({input[0], input[1], input[2]});
    const float a = .875F, b = .995F, k = .6F;
    const float p = std::max(2.0F, 4000.0F / white);
    const float d = (b - a) / std::log(p);
    float y = a * m;
    if (m > k && m < 1.0F)
    {
        const float t = (m - k) / (1.0F - k);
        y += (1.0F - k) * (a - d) * t * t * (1.0F - t);
    }
    else if (m >= 1.0F && m <= p)
        y = a + (b - a) * std::log(m) / std::log(p);
    else if (m > p)
        y = b + (1.0F - b) * (1.0F - std::exp(-(m - p) * d / (p * (1.0F - b))));
    for (std::size_t c = 0; c < 3; ++c)
        input[c] *= m > 0.0F ? y / m : a;
    input[3] = 1.0F;
    return input;
}

class AdaptiveCandidateGpuTest : public testing::Test
{
  protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID2D1DeviceContext> colorContext;
    ComPtr<ID2D1Effect> color;

    // 建立真实硬件 D3D11 compute 和与生产相同的 D2D 色彩末端。
    // 入参：无。
    // 返回：无；无硬件时明确跳过，设备或效果错误报告失败。
    void SetUp() override
    {
        ComPtr<IDXGIFactory1> adapters;
        ASSERT_NO_THROW(Require(CreateDXGIFactory1(IID_PPV_ARGS(adapters.GetAddressOf())), "DXGI factory"));
        ComPtr<IDXGIAdapter1> hardware;
        for (UINT i = 0;; ++i)
        {
            const HRESULT result = adapters->EnumAdapters1(i, hardware.ReleaseAndGetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
                GTEST_SKIP() << "No hardware adapter; GPU candidate not executed.";
            ASSERT_NO_THROW(Require(result, "EnumAdapters1"));
            DXGI_ADAPTER_DESC1 description{};
            ASSERT_NO_THROW(Require(hardware->GetDesc1(&description), "GetDesc1"));
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U)
            {
                std::cout << "GPU_ADAPTER," << description.VendorId << ',' << description.DeviceId << '\n';
                break;
            }
        }
        const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL actual{};
        ASSERT_NO_THROW(Require(D3D11CreateDevice(hardware.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1, D3D11_SDK_VERSION,
                                                  this->device.GetAddressOf(), &actual, this->context.GetAddressOf()),
                                "hardware D3D11"));
        ComPtr<ID3DBlob> code, errors;
        const HRESULT compiled = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr, "main",
                                            "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
                                            code.GetAddressOf(), errors.GetAddressOf());
        ASSERT_TRUE(SUCCEEDED(compiled)) << (errors ? static_cast<const char*>(errors->GetBufferPointer())
                                                    : "compile failed");
        ASSERT_NO_THROW(Require(this->device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                                  nullptr, this->shader.GetAddressOf()),
                                "compute shader"));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> d2d;
        const D2D1_FACTORY_OPTIONS options{};
        ASSERT_NO_THROW({
            Require(this->device.As(&dxgi), "DXGI device");
            Require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                      reinterpret_cast<void**>(factory.GetAddressOf())),
                    "D2D factory");
            Require(factory->CreateDevice(dxgi.Get(), d2d.GetAddressOf()), "D2D device");
            Require(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, this->colorContext.GetAddressOf()),
                    "D2D context");
            this->colorContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            this->colorContext->SetDpi(96.0F, 96.0F);
            Require(this->colorContext->CreateEffect(CLSID_D2D1ColorManagement, this->color.GetAddressOf()),
                    "ColorManagement");
            ComPtr<ID2D1ColorContext> source;
            ComPtr<ID2D1ColorContext> destination;
            Require(this->colorContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, source.GetAddressOf()),
                    "scRGB");
            Require(
                this->colorContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, destination.GetAddressOf()),
                "sRGB");
            Require(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, source.Get()),
                    "source context");
            Require(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, destination.Get()),
                    "destination context");
            Require(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST),
                    "BEST");
            Require(this->color->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE,
                                          D2D1_COLORMANAGEMENT_ALPHA_MODE_PREMULTIPLIED),
                    "alpha");
            Require(
                this->color->SetValue(static_cast<UINT32>(D2D1_PROPERTY_PRECISION), D2D1_BUFFER_PRECISION_16BPC_FLOAT),
                "FP16");
        });
    }

    // 上传原始 FP16 解码值，在 GPU 执行 FP32 曲线后 staging 回读。
    // 入参：input 为紧凑像素；white 为参考白 nit。
    // 返回：GPU FP32 输出；资源失败抛异常，CPU 不代算 GPU 曲线。
    std::vector<Pixel> RunGpu(std::span<const Pixel> input, float white)
    {
        const UINT count = static_cast<UINT>(input.size());
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = count * static_cast<UINT>(sizeof(Pixel));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = sizeof(Pixel);
        const D3D11_SUBRESOURCE_DATA data{input.data(), 0, 0};
        ComPtr<ID3D11Buffer> source, target, readback, parameters;
        Require(this->device->CreateBuffer(&description, &data, source.GetAddressOf()), "input buffer");
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Require(this->device->CreateBuffer(&description, nullptr, target.GetAddressOf()), "output buffer");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        description.StructureByteStride = 0;
        Require(this->device->CreateBuffer(&description, nullptr, readback.GetAddressOf()), "staging buffer");
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Require(this->device->CreateShaderResourceView(source.Get(), nullptr, srv.GetAddressOf()), "SRV");
        Require(this->device->CreateUnorderedAccessView(target.Get(), nullptr, uav.GetAddressOf()), "UAV");
        struct Constants
        {
            float white;
            UINT count;
            float padding[2];
        };
        const Constants constants{white, count, {0, 0}};
        description = {};
        description.ByteWidth = sizeof(Constants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA constantData{&constants, 0, 0};
        Require(this->device->CreateBuffer(&description, &constantData, parameters.GetAddressOf()), "constants");
        this->context->CSSetShader(this->shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* inputView = srv.Get();
        ID3D11UnorderedAccessView* outputView = uav.Get();
        ID3D11Buffer* constantBuffer = parameters.Get();
        this->context->CSSetShaderResources(0, 1, &inputView);
        this->context->CSSetUnorderedAccessViews(0, 1, &outputView, nullptr);
        this->context->CSSetConstantBuffers(0, 1, &constantBuffer);
        this->context->Dispatch((count + 63U) / 64U, 1, 1);
        this->context->ClearState();
        this->context->CopyResource(readback.Get(), target.Get());
        std::vector<Pixel> output(input.size());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(this->context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "GPU readback");
        std::memcpy(output.data(), mapped.pData, output.size() * sizeof(Pixel));
        this->context->Unmap(readback.Get(), 0);
        return output;
    }

    // 将 CPU 或 GPU 线性结果量化 FP16，经过生产同款 ColorManagement 到 BGRA8。
    // 入参：pixels 为线性结果；width 为像素宽，大小必须整除 width。
    // 返回：紧凑 BGRA8，失败抛异常。
    std::vector<std::uint8_t> Finish(std::span<const Pixel> pixels, UINT width)
    {
        std::vector<std::uint16_t> half(pixels.size() * 4);
        for (std::size_t i = 0; i < pixels.size(); ++i)
            for (std::size_t c = 0; c < 4; ++c)
                half[i * 4 + c] = open_st::EncodeFloat16(pixels[i][c]);
        const D2D1_SIZE_U size{width, static_cast<UINT>(pixels.size() / width)};
        const D2D1_BITMAP_PROPERTIES1 inputProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const D2D1_BITMAP_PROPERTIES1 readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, targetProperties.pixelFormat, 96, 96);
        ComPtr<ID2D1Bitmap1> input, target, readback;
        Require(this->colorContext->CreateBitmap(size, half.data(), width * 8, &inputProperties, input.GetAddressOf()),
                "FP16 upload");
        Require(this->colorContext->CreateBitmap(size, nullptr, 0, &targetProperties, target.GetAddressOf()),
                "BGRA target");
        Require(this->colorContext->CreateBitmap(size, nullptr, 0, &readProperties, readback.GetAddressOf()),
                "BGRA readback");
        this->color->SetInput(0, input.Get());
        this->colorContext->SetTarget(target.Get());
        this->colorContext->BeginDraw();
        this->colorContext->Clear(D2D1::ColorF(0, 0, 0, 1));
        this->colorContext->DrawImage(
            this->color.Get(), D2D1::Point2F(0, 0),
            D2D1::RectF(0, 0, static_cast<float>(size.width), static_cast<float>(size.height)),
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        const HRESULT drawn = this->colorContext->EndDraw();
        this->colorContext->SetTarget(nullptr);
        this->color->SetInput(0, nullptr);
        Require(drawn, "ColorManagement draw");
        Require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr), "BGRA copy");
        std::vector<std::uint8_t> output(pixels.size() * 4);
        D2D1_MAPPED_RECT mapped{};
        Require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped), "BGRA map");
        for (UINT y = 0; y < size.height; ++y)
            std::memcpy(output.data() + static_cast<std::size_t>(y) * width * 4,
                        mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(width) * 4);
        Require(readback->Unmap(), "BGRA unmap");
        return output;
    }
};

// 验证三档参考白的灰阶、颜色、抗锯齿、高光及负色精度，且布局和裁剪逐字节一致。
// 入参：无。
// 返回：无；输出逐样本 CSV，CPU/GPU 最大 RGB 差超过 2 LSB 判失败。
TEST_F(AdaptiveCandidateGpuTest, compares_cpu_reference_and_layouts)
{
    const std::array<Pixel, 6> colors{
        {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 1, 1}}};
    for (const float white : {80.0F, 203.0F, 260.0F})
    {
        std::vector<Pixel> input;
        std::vector<const char*> families;
        for (UINT gray = 0; gray < 256; ++gray)
        {
            const float value = open_st::SrgbToLinear(static_cast<float>(gray) / 255.0F) * white / 80.0F;
            input.push_back({value, value, value, 1});
            families.push_back("gray");
        }
        for (const Pixel& colorSample : colors)
            for (const float coverage : {1.0F, .75F, .5F, .25F})
            {
                Pixel value = colorSample;
                for (std::size_t c = 0; c < 3; ++c)
                    value[c] *= coverage * white / 80.0F;
                input.push_back(value);
                families.push_back("color_aa");
            }
        for (const Pixel& colorSample : colors)
            for (const float nits : {400.0F, 600.0F, 1000.0F, 2000.0F, 4000.0F, 6000.0F, 8000.0F, 10000.0F})
            {
                Pixel value = colorSample;
                for (std::size_t c = 0; c < 3; ++c)
                    value[c] *= nits / 80.0F;
                input.push_back(value);
                families.push_back("highlight");
            }
        for (const float nits : {100.0F, 203.0F, 260.0F, 400.0F, 600.0F, 1000.0F, 2000.0F, 4000.0F})
        {
            const float value = nits / 80.0F;
            input.push_back({value, value, value, 1});
            families.push_back("gray_highlight");
        }
        for (const Pixel& colorSample : colors)
            for (const float coverage : {1.0F, .75F, .5F, .25F})
            {
                Pixel value = colorSample;
                for (std::size_t c = 0; c < 3; ++c)
                    value[c] = (value[c] * coverage + 1.0F - coverage) * white / 80.0F;
                input.push_back(value);
                families.push_back("color_on_white");
            }
        for (const Pixel& value :
             std::array<Pixel, 4>{{{-.1F, .5F, 1, 1}, {1, -.2F, .4F, 1}, {.1F, 1, -.3F, 1}, {-.1F, -.2F, -.3F, 1}}})
        {
            input.push_back(value);
            families.push_back("negative");
        }
        // 原始输入先存储为 FP16，再解码；与旧 offline 的 normalized-FP16 顺序不同。
        for (Pixel& pixel : input)
            for (float& channel : pixel)
                channel = open_st::DecodeFloat16(open_st::EncodeFloat16(channel));
        ASSERT_NO_THROW({
            const std::vector<Pixel> gpu = this->RunGpu(input, white);
            std::vector<Pixel> cpu;
            for (const Pixel& pixel : input)
                cpu.push_back(CpuCurve(pixel, white));
            for (const Pixel& pixel : gpu)
            {
                for (const float channel : pixel)
                    EXPECT_TRUE(std::isfinite(channel));
                EXPECT_EQ(pixel[3], 1.0F);
            }
            float maxFloatError = 0.0F;
            std::size_t halfDifferentChannels = 0;
            for (std::size_t i = 0; i < gpu.size(); ++i)
                for (std::size_t c = 0; c < 3; ++c)
                {
                    maxFloatError = std::max(maxFloatError, std::abs(cpu[i][c] - gpu[i][c]));
                    if (open_st::EncodeFloat16(cpu[i][c]) != open_st::EncodeFloat16(gpu[i][c]))
                        ++halfDifferentChannels;
                }
            std::cout << "GPU_PRECISION," << white << ',' << maxFloatError << ',' << halfDifferentChannels << '\n';
            const std::vector<std::uint8_t> cpuBytes = this->Finish(cpu, static_cast<UINT>(input.size()));
            const std::vector<std::uint8_t> gpuBytes = this->Finish(gpu, static_cast<UINT>(input.size()));
            int maximum = 0;
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                std::cout << "GPU_PROBE," << white << ',' << families[i] << ',' << i;
                for (const std::vector<std::uint8_t>* bytes : {&cpuBytes, &gpuBytes})
                    for (const std::size_t c : {2U, 1U, 0U})
                        std::cout << ',' << static_cast<unsigned int>((*bytes)[i * 4 + c]);
                std::cout << '\n';
                for (std::size_t c = 0; c < 3; ++c)
                    maximum = std::max(maximum, std::abs(static_cast<int>(cpuBytes[i * 4 + c]) - gpuBytes[i * 4 + c]));
                EXPECT_EQ(gpuBytes[i * 4 + 3], 255);
            }
            EXPECT_LE(maximum, 2) << "white=" << white;
            EXPECT_EQ(gpuBytes, this->Finish(this->RunGpu(input, white), 4));
            // 4 列完整图取第 70 至 80 行、第 1 至 2 列，含高光；原图行 stride 为 4 像素。
            std::vector<Pixel> cropped;
            std::vector<std::uint8_t> expected;
            for (std::size_t row = 70; row < 81; ++row)
                for (std::size_t column = 1; column < 3; ++column)
                {
                    const std::size_t index = row * 4 + column;
                    cropped.push_back(input[index]);
                    expected.insert(expected.end(), gpuBytes.begin() + static_cast<std::ptrdiff_t>(index * 4),
                                    gpuBytes.begin() + static_cast<std::ptrdiff_t>(index * 4 + 4));
                }
            EXPECT_EQ(expected, this->Finish(this->RunGpu(cropped, white), 2));
        });
    }
}
} // namespace
