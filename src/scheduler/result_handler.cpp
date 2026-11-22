#include "scheduler/classic_result_handler.hpp"

namespace paintboard {

void ClassicResultHandler::on_tick(const TickEvent& event) {
  for (const TickResult& result : event.results) {
    const PaintOp& op = result.op;
    if (result.status == PaintStatus::Ok) {
      ++status_.painted;
      const bool hit = op.idx < target_.known.size() &&
                       target_.known[op.idx] &&
                       target_.pix[op.idx] == op.color;
      if (hit)
        plan_.mark_done(op.idx);
      else if (op.group == Group::Sonar)
        plan_.release_sonar(op.idx);
      else
        plan_.release(op.idx);
    } else if (result.status == PaintStatus::Cooling) {
      PaintOp retry = op;
      retry.due = 0; // ready_for 的 not_before 会在冷却期内挡住派发
      queue_.restore_consumed(retry);
    } else {
      if (op.group == Group::Sonar)
        plan_.release_sonar(op.idx);
      else
        plan_.release(op.idx);
    }
  }
}

void ClassicResultHandler::on_disconnect(std::span<const PaintOp> pending) {
  for (const PaintOp& original : pending) {
    PaintOp op = original;
    op.due = 0;
    // pending 里的操作在发送时已经 consume，重排必须把计数加回来。
    queue_.restore_consumed(op);
  }
  // 发送异常/重连边界上以真实队列为准修正计数。
  queue_.recount();
}

} // namespace paintboard
