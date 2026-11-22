#include "paintboard/painter.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace paintboard {
namespace {

std::uint64_t manhattan(std::uint32_t a, std::uint32_t b, std::uint32_t width) {
  if (width == 0)
    return 0;
  const std::uint64_t ax = a % width;
  const std::uint64_t ay = a / width;
  const std::uint64_t bx = b % width;
  const std::uint64_t by = b / width;
  return (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
}

} // namespace

TokenPainter::TokenPainter(SlotId id, Group group, Token token, bool follow)
    : id_(id), group_(group), token_(token), follow_(follow) {}

SlotId TokenPainter::id() const { return id_; }
Group TokenPainter::group() const { return group_; }
Token TokenPainter::token() const { return token_; }

SlotView TokenPainter::view() const {
  std::lock_guard<std::mutex> lock(mu_);
  SlotView out;
  out.id = id_;
  out.group = group_;
  out.follow = follow_;
  out.bound = bound_;
  out.want_bind = want_bind_;
  out.inflight = inflight_;
  out.has_brush = has_brush_;
  out.brush_idx = brush_idx_;
  out.brush_time = brush_time_;
  out.has_projected = has_projected_;
  out.projected_idx = projected_idx_;
  out.projected_time = projected_time_;
  out.not_before = not_before_;
  return out;
}

bool TokenPainter::can_accept(Tick) const {
  std::lock_guard<std::mutex> lock(mu_);
  return bound_ && inflight_ < 2;
}

bool TokenPainter::ready_for(std::uint32_t idx, Tick now,
                             std::uint64_t cooldown_base,
                             std::uint64_t cooldown_per_px,
                             std::uint32_t width) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!bound_)
    return false;

  Tick ready = not_before_;
  if (has_projected_) {
    const Tick travel = cooldown_base + cooldown_per_px *
                                             manhattan(idx, projected_idx_, width);
    Tick projected_ready = projected_time_ + travel;
    ready = std::max(ready, projected_ready);
  }
  return ready <= now;
}

bool TokenPainter::submit(const PaintOp& op, Tick now) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!bound_ || inflight_ >= 2)
    return false;

  has_projected_ = true;
  projected_idx_ = op.idx;
  projected_time_ = now;
  ++inflight_;
  outbox_.push_back(op);
  return true;
}

void TokenPainter::request_bind() {
  std::lock_guard<std::mutex> lock(mu_);
  bound_ = false;
  want_bind_ = true;
  inflight_ = 0;
  has_projected_ = false;
  not_before_ = 0;
}

bool TokenPainter::wants_bind() const {
  std::lock_guard<std::mutex> lock(mu_);
  return want_bind_;
}

void TokenPainter::on_bind_result(bool ok) {
  std::lock_guard<std::mutex> lock(mu_);
  if (ok) {
    bound_ = true;
    want_bind_ = false;
  } else {
    bound_ = false;
    // 失败 token 不再每 50ms 重绑，避免刷屏和占满绑定队列；
    // 重连时 request_bind() 会重新置 want_bind_。
    want_bind_ = false;
  }
}

void TokenPainter::on_paint_result(const PaintOp& op, PaintStatus status,
                                   std::uint64_t value, Tick now) {
  (void)op;
  std::lock_guard<std::mutex> lock(mu_);
  if (inflight_ > 0)
    --inflight_;

  switch (status) {
    case PaintStatus::Ok:
      break;
    case PaintStatus::Cooling:
      has_projected_ = has_brush_;
      projected_idx_ = brush_idx_;
      projected_time_ = brush_time_;
      not_before_ = now + (value ? value : 1000);
      break;
    case PaintStatus::SlotInvalid:
      bound_ = false;
      want_bind_ = true;
      inflight_ = 0;
      has_projected_ = false;
      break;
    case PaintStatus::Error:
      inflight_ = 0;
      has_projected_ = false;
      break;
  }
}

void TokenPainter::on_brush(std::uint32_t idx, Tick t) {
  std::lock_guard<std::mutex> lock(mu_);
  has_brush_ = (t != 0);
  brush_idx_ = idx;
  brush_time_ = t;
  has_projected_ = has_brush_;
  projected_idx_ = brush_idx_;
  projected_time_ = brush_time_;
}

void TokenPainter::on_lost() {
  std::lock_guard<std::mutex> lock(mu_);
  bound_ = false;
  want_bind_ = true;
  inflight_ = 0;
  has_brush_ = false;
  has_projected_ = false;
  not_before_ = 0;
}

std::vector<PaintOp> TokenPainter::outbox_snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return outbox_;
}

std::vector<PaintOp> TokenPainter::take_outbox() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PaintOp> out;
  out.swap(outbox_);
  return out;
}

} // namespace paintboard
