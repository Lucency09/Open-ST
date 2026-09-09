// 定义截图命令代次、输入时间屏障和单请求预订，阻止重复及过期操作。

#pragma once

#include <cstdint>

namespace open_st
{
// UI 线程共用的单请求闸门；代次跨截图会话递增，消息不持有对象指针。
class CaptureCommandGate final
{
  public:
    // 查询当前截图选区的命令代次。
    // 入参：无。
    // 返回：非零代次编号，用于工具栏显示和排队命令的有效性校验。
    [[nodiscard]] std::uint64_t Token() const noexcept
    {
        return this->token_;
    }
    // 查询是否已有完成命令占用待处理位置。
    // 入参：无。
    // 返回：存在已预订请求时为 true，否则为 false。
    [[nodiscard]] bool Pending() const noexcept
    {
        return this->pending_;
    }
    // 记录输入时间边界，以拒绝会话变化或模态操作之前积压的快捷键。
    // 入参：time：GetTickCount 或 Win32 消息时钟的毫秒值。
    // 返回：无返回值；后续输入按本次边界判断。
    void SetInputBarrier(std::uint32_t time) noexcept
    {
        this->inputBarrier_ = time;
        this->hasInputBarrier_ = true;
    }
    // 判断输入消息是否晚于最近一次会话或模态时间边界。
    // 入参：time：输入消息的 Win32 毫秒时钟值，允许计数回绕。
    // 返回：尚未设置边界或输入严格晚于边界时为 true；早于或同毫秒时为 false。
    [[nodiscard]] bool AcceptsInput(std::uint32_t time) const noexcept
    {
        return !this->hasInputBarrier_ || static_cast<std::int32_t>(time - this->inputBarrier_) > 0;
    }
    // 使旧选区命令失效并释放待处理位置。
    // 入参：无。
    // 返回：无返回值；代次递增且跳过零，清除 pending 状态。
    void Invalidate() noexcept
    {
        ++this->token_;
        if (this->token_ == 0)
        {
            ++this->token_;
        }
        this->pending_ = false;
    }
    // 为按钮或快捷键预订唯一的截图完成请求。
    // 入参：token：请求携带的选区代次；command：稳定命令 ID；ready：业务当前是否允许完成。
    // 返回：业务就绪、代次匹配且无待处理请求时为 true 并记录请求；否则 false 且不改状态。
    [[nodiscard]] bool Reserve(std::uint64_t token, std::uint32_t command, bool ready) noexcept
    {
        if (!ready || this->pending_ || token != this->token_)
        {
            return false;
        }
        this->pending_ = true;
        this->command_ = command;
        return true;
    }
    // 消费匹配的排队命令并防止重复执行。
    // 入参：token：请求代次；command：请求命令 ID；ready：消费时业务是否仍可执行。
    // 返回：匹配预订时使其失效并返回 ready；不匹配时 false，保留其他有效预订。
    [[nodiscard]] bool Consume(std::uint64_t token, std::uint32_t command, bool ready) noexcept
    {
        if (!this->pending_ || token != this->token_ || command != this->command_)
        {
            return false;
        }
        this->Invalidate();
        return ready;
    }

  private:
    std::uint64_t token_{1};
    std::uint32_t command_{};
    bool pending_{};
    std::uint32_t inputBarrier_{};
    bool hasInputBarrier_{};
};
} // namespace open_st
