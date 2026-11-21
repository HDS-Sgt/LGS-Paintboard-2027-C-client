#pragma once

#include "paintboard/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

namespace paintboard {

class PaintQueue {
 public:
  struct Stats {
    std::size_t total = 0;
    std::array<std::size_t, 3> per_group{};
  };

  std::uint64_t push(PaintOp op);
  void take_due(Tick now, std::vector<PaintOp>& out);
  void requeue(PaintOp op);
  void consume(const PaintOp& op);
  std::vector<PaintOp> remove_if(
      const std::function<bool(const PaintOp&)>& predicate);
  void recount();

  std::size_t size() const;
  std::size_t size(Group group) const;
  Stats stats() const;

 private:
  struct OpCmp {
    bool operator()(const PaintOp& a, const PaintOp& b) const;
  };

  void count_add(Group group);
  void count_remove(Group group);

  mutable std::mutex mu_;
  std::multiset<PaintOp, OpCmp> ops_;
  std::uint64_t next_seq_ = 1;
  std::size_t total_ = 0;
  std::array<std::size_t, 3> per_group_{};
};

} // namespace paintboard
