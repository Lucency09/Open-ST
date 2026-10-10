// 文件职责：定义 HDR 私有 D2D 像素效果与预编译着色器注册边界。
#pragma once
#include <d2d1_1.h>

namespace open_st::hdr_detail
{
inline constexpr GUID CLSID_ADAPTIVE_SHOULDER = {
    0x65b5b271, 0x865e, 0x4c4c, {0xa3, 0x4f, 0x52, 0x38, 0xc8, 0x47, 0x81, 0x3a}};
// 在设备工厂注册唯一像素效果；不加载运行期文件或编译着色器。
// 入参：factory 为所属 D2D 工厂。返回：注册结果 HRESULT。
HRESULT RegisterAdaptiveShoulderEffect(ID2D1Factory1* factory);
} // namespace open_st::hdr_detail
