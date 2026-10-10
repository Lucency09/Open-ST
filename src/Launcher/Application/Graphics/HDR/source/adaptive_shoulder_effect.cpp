// 文件职责：实现单输入、同尺寸的 D2D 自定义像素效果，使用构建期嵌入字节码。
#include <adaptive_shoulder_effect.h>
#include <d2d1effectauthor.h>
#include <adaptive_shoulder_shader.h>
#include <cstring>
#include <wrl.h>

namespace open_st::hdr_detail
{
namespace
{
inline constexpr GUID SHADER_ADAPTIVE_SHOULDER = {
    0xba031ff8, 0xb5a5, 0x48c9, {0x82, 0x26, 0x4f, 0xdb, 0x15, 0x76, 0x97, 0x1d}};
class AdaptiveShoulderEffect final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ID2D1EffectImpl,
          Microsoft::WRL::ChainInterfaces<ID2D1DrawTransform, ID2D1Transform, ID2D1TransformNode>>
{
  public:
    // 载入预编译像素着色器并安装单节点图。
    // 入参：context 为效果资源上下文，graph 为效果图。返回：安装结果。
    HRESULT STDMETHODCALLTYPE Initialize(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override
    {
        const HRESULT result = context->LoadPixelShader(SHADER_ADAPTIVE_SHOULDER, g_adaptiveShoulderShader,
                                                        sizeof(g_adaptiveShoulderShader));
        return FAILED(result) ? result : graph->SetSingleTransformNode(this);
    }
    // 每次绘制更新常量；不读取像素、不统计内容峰值。
    // 入参：changeType 为 D2D 变化标志。返回：常量上传结果。
    HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE changeType) override
    {
        (void)changeType;
        return this->drawInfo_->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&this->parameters_),
                                                             sizeof(this->parameters_));
    }
    // 固定单节点图不支持替换。入参：graph 为替代图。返回：E_NOTIMPL。
    HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph* graph) override
    {
        (void)graph;
        return E_NOTIMPL;
    }
    // 返回唯一输入数量。入参：无。返回：1。
    UINT32 STDMETHODCALLTYPE GetInputCount() const override
    {
        return 1U;
    }
    // 输出区域原样映射到输入，不产生邻域取样。
    // 入参：output 为输出矩形，inputs 为输入矩形数组，count 为数量。返回：参数校验结果。
    HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(const D2D1_RECT_L* output, D2D1_RECT_L* inputs,
                                                        UINT32 count) const override
    {
        if (!output || !inputs || count != 1U)
            return E_INVALIDARG;
        inputs[0] = *output;
        return S_OK;
    }
    // 输入区域原样映射到输出，输出不透明。
    // 入参：inputs/opaque 为输入范围，count 为数量，output/outputOpaque 接收输出范围。返回：参数校验结果。
    HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(const D2D1_RECT_L* inputs, const D2D1_RECT_L* opaque,
                                                        UINT32 count, D2D1_RECT_L* output,
                                                        D2D1_RECT_L* outputOpaque) override
    {
        (void)opaque;
        if (!inputs || !output || !outputOpaque || count != 1U)
            return E_INVALIDARG;
        *output = inputs[0];
        *outputOpaque = inputs[0];
        return S_OK;
    }
    // 单输入无效区域原样传播。入参：index 为输入下标，invalid 为区域，output 接收传播结果。返回：校验结果。
    HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT32 index, D2D1_RECT_L invalid, D2D1_RECT_L* output) const override
    {
        if (index != 0U || !output)
            return E_INVALIDARG;
        *output = invalid;
        return S_OK;
    }
    // 固定 FP16 中间精度并绑定像素着色器。
    // 入参：info 为 D2D 绘制描述。返回：着色器和精度设置结果。
    HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* info) override
    {
        this->drawInfo_ = info;
        const HRESULT result = info->SetPixelShader(SHADER_ADAPTIVE_SHOULDER, D2D1_PIXEL_OPTIONS_NONE);
        return FAILED(result) ? result : info->SetOutputBuffer(D2D1_BUFFER_PRECISION_16BPC_FLOAT, D2D1_CHANNEL_DEPTH_4);
    }
    // D2D 创建回调。入参：result 接收新实例。返回：分配结果。
    static HRESULT CALLBACK Create(IUnknown** result)
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        const Microsoft::WRL::ComPtr<AdaptiveShoulderEffect> instance = Microsoft::WRL::Make<AdaptiveShoulderEffect>();
        return instance ? instance.CopyTo(result) : E_OUTOFMEMORY;
    }
    // 设置每次转换的有效常量。入参：effect 为实例，data/size 为 vector4 字节。返回：校验结果。
    static HRESULT CALLBACK SetParameters(IUnknown* effect, const BYTE* data, UINT32 size)
    {
        if (!data || size != sizeof(D2D1_VECTOR_4F))
            return E_INVALIDARG;
        AdaptiveShoulderEffect* instance = static_cast<AdaptiveShoulderEffect*>(static_cast<ID2D1EffectImpl*>(effect));
        std::memcpy(&instance->parameters_, data, size);
        return S_OK;
    }
    // 回读常量用于接受值校验。入参：effect 为实例，data/size 为接收缓冲，actual 接收所需大小。返回：校验结果。
    static HRESULT CALLBACK GetParameters(const IUnknown* effect, BYTE* data, UINT32 size, UINT32* actual)
    {
        if (actual)
            *actual = sizeof(D2D1_VECTOR_4F);
        if (!data)
            return size == 0U ? S_OK : E_INVALIDARG;
        if (size < sizeof(D2D1_VECTOR_4F))
            return E_INVALIDARG;
        const AdaptiveShoulderEffect* instance =
            static_cast<const AdaptiveShoulderEffect*>(static_cast<const ID2D1EffectImpl*>(effect));
        std::memcpy(data, &instance->parameters_, sizeof(instance->parameters_));
        return S_OK;
    }

  private:
    Microsoft::WRL::ComPtr<ID2D1DrawInfo> drawInfo_;
    D2D1_VECTOR_4F parameters_{1.0F, 1.0F, 2.0F, 0.0F};
};
} // namespace
// 在设备工厂注册唯一像素效果；不加载运行期文件或编译着色器。
// 入参：factory 为所属 D2D 工厂。返回：注册结果 HRESULT。
HRESULT RegisterAdaptiveShoulderEffect(ID2D1Factory1* factory)
{
    constexpr PCWSTR xml = LR"(<?xml version='1.0'?>
<Effect><Property name='DisplayName' type='string' value='Adaptive HDR Shoulder'/>
<Property name='Author' type='string' value='Open-ST'/><Property name='Category' type='string' value='Color'/>
<Property name='Description' type='string' value='Frozen reference white and fixed highlight range'/>
<Inputs><Input name='Source'/></Inputs><Property name='Parameters' type='vector4'>
<Property name='DisplayName' type='string' value='Parameters'/>
<Property name='Default' type='vector4' value='(1,1,2,0)'/></Property></Effect>)";
    const D2D1_PROPERTY_BINDING binding{L"Parameters", &AdaptiveShoulderEffect::SetParameters,
                                        &AdaptiveShoulderEffect::GetParameters};
    return factory->RegisterEffectFromString(CLSID_ADAPTIVE_SHOULDER, xml, &binding, 1U,
                                             &AdaptiveShoulderEffect::Create);
}
} // namespace open_st::hdr_detail
