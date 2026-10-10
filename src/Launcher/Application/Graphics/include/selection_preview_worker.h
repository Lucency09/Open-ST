// 声明截图选区底图的有界后台转换，独占图形设备并隔离过期结果。
#pragma once

#include <frozen_desktop_frame.h>
#include <native_tone_mapper.h>
#include <sdr_selection_frame.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace open_st
{
struct SelectionPreviewWorkerTestAccess;
// 一次完整转换结果，像素由调用方接管；失败不携带旧底图。
struct SelectionPreviewResult final
{
    std::uint64_t requestId{};
    SdrSelectionFrame frame;
    bool success{};
    std::wstring error;
};
// 单线程最多执行一个请求并保留一个最新待办，不访问窗口或可变截图状态。
class SelectionPreviewWorker final
{
  public:
    // 建立工作线程；notify 只负责通知宿主读取结果，不得在回调中销毁本对象。
    // 入参：notify 为可空完成通知，在工作线程且不持内部锁时调用。
    // 返回：构造完成后可提交任务；线程创建失败抛出异常。
    explicit SelectionPreviewWorker(std::function<void()> notify = {});
    // 停止接收并等待正在执行的转换释放设备，不向已销毁宿主继续通知。
    // 入参：无。返回：无。
    ~SelectionPreviewWorker();
    // 禁止复制线程及任务所有权。
    // 入参：拟复制的实例。返回：已删除。
    SelectionPreviewWorker(const SelectionPreviewWorker&) = delete;
    // 禁止赋值复制线程及任务所有权。
    // 入参：拟复制的实例。返回：已删除。
    SelectionPreviewWorker& operator=(const SelectionPreviewWorker&) = delete;
    // 替换尚未执行的旧请求，保存冻结数据的共享所有权。
    // 入参：desktop 为不可变冻结帧；selection 为裁切；brightness 为25~200；requestId 由宿主标识版本。
    // 返回：任务合法且已接收时 true；失败不改变此前任务。
    [[nodiscard]] bool Submit(std::shared_ptr<const FrozenDesktopFrame> desktop, RectI selection,
                              unsigned int brightness, std::uint64_t requestId, HdrToneMappingOptions options);
    // 立即清除待办及结果资格；正在转换的线程持有输入直到安全结束。
    // 入参：无。返回：无，不等待图形调用。
    void Cancel() noexcept;
    // 接管最新完整结果，旧请求与取消请求的结果永远不可获取。
    // 入参：无。返回：有新结果时返回并清空邮箱，否则为空。
    [[nodiscard]] std::optional<SelectionPreviewResult> TakeResult();
    // 查询尚在转换或等待转换的工作，用于宿主退出屏障。
    // 入参：无。返回：存在工作时 true，完成邮箱本身不算工作。
    [[nodiscard]] bool HasWork() const noexcept;

  private:
    friend struct SelectionPreviewWorkerTestAccess;
    using Converter = std::function<bool(const FrozenDesktopFrame&, RectI, unsigned int, HdrToneMappingOptions,
                                         SdrSelectionFrame&, std::wstring&)>;
    // 为并发协议测试注入受控转换，不对业务调用方暴露第二套执行路径。
    // 入参：notify 为通知，converter 为测试转换。返回：构造工作线程。
    SelectionPreviewWorker(std::function<void()> notify, Converter converter);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
