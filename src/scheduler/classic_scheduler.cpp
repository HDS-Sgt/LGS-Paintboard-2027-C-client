#define STB_IMAGE_IMPLEMENTATION
#include ".deps/stb/stb_image.h"

#include "scheduler/classic_scheduler.hpp"

#include "paintboard/painter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace paintboard {
namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

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

bool load_ppm(const std::string& path, std::uint32_t& width,
              std::uint32_t& height, std::vector<std::uint32_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (data.size() < 2 || data[0] != 'P' || data[1] != '6')
    return false;

  std::size_t pos = 2;
  auto skip = [&]() {
    for (;;) {
      while (pos < data.size() &&
             std::isspace(static_cast<unsigned char>(data[pos])))
        ++pos;
      if (pos < data.size() && data[pos] == '#') {
        while (pos < data.size() && data[pos] != '\n')
          ++pos;
        continue;
      }
      break;
    }
  };
  auto number = [&](std::uint32_t& value) {
    skip();
    if (pos >= data.size() ||
        !std::isdigit(static_cast<unsigned char>(data[pos])))
      return false;
    value = 0;
    while (pos < data.size() &&
           std::isdigit(static_cast<unsigned char>(data[pos]))) {
      value = value * 10 + static_cast<std::uint32_t>(data[pos] - '0');
      ++pos;
    }
    return true;
  };

  std::uint32_t max_value = 0;
  if (!number(width) || !number(height) || !number(max_value) ||
      max_value != 255 || width == 0 || height == 0)
    return false;
  if (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) {
    if (data[pos] == '\r' && pos + 1 < data.size() && data[pos + 1] == '\n')
      pos += 2;
    else
      ++pos;
  }

  const std::uint64_t need = static_cast<std::uint64_t>(width) * height * 3;
  if (need > data.size() - pos)
    return false;

  out.resize(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t r = data[pos], g = data[pos + 1], b = data[pos + 2];
    pos += 3;
    out[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
  }
  return true;
}

bool load_image(const std::string& path, std::uint32_t& width,
                std::uint32_t& height, std::vector<std::uint32_t>& out) {
  if (path.empty())
    return false;

  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (pixels) {
    if (w <= 0 || h <= 0) {
      stbi_image_free(pixels);
      return false;
    }
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    out.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.size(); ++i) {
      std::uint32_t r = pixels[4 * i], g = pixels[4 * i + 1],
                   b = pixels[4 * i + 2], a = pixels[4 * i + 3];
      if (a != 255) {
        r = r * a / 255 + (255 - a);
        g = g * a / 255 + (255 - a);
        b = b * a / 255 + (255 - a);
      }
      out[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    stbi_image_free(pixels);
    return true;
  }
  return load_ppm(path, width, height, out);
}

} // namespace

void Plan::alloc(std::size_t n, std::size_t need) {
  std::lock_guard<std::mutex> lock(mu_);
  enq_.assign(n, 0);
  enq_sonar_.assign(n, 0);
  done_.assign(n, 0);
  need_ = need;
  done_total_ = 0;
}

bool Plan::claim(std::uint32_t idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= enq_.size() || enq_[idx] || done_[idx])
    return false;
  enq_[idx] = 1;
  return true;
}

bool Plan::claim_sonar(std::uint32_t idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= enq_sonar_.size() || enq_sonar_[idx])
    return false;
  enq_sonar_[idx] = 1;
  return true;
}

bool Plan::claimed(std::uint32_t idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  return idx < enq_.size() && enq_[idx] != 0;
}

bool Plan::claimed_sonar(std::uint32_t idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  return idx < enq_sonar_.size() && enq_sonar_[idx] != 0;
}

bool Plan::done(std::uint32_t idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  return idx < done_.size() && done_[idx] != 0;
}

void Plan::mark_done(std::uint32_t idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= done_.size())
    return;
  if (idx < enq_.size())
    enq_[idx] = 0;
  if (idx < enq_sonar_.size())
    enq_sonar_[idx] = 0;
  if (!done_[idx]) {
    done_[idx] = 1;
    ++done_total_;
  }
}

void Plan::release(std::uint32_t idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < enq_.size())
    enq_[idx] = 0;
}

void Plan::release_sonar(std::uint32_t idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < enq_sonar_.size())
    enq_sonar_[idx] = 0;
}

void Plan::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  std::fill(enq_.begin(), enq_.end(), static_cast<std::uint8_t>(0));
  std::fill(enq_sonar_.begin(), enq_sonar_.end(), static_cast<std::uint8_t>(0));
  std::fill(done_.begin(), done_.end(), static_cast<std::uint8_t>(0));
  done_total_ = 0;
}

