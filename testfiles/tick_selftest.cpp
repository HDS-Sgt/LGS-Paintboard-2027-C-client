// 离线自检：BATCH 编码、TICK 解析、声纳菱形顺序、颜色字节序。
// 不联网；直接使用新的 paintboard 协议层。
#include "paintboard/types.hpp"
#include "protocol/wire.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void p16(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 255));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 255));
}

void p32(Bytes& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 255));
}

void p64(Bytes& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 255));
}

} // namespace

int main() {
  using namespace paintboard;

  // ---- 1. encode_batch：按协议布局逐字节核对 ----
  Token token{};
  for (std::size_t i = 0; i < token.size(); ++i)
    token[i] = static_cast<std::uint8_t>((1 * 7 + i) % 256);
  Bind bind{3, token};
  Paint paint{7, 123456u, 0xFF0000FFu};

  Bytes got = protocol::encode_batch({&bind, 1}, {&paint, 1});
  Bytes expected;
  expected.push_back(1);
  expected.push_back(1);
  p16(expected, 0);
  p32(expected, 1);
  p32(expected, 1);
  p32(expected, 0);
  p16(expected, 3);
  p16(expected, 0);
  for (std::uint8_t byte : token)
    expected.push_back(byte);
  p16(expected, 7);
  p16(expected, 0);
  p32(expected, 123456);
  p32(expected, 0xFF0000FFu);
  assert(got == expected);
  assert(got.size() == 48);
  assert(got[16] == 3);
  assert(got[40] == (123456u & 255));
  assert(got.back() == 0xFF); // 颜色最低字节 R
  std::printf("enc_batch OK (%zu bytes)\n", got.size());

  // ---- 2. parse_tick：合成一条 TICK 并检查每个列 ----
  const std::uint32_t radius = 2;
  const std::uint32_t nr = 2, nb = 1, nk = 1, ns = 1;
  const std::uint64_t cells = 2ull * radius * radius + 2ull * radius + 1;
  Bytes payload;
  payload.push_back(0x81);
  payload.push_back(1);
  p16(payload, radius);
  p32(payload, nr);
  p64(payload, 987654321ull);
  p32(payload, nb);
  p32(payload, nk);
  p32(payload, ns);
  p32(payload, 0);
  p64(payload, 111222333ull);
  p32(payload, 4242);
  p16(payload, 3);
  p16(payload, 0);
  payload.push_back(0);
  payload.push_back(1);
  p16(payload, 0);
  p32(payload, 0);
  p32(payload, 500000);
  p32(payload, 4242);
  for (std::uint64_t i = 0; i < cells; ++i)
    p32(payload, static_cast<std::uint32_t>(0x11223300u + i));
  p16(payload, 3);
  p16(payload, 0);

  TickEvent event;
  protocol::parse_tick(payload, event);
  assert(event.tick == 987654321ull);
  assert(event.results.size() == 2);
  assert(event.results[0].status == PaintStatus::Ok);
  assert(event.results[1].status == PaintStatus::Cooling);
  assert(event.results[0].value == 0);
  assert(event.results[1].value == 500000);
  assert(event.brushes.size() == 1);
  assert(event.brushes[0].t == 111222333ull);
  assert(event.brushes[0].idx == 4242);
  assert(event.brushes[0].slot == 3);
  assert(event.sonar.radius == radius);
  assert(event.sonar.center.size() == 1);
  assert(event.sonar.center[0] == 4242);
  assert(event.sonar.color.size() == cells);
  assert(event.sonar.color[12] == 0x11223300u + 12);
  assert(event.lost.size() == 1);
  assert(event.lost[0] == 3);

  bool rejected = false;
  try {
    Bytes bad(payload.begin(), payload.end() - 1);
    TickEvent ignored;
    protocol::parse_tick(bad, ignored);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  std::printf("parse_tick OK, malformed rejected\n");

  // ---- 3. 声纳菱形顺序（R=2 共 13 格） ----
  const int seq[13][2] = {{0, -2}, {-1, -1}, {0, -1}, {1, -1}, {-2, 0},
                          {-1, 0}, {0, 0},   {1, 0},  {2, 0},  {-1, 1},
                          {0, 1},  {1, 1},   {0, 2}};
  int index = 0;
  for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius);
       ++dy) {
    for (int dx = -(static_cast<int>(radius) - (dy < 0 ? -dy : dy));
         dx <= static_cast<int>(radius) - (dy < 0 ? -dy : dy); ++dx) {
      assert(seq[index][0] == dx && seq[index][1] == dy);
      ++index;
    }
  }
  assert(index == 13);
  std::printf("sonar order OK (%d cells)\n", index);

  // ---- 4. 颜色字节序：R 在最低字节，alpha 必须是 255 ----
  assert(paint.color == 0xFF0000FFu);
  assert((paint.color & 255) == 255);
  assert(((paint.color >> 8) & 255) == 0);
  assert(((paint.color >> 16) & 255) == 0);
  assert(((paint.color >> 24) & 255) == 255);
  std::printf("color byte order OK\n");
  return 0;
}
