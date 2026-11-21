#include "paintboard/types.hpp"
#include "protocol/wire.hpp"

#include <cassert>
#include <cstdint>
#include <exception>

int main() {
  using namespace paintboard;
  std::array<std::uint8_t, 16> tok{};
  tok[0] = 0xAB;
  tok[15] = 0xCD;
  Bind b{7, tok};
  Paint p{7, 123, 0xFF0000FFu}; // R=255,G=0,B=0,A=255
  auto wire = protocol::encode_batch({&b, 1}, {&p, 1});
  // 16 字节头 + pad4(2*1)=4 + 16*1 + pad4(2*1)=4 + 8*1 = 48
  assert(wire.size() == 48);
  assert(wire[0] == 0x01 && wire[1] == 1);
  assert(wire[16] == 7);    // bind.slot 小端
  assert(wire[20] == 0xAB); // bind token 起点
  assert(wire[36] == 7);    // paint.slot（bind token 之后）
  assert(wire[40] == 123);  // paint.idx 低字节
  assert(wire[44] == 0xFF); // color 最低字节 R

  std::vector<std::uint8_t> payload(wire.begin(), wire.end());
  auto compressed = protocol::zstd_encode(payload);
  assert(compressed != payload);
  auto decoded = protocol::zstd_decode_one(compressed);
  assert(decoded == payload);

  bool rejected_trailing = false;
  try {
    auto trailing = compressed;
    trailing.push_back(0);
    protocol::zstd_decode_one(trailing);
  } catch (const std::exception&) {
    rejected_trailing = true;
  }
  assert(rejected_trailing);
  return 0;
}