std::size_t Plan::need_total() const {
  std::lock_guard<std::mutex> lock(mu_);
  return need_;
}

std::size_t Plan::done_total() const {
  std::lock_guard<std::mutex> lock(mu_);
  return done_total_;
}

Target build_target(const Config& config, const Rules& rules,
                    const Board&) {
  Target target;
  target.built = true;
  target.W = rules.width;
  target.H = rules.height;
  if (rules.width == 0 || rules.height == 0)
    return target;

  const std::size_t n = static_cast<std::size_t>(rules.width) * rules.height;
  target.pix.assign(n, 0);
  target.known.assign(n, 0);

  std::uint32_t sw = 0, sh = 0;
  std::vector<std::uint32_t> src;
  const bool has_image = load_image(config.image_path, sw, sh, src);
  if (has_image) {
    for (std::uint32_t y = 0; y < sh; ++y) {
      for (std::uint32_t x = 0; x < sw; ++x) {
        const std::int64_t gx = static_cast<std::int64_t>(x) + config.offset_x;
        const std::int64_t gy = static_cast<std::int64_t>(y) + config.offset_y;
        if (gx < 0 || gy < 0 || gx >= rules.width || gy >= rules.height)
          continue;
        const std::size_t p = static_cast<std::size_t>(gy) * rules.width + gx;
        target.pix[p] = src[static_cast<std::size_t>(y) * sw + x];
        target.known[p] = 1;
      }
    }
  }

  for (const Draw& draw : config.draws) {
    if (draw.x >= rules.width || draw.y >= rules.height)
      continue;
    const std::size_t p = static_cast<std::size_t>(draw.y) * rules.width + draw.x;
    target.pix[p] = draw.color;
    target.known[p] = 1;
  }

  target.bb_x0 = rules.width;
  target.bb_y0 = rules.height;
  target.bb_x1 = 0;
  target.bb_y1 = 0;
  for (std::uint32_t y = 0; y < rules.height; ++y) {
    for (std::uint32_t x = 0; x < rules.width; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * rules.width + x;
      if (!target.known[p])
        continue;
      target.known_idx.push_back(static_cast<std::uint32_t>(p));
      target.bb_x0 = std::min(target.bb_x0, x);
      target.bb_y0 = std::min(target.bb_y0, y);
      target.bb_x1 = std::max(target.bb_x1, x);
      target.bb_y1 = std::max(target.bb_y1, y);
    }
  }
  if (target.known_idx.empty()) {
    target.bb_x0 = 0;
    target.bb_y0 = 0;
    target.bb_x1 = rules.width - 1;
    target.bb_y1 = rules.height - 1;
  }

  std::uint32_t stride = config.sonar_stride;
  const std::uint32_t need = 2 * static_cast<std::uint32_t>(rules.radius) + 1;
  if (stride < need)
    stride = need;
  if (stride < 1)
    stride = 1;
  const std::uint32_t bw =
      target.bb_x1 >= target.bb_x0 ? (target.bb_x1 - target.bb_x0) / stride + 1 : 0;
  const std::uint32_t bh =
      target.bb_y1 >= target.bb_y0 ? (target.bb_y1 - target.bb_y0) / stride + 1 : 0;
  for (std::uint32_t k = 0; k < bw * bh; ++k) {
    const std::uint32_t gx = target.bb_x0 + (k % bw) * stride;
    const std::uint32_t gy = target.bb_y0 + (k / bw) * stride;
    if (gx >= rules.width || gy >= rules.height)
      continue;
    target.sp_pts.push_back(gy * rules.width + gx);
  }

  return target;
}

bool target_complete(const Target& target, const Plan& plan) {
  return target.built && !target.known_idx.empty() &&
         plan.done_total() >= target.known_idx.size();
}

ClassicScheduler::ClassicScheduler(SchedulerDeps deps) : deps_(deps) {}

ClassicScheduler::~ClassicScheduler() { stop(); }

