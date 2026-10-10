// 文件职责：声明冻结桌面选区到 SDR 输出的裁切与跨屏拼接入口，隔离后续导出业务。

#pragma once

#include <annotation.h>

#include <frozen_desktop_frame.h>
#include <native_tone_mapper.h>
#include <sdr_selection_frame.h>
#include <string>

namespace open_st
{
class AnnotationMosaicSource;
// 从冻结原生像素生成正式输出；同一线程复用 HDR 设备，绝不读取覆盖窗口。
class SelectionOutputRenderer final
{
  public:
    // 设置 HDR 输出亮度，不改变原生冻结像素或 SDR 内容。
    // 入参：percent 为 25 至 200 的百分比；100 保持原有效果。
    // 返回：参数有效时 true；无效时 false 并保留原值。
    [[nodiscard]] bool SetBrightnessPercent(unsigned int percent) noexcept;

    // 保存本次截图显式策略，非法参数保留旧值。
    // 入参：options 为策略。返回：合法为 true。
    [[nodiscard]] bool SetToneMappingOptions(HdrToneMappingOptions options) noexcept;

    // 建立并预热 HDR 色调映射资源，避免第一次选区输出承担全部初始化成本。
    // 入参：errorMessage：输出参数，失败时接收初始化或预热原因。
    // 返回：设备及微小图像预热成功时为 true；受控回退后仍失败或分配异常时为 false。
    [[nodiscard]] bool Prepare(std::wstring& errorMessage);
    // 从冻结桌面裁切并拼接指定选区，将 HDR 区域转换为 SDR 供最终输出。
    // 入参：desktop：只读原生冻结桌面；selection：虚拟桌面物理像素半开选区；output：输出参数，接收完整 SDR
    // 图像；errorMessage：输出参数，接收转换或几何错误原因。 返回：整张选区完成时为 true；失败时为 false 且清空
    // output，不发布部分图像；显示器之间的空洞填不透明黑色。
    [[nodiscard]] bool Render(const FrozenDesktopFrame& desktop, RectI selection, SdrSelectionFrame& output,
                              std::wstring& errorMessage);
    // 在原有 SDR 输出上合成不可变标注，所有完成入口共享同一扁平化结果。
    // 入参：desktop：冻结桌面；selection：正式选区；annotations：只读对象；output：结果；errorMessage：诊断；
    // source：可选共享马赛克来源，缺省时仅在存在马赛克的调用中建立临时来源。
    // 返回：整图完成为 true；失败清空 output，不发布部分图像。
    [[nodiscard]] bool Render(const FrozenDesktopFrame& desktop, RectI selection, const AnnotationSnapshot& annotations,
                              SdrSelectionFrame& output, std::wstring& errorMessage,
                              AnnotationMosaicSource* source = nullptr);
    // 在已生成的SDR底图上仅合成一次标注，不再次执行HDR转换。
    // 入参：base 为不可变底图；annotations 为快照；output 为结果且不能与base同一对象；
    // errorMessage 为诊断；source 为已同步亮度及底图的可选马赛克来源。
    // 返回：完整合成成功为true；失败清空结果，空标注逐字节复制底图。
    [[nodiscard]] bool RenderFromPreview(const SdrSelectionFrame& base, const AnnotationSnapshot& annotations,
                                         SdrSelectionFrame& output, std::wstring& errorMessage,
                                         AnnotationMosaicSource* source = nullptr);
    // 释放本次图像转换使用的大块资源，同时保留可复用转换设备。
    // 入参：无。
    // 返回：无返回值；效果图不再引用单张图像，图像缓存被释放，可再次执行转换。
    void ReleaseImageResources() noexcept;

  private:
    NativeToneMapper toneMapper_;
    unsigned int brightnessPercent_{100U};
    HdrToneMappingOptions toneMappingOptions_{};
};
} // namespace open_st
