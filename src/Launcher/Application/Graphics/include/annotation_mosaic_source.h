// 提供单次冻结截图共享的马赛克来源，统一跨屏网格、未标注 SDR 采样和有界缓存。
#pragma once

#include <annotation.h>
#include <geometry.h>
#include <memory>
#include <string>

struct ID2D1RenderTarget;
struct ID2D1Geometry;

namespace open_st
{
class FrozenDesktopFrame;

class AnnotationMosaicSource final
{
  public:
    // 绑定本次不可变冻结桌面，供全部覆盖窗口及正式输出共同使用。
    // 入参：desktop 为借用帧，必须保持内容不变且比本对象活得更久。
    // 返回：构造函数无返回值；只分配会话状态，不转换整张桌面。
    explicit AnnotationMosaicSource(const FrozenDesktopFrame& desktop);
    // 释放全部颜色 tile、图形缓存及局部转换资源。
    // 入参：无。
    // 返回：无。
    ~AnnotationMosaicSource();
    // 禁止复制冻结会话来源。
    // 入参：未命名对象为拟复制来源。
    // 返回：已删除。
    AnnotationMosaicSource(const AnnotationMosaicSource&) = delete;
    // 禁止复制赋值冻结会话来源。
    // 入参：未命名对象为拟复制来源。
    // 返回：已删除。
    AnnotationMosaicSource& operator=(const AnnotationMosaicSource&) = delete;
    // 在发布候选前准备其需要的全部马赛克 tile，失败保持此前已准备候选不变。
    // 入参：annotations 为候选文档；selection 为候选正式裁剪；error 接收诊断。
    // 返回：完整准备成功为 true；非法或超预算输入不发布部分候选。
    bool Prepare(const AnnotationSnapshot& annotations, RectI selection, std::wstring& error);
    // 验证来源属于同一个且仍有效的冻结帧，阻止跨会话误用已缓存像素。
    // 入参：desktop 为拟输出的冻结桌面。
    // 返回：绑定一致且冻结存储身份没有改变为 true。
    bool IsFor(const FrozenDesktopFrame& desktop) const noexcept;
    // 在目标结束前释放其位图缓存，避免保留已关闭窗口或离屏整张图像的 COM 目标。
    // 入参：target 为即将释放的目标，不取得所有权。
    // 返回：无；颜色来源缓存仍可复用。
    void ReleaseTarget(ID2D1RenderTarget* target) noexcept;
    // 将已准备的块均值填入实际可见几何，保留对象顺序及局部擦痕。
    // 入参：target 为正在绘制的目标；object 为马赛克；visible 为扣除擦痕的几何；origin 为目标桌面原点；
    // selection 为正式裁剪；hdr、whiteScale 为目标颜色语义；error 接收诊断。
    // 返回：所有相关 tile 成功提交为 true；缺少准备或设备失败返回 false。
    bool Draw(ID2D1RenderTarget* target, const AnnotationObject& object, ID2D1Geometry* visible, AnnotationPoint origin,
              RectI selection, bool hdr, float whiteScale, std::wstring& error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
