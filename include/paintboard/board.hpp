#pragma once

#include "paintboard/types.hpp"

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace paintboard {

struct BoardView {
  Rules rules;
  Tick last_tick = 0;
  bool has_public_canvas = false;
};

struct SonarSnapshot {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint32_t> color;
  std::vector<Tick> tick;
};

class Board {
 public:
  void apply_rules(const Rules& rules);
  bool apply_snapshot(std::span<const std::uint8_t> payload);
  bool apply_public(std::span<const std::uint8_t> payload);
  void apply_sonar(const SonarBatch& batch);

  Tick since() const;
  BoardView view() const;
  SonarSnapshot sonar_snapshot() const;

  bool copy_canvas_rgb(std::vector<std::uint8_t>& out) const;
  bool copy_sonar_rgb(std::vector<std::uint8_t>& out) const;

  // 调度器只读一份画布颜色副本；不在 Board 上暴露可变引用。
  std::vector<std::uint32_t> canvas_snapshot() const;

 private:
  mutable std::mutex mu_;
  Rules rules_;
  Tick since_ = 0;
  bool has_public_canvas_ = false;
  std::vector<std::uint32_t> cols_;
  std::vector<std::uint32_t> l1_;
  std::vector<std::uint32_t> l2_;
  std::vector<std::uint32_t> sonar_col_;
  std::vector<Tick> sonar_tick_;
};

} // namespace paintboard
