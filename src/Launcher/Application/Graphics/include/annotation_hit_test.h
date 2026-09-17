// 文件职责：声明正式选区内已提交标注的几何命中入口，隔离 D2D 实现。
#pragma once
#include <annotation.h>
#include <geometry.h>
#include <string>

namespace open_st
{
// 按叠放逆序选取实际覆盖点击位置的元素，所有精确命中失败后才尝试描边／箭头容差。
// 入参：annotations：只读快照，忽略 ID 为零和完全透明对象；selection：有效物理半开选区；
// point：桌面物理坐标；tolerance：非负有限物理像素容差；objectId：接收稳定 ID；error：失败诊断。
// 返回：查询成功为 true，未命中时 ID 为零；输入或 D2D 失败为 false，清零 ID 并写入诊断。
[[nodiscard]] bool HitTestAnnotations(const AnnotationSnapshot& annotations, RectI selection, AnnotationPoint point,
                                      float tolerance, std::uint64_t& objectId, std::wstring& error);
} // namespace open_st
