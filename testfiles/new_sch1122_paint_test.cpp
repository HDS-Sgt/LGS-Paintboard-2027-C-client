// NewSch1122 paint 矩形分块 + 块内蛇形的调度层集成测试。
//
// 覆盖：
//   1. paint op 带 affinity，只有归属的那个 paint slot 能接到；
//   2. 12x10 目标用 4 个 paint 槽时切成 2x2 的 6x5 矩形，每槽一块；
//   3. 每个槽的提交顺序是块内逐行蛇形（不是全局行优先或菱形跳点）；
//   4. --paint-loop 重刷后，每槽仍只在本块内重画。
#include "paintboard/board.hpp"
#include "paintboard/config.hpp"
#include "paintboard/paint_queue.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/scheduler.hpp"
#include "scheduler/new_sch1122.hpp"

#include <algorithm>
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

// 连续分块映射：blocks <= slots 时一块一槽；否则每槽接一段连续块号。
void check_paint_slot_mapping() {
  for (std::size_t blocks : {std::size_t{1}, std::size_t{5}, std::size_t{20},
                             std::size_t{300}}) {
    for (std::size_t slots :
         {std::size_t{1}, std::size_t{6}, std::size_t{20}, std::size_t{300},
          std::size_t{1000}}) {
      if (blocks <= slots) {
        for (std::size_t b = 0; b < blocks; ++b)
          check(paint_slot_for_block(b, blocks, slots) == b,
                "one block per slot when blocks<=slots");
        continue;
      }
      // 每个槽拿到的是连续区间：b 的槽号随 b 单调不减。
      std::size_t prev = 0;
      for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t s = paint_slot_for_block(b, blocks, slots);
        check(s < slots, "paint slot index in range");
        check(b == 0 || s >= prev, "slot ranges are contiguous and ordered");
        prev = s;
      }
      check(paint_slot_for_block(0, blocks, slots) == 0,
            "first block maps to first slot");
      check(paint_slot_for_block(blocks - 1, blocks, slots) == slots - 1,
            "last block maps to last slot");
    }
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

void ack_all(NewSch1122& scheduler,
             const std::vector<std::unique_ptr<FakePainter>>& owned,
             std::vector<std::size_t>& acked) {
  TickEvent event;
  event.tick = 1;
  for (std::size_t i = 0; i < owned.size(); ++i) {
    const auto& ops = owned[i]->submitted();
    for (; acked[i] < ops.size(); ++acked[i]) {
      TickResult result;
      result.op = ops[acked[i]];
      result.status = PaintStatus::Ok;
      event.results.push_back(result);
    }
  }
  if (!event.results.empty())
    scheduler.on_tick(event);
}

// 12x10 目标、4 个 paint 槽：2x2 矩形，6x5 一块；块内逐行蛇形。
std::vector<std::uint32_t> expected_block(std::size_t slot) {
  const std::uint32_t x0 = (slot % 2 == 0) ? 2u : 8u;
  const std::uint32_t x1 = x0 + 5;
  const std::uint32_t y0 = (slot < 2) ? 2u : 7u;
  const std::uint32_t y1 = y0 + 4;
  std::vector<std::uint32_t> pixels;
  for (std::uint32_t y = y0; y <= y1; ++y) {
    const bool reverse = ((y - y0) & 1u) != 0;
    for (std::uint32_t i = 0; i <= x1 - x0; ++i) {
      const std::uint32_t x = reverse ? x1 - i : x0 + i;
      pixels.push_back(y * 16 + x);
    }
  }
  return pixels;
}

// 4096 个 paint 槽时，round_cap=256 不应把一波截成 16 段：一块一槽，
// 第一波应能把 4096 个块全部入队（一帧 BATCH 约 32 KiB paint 列）。
void check_large_wave() {
  Config config;
  config.paint_ms = 1;
  config.round_cap = 256;
  config.queue_cap = 2048;
  constexpr std::uint32_t N = 64; // 64x64 = 4096 个目标像素
  for (std::uint32_t y = 0; y < N; ++y)
    for (std::uint32_t x = 0; x < N; ++x)
      config.draws.push_back(Draw{x, y, 0xFFFFFFFFu});

  RuntimeStatus status;
  Logger log;
  Board board;
  Rules rules;
  rules.width = N;
  rules.height = N;
  rules.max_slots = 8192;
  board.apply_rules(rules);

  PaintQueue queue;
  std::vector<std::unique_ptr<FakePainter>> owned;
  std::vector<IPainter*> painters;
  for (SlotId id = 0; id < 4096; ++id) {
    owned.push_back(std::make_unique<FakePainter>(id, Group::Paint));
    painters.push_back(owned.back().get());
  }

  NewSch1122 scheduler(
      SchedulerDeps{queue, painters, board, config, status, log});
  scheduler.prepare();
  scheduler.on_all_bound();

  for (int i = 0; i < 2000 && queue.size() < 4096; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  scheduler.stop();
  check(queue.size() == 4096,
        "one wave enqueues all 4096 blocks despite round_cap=256");
}

} // namespace

int main() {
  check_large_wave();
  check_paint_slot_mapping();
  Config config;
  config.paint_ms = 1;
  config.paint_loop_ms = 0;
  config.round_cap = 64;
  config.queue_cap = 2048;
  for (std::uint32_t y = 2; y < 12; ++y)
    for (std::uint32_t x = 2; x < 14; ++x)
      config.draws.push_back(Draw{x, y, 0xFFFFFFFFu});

  RuntimeStatus status;
  Logger log;
  Board board;
  Rules rules;
  rules.width = 16;
  rules.height = 14;
  rules.max_slots = 32;
  board.apply_rules(rules);

  PaintQueue queue;
  std::vector<std::unique_ptr<FakePainter>> owned;
  std::vector<IPainter*> painters;
  for (SlotId id = 0; id < 4; ++id) {
    owned.push_back(std::make_unique<FakePainter>(id, Group::Paint));
    painters.push_back(owned.back().get());
  }

  NewSch1122 scheduler(
      SchedulerDeps{queue, painters, board, config, status, log});
  scheduler.prepare();
  scheduler.on_all_bound();

  Tick now = 1000;
  std::vector<std::size_t> acked(owned.size(), 0);
  std::size_t processed = 0;
  for (int iter = 0; iter < 3000; ++iter) {
    scheduler.step(now);
    now += 50000;
    ack_all(scheduler, owned, acked);
    processed = 0;
    for (std::size_t i = 0; i < owned.size(); ++i)
      processed += owned[i]->submitted().size();
    if (processed >= 120)
      break;
    if (iter % 8 == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(processed == 120, "all 120 target pixels were submitted and acked");

  // 第一轮：每槽 30 个像素，顺序必须是块内蛇形。
  std::vector<std::vector<PaintOp>> first;
  for (const auto& p : owned)
    first.push_back(p->submitted());
  for (std::size_t slot = 0; slot < owned.size(); ++slot) {
    const auto expected = expected_block(slot);
    check(first[slot].size() >= expected.size(),
          "paint slot painted its whole block");
    for (std::size_t k = 0;
         k < expected.size() && k < first[slot].size(); ++k) {
      check(first[slot][k].idx == expected[k],
            "paint block is a rectangle scanned in row-snake order");
      check(first[slot][k].affinity &&
                *first[slot][k].affinity == static_cast<SlotId>(slot),
            "paint op affinity is its owning slot");
      check(first[slot][k].group == Group::Paint, "op group is Paint");
    }
  }

  // 第二轮：打开 paint-loop，确认重刷仍只发生在各自块内。
  config.paint_loop_ms = 1;
  for (int i = 0; i < 200 && status.passes.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(status.passes.load() >= 1, "paint-loop restarted a pass");

  for (int iter = 0; iter < 500; ++iter) {
    scheduler.step(now);
    now += 50000;
    ack_all(scheduler, owned, acked);
    bool repainted = false;
    for (std::size_t slot = 0; slot < owned.size(); ++slot)
      repainted = repainted || owned[slot]->submitted().size() > first[slot].size();
    if (repainted)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  scheduler.stop();

  for (std::size_t slot = 0; slot < owned.size(); ++slot) {
    const auto expected = expected_block(slot);
    const auto& all = owned[slot]->submitted();
    check(all.size() > first[slot].size(),
          "paint-loop produced a repaint op");
    for (std::size_t k = first[slot].size(); k < all.size(); ++k) {
      const bool in_block =
          std::find(expected.begin(), expected.end(), all[k].idx) != expected.end();
      check(in_block, "repaint stays inside the owning block");
      check(all[k].affinity &&
                *all[k].affinity == static_cast<SlotId>(slot),
            "repaint affinity is still the owning slot");
    }
  }

  scheduler.stop();

  if (failures == 0) {
    std::cout << "new_sch1122_paint_test OK (4 slots, 2x2 rectangles, snake, "
                 "block-local repaint)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
