// NewSch1122 声纳密铺的调度层集成测试（Classic 旧实现不参与）。
//
// 覆盖 sonar_tiling_test 管不到的路径：sonar_loop 从 slot_centers 取中心、
// 按分块把 affinity 写成“负责该块的槽位”，step 只把 op 派给 affinity 对应
// 的槽位，且不同槽位之间不会重复同一个中心。
#include "paintboard/board.hpp"
#include "paintboard/config.hpp"
#include "paintboard/paint_queue.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/scheduler.hpp"
#include "scheduler/new_sch1122.hpp"

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
    // 本测试只验证派发/归属，不模拟在途结果：假装上一笔立刻回来。
    v.inflight = 0;
    return v;
  }

  bool can_accept(Tick) const override { return bound_; }
  bool ready_for(std::uint32_t, Tick, std::uint64_t, std::uint64_t,
                 std::uint32_t) const override {
    return bound_;
  }

  bool submit(const PaintOp& op, Tick) override {
    if (!can_accept(0))
      return false;
    submitted_.push_back(op);
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

  const std::vector<PaintOp>& submitted() const { return submitted_; }

 private:
  SlotId id_;
  Group group_;
  bool bound_ = true;
  std::vector<PaintOp> submitted_;
};

bool wait_sonar(PaintQueue& queue, std::size_t want) {
  for (int i = 0; i < 2000; ++i) {
    if (queue.size(Group::Sonar) >= want)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

} // namespace

int main() {
  Config config;
  config.paint_ms = 1;
  config.repair_ms = 1;
  config.sonar_ms = 1;
  config.round_cap = 32;
  config.queue_cap = 1024;
  // 12x10 的已知像素块，放在 16x14 画布上；用 Draw 而不用图片，
  // 测试不依赖外部文件。
  for (std::uint32_t y = 2; y < 12; ++y)
    for (std::uint32_t x = 2; x < 14; ++x)
      config.draws.push_back(Draw{x, y, 0xFFFFFFFFu});

  RuntimeStatus status;
  Logger log;
  Board board;
  Rules rules;
  rules.width = 16;
  rules.height = 14;
  rules.radius = 2;
  rules.tps = 100;
  rules.cooldown_base = 20;
  rules.cooldown_per_px = 2;
  rules.max_slots = 16;
  board.apply_rules(rules);

  PaintQueue queue;
  std::vector<std::unique_ptr<FakePainter>> owned;
  std::vector<IPainter*> painters;
  for (SlotId id = 0; id < 3; ++id) {
    owned.push_back(std::make_unique<FakePainter>(id, Group::Sonar));
    painters.push_back(owned.back().get());
  }

  NewSch1122 scheduler(
      SchedulerDeps{queue, painters, board, config, status, log});
  scheduler.prepare();
  scheduler.on_all_bound();

  check(wait_sonar(queue, 3), "sonar_loop enqueued block-owned centers");
  scheduler.stop();

  // step 会把本轮没派出去的 op 推迟 20ms（20000 微秒）再入队，所以测试
  // 必须把 now 往前推，否则第二轮 take_due 什么也取不到。
  for (int i = 0; i < 50 && queue.size(Group::Sonar) > 0; ++i)
    scheduler.step(1000 + static_cast<Tick>(i + 1) * 100000);

  check(queue.size(Group::Sonar) == 0, "all sonar ops dispatched");

  std::vector<bool> seen(16 * 14, false);
  std::size_t total = 0;
  for (std::size_t i = 0; i < painters.size(); ++i) {
    const auto& ops = owned[i]->submitted();
    check(!ops.empty(), "every sonar slot got at least one center");
    for (const PaintOp& op : ops) {
      ++total;
      check(op.group == Group::Sonar, "only sonar ops on sonar slots");
      check(op.affinity && *op.affinity == static_cast<SlotId>(i),
            "op affinity matches owning slot");
      check(op.idx < seen.size(), "center inside canvas");
      check(!seen[op.idx], "center not duplicated across slots");
      if (op.idx < seen.size())
        seen[op.idx] = true;
      check(op.color == 0xFFFFFFFFu, "center color comes from target");
    }
  }
  check(total >= 3, "at least one center per slot was dispatched");

  if (failures == 0) {
    std::cout << "new_sch1122_sonar_test OK (3 slots, R=2, " << total
              << " centers)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
