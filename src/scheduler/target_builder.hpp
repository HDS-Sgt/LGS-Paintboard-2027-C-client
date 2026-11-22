#pragma once

#include "paintboard/config.hpp"
#include "paintboard/scheduler.hpp"

namespace paintboard {

// 目标图构建：image/draws + offset -> pix/known/known_idx/bbox/sp_pts。
// 不依赖 Board、队列或线程，纯输入输出，便于直接单测。
class TargetBuilder {
 public:
  Target build(const Config& config, const Rules& rules) const;
};

} // namespace paintboard
