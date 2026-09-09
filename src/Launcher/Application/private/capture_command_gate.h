#pragma once

#include <cstdint>

namespace open_st
{
// UI 线程共用的单请求闸门；代次跨截图会话递增，消息不持有对象指针。
class CaptureCommandGate final
{
  public:
    // 返回当前选区操作代次，用于工具栏显示及消息投递。
    [[nodiscard]] std::uint64_t Token() const noexcept
    {
        return this->token_;
    }
    // 返回是否已有按钮或快捷键命令等待处理。
    [[nodiscard]] bool Pending() const noexcept
    {
        return this->pending_;
    }
    // 选区/会话变更及模态边界记录系统消息时钟，屏蔽此前尚未分派的完成快捷键。
    void SetInputBarrier(std::uint32_t time) noexcept
    {
        this->inputBarrier_ = time;
        this->hasInputBarrier_ = true;
    }
    // Win32 毫秒计数允许回绕；与边界同毫秒的输入保守拒绝。
    [[nodiscard]] bool AcceptsInput(std::uint32_t time) const noexcept
    {
        return !this->hasInputBarrier_ || static_cast<std::int32_t>(time - this->inputBarrier_) > 0;
    }
    // 选区改变、关闭或进入模态操作时，撤销旧请求并切换代次。
    void Invalidate() noexcept
    {
        ++this->token_;
        if (this->token_ == 0)
        {
            ++this->token_;
        }
        this->pending_ = false;
    }
    // 两种输入共享预订；投递失败由调用者 Invalidate 回滚。
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
    // 消息只能消费一次；旧消息不能释放新请求的 pending 状态。
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
