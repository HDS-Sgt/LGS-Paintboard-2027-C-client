#pragma once

#include "paintboard/scheduler.hpp"
#include "paintboard/types.hpp"

#include <span>
#include <vector>

namespace paintboard {

// 把服务器 TICK/断线事件翻译成 Plan/Queue/Status 的变化。
// 纯状态机：不持有线程、socket 或调度策略，便于直接单测。
class ClassicResultHandler {
 public:
  ClassicResultHandler(Target& target, Plan& plan, PaintQueue& queue,
                       RuntimeStatus& status)
      : target_(target), plan_(plan), queue_(queue), status_(status) {}

  void on_tick(const TickEvent& event);
  void on_disconnect(std::span<const PaintOp> pending);

 private:
  Target& target_;
  Plan& plan_;
  PaintQueue& queue_;
  RuntimeStatus& status_;
};

} // namespace paintboard
