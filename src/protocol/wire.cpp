#include "protocol/wire.hpp"

#include "zstdwrap.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace paintboard::protocol {
namespace {

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool end() const { return pos_ >= bytes_.size(); }
  std::size_t pos() const { return pos_; }
  void skip(std::size_t n) {
    if (pos_ + n > bytes_.size()) {
      bad_ = true;
      pos_ = bytes_.size();
      return;
    }
    pos_ += n;
  }

  std::uint8_t read8() {
    if (bad_ || pos_ >= bytes_.size()) {
      bad_ = true;
      return 0;
    }
    return bytes_[pos_++];
  }

  std::uint16_t read16() {
    return static_cast<std::uint16_t>(read8()) |
           (static_cast<std::uint16_t>(read8()) << 8);
  }

  std::uint32_t read32() {
    return static_cast<std::uint32_t>(read16()) |
           (static_cast<std::uint32_t>(read16()) << 16);
  }

  std::uint64_t read64() {
    return static_cast<std::uint64_t>(read32()) |
           (static_cast<std::uint64_t>(read32()) << 32);
  }

  bool bad() const { return bad_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_ = 0;
  bool bad_ = false;
};

std::size_t pad4(std::size_t n) { return (n + 3) & ~std::size_t(3); }

std::uint16_t rd16(std::span<const std::uint8_t> p, std::size_t off) {
  return static_cast<std::uint16_t>(p[off]) |
         (static_cast<std::uint16_t>(p[off + 1]) << 8);
}

std::uint32_t rd32(std::span<const std::uint8_t> p, std::size_t off) {
  return static_cast<std::uint32_t>(p[off]) |
         (static_cast<std::uint32_t>(p[off + 1]) << 8) |
         (static_cast<std::uint32_t>(p[off + 2]) << 16) |
         (static_cast<std::uint32_t>(p[off + 3]) << 24);
}

std::uint64_t rd64(std::span<const std::uint8_t> p, std::size_t off) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(p[off + i]) << (8 * i);
  return value;
}

PaintStatus map_status(std::uint8_t code) {
  switch (code) {
    case 0:
      return PaintStatus::Ok;
    case 1:
      return PaintStatus::Cooling;
    case 2:
    case 3:
      return PaintStatus::SlotInvalid;
    default:
      return PaintStatus::Error;
  }
}

} // namespace

Rules parse_rules(std::span<const std::uint8_t> payload) {
  if (payload.size() != 96 || payload[0] != 0x84 || payload[1] != 1)
    throw std::runtime_error("rules: malformed payload");

  Reader in(payload);
  in.read8(); // type
  in.read8(); // version
  in.read16(); // reserved (实测 0x20，不能当字段)
  Rules out;
  out.width = in.read16();
  out.height = in.read16();
  out.radius = in.read16();
  in.read16(); // reserved
  out.tps = in.read32();
  out.cooldown_base = in.read64();
  out.cooldown_per_px = in.read64();
  in.read64(); // public_delay_ms
  in.read64(); // public_period_ms
  in.read64(); // event_start
  in.read64(); // event_end
  out.max_slots = in.read32();
  out.max_paints_per_message = in.read32();
  in.read32(); // msg_rate
  in.read32(); // msg_burst
  out.max_message_bytes = in.read32();
  in.read32(); // max_payload_bytes
  out.server_time = in.read64();

  if (in.bad() || out.width == 0 || out.height == 0 ||
      static_cast<std::uint64_t>(out.width) * out.height > (16u << 20))
    throw std::runtime_error("rules: invalid canvas");
  return out;
}

