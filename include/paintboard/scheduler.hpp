#pragma once

#include "paintboard/board.hpp"
#include "paintboard/config.hpp"
#include "paintboard/paint_queue.hpp"
#include "paintboard/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace paintboard {

class IPainter;

struct Target {
  bool built = false;
  std::uint32_t W = 0, H = 0;
  std::vector<std::uint32_t> pix;
  std::vector<std::uint8_t> known;
  std::vector<std::uint32_t> known_idx;
  std::uint32_t bb_x0 = 0, bb_y0 = 0, bb_x1 = 0, bb_y1 = 0;
  std::vector<std::uint32_t> sp_pts;
};

class Plan {
 public:
  // need 是目标像素总数；默认 0 表示由调用方后续用 target 判断完成度。
  void alloc(std::size_t n, std::size_t need = 0);

  bool claim(std::uint32_t idx);
  // repair 专用：允许认领已经 done 的像素；paint 的 claim 仍拒绝 done。
  bool claim_repair(std::uint32_t idx);
  bool claim_sonar(std::uint32_t idx);
  bool claimed(std::uint32_t idx) const;
  bool claimed_sonar(std::uint32_t idx) const;
  bool done(std::uint32_t idx) const;
  void mark_done(std::uint32_t idx);
  void release(std::uint32_t idx);
  void release_sonar(std::uint32_t idx);
  void reset();

  std::size_t need_total() const;
  std::size_t done_total() const;

 private:
  mutable std::mutex mu_;
  std::vector<std::uint8_t> enq_;
  std::vector<std::uint8_t> enq_sonar_;
  std::vector<std::uint8_t> done_;
  std::size_t need_ = 0;
  std::size_t done_total_ = 0;
};

Target build_target(const Config& config, const Rules& rules,
                    const Board& board);
bool target_complete(const Target& target, const Plan& plan);

struct SchedulerDeps {
  PaintQueue& queue;
  std::vector<IPainter*>& painters;
  Board& board;
  Config& config;
  RuntimeStatus& status;
  Logger& log;
};

struct SchedulerStats {
  std::size_t queue_total = 0;
  std::uint64_t painted = 0;
  std::uint64_t passes = 0;
};

class IScheduler {
 public:
  virtual ~IScheduler() = default;

  virtual void prepare() = 0;
  virtual void on_all_bound() = 0;
  virtual void step(Tick now) = 0;
  virtual void on_tick(const TickEvent& event) = 0;
  virtual void on_disconnect(std::vector<PaintOp> pending) = 0;
  virtual bool done() const = 0;
  virtual SchedulerStats stats() const = 0;
  virtual void stop() = 0;
};

} // namespace paintboard
