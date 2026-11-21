#include "paintboard/paint_queue.hpp"

namespace paintboard {
namespace {

std::size_t group_index(Group group) {
  const auto value = static_cast<std::size_t>(group);
  return value < 3 ? value : 0;
}

} // namespace

bool PaintQueue::OpCmp::operator()(const PaintOp& a, const PaintOp& b) const {
  if (a.due != b.due) return a.due < b.due;
  return a.seq < b.seq;
}

void PaintQueue::count_add(Group group) {
  ++total_;
  ++per_group_[group_index(group)];
}

void PaintQueue::count_remove(Group group) {
  if (total_ > 0) --total_;
  auto& count = per_group_[group_index(group)];
  if (count > 0) --count;
}

std::uint64_t PaintQueue::push(PaintOp op) {
  std::lock_guard<std::mutex> lock(mu_);
  op.seq = next_seq_++;
  ops_.insert(op);
  count_add(op.group);
  return op.seq;
}

void PaintQueue::take_due(Tick now, std::vector<PaintOp>& out) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = ops_.begin();
  while (it != ops_.end() && it->due <= now) {
    out.push_back(*it);
    it = ops_.erase(it);
  }
}

void PaintQueue::requeue(PaintOp op) {
  std::lock_guard<std::mutex> lock(mu_);
  ops_.insert(op);
}

void PaintQueue::consume(const PaintOp& op) {
  std::lock_guard<std::mutex> lock(mu_);
  count_remove(op.group);
}

std::vector<PaintOp> PaintQueue::remove_if(
    const std::function<bool(const PaintOp&)>& predicate) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PaintOp> removed;
  for (auto it = ops_.begin(); it != ops_.end();) {
    if (!predicate(*it)) {
      ++it;
      continue;
    }
    removed.push_back(*it);
    count_remove(it->group);
    it = ops_.erase(it);
  }
  return removed;
}

void PaintQueue::recount() {
  std::lock_guard<std::mutex> lock(mu_);
  total_ = ops_.size();
  per_group_.fill(0);
  for (const auto& op : ops_) {
    ++per_group_[group_index(op.group)];
  }
}

std::size_t PaintQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return total_;
}

std::size_t PaintQueue::size(Group group) const {
  std::lock_guard<std::mutex> lock(mu_);
  return per_group_[group_index(group)];
}

PaintQueue::Stats PaintQueue::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  Stats out;
  out.total = total_;
  out.per_group = per_group_;
  return out;
}

} // namespace paintboard