void parse_tick(std::span<const std::uint8_t> payload, TickEvent& out) {
  out = TickEvent{};
  if (payload.size() < 32 || payload[0] != 0x81 || payload[1] != 1)
    throw std::runtime_error("tick: malformed header");

  const std::uint16_t radius = rd16(payload, 2);
  const std::uint32_t nr = rd32(payload, 4);
  const std::uint64_t tick = rd64(payload, 8);
  const std::uint32_t nb = rd32(payload, 16);
  const std::uint32_t nk = rd32(payload, 20);
  const std::uint32_t ns = rd32(payload, 24);
  if (radius > 16)
    throw std::runtime_error("tick: sonar radius too large");

  const std::uint64_t cells = 2ull * radius * radius + 2ull * radius + 1;
  const std::uint64_t need = 32 + 12ull * nb + pad4(2ull * nb) +
                             pad4(nr) + 4ull * nr + 4ull * nk +
                             4ull * nk * cells + pad4(2ull * ns);
  if (need != payload.size())
    throw std::runtime_error("tick: length mismatch");

  const std::size_t bt = 32;
  const std::size_t bi = bt + 8ull * nb;
  const std::size_t bs = bi + 4ull * nb;
  const std::size_t st = bs + pad4(2ull * nb);
  const std::size_t va = st + pad4(nr);
  const std::size_t si = va + 4ull * nr;
  const std::size_t sc = si + 4ull * nk;
  const std::size_t ls = sc + 4ull * nk * cells;

  out.tick = tick;
  out.results.resize(nr);
  for (std::uint32_t i = 0; i < nr; ++i) {
    out.results[i].status = map_status(payload[st + i]);
    out.results[i].value = rd32(payload, va + 4ull * i);
  }

  out.brushes.resize(nb);
  for (std::uint32_t i = 0; i < nb; ++i) {
    out.brushes[i].t = rd64(payload, bt + 8ull * i);
    out.brushes[i].idx = rd32(payload, bi + 4ull * i);
    out.brushes[i].slot = rd16(payload, bs + 2ull * i);
  }

  out.sonar.radius = radius;
  out.sonar.tick = tick;
  out.sonar.center.resize(nk);
  out.sonar.color.resize(static_cast<std::size_t>(nk) * cells);
  for (std::uint32_t i = 0; i < nk; ++i) {
    out.sonar.center[i] = rd32(payload, si + 4ull * i);
    for (std::uint64_t j = 0; j < cells; ++j)
      out.sonar.color[static_cast<std::size_t>(i) * cells + j] =
          rd32(payload, sc + 4ull * (i * cells + j));
  }

  out.lost.resize(ns);
  for (std::uint32_t i = 0; i < ns; ++i)
    out.lost[i] = rd16(payload, ls + 2ull * i);
}

std::vector<std::uint8_t> encode_batch(std::span<const Bind> binds,
                                       std::span<const Paint> paints) {
  std::vector<std::uint8_t> out;
  out.reserve(16 + 2 * binds.size() + 16 * binds.size() +
              2 * paints.size() + 8 * paints.size() + 8);

  auto put16 = [&](std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 255));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 255));
  };
  auto put32 = [&](std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 255));
  };
  auto pad = [&]() {
    while (out.size() & 3)
      out.push_back(0);
  };

  out.push_back(0x01);
  out.push_back(1);
  put16(0);
  put32(static_cast<std::uint32_t>(binds.size()));
  put32(static_cast<std::uint32_t>(paints.size()));
  put32(0);

  for (const Bind& bind : binds)
    put16(bind.slot);
  pad();
  for (const Bind& bind : binds)
    for (std::uint8_t byte : bind.token)
      out.push_back(byte);

  for (const Paint& paint : paints)
    put16(paint.slot);
  pad();
  for (const Paint& paint : paints)
    put32(paint.idx);
  for (const Paint& paint : paints)
    put32(paint.color);

  return out;
}

std::vector<std::uint8_t> zstd_decode_one(
    std::span<const std::uint8_t> wire) {
  std::vector<std::uint8_t> src(wire.begin(), wire.end());
  std::vector<std::uint8_t> out;
  zstddec(src, out, 16u << 20);
  return out;
}

std::vector<std::uint8_t> zstd_encode(
    std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> src(payload.begin(), payload.end());
  std::vector<std::uint8_t> out;
  zstdenc(src, out, 1);
  return out;
}

} // namespace paintboard::protocol
