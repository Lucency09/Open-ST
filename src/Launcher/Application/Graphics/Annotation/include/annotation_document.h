// 文件职责：声明标准库不可变标注文档操作，不依赖窗口、捕获或渲染模块。
#pragma once
#include <annotation.h>
#include <cstddef>
#include <span>

namespace open_st
{
// 按稳定 ID 查找已提交对象，不检查零 ID 绘制草稿。
// 入参：document：调用期间借用的不可变文档；id：非零稳定 ID。
// 返回：文档内对象的借用指针；未找到返回空，指针不可超出 document 的存活期。
[[nodiscard]] const AnnotationObject* FindAnnotation(const AnnotationSnapshot& document, std::uint64_t id) noexcept;

// 创建追加有效对象的新快照，不修改原文档。
// 入参：document：已有文档，可空；object：新增对象，零 ID 仅用于草稿；result：成功接收新快照。
// 返回：成功为 true；非法对象、重复非零 ID 或分配失败为 false，并保持 result 原值。
[[nodiscard]] bool AppendAnnotation(const AnnotationSnapshot& document, const AnnotationObject& object,
                                    AnnotationSnapshot& result) noexcept;

// 提取对象可编辑参数，不携带正文或几何副本。
// 入参：object：有效对象。
// 返回：匹配该种类的样式、字号或块大小 DTO。
[[nodiscard]] AnnotationProperties PropertiesOf(const AnnotationObject& object) noexcept;

// 替换对象允许编辑的参数，保留正文、几何、ID、叠放位置和擦痕。
// 入参：document：原文档；id：目标；properties：按种类匹配的参数；result：结果。
// 返回：成功为 true，同值共享原文档；非法或分配失败保留 result。
[[nodiscard]] bool ReplaceAnnotationProperties(const AnnotationSnapshot& document, std::uint64_t id,
                                               const AnnotationProperties& properties,
                                               AnnotationSnapshot& result) noexcept;

// 规范化并替换已有文字正文，字号与旧擦痕不随正文重排而改变。
// 入参：document：原文档；id：文字 ID；text：原始 UTF-16；result：结果。
// 返回：成功为 true，同值共享原文档；空白、非法或分配失败保留 result。
[[nodiscard]] bool ReplaceAnnotationText(const AnnotationSnapshot& document, std::uint64_t id, std::u16string_view text,
                                         AnnotationSnapshot& result) noexcept;

// 为原位编辑生成不包含目标元素的预览视图，不提交删除事务。
// 入参：document：原文档；id：被输入控件暂时替代的目标；result：预览结果。
// 返回：目标存在且分配成功为 true，否则保留 result。
[[nodiscard]] bool HideAnnotationForPreview(const AnnotationSnapshot& document, std::uint64_t id,
                                            AnnotationSnapshot& result) noexcept;

// 按桌面物理像素平移所有对象原点，保留局部几何及样式。
// 入参：document：原文档；delta：有限的物理坐标偏移；result：成功接收结果快照。
// 返回：成功为 true，零平移或空文档共享原快照；越界或分配失败为 false，不修改 result。
[[nodiscard]] bool TranslateAnnotations(const AnnotationSnapshot& document, AnnotationPoint delta,
                                        AnnotationSnapshot& result) noexcept;

// 向指定已有对象追加同一擦除路径的局部引用，不影响后来新建的对象。
// 入参：document：手势基线；ids：实际覆盖的稳定 ID；stroke：共享桌面路径和裁剪；result：结果。
// 返回：成功为 true，空 ID 或同一引用已应用时共享原文档；非法或分配失败保留 result。
[[nodiscard]] bool ApplyAnnotationErase(const AnnotationSnapshot& document, std::span<const std::uint64_t> ids,
                                        std::shared_ptr<const AnnotationEraseStroke> stroke,
                                        AnnotationSnapshot& result) noexcept;

// 对多份文档及历史快照统一计算动态存储，共享分配按身份只计一次。
// 入参：documents：调用期间借用的快照数组。
// 返回：深层动态字节总数，不含调用方历史包装；溢出或统计分配失败返回 size_t 最大值。
[[nodiscard]] std::size_t AnnotationDocumentsStorageBytes(
    std::span<const AnnotationSnapshot> documents,
    const std::shared_ptr<const AnnotationEraseStroke>& activeErase = {}) noexcept;
} // namespace open_st