void ClassicScheduler::prepare() {
  const BoardView view = deps_.board.view();
  target_ = build_target(deps_.config, view.rules, deps_.board);
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

void ClassicScheduler::on_all_bound() {
  if (started_.exchange(true))
    return;

  if (painter_count(Group::Paint) > 0)
    threads_.emplace_back(&ClassicScheduler::paint_loop, this);
  if (painter_count(Group::Repair) > 0)
    threads_.emplace_back(&ClassicScheduler::repair_loop, this);
  if (painter_count(Group::Sonar) > 0)
    threads_.emplace_back(&ClassicScheduler::sonar_loop, this);
}

void ClassicScheduler::step(Tick now) {
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
      if (op.affinity && plan_.claimed_sonar(op.idx)) {
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
    } else {
      if (plan_.claimed(op.idx)) {
        if (op.group == Group::Paint)
          valid = !plan_.done(op.idx);
        else
          valid = true;
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
    if (view.group == Group::Sonar) {
      for (std::size_t j = 0; j < live.size(); ++j) {
        if (used[j] || live[j].group != Group::Sonar)
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
        if (view.follow && !is_sonar_post(live[j].idx))
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
    retry.due = qnow + 20000;
    deps_.queue.requeue(retry);
  }
}

void ClassicScheduler::on_tick(const TickEvent& event) {
  for (const TickResult& result : event.results) {
    const PaintOp& op = result.op;
    if (result.status == PaintStatus::Ok) {
      ++deps_.status.painted;
      const bool hit = op.idx < target_.known.size() &&
                       target_.known[op.idx] &&
                       target_.pix[op.idx] == op.color;
      if (hit)
        plan_.mark_done(op.idx);
      else if (op.group == Group::Sonar)
        plan_.release_sonar(op.idx);
      else
        plan_.release(op.idx);
    } else if (result.status == PaintStatus::Cooling) {
      PaintOp retry = op;
      retry.due = 0; // ready_for 的 not_before 会在冷却期内挡住派发
      deps_.queue.restore_consumed(retry);
    } else {
      if (op.group == Group::Sonar)
        plan_.release_sonar(op.idx);
      else
        plan_.release(op.idx);
    }
  }
}

void ClassicScheduler::on_disconnect(std::vector<PaintOp> pending) {
  for (PaintOp& op : pending) {
    op.due = 0;
    // pending 里的操作在发送时已经 consume，重排必须把计数加回来。
    deps_.queue.restore_consumed(op);
  }
}

bool ClassicScheduler::done() const {
  if (deps_.config.paint_loop_ms > 0)
    return false;
  return target_complete(target_, plan_);
}

SchedulerStats ClassicScheduler::stats() const {
  const PaintQueue::Stats queue = deps_.queue.stats();
  SchedulerStats out;
  out.queue_total = queue.total;
  out.painted = deps_.status.painted.load();
  out.passes = deps_.status.passes.load();
  return out;
}

void ClassicScheduler::stop() {
  if (stop_.exchange(true))
    return;
  for (std::thread& thread : threads_)
    if (thread.joinable())
      thread.join();
  threads_.clear();
}

void ClassicScheduler::paint_loop() {
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
        paint_cursor_ = 0;
        ++deps_.status.passes;
        deps_.log.logf("[paint] pass {} complete -> restart",
                       deps_.status.passes.load());
        continue;
      }
      sleep_ms(deps_.config.paint_ms);
      continue;
    }

    const std::size_t n =
        static_cast<std::size_t>(target_.W) * target_.H;
    std::size_t made = 0, scanned = 0;
    for (std::size_t i = 0;
         i < n && made < deps_.config.round_cap &&
         deps_.queue.size() + 256 < deps_.config.queue_cap;
         ++i) {
      const std::size_t p = (paint_cursor_ + i) % n;
      scanned = i + 1;
      if (!target_.known[p] || plan_.done(static_cast<std::uint32_t>(p)))
        continue;
      if (!plan_.claim(static_cast<std::uint32_t>(p)))
        continue;
      PaintOp op;
      op.idx = static_cast<std::uint32_t>(p);
      op.color = target_.pix[p];
      op.group = Group::Paint;
      deps_.queue.push(op);
      ++made;
    }
    if (n)
      paint_cursor_ = (paint_cursor_ + scanned) % n;
    sleep_ms(deps_.config.paint_ms);
  }
}

void ClassicScheduler::repair_loop() {
  while (!stop_.load() && !deps_.status.stop.load()) {
    if (!target_.built) {
      sleep_ms(50);
      continue;
    }
    if (deps_.config.limit &&
        deps_.status.painted.load() >= deps_.config.limit)
      break;

    const BoardView board = deps_.board.view();
    const SonarSnapshot sonar = deps_.board.sonar_snapshot();
    const std::size_t n =
        static_cast<std::size_t>(target_.W) * target_.H;
    if (sonar.color.size() != n || sonar.tick.size() != n) {
      sleep_ms(deps_.config.repair_ms);
      continue;
    }

    const Tick last_tick = deps_.status.last_tick.load();
    const Tick verify_span =
        deps_.config.repair_verify_ms
            ? static_cast<Tick>(board.rules.tps) *
                  deps_.config.repair_verify_ms / 1000
            : 0;

    std::vector<std::uint32_t> damaged, stale;
    const std::size_t known = target_.known_idx.size();
    if (known == 0) {
      sleep_ms(deps_.config.repair_ms);
      continue;
    }
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

    std::size_t limit = painter_count(Group::Repair);
    if (limit < 16)
      limit = 16;
    std::size_t made = 0;
    for (int pass = 0; pass < 2; ++pass) {
      const std::vector<std::uint32_t>& source = pass == 0 ? damaged : stale;
      for (std::uint32_t idx : source) {
        if (made >= deps_.config.round_cap ||
            deps_.queue.size(Group::Repair) >= limit)
          break;
        if (!plan_.claim(idx))
          continue;
        PaintOp op;
        op.idx = idx;
        op.color = target_.pix[idx];
        op.group = Group::Repair;
        op.priority = static_cast<std::uint32_t>(pass);
        deps_.queue.push(op);
        ++made;
      }
    }
    if (scanned)
      repair_cursor_ = (repair_cursor_ + scanned) % known;
    sleep_ms(deps_.config.repair_ms);
  }
}

void ClassicScheduler::sonar_loop() {
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
    std::uint32_t stride = deps_.config.sonar_stride;
    const std::uint32_t need =
        2 * static_cast<std::uint32_t>(board.rules.radius) + 1;
    if (stride < need)
      stride = need;
    if (stride < 1)
      stride = 1;

    const std::uint32_t bw =
        target_.bb_x1 >= target_.bb_x0
            ? (target_.bb_x1 - target_.bb_x0) / stride + 1
            : 0;
    const std::uint32_t bh =
        target_.bb_y1 >= target_.bb_y0
            ? (target_.bb_y1 - target_.bb_y0) / stride + 1
            : 0;
    if (!bw || !bh) {
      sleep_ms(deps_.config.sonar_ms);
      continue;
    }

    if (sonar_sig_bw_ != bw || sonar_sig_bh_ != bh ||
        sonar_sig_sk_ != sk) {
      sonar_cursor_.assign(sk, 0);
      sonar_sig_bw_ = bw;
      sonar_sig_bh_ = bh;
      sonar_sig_sk_ = sk;
    }

    const std::uint32_t sbw =
        (bw + static_cast<std::uint32_t>(sk) - 1) /
        static_cast<std::uint32_t>(sk);
    std::size_t made = 0;
    for (std::size_t t = 0; t < sk; ++t) {
      if (made >= deps_.config.round_cap ||
          deps_.queue.size(Group::Sonar) >= sk * 4)
        break;
      const std::uint32_t x0 = static_cast<std::uint32_t>(t) * sbw;
      if (x0 >= bw)
        continue;
      const std::uint32_t w = std::min(sbw, bw - x0);
      const std::uint32_t per = w * bh;
      if (!per)
        continue;
      const std::uint32_t m =
          static_cast<std::uint32_t>(sonar_cursor_[t] % per);
      std::uint32_t row = m / w, col = m % w;
      if (row & 1)
        col = w - 1 - col;
      const std::uint32_t k = row * bw + x0 + col;
      const std::uint32_t gx = target_.bb_x0 + (k % bw) * stride;
      const std::uint32_t gy = target_.bb_y0 + (k / bw) * stride;
      sonar_cursor_[t] = (m + 1) % per;
      if (gx >= target_.W || gy >= target_.H)
        continue;
      const std::size_t p = static_cast<std::size_t>(gy) * target_.W + gx;
      if (!target_.known[p])
        continue;
      const std::uint32_t idx = static_cast<std::uint32_t>(p);
      if (!plan_.claim_sonar(idx))
        continue;
      PaintOp op;
      op.idx = idx;
      op.color = target_.pix[p];
      op.group = Group::Sonar;
      op.affinity = slots[t];
      deps_.queue.push(op);
      ++made;
    }
    sleep_ms(deps_.config.sonar_ms);
  }
}

void ClassicScheduler::drop_nonpaint() {
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

bool ClassicScheduler::is_sonar_post(std::uint32_t idx) const {
  return std::find(target_.sp_pts.begin(), target_.sp_pts.end(), idx) !=
         target_.sp_pts.end();
}

std::size_t ClassicScheduler::painter_count(Group group) const {
  std::size_t count = 0;
  for (const IPainter* painter : deps_.painters)
    if (painter && painter->group() == group)
      ++count;
  return count;
}

} // namespace paintboard
