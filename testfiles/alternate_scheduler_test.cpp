// 证明调度策略可替换：RoundRobinScheduler 只依赖 PaintQueue + IPainter，
// 不需要 Target/Plan，也不需要改队列或画笔接口。
#include "paintboard/paint_queue.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/scheduler.hpp"
#include "scheduler/classic_scheduler.hpp"
#include "scheduler/round_robin.hpp"

#include <cstdio>
#include <type_traits>
#include <vector>

using namespace paintboard;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

class FakePainter final : public IPainter {
 public:
  FakePainter(SlotId id, Group group, std::uint32_t capacity)
      : id_(id), group_(group), capacity_(capacity) {}

  SlotId id() const override { return id_; }
  Group group() const override { return group_; }
  Token token() const override { return Token{}; }

  SlotView view() const override {
    SlotView view;
    view.id = id_;
    view.group = group_;
    view.bound = bound_;
    view.inflight = inflight_;
    return view;
  }

  bool can_accept(Tick) const override {
    return bound_ && inflight_ < capacity_;
  }

  bool ready_for(std::uint32_t, Tick now, std::uint64_t, std::uint64_t,
                 std::uint32_t) const override {
    return can_accept(now);
  }

  bool submit(const PaintOp& op, Tick) override {
    if (!can_accept(0))
      return false;
    sent.push_back(op);
    ++inflight_;
    return true;
  }

  void request_bind() override {}
  bool wants_bind() const override { return false; }
  void on_bind_result(bool ok) override { bound_ = ok; }
  void on_paint_result(const PaintOp&, PaintStatus, std::uint64_t,
                       Tick) override {
    if (inflight_ > 0)
      --inflight_;
  }
  void on_brush(std::uint32_t, Tick) override {}
  void on_lost() override { bound_ = false; }
  std::vector<PaintOp> take_outbox() override { return {}; }

  void set_bound(bool bound) { bound_ = bound; }
  void set_capacity(std::uint32_t capacity) { capacity_ = capacity; }

  std::vector<PaintOp> sent;

 private:
  SlotId id_;
  Group group_;
  std::uint32_t capacity_;
  bool bound_ = true;
  std::uint32_t inflight_ = 0;
};

PaintOp op(std::uint32_t idx) {
  PaintOp out;
  out.id = idx + 1;
  out.idx = idx;
  out.color = 0xFF0000FFu;
  out.group = Group::Paint;
  out.due = 0;
  return out;
}

} // namespace

int main() {
  static_assert(std::is_base_of_v<IScheduler, RoundRobinScheduler>);
  static_assert(std::is_base_of_v<IScheduler, ClassicScheduler>);

  RuntimeStatus status;
  Logger log;
  PaintQueue queue;
  FakePainter p0(0, Group::Paint, 2);
  FakePainter p1(1, Group::Paint, 2);
  FakePainter p2(2, Group::Paint, 2);
  std::vector<IPainter*> painters{&p0, &p1, &p2};
  RoundRobinScheduler scheduler(queue, painters, status, log);

  for (std::uint32_t i = 0; i < 5; ++i)
    queue.push(op(i));
  scheduler.step(100);

  check(p0.sent.size() == 2 && p1.sent.size() == 2 && p2.sent.size() == 1,
        "round-robin distributes 5 ops across 3 painters");
  check(p0.sent[0].idx == 0 && p1.sent[0].idx == 1 && p2.sent[0].idx == 2,
        "first round follows queue order");
  check(p0.sent[1].idx == 3 && p1.sent[1].idx == 4,
        "second round continues queue order");
  check(queue.size() == 0, "sent ops are consumed");
  check(status.painted.load() == 0, "painted counts only OK results");

  for (std::uint32_t i = 5; i < 7; ++i)
    queue.push(op(i));
  p0.set_bound(false);
  p1.set_bound(false);
  p2.set_bound(false);
  scheduler.step(200);
  check(queue.size() == 2, "blocked painters leave ops queued");
  check(p0.sent.size() == 2 && p1.sent.size() == 2 && p2.sent.size() == 1,
        "blocked painters receive nothing");

  // 分组匹配：repair op 不会被 paint 槽位接走。
  PaintOp repair = op(9);
  repair.group = Group::Repair;
  queue.push(repair);
  p0.set_bound(true);
  p1.set_bound(true);
  p2.set_bound(true);
  scheduler.step(300);
  check(p0.sent.size() == 2 && p1.sent.size() == 2 && p2.sent.size() == 1,
        "paint painters do not receive repair ops");
  check(queue.size() == 3, "unmatched group stays queued");

  if (failures != 0) {
    std::fprintf(stderr, "alternate_scheduler_test FAILED (%d)\n", failures);
    return 1;
  }
  std::puts("alternate_scheduler_test OK");
  return 0;
}
