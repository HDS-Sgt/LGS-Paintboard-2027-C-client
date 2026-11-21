#include "paintboard/board.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace paintboard {
namespace {

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  std::uint8_t u8() {
    if (pos_ + 1 > data_.size()) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }

  std::uint16_t u16() {
    if (pos_ + 2 > data_.size()) {
      ok_ = false;
      return 0;
    }
    const std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                            (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }

  std::uint32_t u32() {
    if (pos_ + 4 > data_.size()) {
      ok_ = false;
      return 0;
    }
    const std::uint32_t v = static_cast<std::uint32_t>(data_[pos_]) |
                            (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                            (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                            (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
  }

  std::uint64_t u64() {
    const std::uint64_t lo = u32();
    const std::uint64_t hi = u32();
    return lo | (hi << 32);
  }

  void skip(std::size_t n) {
    if (pos_ + n > data_.size()) {
      ok_ = false;
      return;
    }
    pos_ += n;
  }

  bool ok() const { return ok_; }
  std::size_t pos() const { return pos_; }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

std::uint8_t blend_channel(std::uint32_t color, unsigned shift) {
  const std::uint32_t alpha = color >> 24;
  return static_cast<std::uint8_t>(((color >> shift) & 0xFFu) * alpha / 255u +
                                   (255u - alpha));
}

bool rgb_from_pixels(const std::vector<std::uint32_t>& pixels,
                     std::vector<std::uint8_t>& out) {
  out.resize(pixels.size() * 3);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    out[i * 3 + 0] = blend_channel(pixels[i], 0);
    out[i * 3 + 1] = blend_channel(pixels[i], 8);
    out[i * 3 + 2] = blend_channel(pixels[i], 16);
  }
  return true;
}

} // namespace

void Board::apply_rules(const Rules& rules) {
  if (rules.width == 0 || rules.height == 0) return;
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(rules.width) * rules.height;
  if (pixels > (16ull << 20)) return;

  std::lock_guard<std::mutex> lock(mu_);
  const bool dims_changed =
      rules_.width != rules.width || rules_.height != rules.height;
  rules_ = rules;
  if (dims_changed || cols_.empty()) {
    cols_.assign(static_cast<std::size_t>(pixels), 0);
    l1_.assign(static_cast<std::size_t>(pixels), 0);
    l2_.assign(static_cast<std::size_t>(pixels), 0);
    sonar_col_.assign(static_cast<std::size_t>(pixels), 0xFFFFFFFFu);
    sonar_tick_.assign(static_cast<std::size_t>(pixels), 0);
    has_public_canvas_ = false;
  }
}

bool Board::apply_snapshot(std::span<const std::uint8_t> payload) {
  if (payload.size() < 16) return false;
  Reader reader(payload);
  if (reader.u8() != 0x83) return false;
  if (reader.u8() != 1) return false;
  const std::uint8_t codec = reader.u8();
  reader.u8(); // reserved
  const std::uint16_t width = reader.u16();
  const std::uint16_t height = reader.u16();
  const Tick version = reader.u64();
  if (!reader.ok() || codec > 1) return false;

  std::lock_guard<std::mutex> lock(mu_);
  if (cols_.empty()) return false;
  if (width != rules_.width || height != rules_.height) return false;

  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  const std::size_t expected =
      16 + pixels * 4 * (codec == 1 ? 3u : 1u);
  if (payload.size() != expected) return false;

  for (std::size_t i = 0; i < pixels; ++i) cols_[i] = reader.u32();
  if (codec == 1) {
    for (std::size_t i = 0; i < pixels; ++i) l1_[i] = reader.u32();
    for (std::size_t i = 0; i < pixels; ++i) l2_[i] = reader.u32();
  }
  if (!reader.ok()) return false;

  since_ = version;
  has_public_canvas_ = true;
  return true;
}

bool Board::apply_public(std::span<const std::uint8_t> payload) {
  if (payload.size() < 36) return false;
  Reader reader(payload);
  if (reader.u8() != 0x82) return false;
  if (reader.u8() != 1) return false;
  reader.u16(); // p
  const std::uint32_t count = reader.u32();
  const Tick from = reader.u64();
  reader.u64(); // to
  const std::uint32_t segments = reader.u32();
  const std::uint32_t big_count = reader.u32();
  const std::uint32_t order = reader.u32();
  if (!reader.ok() || segments == 0 || segments > count) return false;

  std::vector<std::uint32_t> ticks(segments);
  std::vector<std::uint32_t> counts(segments);
  for (auto& v : ticks) v = reader.u32();
  for (auto& v : counts) v = reader.u32();

  std::vector<std::uint8_t> deltas(count);
  for (auto& v : deltas) v = reader.u8();
  reader.skip((4 - count % 4) % 4);

  std::vector<std::uint32_t> big(big_count);
  for (auto& v : big) v = reader.u32();

  std::vector<std::uint32_t> colors(count);
  for (auto& v : colors) v = reader.u32();
  if (!reader.ok()) return false;

  std::uint64_t total = 0;
  for (std::uint32_t c : counts) total += c;
  if (total != count) return false;

  std::lock_guard<std::mutex> lock(mu_);
  if (cols_.empty()) return false;
  if (from != since_) return false;

  const std::uint32_t width = rules_.width;
  const std::uint32_t height = rules_.height;
  if (width == 0 || height == 0) return false;

  std::size_t di = 0;
  std::size_t bi = 0;
  Tick since = from;
  for (std::uint32_t segment = 0; segment < segments; ++segment) {
    since = from + ticks[segment];
    std::uint32_t pos = 0;
    for (std::uint32_t j = 0; j < counts[segment]; ++j) {
      if (di >= deltas.size()) return false;
      if (deltas[di] == 255) {
        if (bi >= big.size()) return false;
        pos += big[bi++];
      } else {
        pos += deltas[di];
      }

      std::int64_t row = 0;
      std::int64_t col = 0;
      if (order == 0) {
        row = pos / width;
        col = pos % width;
      } else if (order == 1) {
        const std::int64_t idx =
            static_cast<std::int64_t>(pos % height) * width +
            static_cast<std::int64_t>(pos / height);
        col = idx % width;
        row = idx / width;
      } else {
        return false;
      }
      if (row < 0 || col < 0 || row >= height || col >= width) return false;

      const std::size_t at = static_cast<std::size_t>(row) * width + col;
      const std::uint32_t old = cols_[at];
      const std::uint32_t ref = colors[di];
      std::uint32_t next = old;
      if (ref == 0) {
        next = l1_[at];
      } else if (ref == 1) {
        next = l2_[at];
      } else if (ref == 2) {
        next = old;
      } else {
        next = ref;
      }

      if (next != old) {
        const std::uint32_t h1 = l1_[at];
        const std::uint32_t h2 = l2_[at];
        std::uint32_t next_l2 = next;
        if (h1 != old && h1 != next) {
          next_l2 = h1;
        } else if (h2 != old && h2 != next) {
          next_l2 = h2;
        }
        l2_[at] = next_l2;
        l1_[at] = old;
        cols_[at] = next;
      }
      ++di;
    }
  }

  since_ = since;
  return true;
}

void Board::apply_sonar(const SonarBatch& batch) {
  std::lock_guard<std::mutex> lock(mu_);
  if (cols_.empty()) return;
  const std::uint32_t width = rules_.width;
  const std::uint32_t height = rules_.height;
  if (width == 0 || height == 0) return;

  const std::uint64_t cells = 2ull * batch.radius * batch.radius +
                              2ull * batch.radius + 1;
  if (batch.center.size() * cells > batch.color.size()) return;

  for (std::size_t k = 0; k < batch.center.size(); ++k) {
    const std::uint32_t idx = batch.center[k];
    if (static_cast<std::uint64_t>(idx) >=
        static_cast<std::uint64_t>(width) * height) {
      continue;
    }
    const std::int64_t cx = idx % width;
    const std::int64_t cy = idx / width;
    std::size_t q = 0;
    for (std::int64_t dy = -batch.radius; dy <= batch.radius; ++dy) {
      const std::int64_t span = batch.radius - std::abs(dy);
      for (std::int64_t dx = -span; dx <= span; ++dx) {
        const std::uint32_t color = batch.color[k * cells + q++];
        const std::int64_t px = cx + dx;
        const std::int64_t py = cy + dy;
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        const std::size_t at = static_cast<std::size_t>(py) * width + px;
        sonar_col_[at] = color;
        sonar_tick_[at] = batch.tick;
      }
    }
  }
}

Tick Board::since() const {
  std::lock_guard<std::mutex> lock(mu_);
  return since_;
}

BoardView Board::view() const {
  std::lock_guard<std::mutex> lock(mu_);
  BoardView out;
  out.rules = rules_;
  out.last_tick = since_;
  out.has_public_canvas = has_public_canvas_;
  return out;
}

SonarSnapshot Board::sonar_snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  SonarSnapshot out;
  out.width = rules_.width;
  out.height = rules_.height;
  out.color = sonar_col_;
  out.tick = sonar_tick_;
  return out;
}

bool Board::copy_canvas_rgb(std::vector<std::uint8_t>& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (cols_.empty()) return false;
  return rgb_from_pixels(cols_, out);
}

bool Board::copy_sonar_rgb(std::vector<std::uint8_t>& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (sonar_col_.empty()) return false;
  return rgb_from_pixels(sonar_col_, out);
}

std::vector<std::uint32_t> Board::canvas_snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return cols_;
}

} // namespace paintboard
