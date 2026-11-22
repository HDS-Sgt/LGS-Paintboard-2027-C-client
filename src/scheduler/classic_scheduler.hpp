#pragma once

#include "paintboard/scheduler.hpp"

#include "scheduler/classic_result_handler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace paintboard {

class ClassicScheduler final : public IScheduler {
 public:
  explicit ClassicScheduler(SchedulerDeps deps);
  ~ClassicScheduler() override;

  ClassicScheduler(const ClassicScheduler&) = delete;
  ClassicScheduler& operator=(const ClassicScheduler&) = delete;

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
  bool is_sonar_post(std::uint32_t idx) const;
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

  std::size_t paint_cursor_ = 0;
  std::size_t repair_cursor_ = 0;
  std::uint32_t sonar_sig_bw_ = 0, sonar_sig_bh_ = 0, sonar_sig_sk_ = 0;
  std::vector<std::size_t> sonar_cursor_;
  std::uint64_t dispatched_ = 0;
};

} // namespace paintboard
