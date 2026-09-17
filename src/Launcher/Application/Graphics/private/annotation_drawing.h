// 文件职责：提供预览与输出共用的物理像素标注绘制及 SDR 透明层合成。
#pragma once
#include <annotation.h>
#include <d2d1.h>
#include <geometry.h>
#include <span>
#include <string>
#include <vector>

namespace open_st
{
class AnnotationMosaicSource;
// 绘制不可变标注快照，所有对象始终裁到正式选区。
// 入参：target：借用且处于 BeginDraw 内的目标；annotations：快照；selection：全局裁剪区；
// targetOrigin：目标左上桌面坐标；hdr、whiteScale：目标颜色和 SDR 参考白参数；error：失败原因；
// source：可选冻结来源，文档含马赛克时必须提供。
// 返回：同步提交全部命令为 true；非法对象或资源创建失败为 false，不负责 EndDraw。
[[nodiscard]] bool DrawAnnotations(ID2D1RenderTarget* target, const AnnotationSnapshot& annotations, RectI selection,
                                   AnnotationPoint targetOrigin, bool hdr, float whiteScale, std::wstring& error,
                                   AnnotationMosaicSource* source = nullptr);

// 在 SDR 编码域中合成透明标注层，透明像素保持底图全部四字节。
// 入参：selection：输出全局矩形；annotations：快照；base：紧凑 BGRX 原图；pixels：完整结果；error：失败原因。
// 返回：生成成功为 true，失败清空 pixels，不发布部分结果。
[[nodiscard]] bool CompositeAnnotations(RectI selection, const AnnotationSnapshot& annotations,
                                        std::span<const std::uint8_t> base, std::vector<std::uint8_t>& pixels,
                                        std::wstring& error, AnnotationMosaicSource* source = nullptr);
} // namespace open_st
