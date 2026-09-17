// 使用 DirectWrite 的真实字形轮廓建立标注文字几何，供显示、命中和擦除共同消费。
#pragma once
#include <annotation.h>
#include <d2d1.h>
#include <wrl/client.h>

namespace open_st
{
// 按固定字体和显式换行生成对象局部文字轮廓，不做自动软换行。
// 入参：factory 为实际 D2D 工厂；text 为已校验文字；geometry 接收完整填充轮廓。
// 返回：完整布局及字形提取成功为 S_OK；错误或路径超预算不发布部分几何。
HRESULT CreateAnnotationTextGeometry(ID2D1Factory* factory, const AnnotationText& text,
                                     Microsoft::WRL::ComPtr<ID2D1Geometry>& geometry);
} // namespace open_st
