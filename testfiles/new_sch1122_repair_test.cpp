// NewSch1122 repair 匀配的调度层集成测试。
//
// 覆盖：
//   1. Plan::claim_repair 允许对已 done 的目标像素再次认领（旧 claim 会拒绝）；
//   2. 末尾 16 个 repair 槽作为自由槽，其余槽绑定声纳中心；
//   3. 绑定槽只接到自己负责中心的 op（affinity == 槽 id）；
//   4. 自由槽仍能通过原 damaged 扫描发现并修复被破坏的像素。
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

void ack_ok(NewSch1122& scheduler, const std::vector<PaintOp>& ops,
            std::size_t& processed) {
  TickEvent event;
  event.tick = 1;
  for (; processed < ops.size(); ++processed) {
    TickResult result;
    result.op = ops[processed];
    result.status = PaintStatus::Ok;
    event.results.push_back(result);
  }
  if (!event.results.empty())
    scheduler.on_tick(event);
}

} // namespace

int main() {
  Config config;
  config.paint_ms = 1;
  config.repair_ms = 1;
  config.sonar_ms = 1;
  config.round_cap = 64;
  config.queue_cap = 2048;
  config.repair_verify_ms = 0; // 没有真实声纳时间线，关闭 stale 判定
  // 12x10 已知块，画在 16x14 画布上。
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
  rules.max_slots = 32;
  board.apply_rules(rules);

  PaintQueue queue;
  std::vector<std::unique_ptr<FakePainter>> owned;
  std::vector<IPainter*> painters;

  // id 0：sonar 槽，负责生成密铺中心；id 1：paint 槽，把目标画完。
  owned.push_back(std::make_unique<FakePainter>(0, Group::Sonar));
  painters.push_back(owned.back().get());
  owned.push_back(std::make_unique<FakePainter>(1, Group::Paint));
  painters.push_back(owned.back().get());

  // id 2..21：20 个 repair 槽。自由槽取末尾 16 个（6..21），
  // 绑定槽是前 4 个（2..5）。
  for (SlotId id = 2; id <= 21; ++id) {
    owned.push_back(std::make_unique<FakePainter>(id, Group::Repair));
    painters.push_back(owned.back().get());
  }
  FakePainter* paint_painter = owned[1].get();

  NewSch1122 scheduler(
      SchedulerDeps{queue, painters, board, config, status, log});
  scheduler.prepare();
  scheduler.on_all_bound();

  // 先把 120 个目标像素真正画完，让 repair 的 done 条件成立。
  Tick now = 1000;
  std::size_t processed = 0;
  for (int iter = 0; iter < 5000 && processed < 120 && failures == 0; ++iter) {
    scheduler.step(now);
    now += 50000;
    ack_ok(scheduler, paint_painter->submitted(), processed);
    if (processed >= 120)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(processed == 120, "all target pixels were painted and acked");

  // 破坏一个已画好的像素：自由 repair 的 damaged 扫描必须能发现它。
  const std::uint32_t damaged_idx = 2 * 16 + 2;
  SonarBatch batch;
  batch.radius = 2;
  batch.tick = 1;
  batch.center.push_back(damaged_idx);
  const std::uint64_t cells =
      2ull * batch.radius * batch.radius + 2ull * batch.radius + 1;
  batch.color.assign(static_cast<std::size_t>(cells), 0xFF0000FFu); // red
  board.apply_sonar(batch);
  const SonarSnapshot sonar = board.sonar_snapshot();

  // 给 repair_loop 足够时间跑到 damaged 像素。绑定槽每轮都会补充中心，
  // 不能只看队列长度（它可能在打补丁前就已经 >=5）。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  scheduler.stop();

  // step 会把未派发 op 推迟 20ms，必须同步推进 now 才能排空。
  for (int i = 0; i < 200 && queue.size() > 0; ++i)
    scheduler.step(now + static_cast<Tick>(i + 1) * 100000);

  std::size_t bound_ops = 0;
  std::size_t free_ops = 0;
  bool free_saw_damaged = false;
  for (std::size_t i = 0; i < owned.size(); ++i) {
    const SlotId id = static_cast<SlotId>(i);
    const auto& ops = owned[i]->submitted();
    for (const PaintOp& op : ops) {
      if (op.group != Group::Repair)
        continue;
      check(op.affinity && *op.affinity == id,
            "repair op affinity matches the dispatching slot");
      if (id >= 2 && id <= 5) {
        ++bound_ops;
      } else if (id >= 6 && id <= 21) {
        ++free_ops;
        // 自由槽只可能来自 damaged 扫描（verify_span=0 已关闭 stale）。
        // 声纳读数与目标色不同，就说明它确实在修被破坏的像素。
        if (op.idx < sonar.color.size() && sonar.tick[op.idx] != 0 &&
            sonar.color[op.idx] != 0xFFFFFFFFu)
          free_saw_damaged = true;
        if (op.idx == damaged_idx) {
          free_saw_damaged = true;
          check(op.color == 0xFFFFFFFFu,
                "free repair repaints the target color");
        }
      }
    }
  }

  check(bound_ops >= 4, "four bound repair slots each got a sonar center");
  check(free_ops >= 1, "free repair slots handled damaged pixels");
  check(free_saw_damaged, "damaged pixel was repaired by a free slot");
  check(!paint_painter->submitted().empty(), "paint slot did work");

  scheduler.stop();

  if (failures == 0) {
    std::cout << "new_sch1122_repair_test OK (20 repair slots: 4 bound + 16 "
                 "free, damaged repaired)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
