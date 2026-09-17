// 查询局部擦除实际接触的已有标注，使用与绘制、命中相同的可见几何。
#pragma once

#include <annotation.h>
#include <string>

namespace open_st
{
// 查找新擦痕与扣除旧擦痕后的对象交集，不按图层遮挡忽略下层对象。
// 入参：annotations 为固定文档；stroke 为桌面坐标圆笔迹及手势选区；ids 接收稳定目标 ID；error 接收诊断。
// 返回：查询成功为 true，空擦除返回空 ID；非法输入、超预算或图形错误为 false 并清空 ID。
bool FindAnnotationEraseTargets(const AnnotationSnapshot& annotations, const AnnotationEraseStroke& stroke,
                                std::vector<std::uint64_t>& ids, std::wstring& error);
} // namespace open_st
