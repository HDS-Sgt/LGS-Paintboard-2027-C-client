#pragma once

#include "paintboard/scheduler.hpp"

#include "scheduler/classic_result_handler.hpp"
#include "scheduler/sonar_tiling.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace paintboard {

// paint 块 → 槽位的归属：块号按行优先排序，每个槽拿连续的一段块。
// blocks <= slots 时就是一块一个槽（前面的槽先用满），blocks > slots 时
// 槽 s 拿 [s*blocks/slots, (s+1)*blocks/slots) 这一段，绝不交错。
std::size_t paint_slot_for_block(std::size_t block, std::size_t blocks,
                                 std::size_t slots);

// 调度优化用的可编辑副本（2026-11-22 起）。当前与 ClassicScheduler 行为
// 逐行一致；实验改动只放这里，ClassicScheduler 保持线上基线。
class NewSch1122 final : public IScheduler {
 public:
  explicit NewSch1122(SchedulerDeps deps);
  ~NewSch1122() override;

  NewSch1122(const NewSch1122&) = delete;
  NewSch1122& operator=(const NewSch1122&) = delete;

  void prepare() override;
  void on_all_bound() override;
  void step(Tick now) override;
  void on_tick(const TickEvent& event) override;
  void on_disconnect(std::vector<PaintOp> pending) override;
  bool done() const override;
  SchedulerStats stats() const override;
  void stop() override;

 private:
  void paint_loop();
  void repair_loop();
  void sonar_loop();

  void drop_nonpaint();
  void drop_paint_queue();
  void drop_sonar_queue();
  bool is_sonar_post(std::uint32_t idx) const;
  bool is_sonar_center(std::uint32_t idx) const;
  std::size_t painter_count(Group group) const;

  SchedulerDeps deps_;
  Target target_;
  Plan plan_;
  ClassicResultHandler results_;
  Tick c0_ = 0;
  Tick k_ = 0;
  std::uint32_t width_ = 0, height_ = 0;

  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
  std::vector<std::thread> threads_;

  // paint 矩形分块：每块由一个 paint slot 负责，块内按行蛇形。
  // paint_blocks_[t] 是 slot paint_block_ids_[t] 负责的像素（蛇形序）。
  std::vector<SlotId> paint_block_ids_;
  std::vector<std::vector<std::uint32_t>> paint_blocks_;
  std::vector<std::size_t> paint_cursor_;
  std::uint32_t paint_sig_w_ = 0, paint_sig_h_ = 0;
  std::size_t paint_sig_known_ = 0;
  std::uint64_t paint_round_ = 0;
  bool paint_verbose_ = false;

  std::size_t repair_cursor_ = 0;
  // 绑定 repair 每轮各发 1 个中心；分块时用它轮转块内中心。
  std::uint64_t repair_round_ = 0;

  // 声纳密铺快照：由 sonar_loop 重建，step/is_sonar_post 只读。
  // 分块归属在 SonarTiling::slot_centers 里，每个中心只属于一个 slot。
  std::uint32_t sonar_sig_w_ = 0, sonar_sig_h_ = 0, sonar_sig_r_ = 0;
  std::size_t sonar_sig_slots_ = 0, sonar_sig_known_ = 0;
  std::shared_ptr<const SonarTiling> sonar_tiles_;
  mutable std::mutex sonar_mu_;
  std::vector<std::size_t> sonar_cursor_;
  std::uint64_t dispatched_ = 0;
};

} // namespace paintboard
