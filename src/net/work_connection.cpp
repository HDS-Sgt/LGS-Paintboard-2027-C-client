#include "net/work_connection.hpp"

#include "protocol/wire.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace paintboard {

WorkConnection::WorkConnection(std::vector<IPainter*> painters,
                               std::unique_ptr<ITransport> transport,
                               const Config& config, RuntimeStatus& status,
                               Logger& log)
    : painters_(std::move(painters)),
      transport_(std::move(transport)),
      config_(config),
      status_(status),
      log_(log) {}

void WorkConnection::connect() {
  if (!transport_)
    throw std::runtime_error("work: missing transport");

  const std::string url = "wss://" + config_.host + ":" + config_.port +
                          "/ws?public=0";
  transport_->connect(url);

  pend_.clear();
  bind_pending_.clear();
  bind_fail_count_ = 0;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    auto payload = transport_->recv(std::chrono::milliseconds(500));
    if (!payload || payload->empty())
      continue;
    if ((*payload)[0] == 0x84) {
      rules_ = protocol::parse_rules(*payload);
      clock_.calibrate(rules_.server_time);
      status_.ready.store(true);
      return;
    }
    // RULES 之前的 TICK 没有可路由的 pend；按前向兼容忽略。
  }
  throw std::runtime_error("work: no RULES received");
}

void WorkConnection::request_all_binds() {
  for (IPainter* painter : painters_)
    if (painter)
      painter->request_bind();
}

void WorkConnection::flush() {
  std::vector<Bind> binds;
  std::vector<std::pair<IPainter*, PaintOp>> paints;

  for (IPainter* painter : painters_) {
    if (!painter)
      continue;

    const SlotView view = painter->view();
    if (view.want_bind && bind_pending_.count(painter->id()) == 0)
      binds.push_back(Bind{painter->id(), painter->token()});

    for (const PaintOp& op : painter->outbox_snapshot()) {
      // 队列里的 op 属于哪一组由生产者决定；这里只负责按画笔槽位发送。
      PaintOp queued = op;
      if (queued.group != painter->group())
        queued.group = painter->group();
      paints.emplace_back(painter, std::move(queued));
    }
  }

  if (binds.empty() && paints.empty())
    return;

  std::vector<Paint> wire_paints;
  wire_paints.reserve(paints.size());
  for (const auto& entry : paints)
    wire_paints.push_back(
        Paint{entry.first->id(), entry.second.idx, entry.second.color});

  const std::vector<std::uint8_t> payload =
      protocol::encode_batch(binds, wire_paints);
  transport_->send(payload);

  // 只有 send 成功后才清空 outbox；send 抛异常时 op 留在原处，重连后重发，
  // 不会出现“队列已 consume、outbox 已清空、pend 又没记录”的认领泄漏。
  for (IPainter* painter : painters_)
    if (painter)
      painter->take_outbox();

  for (const Bind& bind : binds) {
    bind_pending_.insert(bind.slot);
    Pend rec;
    rec.bind = true;
    rec.slot = bind.slot;
    pend_.push_back(rec);
  }
  for (const auto& entry : paints) {
    Pend rec;
    rec.bind = false;
    rec.slot = entry.first->id();
    rec.op = entry.second;
    pend_.push_back(rec);
    ++status_.sent[group_index(entry.second.group)];
  }
}

std::optional<TickEvent> WorkConnection::poll(
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() < 0)
      return std::nullopt;

    auto payload = transport_->recv(left);
    if (!payload || payload->empty())
      return std::nullopt;

    const std::uint8_t type = (*payload)[0];
    if (type == 0x84) {
      rules_ = protocol::parse_rules(*payload);
      clock_.calibrate(rules_.server_time);
      status_.ready.store(true);
      continue;
    }
    if (type == 0x81) {
      TickEvent event;
      protocol::parse_tick(*payload, event);
      handle_tick(event);
      return event;
    }
    // 未知下行类型必须忽略，保持前向兼容。
  }
}

void WorkConnection::handle_tick(TickEvent& event) {
  const Tick now = clock_.now();
  status_.last_tick.store(event.tick);

  std::vector<TickResult> paint_results;
  paint_results.reserve(event.results.size());

  for (const TickResult& result : event.results) {
    if (pend_.empty()) {
      log_.logf("[work] {} results but no pending op; extra ignored",
                event.results.size());
      break;
    }
    Pend rec = std::move(pend_.front());
    pend_.pop_front();

    IPainter* painter = find(rec.slot);
    if (rec.bind) {
      bind_pending_.erase(rec.slot);
      if (result.status != PaintStatus::Ok)
        ++bind_fail_count_;
      if (painter)
        painter->on_bind_result(result.status == PaintStatus::Ok);
      continue;
    }

    if (painter)
      painter->on_paint_result(rec.op, result.status, result.value, now);
    if (result.status == PaintStatus::Ok)
      ++status_.ok[group_index(rec.op.group)];

    TickResult out = result;
    out.op = rec.op;
    paint_results.push_back(std::move(out));
  }

  for (SlotId slot : event.lost) {
    bind_pending_.erase(slot);
    if (IPainter* painter = find(slot))
      painter->on_lost();
  }

  // 画笔是服务器权威值，最后应用。
  for (const BrushUpdate& brush : event.brushes)
    if (IPainter* painter = find(brush.slot))
      painter->on_brush(brush.idx, brush.t);

  event.results = std::move(paint_results);
}

bool WorkConnection::binds_settled() const {
  if (!bind_pending_.empty())
    return false;
  for (const IPainter* painter : painters_)
    if (painter && painter->view().want_bind)
      return false;
  return true;
}

bool WorkConnection::all_bound() const {
  for (const IPainter* painter : painters_)
    if (painter && !painter->view().bound)
      return false;
  return true;
}

std::vector<PaintOp> WorkConnection::take_pending() {
  std::vector<PaintOp> out;
  for (auto it = pend_.begin(); it != pend_.end();) {
    if (it->bind) {
      ++it;
      continue;
    }
    out.push_back(it->op);
    it = pend_.erase(it);
  }
  return out;
}

std::size_t WorkConnection::pending_paint_count() const {
  std::size_t count = 0;
  for (const Pend& rec : pend_)
    if (!rec.bind)
      ++count;
  return count;
}

void WorkConnection::close() {
  if (transport_)
    transport_->close();
}

Rules WorkConnection::rules() const { return rules_; }

IPainter* WorkConnection::find(SlotId slot) const {
  for (IPainter* painter : painters_)
    if (painter && painter->id() == slot)
      return painter;
  return nullptr;
}

std::size_t WorkConnection::group_index(Group group) {
  return static_cast<std::size_t>(group);
}

} // namespace paintboard
