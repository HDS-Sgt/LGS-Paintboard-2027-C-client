#include "scheduler/round_robin.hpp"

#include "paintboard/painter.hpp"

#include <utility>

namespace paintboard {

RoundRobinScheduler::RoundRobinScheduler(PaintQueue& queue,
                                         std::vector<IPainter*>& painters,
                                         RuntimeStatus& status, Logger& log)
    : queue_(queue), painters_(painters), status_(status), log_(log) {}

void RoundRobinScheduler::prepare() {}

void RoundRobinScheduler::on_all_bound() {}

void RoundRobinScheduler::step(Tick now) {
  std::vector<PaintOp> live;
  queue_.take_due(now, live);

  const std::size_t count = painters_.size();
  for (PaintOp& op : live) {
    bool sent = false;
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t i = (next_ + k) % count;
      IPainter* painter = painters_[i];
      if (!painter || painter->group() != op.group)
        continue;
      if (!painter->can_accept(now))
        continue;
      if (!painter->submit(op, now))
        continue;

      queue_.consume(op);
      next_ = (i + 1) % count;
      sent = true;
      break;
    }
    if (!sent) {
      // 没有本组空闲画笔：留在队列里，稍后再试；计数不变。
      op.due = now + 1000;
      queue_.requeue(op);
    }
  }
  stats_.queue_total = queue_.size();
}

void RoundRobinScheduler::on_tick(const TickEvent& event) {
  for (const TickResult& result : event.results) {
    if (result.status != PaintStatus::Ok)
      continue;
    ++stats_.painted;
    status_.painted.fetch_add(1);
  }
}

void RoundRobinScheduler::on_disconnect(std::vector<PaintOp> pending) {
  for (PaintOp& op : pending) {
    op.due = 0;
    // pending 里的操作在发送时已经 consume，重排必须把计数加回来。
    queue_.restore_consumed(op);
  }
}

bool RoundRobinScheduler::done() const { return false; }

SchedulerStats RoundRobinScheduler::stats() const {
  SchedulerStats out = stats_;
  out.queue_total = queue_.size();
  return out;
}

void RoundRobinScheduler::stop() {}

} // namespace paintboard
