#include "paintboard/board.hpp"
#include "paintboard/config.hpp"
#include "paintboard/paint_queue.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/scheduler.hpp"
#include "scheduler/classic_result_handler.hpp"

// 同一套场景被 classic_scheduler_test 与 new_sch1122_test 复用：
// 后者通过 -DPB_SCHED_* 换掉被测调度器，见 Makefile 的 test-new-scheduler。
#ifndef PB_SCHED_HEADER
#define PB_SCHED_HEADER "scheduler/classic_scheduler.hpp"
#endif
#ifndef PB_SCHED_CLASS
#define PB_SCHED_CLASS ClassicScheduler
#endif
#ifndef PB_SCHED_NAME
#define PB_SCHED_NAME "classic_scheduler_test"
#endif

#include PB_SCHED_HEADER

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace paintboard;

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

// 只实现调度器需要的画笔状态。它不模拟冷却，冷却由测试显式切换 ready。
class FakePainter final : public IPainter {
 public:
  FakePainter(SlotId id, Group group) : id_(id), group_(group) {}

  SlotId id() const override { return id_; }
  Group group() const override { return group_; }
  Token token() const override { return Token{}; }

  SlotView view() const override {
    SlotView v;
    v.id = id_;
    v.group = group_;
    v.bound = bound_;
    v.inflight = inflight_;
    return v;
  }

  bool can_accept(Tick) const override { return bound_ && inflight_ < 2; }

  bool ready_for(std::uint32_t, Tick, std::uint64_t, std::uint64_t,
                 std::uint32_t) const override {
    return ready_;
  }

  bool submit(const PaintOp& op, Tick) override {
    if (!can_accept(0))
      return false;
    submitted.push_back(op);
    ++inflight_;
    return true;
  }

  void request_bind() override {}
  bool wants_bind() const override { return false; }
  void on_bind_result(bool ok) override { bound_ = ok; }
  void on_paint_result(const PaintOp&, PaintStatus, std::uint64_t, Tick) override {}
  void on_brush(std::uint32_t, Tick) override {}
  void on_lost() override { bound_ = false; }
  std::vector<PaintOp> outbox_snapshot() const override { return {}; }
  std::vector<PaintOp> take_outbox() override { return {}; }

  void set_ready(bool ready) { ready_ = ready; }
  void clear_inflight() { inflight_ = 0; }

  const std::vector<PaintOp>& submitted_ops() const { return submitted; }

 private:
  SlotId id_;
  Group group_;
  bool bound_ = true;
  bool ready_ = true;
  std::uint32_t inflight_ = 0;
  std::vector<PaintOp> submitted;
};

struct Harness {
  Config config;
  RuntimeStatus status;
  Logger log;
  Board board;
  PaintQueue queue;
  std::vector<std::unique_ptr<FakePainter>> painters;
  std::vector<IPainter*> raw;
  std::unique_ptr<PB_SCHED_CLASS> scheduler;

  FakePainter& painter(std::size_t i) { return *painters[i]; }
};

std::unique_ptr<Harness> make_harness(Group group, std::vector<Draw> draws) {
  auto h = std::make_unique<Harness>();
  h->config.draws = std::move(draws);
  h->config.paint_ms = 1;
  h->config.repair_ms = 1;
  h->config.sonar_ms = 1;
  h->config.paint_loop_ms = 0;
  h->config.round_cap = 8;
  h->config.queue_cap = 1024;

  auto painter = std::make_unique<FakePainter>(0, group);
  h->raw.push_back(painter.get());
  h->painters.push_back(std::move(painter));

  Rules rules;
  rules.width = 4;
  rules.height = 3;
  rules.radius = 1;
  rules.tps = 100;
  rules.cooldown_base = 20;
  rules.cooldown_per_px = 2;
  rules.max_slots = 16;
  h->board.apply_rules(rules);

  h->scheduler = std::make_unique<PB_SCHED_CLASS>(
      SchedulerDeps{h->queue, h->raw, h->board, h->config, h->status, h->log});
  h->scheduler->prepare();
  return h;
}

