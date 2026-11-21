#pragma once

#include "paintboard/scheduler.hpp"

#include <cstddef>
#include <vector>

namespace paintboard {

// 最小的可替换调度器：不依赖 Target/Plan，只把到期队列按顺序轮流交给
// 本组空闲画笔。用于证明 IScheduler 边界足够，不代表生产策略。
class RoundRobinScheduler final : public IScheduler {
 public:
  RoundRobinScheduler(PaintQueue& queue, std::vector<IPainter*>& painters,
                      RuntimeStatus& status, Logger& log);

  void prepare() override;
  void on_all_bound() override;
  void step(Tick now) override;
  void on_tick(const TickEvent& event) override;
  void on_disconnect(std::vector<PaintOp> pending) override;
  bool done() const override;
  SchedulerStats stats() const override;
  void stop() override;

 private:
  PaintQueue& queue_;
  std::vector<IPainter*>& painters_;
  RuntimeStatus& status_;
  Logger& log_;
  std::size_t next_ = 0;
  SchedulerStats stats_;
};

} // namespace paintboard
