#include "scheduler/new_sch1122.hpp"

#include "paintboard/painter.hpp"
#include "scheduler/target_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace paintboard {
namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
// 留作自由 repair 的槽位数（用户要求 10~20，取中间值）。
constexpr std::size_t kFreeRepairSlots = 16;

void sleep_ms(std::uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::uint32_t manhattan(std::uint32_t a, std::uint32_t b, std::uint32_t width) {
  if (width == 0)
    return 0;
  const std::uint32_t ax = a % width, ay = a / width;
  const std::uint32_t bx = b % width, by = b / width;
  return (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
}

std::uint32_t color_diff(std::uint32_t a, std::uint32_t b) {
  const std::uint32_t ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
  const std::uint32_t br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
  const std::uint32_t dr = ar > br ? ar - br : br - ar;
  const std::uint32_t dg = ag > bg ? ag - bg : bg - ag;
  const std::uint32_t db = ab > bb ? ab - bb : bb - ab;
  return dr + dg + db;
}

// paint 的矩形分块：把目标 bbox 划成 kx*ky 个近似方形的矩形，每个矩形
// 交给一个 paint slot；块内按行蛇形（奇数行反向）扫描，相邻两笔只差一个
// 像素，避免菱形密铺那种跨块跳动。只收录 target.known 的像素。
std::pair<std::size_t, std::size_t> choose_paint_grid(std::int64_t bw,
                                                       std::int64_t bh,
                                                       std::size_t slots) {
  std::size_t best_blocks = 0, best_kx = 1, best_ky = 1;
  double best_aspect = std::numeric_limits<double>::max();
  const std::int64_t kx_cap =
      std::min<std::int64_t>(static_cast<std::int64_t>(slots), bw);
  for (std::int64_t kx = 1; kx <= kx_cap; ++kx) {
    std::int64_t ky = static_cast<std::int64_t>(slots) / kx;
    if (ky > bh)
      ky = bh;
    if (ky <= 0)
      continue;
    const std::size_t blocks = static_cast<std::size_t>(kx * ky);
    const double aspect =
        std::fabs(std::log(static_cast<double>(kx) * bh /
                           (static_cast<double>(ky) * bw)));
    if (blocks > best_blocks ||
        (blocks == best_blocks && aspect < best_aspect)) {
      best_blocks = blocks;
      best_kx = static_cast<std::size_t>(kx);
      best_ky = static_cast<std::size_t>(ky);
      best_aspect = aspect;
    }
  }
  return {best_kx, best_ky};
}

std::vector<std::vector<std::uint32_t>>
build_paint_blocks(const Target& target, std::size_t slots) {
  std::vector<std::vector<std::uint32_t>> out(slots);
  if (!target.built || slots == 0 || target.known_idx.empty())
    return out;

  const std::int64_t bw =
      static_cast<std::int64_t>(target.bb_x1) - target.bb_x0 + 1;
  const std::int64_t bh =
      static_cast<std::int64_t>(target.bb_y1) - target.bb_y0 + 1;
  const auto [kx, ky] = choose_paint_grid(bw, bh, slots);
  const std::size_t block_count = kx * ky;
  if (block_count == 0)
    return out;

  for (std::size_t by = 0; by < ky; ++by) {
    const std::int64_t y0 =
        target.bb_y0 + bh * static_cast<std::int64_t>(by) /
                           static_cast<std::int64_t>(ky);
    const std::int64_t y1 =
        target.bb_y0 + bh * static_cast<std::int64_t>(by + 1) /
                           static_cast<std::int64_t>(ky) -
        1;
    for (std::size_t bx = 0; bx < kx; ++bx) {
      const std::int64_t x0 =
          target.bb_x0 + bw * static_cast<std::int64_t>(bx) /
                             static_cast<std::int64_t>(kx);
      const std::int64_t x1 =
          target.bb_x0 + bw * static_cast<std::int64_t>(bx + 1) /
                             static_cast<std::int64_t>(kx) -
          1;
      const std::size_t block = by * kx + bx;
      std::vector<std::uint32_t>& pixels =
          out[paint_slot_for_block(block, block_count, slots)];
      for (std::int64_t y = y0; y <= y1; ++y) {
        const bool reverse = ((y - y0) & 1) != 0;
        for (std::int64_t i = 0; i <= x1 - x0; ++i) {
          const std::int64_t x = reverse ? x1 - i : x0 + i;
          const std::size_t p = static_cast<std::size_t>(y) * target.W +
                                static_cast<std::size_t>(x);
          if (p < target.known.size() && target.known[p])
            pixels.push_back(static_cast<std::uint32_t>(p));
        }
      }
    }
  }
  return out;
}

} // namespace

std::size_t paint_slot_for_block(std::size_t block, std::size_t blocks,
                                 std::size_t slots) {
  if (slots == 0)
    return 0;
  if (blocks <= slots)
    return block;
  return block * slots / blocks;
}

// Plan::* 与 target_complete 定义在 classic_scheduler.cpp（共享状态机，
// 本文件只放 NewSch1122 自己的策略实现），不要在此重复定义，否则链接冲突。

NewSch1122::NewSch1122(SchedulerDeps deps)
    : deps_(deps), results_(target_, plan_, deps.queue, deps.status) {
  paint_verbose_ = std::getenv("IKAPB_PAINT_LOG") != nullptr;
}

NewSch1122::~NewSch1122() { stop(); }

void NewSch1122::prepare() {
  const BoardView view = deps_.board.view();
  target_ = TargetBuilder{}.build(deps_.config, view.rules);
  width_ = target_.W;
  height_ = target_.H;
  c0_ = view.rules.cooldown_base;
  k_ = view.rules.cooldown_per_px;
  plan_.alloc(static_cast<std::size_t>(width_) * height_,
              target_.known_idx.size());
  deps_.log.logf("[paint] target known={} bbox={},{}..{},{} sp={}\n",
                 target_.known_idx.size(), target_.bb_x0, target_.bb_y0,
                 target_.bb_x1, target_.bb_y1, target_.sp_pts.size());
}

void NewSch1122::on_all_bound() {
  if (started_.exchange(true))
    return;

  if (painter_count(Group::Paint) > 0)
    threads_.emplace_back(&NewSch1122::paint_loop, this);
  if (painter_count(Group::Repair) > 0)
    threads_.emplace_back(&NewSch1122::repair_loop, this);
  if (painter_count(Group::Sonar) > 0)
    threads_.emplace_back(&NewSch1122::sonar_loop, this);
}

void NewSch1122::step(Tick now) {
  if (!target_.built)
    return;

  const Tick qnow = now ? now : 1;
  std::vector<PaintOp> due;
  deps_.queue.take_due(qnow, due);
  if (due.empty())
    return;

  std::vector<PaintOp> live;
  live.reserve(due.size());
  for (const PaintOp& op : due) {
    bool valid = false;
    if (op.group == Group::Sonar) {
      if (op.affinity && plan_.claimed_sonar(op.idx) &&
          is_sonar_center(op.idx)) {
        for (const IPainter* painter : deps_.painters) {
          if (painter && painter->id() == *op.affinity &&
              painter->view().bound) {
            valid = true;
            break;
          }
        }
      }
      if (!valid)
        plan_.release_sonar(op.idx);
    } else if (op.group == Group::Paint) {
      // paint 也是块归属：只允许 affinity 指向的那个 paint slot 接。
      if (plan_.claimed(op.idx) && !plan_.done(op.idx)) {
        if (!op.affinity) {
          valid = true;
        } else {
          for (const IPainter* painter : deps_.painters) {
            if (painter && painter->id() == *op.affinity &&
                painter->group() == Group::Paint &&
                painter->view().bound) {
              valid = true;
              break;
            }
          }
        }
      }
      if (!valid)
        plan_.release(op.idx);
    } else {
      // Repair：绑定槽的操作只允许 affinity 指向的那个槽位接；没有
      // affinity 的旧操作/自由扫描操作允许任意 repair 槽位接。
      if (plan_.claimed(op.idx)) {
        if (!op.affinity) {
          valid = true;
        } else {
          for (const IPainter* painter : deps_.painters) {
            if (painter && painter->id() == *op.affinity &&
                painter->group() == Group::Repair &&
                painter->view().bound) {
              valid = true;
              break;
            }
          }
        }
      }
      if (!valid)
        plan_.release(op.idx);
    }

    if (valid)
      live.push_back(op);
    else
      deps_.queue.consume(op);
  }
  if (live.empty())
    return;

  std::vector<std::uint8_t> used(live.size(), 0);

  for (IPainter* painter : deps_.painters) {
    if (!painter)
      continue;
    const SlotView view = painter->view();
    if (!view.bound || view.want_bind || view.inflight >= 2)
      continue;

    std::size_t best = kNoSlot;
    std::uint32_t best_distance = 0;
    if (view.group == Group::Sonar || view.group == Group::Paint) {
      // sonar/paint 的 op 都带块归属：按队列顺序取自己块的头一条，
      // 队列本身按块内蛇形（paint）或蛇形中心（sonar）生成，保持移动连续。
      for (std::size_t j = 0; j < live.size(); ++j) {
        if (used[j] || live[j].group != view.group)
          continue;
        if (!live[j].affinity || *live[j].affinity != view.id)
          continue;
        if (!painter->ready_for(live[j].idx, now, c0_, k_, width_))
          break;
        best = j;
        break;
      }
    } else {
      for (std::size_t j = 0; j < live.size(); ++j) {
        if (used[j] || live[j].group != view.group)
          continue;
        if (live[j].affinity && *live[j].affinity != view.id)
          continue;
        if (!painter->ready_for(live[j].idx, now, c0_, k_, width_))
          continue;
        const std::uint32_t distance =
            view.has_projected
                ? manhattan(live[j].idx, view.projected_idx, width_)
                : 0;
        if (best == kNoSlot ||
            live[j].priority < live[best].priority ||
            (live[j].priority == live[best].priority &&
             distance < best_distance)) {
          best = j;
          best_distance = distance;
        }
      }
    }

    if (best == kNoSlot)
      continue;

    const PaintOp op = live[best];
    if (painter->submit(op, now)) {
      used[best] = 1;
      deps_.queue.consume(op);
      ++dispatched_;
    }
  }

  for (std::size_t j = 0; j < live.size(); ++j) {
    if (used[j])
      continue;
    PaintOp retry = live[j];
    // Paint/Sonar 按队列顺序消费（块内蛇形）；不能用未来 due 重排，
    // 否则新 push 的 due=0 操作会插到已重排操作前面，蛇形顺序被打乱。
    retry.due = (retry.group == Group::Paint || retry.group == Group::Sonar)
                    ? 0
                    : qnow + 20000;
    deps_.queue.requeue(retry);
  }
}

void NewSch1122::on_tick(const TickEvent& event) {
  results_.on_tick(event);
}

void NewSch1122::on_disconnect(std::vector<PaintOp> pending) {
  results_.on_disconnect(pending);
}

bool NewSch1122::done() const {
  // 与旧 should_stop 对齐：常驻/paint-loop 不自己退出；带 repair/sonar 组的
  // 会话在目标画完后继续值守，只有纯 paint 组才允许按 target_complete 收尾。
  if (deps_.config.daemon || deps_.config.paint_loop_ms > 0)
    return false;
  if (painter_count(Group::Repair) > 0 || painter_count(Group::Sonar) > 0)
    return false;
  return target_complete(target_, plan_);
}

SchedulerStats NewSch1122::stats() const {
  const PaintQueue::Stats queue = deps_.queue.stats();
  SchedulerStats out;
  out.queue_total = queue.total;
  out.painted = deps_.status.painted.load();
  out.passes = deps_.status.passes.load();
  return out;
}

void NewSch1122::stop() {
  if (stop_.exchange(true))
    return;
  for (std::thread& thread : threads_)
    if (thread.joinable())
      thread.join();
  threads_.clear();
}

void NewSch1122::paint_loop() {
  while (!stop_.load() && !deps_.status.stop.load()) {
    if (!target_.built) {
      sleep_ms(50);
      continue;
    }
    if (deps_.config.limit &&
        deps_.status.painted.load() >= deps_.config.limit)
      break;

    if (target_complete(target_, plan_)) {
      if (deps_.config.paint_loop_ms > 0) {
        for (std::uint32_t waited = 0;
             waited < deps_.config.paint_loop_ms && !stop_.load();
             waited += 100)
          sleep_ms(100);
        if (stop_.load())
          return;
        drop_nonpaint();
        plan_.reset();
        std::fill(paint_cursor_.begin(), paint_cursor_.end(), 0);
        ++deps_.status.passes;
        deps_.log.logf("[paint] pass {} complete -> restart",
                       deps_.status.passes.load());
        continue;
      }
      sleep_ms(deps_.config.paint_ms);
      continue;
    }

    std::vector<SlotId> paint_ids;
    for (IPainter* painter : deps_.painters) {
      if (!painter)
        continue;
      const SlotView view = painter->view();
      if (view.group == Group::Paint && view.bound)
        paint_ids.push_back(view.id);
    }
    if (paint_ids.empty()) {
      sleep_ms(deps_.config.paint_ms);
      continue;
    }

    if (paint_block_ids_ != paint_ids || paint_sig_w_ != target_.W ||
        paint_sig_h_ != target_.H ||
        paint_sig_known_ != target_.known_idx.size()) {
      drop_paint_queue();
      paint_blocks_ = build_paint_blocks(target_, paint_ids.size());
      paint_block_ids_ = paint_ids;
      paint_cursor_.assign(paint_ids.size(), 0);
      paint_sig_w_ = target_.W;
      paint_sig_h_ = target_.H;
      paint_sig_known_ = target_.known_idx.size();
      const std::int64_t bw = static_cast<std::int64_t>(target_.bb_x1) -
                             target_.bb_x0 + 1;
      const std::int64_t bh = static_cast<std::int64_t>(target_.bb_y1) -
                             target_.bb_y0 + 1;
      const auto [kx, ky] = choose_paint_grid(bw, bh, paint_ids.size());
      const std::size_t block_count = kx * ky;
      deps_.log.logf(
          "[paint] grid kx={} ky={} blocks={} slots={} known={} "
          "bbox={},{}..{},{} round_cap={} queue_cap={} paint_ms={}",
          kx, ky, block_count, paint_ids.size(), target_.known_idx.size(),
          target_.bb_x0, target_.bb_y0, target_.bb_x1, target_.bb_y1,
          deps_.config.round_cap, deps_.config.queue_cap,
          deps_.config.paint_ms);
      if (deps_.config.round_cap && block_count > deps_.config.round_cap) {
        deps_.log.logf(
            "[paint] note blocks={} > round_cap={}: per-wave paint budget "
            "is raised to blocks (one wave can serve every block once)",
            block_count, deps_.config.round_cap);
      }
      if (paint_verbose_) {
        for (std::size_t b = 0; b < block_count; ++b) {
          const std::size_t bx = b % kx, by = b / kx;
          const std::int64_t x0 =
              target_.bb_x0 + bw * static_cast<std::int64_t>(bx) /
                                    static_cast<std::int64_t>(kx);
          const std::int64_t x1 =
              target_.bb_x0 + bw * static_cast<std::int64_t>(bx + 1) /
                                    static_cast<std::int64_t>(kx) - 1;
          const std::int64_t y0 =
              target_.bb_y0 + bh * static_cast<std::int64_t>(by) /
                                    static_cast<std::int64_t>(ky);
          const std::int64_t y1 =
              target_.bb_y0 + bh * static_cast<std::int64_t>(by + 1) /
                                    static_cast<std::int64_t>(ky) - 1;
          const std::size_t owner =
              paint_slot_for_block(b, block_count, paint_ids.size());
          deps_.log.logf(
              "[paint] block={} owner_t={} slot={} rect=({},{}..{},{})",
              b, owner, paint_block_ids_[owner], x0, y0, x1, y1);
        }
        for (std::size_t t = 0; t < paint_blocks_.size(); ++t) {
          const auto& pix = paint_blocks_[t];
          if (pix.empty()) {
            deps_.log.logf("[paint] slot={} pixels=0", paint_block_ids_[t]);
          } else {
            deps_.log.logf(
                "[paint] slot={} pixels={} first=({},{}) last=({},{})",
                paint_block_ids_[t], pix.size(), pix.front() % target_.W,
                pix.front() / target_.W, pix.back() % target_.W,
                pix.back() / target_.W);
          }
        }
      }
    }

    // One wave should serve every paint block once.  With
    // --round-cap 256 and 4096 slots the old cap forced 16 waves and
    // starved the lower rows.  4096 paints still fit in one BATCH
    // (4096 * 8 B of paint columns is ~32 KiB, far below 1 MiB).
    const std::size_t paint_slots = paint_blocks_.size();
    const std::size_t paint_budget =
        std::max<std::size_t>(deps_.config.round_cap, paint_slots);
    const std::size_t paint_queue_limit = std::max<std::size_t>(
        deps_.config.queue_cap, paint_slots + 256);
    std::size_t made = 0;
    while (made < paint_budget &&
           deps_.queue.size() + 256 < paint_queue_limit) {
      bool progressed = false;
      for (std::size_t t = 0;
           t < paint_blocks_.size() && made < paint_budget &&
           deps_.queue.size() + 256 < paint_queue_limit;
           ++t) {
        const std::vector<std::uint32_t>& block = paint_blocks_[t];
        if (block.empty())
          continue;
        std::size_t& cursor = paint_cursor_[t];
        for (std::size_t k = 0; k < block.size(); ++k) {
          const std::size_t pos = (cursor + k) % block.size();
          const std::uint32_t p = block[pos];
          if (!target_.known[p] || plan_.done(p))
            continue;
          if (!plan_.claim(p))
            continue;
          PaintOp op;
          op.idx = p;
          op.color = target_.pix[p];
          op.group = Group::Paint;
          op.affinity = paint_block_ids_[t];
          deps_.queue.push(op);
          if (paint_verbose_) {
            deps_.log.logf(
                "[paint] enqueue round={} t={} slot={} idx={} x={} y={}",
                paint_round_, t, paint_block_ids_[t], p, p % target_.W,
                p / target_.W);
          }
          cursor = (pos + 1) % block.size();
          ++made;
          progressed = true;
          break;
        }
      }
      if (!progressed)
        break;
    }
    if (paint_verbose_) {
      deps_.log.logf("[paint] round={} made={} queue={} cursors={}",
                     paint_round_, made, deps_.queue.size(),
                     paint_cursor_.size());
    }
    ++paint_round_;
    sleep_ms(deps_.config.paint_ms);
  }
}

void NewSch1122::repair_loop() {
  while (!stop_.load() && !deps_.status.stop.load()) {
    if (!target_.built) {
      sleep_ms(50);
      continue;
    }
    if (deps_.config.limit &&
        deps_.status.painted.load() >= deps_.config.limit)
      break;

    // 收集 repair 槽位：末尾 10~20 个作为自由槽走原策略，其余按
    // 声纳中心匀配。
    std::vector<SlotId> repair_ids;
    for (IPainter* painter : deps_.painters) {
      if (!painter)
        continue;
      const SlotView view = painter->view();
      if (view.group == Group::Repair && view.bound)
        repair_ids.push_back(view.id);
    }
    if (repair_ids.empty()) {
      sleep_ms(deps_.config.repair_ms);
      continue;
    }
    const std::size_t free_take =
        std::min(repair_ids.size(), kFreeRepairSlots);
    const std::vector<SlotId> free_ids(
        repair_ids.end() - static_cast<std::ptrdiff_t>(free_take),
        repair_ids.end());
    const std::vector<SlotId> bound_ids(
        repair_ids.begin(),
        repair_ids.end() - static_cast<std::ptrdiff_t>(free_take));

    const BoardView board = deps_.board.view();
    const SonarSnapshot sonar = deps_.board.sonar_snapshot();
    const std::size_t n =
        static_cast<std::size_t>(target_.W) * target_.H;
    const bool sonar_ok = sonar.color.size() == n && sonar.tick.size() == n;

    std::size_t limit = painter_count(Group::Repair);
    if (limit < 16)
      limit = 16;
    if (limit < repair_ids.size() * 2)
      limit = repair_ids.size() * 2;
    std::size_t made = 0;
    std::size_t free_op_cursor = 0;

    // 自由槽：保持原来的全局 damaged/stale 扫描。先跑它并预留足够位置，
    // 避免主动轮询的绑定槽把自由槽饿死。
    const std::size_t free_limit =
        bound_ids.empty() ? limit : std::max(free_take, limit / 2);
    const std::size_t known = target_.known_idx.size();
    if (sonar_ok && known != 0) {
      const Tick last_tick = deps_.status.last_tick.load();
      const Tick verify_span =
          deps_.config.repair_verify_ms
              ? static_cast<Tick>(board.rules.tps) *
                    deps_.config.repair_verify_ms / 1000
              : 0;

      std::vector<std::uint32_t> damaged, stale;
      std::size_t win = deps_.config.round_cap;
      if (win > known)
        win = known;
      std::size_t scanned = 0;
      for (std::size_t i = 0; i < win; ++i) {
        const std::size_t p = target_.known_idx[(repair_cursor_ + i) % known];
        scanned = i + 1;
        if (!target_.known[p])
          continue;
        if (!plan_.done(static_cast<std::uint32_t>(p)))
          continue;
        if (!sonar.tick[p]) {
          if (verify_span)
            stale.push_back(static_cast<std::uint32_t>(p));
        } else if (color_diff(sonar.color[p], target_.pix[p]) >
                   deps_.config.repair_thresh) {
          damaged.push_back(static_cast<std::uint32_t>(p));
        } else if (verify_span && last_tick > 0 &&
                   sonar.tick[p] + verify_span < last_tick) {
          stale.push_back(static_cast<std::uint32_t>(p));
        }
      }

      for (int pass = 0; pass < 2; ++pass) {
        const std::vector<std::uint32_t>& source = pass == 0 ? damaged : stale;
        for (std::uint32_t idx : source) {
          if (made >= deps_.config.round_cap || made >= free_limit ||
              deps_.queue.size(Group::Repair) >= limit)
            break;
          if (!plan_.claim_repair(idx))
            continue;
          PaintOp op;
          op.idx = idx;
          op.color = target_.pix[idx];
          op.group = Group::Repair;
          op.priority = static_cast<std::uint32_t>(pass);
          op.affinity = free_ids[free_op_cursor++ % free_ids.size()];
          deps_.queue.push(op);
          ++made;
        }
      }
      if (scanned)
        repair_cursor_ = (repair_cursor_ + scanned) % known;
    }

    // 绑定槽：主动轮询自己负责的声纳中心。
    // B>=C 时每个槽位只拿到 1 个中心（多个槽共享同一点）；
    // B<C 时每个槽位拿一段连续块，按 repair_round_ 在块内轮转。
    if (!bound_ids.empty()) {
      std::shared_ptr<const SonarTiling> tiles;
      {
        std::lock_guard<std::mutex> lk(sonar_mu_);
        tiles = sonar_tiles_;
      }
      if (tiles && !tiles->all_centers.empty()) {
        const std::vector<std::vector<std::uint32_t>> lists =
            partition_sonar_centers(tiles->all_centers, bound_ids.size());
        ++repair_round_;
        for (std::size_t s = 0; s < lists.size(); ++s) {
          if (made >= deps_.config.round_cap ||
              deps_.queue.size(Group::Repair) >= limit)
            break;
          if (lists[s].empty())
            continue;
          const std::uint32_t idx =
              lists[s][repair_round_ % lists[s].size()];
          if (idx >= target_.known.size() || !target_.known[idx])
            continue;
          if (!plan_.done(idx))
            continue;
          if (!plan_.claim_repair(idx))
            continue;
          PaintOp op;
          op.idx = idx;
          op.color = target_.pix[idx];
          op.group = Group::Repair;
          op.priority = 0;
          op.affinity = bound_ids[s];
          deps_.queue.push(op);
          ++made;
        }
      }
    }

    sleep_ms(deps_.config.repair_ms);
  }
}

void NewSch1122::sonar_loop() {
  while (!stop_.load() && !deps_.status.stop.load()) {
    if (!target_.built) {
      sleep_ms(50);
      continue;
    }
    if (deps_.config.limit &&
        deps_.status.painted.load() >= deps_.config.limit)
      break;

    std::vector<SlotId> slots;
    for (IPainter* painter : deps_.painters) {
      if (!painter)
        continue;
      const SlotView view = painter->view();
      if (view.group == Group::Sonar && view.bound)
        slots.push_back(view.id);
    }
    const std::size_t sk = slots.size();
    if (!sk) {
      sleep_ms(deps_.config.sonar_ms);
      continue;
    }

    const BoardView board = deps_.board.view();
    const std::uint32_t radius = board.rules.radius;

    bool rebuild = false;
    {
      std::lock_guard<std::mutex> lk(sonar_mu_);
      rebuild = !sonar_tiles_ || sonar_sig_w_ != target_.W ||
                sonar_sig_h_ != target_.H || sonar_sig_r_ != radius ||
                sonar_sig_slots_ != sk ||
                sonar_sig_known_ != target_.known_idx.size();
    }
    if (rebuild) {
      // 旧密铺的排队操作可能指向不再属于任何分块的中心，先撤掉并释放认领；
      // step() 里还有 is_sonar_center 守卫，能拦住已经取到 live 的旧 op。
      drop_sonar_queue();

      auto fresh =
          std::make_shared<SonarTiling>(build_sonar_tiling(target_, radius, sk));
      std::lock_guard<std::mutex> lk(sonar_mu_);
      sonar_tiles_ = fresh;
      sonar_cursor_.assign(sk, 0);
      sonar_sig_w_ = target_.W;
      sonar_sig_h_ = target_.H;
      sonar_sig_r_ = radius;
      sonar_sig_slots_ = sk;
      sonar_sig_known_ = target_.known_idx.size();
      deps_.log.logf(
          "[sonar] dense-tiling R={} slots={} known={} centers={} tiling={} "
          "extra={} covered={}\n",
          radius, sk, fresh->known_total, fresh->slot_centers.size(),
          fresh->tiling_centers, fresh->extra_centers, fresh->covered_total);
    }

    std::shared_ptr<const SonarTiling> tiles;
    {
      std::lock_guard<std::mutex> lk(sonar_mu_);
      tiles = sonar_tiles_;
    }
    if (!tiles || tiles->slot_centers.empty()) {
      sleep_ms(deps_.config.sonar_ms);
      continue;
    }

    // 槽位多时每轮每槽只补 1 发，避免 round_cap 只喂饱前几个槽位；
    // 槽位少时每个槽位每轮补 2 发，把声纳队列打满。
    const std::size_t per_slot =
        sk * 2 <= deps_.config.round_cap ? 2 : 1;
    std::size_t made = 0;
    for (std::size_t t = 0; t < sk; ++t) {
      if (made >= deps_.config.round_cap ||
          deps_.queue.size(Group::Sonar) >= sk * 4)
        break;
      if (t >= tiles->slot_centers.size())
        break;
      const std::vector<std::uint32_t>& centers = tiles->slot_centers[t];
      if (centers.empty())
        continue;
      std::size_t& cursor = sonar_cursor_[t];
      for (std::size_t k = 0; k < per_slot && made < deps_.config.round_cap;
           ++k) {
        if (deps_.queue.size(Group::Sonar) >= sk * 4)
          break;
        if (cursor >= centers.size())
          cursor = 0;
        const std::uint32_t idx = centers[cursor];
        ++cursor;
        if (idx >= target_.known.size() || !target_.known[idx])
          continue;
        if (!plan_.claim_sonar(idx))
          continue;
        PaintOp op;
        op.idx = idx;
        op.color = target_.pix[idx];
        op.group = Group::Sonar;
        op.affinity = slots[t];
        deps_.queue.push(op);
        ++made;
      }
    }
    sleep_ms(deps_.config.sonar_ms);
  }
}

void NewSch1122::drop_paint_queue() {
  const std::vector<PaintOp> removed = deps_.queue.remove_if(
      [](const PaintOp& op) { return op.group == Group::Paint; });
  for (const PaintOp& op : removed)
    plan_.release(op.idx);
}

void NewSch1122::drop_sonar_queue() {
  const std::vector<PaintOp> removed = deps_.queue.remove_if(
      [](const PaintOp& op) { return op.group == Group::Sonar; });
  for (const PaintOp& op : removed)
    plan_.release_sonar(op.idx);
}

void NewSch1122::drop_nonpaint() {
  const std::vector<PaintOp> removed = deps_.queue.remove_if(
      [](const PaintOp& op) { return op.group != Group::Paint; });
  for (const PaintOp& op : removed) {
    if (op.group == Group::Sonar)
      plan_.release_sonar(op.idx);
    else
      plan_.release(op.idx);
  }
  deps_.status.drop_np.fetch_add(removed.size());
}

bool NewSch1122::is_sonar_post(std::uint32_t idx) const {
  {
    std::lock_guard<std::mutex> lk(sonar_mu_);
    if (sonar_tiles_ && idx < sonar_tiles_->is_center.size())
      return sonar_tiles_->is_center[idx] != 0;
  }
  // 密铺快照还没建好时退回旧的稀疏网格，避免 follow 画笔空转。
  return std::find(target_.sp_pts.begin(), target_.sp_pts.end(), idx) !=
         target_.sp_pts.end();
}

bool NewSch1122::is_sonar_center(std::uint32_t idx) const {
  std::lock_guard<std::mutex> lk(sonar_mu_);
  return sonar_tiles_ && idx < sonar_tiles_->is_center.size() &&
         sonar_tiles_->is_center[idx] != 0;
}

std::size_t NewSch1122::painter_count(Group group) const {
  std::size_t count = 0;
  for (const IPainter* painter : deps_.painters)
    if (painter && painter->group() == group)
      ++count;
  return count;
}

} // namespace paintboard