// 生产者是异步线程；测试只等它把一笔已 claim 的 op 放进队列，然后立刻
// 停掉线程，之后 step/on_tick 都在单线程下确定性执行。
bool wait_for_queue(PaintQueue& queue, std::size_t want) {
  for (int i = 0; i < 1000; ++i) {
    if (queue.size() >= want)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

struct ResultFixture {
  Target target;
  Plan plan;
  PaintQueue queue;
  RuntimeStatus status;
  ClassicResultHandler handler{target, plan, queue, status};
  PaintOp op;

  ResultFixture() {
    target.built = true;
    target.W = 4;
    target.H = 3;
    target.pix.assign(12, 0);
    target.known.assign(12, 0);
    target.known_idx.push_back(6);
    target.known[6] = 1;
    target.pix[6] = 0xFF112233u;

    plan.alloc(12, 1);
    (void)plan.claim(6);

    op.idx = 6;
    op.color = target.pix[6];
    op.group = Group::Paint;
  }

  // 模拟“已发送”：先入队、取出、consume，队列计数归零。
  void consume_op() {
    queue.push(op);
    std::vector<PaintOp> out;
    queue.take_due(0, out);
    for (const PaintOp& taken : out)
      queue.consume(taken);
  }
};

void test_result_handler_ok() {
  ResultFixture f;
  TickEvent event;
  event.results.push_back(TickResult{f.op, PaintStatus::Ok, 0});
  f.handler.on_tick(event);

  check(f.plan.done(6), "handler OK marks target done");
  check(f.status.painted.load() == 1, "handler OK counts painted");
}

void test_result_handler_cooling() {
  ResultFixture f;
  f.consume_op();
  check(f.queue.size() == 0, "op consumed before cooling");

  TickEvent event;
  event.results.push_back(TickResult{f.op, PaintStatus::Cooling, 500});
  f.handler.on_tick(event);

  check(f.queue.size() == 1, "handler cooling requeues same op");
  check(f.status.painted.load() == 0, "handler cooling not painted");
}

void test_result_handler_disconnect() {
  ResultFixture f;
  f.consume_op();
  std::vector<PaintOp> pending{f.op};
  f.handler.on_disconnect(pending);

  check(f.queue.size() == 1, "handler disconnect restores pending");
  check(f.queue.stats().total == 1, "handler disconnect recounts");
}

void test_step_dispatches_claimed_paint() {
  auto h = make_harness(Group::Paint, {Draw{2, 1, 0xFF112233u}});
  h->scheduler->on_all_bound();
  check(wait_for_queue(h->queue, 1), "paint producer enqueued one op");
  h->scheduler->stop();

  check(h->queue.size() == 1, "one due op before step");
  h->scheduler->step(1000);

  check(h->painter(0).submitted_ops().size() == 1, "step dispatched paint op");
  check(h->painter(0).submitted_ops()[0].idx == 6, "dispatched target pixel");
  check(h->painter(0).submitted_ops()[0].group == Group::Paint,
        "dispatched to paint group");
  check(h->queue.size() == 0, "dispatched op consumed");
  check(h->status.painted.load() == 0, "dispatch is not a result");
}

void test_on_tick_ok_marks_done_and_stops() {
  auto h = make_harness(Group::Paint, {Draw{2, 1, 0xFF112233u}});
  h->scheduler->on_all_bound();
  check(wait_for_queue(h->queue, 1), "paint producer enqueued one op");
  h->scheduler->stop();
  h->scheduler->step(1000);

  TickEvent event;
  event.results.push_back(
      TickResult{h->painter(0).submitted_ops()[0], PaintStatus::Ok, 0});
  h->scheduler->on_tick(event);

  check(h->status.painted.load() == 1, "OK increments painted");
  check(h->scheduler->done(), "target complete -> done");
  check(h->scheduler->stats().painted == 1, "stats reports painted");
}

void test_on_tick_cooling_requeues_same_op() {
  auto h = make_harness(Group::Paint, {Draw{2, 1, 0xFF112233u}});
  h->scheduler->on_all_bound();
  check(wait_for_queue(h->queue, 1), "paint producer enqueued one op");
  h->scheduler->stop();
  h->scheduler->step(1000);
  const PaintOp op = h->painter(0).submitted_ops()[0];

  TickEvent event;
  event.results.push_back(TickResult{op, PaintStatus::Cooling, 500});
  h->scheduler->on_tick(event);

  check(h->status.painted.load() == 0, "cooling is not painted");
  check(h->queue.size() == 1, "cooling requeues the same op");
  check(h->painter(0).submitted_ops().size() == 1, "no duplicate submit");

  // 画笔还没就绪：step 取到 op 后必须原样放回，不能丢。
  h->painter(0).set_ready(false);
  h->scheduler->step(1000);
  check(h->painter(0).submitted_ops().size() == 1, "blocked painter gets nothing");
  check(h->queue.size() == 1, "blocked op returned to queue");

  // 下一轮 now 已过 20s 重排时间，画笔就绪后可以重发。
  h->painter(0).set_ready(true);
  h->scheduler->step(25000);
  check(h->painter(0).submitted_ops().size() == 2, "same op resubmitted");
  check(h->queue.size() == 0, "resubmitted op consumed");
}

void test_on_tick_error_releases_claim() {
  auto h = make_harness(Group::Paint, {Draw{2, 1, 0xFF112233u}});
  h->scheduler->on_all_bound();
  check(wait_for_queue(h->queue, 1), "paint producer enqueued one op");
  h->scheduler->step(1000);

  TickEvent event;
  event.results.push_back(
      TickResult{h->painter(0).submitted_ops()[0], PaintStatus::Error, 0});
  h->scheduler->on_tick(event);

  check(h->status.painted.load() == 0, "error is not painted");
  check(h->queue.size() == 0, "error does not create an op");

  // 生产者仍在跑；claim 释放后它应该重新认领同一个像素。
  check(wait_for_queue(h->queue, 1), "released pixel claimed again");
  h->scheduler->stop();
}

void test_on_disconnect_restores_pending() {
  auto h = make_harness(Group::Paint, {Draw{2, 1, 0xFF112233u}});
  h->scheduler->on_all_bound();
  check(wait_for_queue(h->queue, 1), "paint producer enqueued one op");
  h->scheduler->stop();
  h->scheduler->step(1000);

  const PaintOp op = h->painter(0).submitted_ops()[0];
  check(h->queue.size() == 0, "op consumed on send");
  h->scheduler->on_disconnect({op});

  check(h->queue.size() == 1, "disconnect restores pending op");
  check(h->scheduler->stats().queue_total == 1, "queue count recounted");

  h->painter(0).clear_inflight();
  h->scheduler->step(1000);
  check(h->painter(0).submitted_ops().size() == 2, "pending op resent");
}

} // namespace

int main() {
  test_result_handler_ok();
  test_result_handler_cooling();
  test_result_handler_disconnect();
  test_step_dispatches_claimed_paint();
  test_on_tick_ok_marks_done_and_stops();
  test_on_tick_cooling_requeues_same_op();
  test_on_tick_error_releases_claim();
  test_on_disconnect_restores_pending();

  if (failures == 0) {
    std::cout << PB_SCHED_NAME << " OK (8 scenarios)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
