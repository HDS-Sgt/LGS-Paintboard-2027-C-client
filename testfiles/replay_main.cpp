#include "paintboard/board.hpp"
#include "protocol/wire.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> slurp(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::perror(path);
    std::exit(1);
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

std::size_t public_size(std::span<const std::uint8_t> payload) {
  if (payload.size() < 36) return 0;
  auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(payload[at]) |
           (static_cast<std::uint32_t>(payload[at + 1]) << 8) |
           (static_cast<std::uint32_t>(payload[at + 2]) << 16) |
           (static_cast<std::uint32_t>(payload[at + 3]) << 24);
  };
  const std::uint32_t count = u32(4);
  const std::uint32_t segments = u32(24);
  const std::uint32_t big = u32(28);
  const std::size_t fixed = 36 + 8ull * segments + count +
                            (4 - count % 4) % 4 + 4ull * big +
                            4ull * count;
  return fixed <= payload.size() ? fixed : 0;
}

std::size_t snapshot_size(std::span<const std::uint8_t> payload) {
  if (payload.size() < 16) return 0;
  const std::uint8_t codec = payload[2];
  if (codec > 1) return 0;
  const std::size_t pixels = static_cast<std::size_t>(payload[4]) |
                             (static_cast<std::size_t>(payload[5]) << 8);
  const std::size_t height = static_cast<std::size_t>(payload[6]) |
                             (static_cast<std::size_t>(payload[7]) << 8);
  const std::size_t size = 16 + pixels * height * 4 * (codec == 1 ? 3u : 1u);
  return size <= payload.size() ? size : 0;
}

bool write_ppm(const char* path, paintboard::Board& board) {
  std::vector<std::uint8_t> rgb;
  if (!board.copy_canvas_rgb(rgb)) return false;
  const auto view = board.view();
  FILE* out = std::fopen(path, "wb");
  if (!out) {
    std::perror(path);
    return false;
  }
  std::fprintf(out, "P6\n%u %u\n255\n", view.rules.width, view.rules.height);
  std::fwrite(rgb.data(), 1, rgb.size(), out);
  std::fclose(out);
  std::printf("wrote %s (%ux%u) since=%llu\n", path, view.rules.width,
              view.rules.height,
              static_cast<unsigned long long>(board.since()));
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <snap.zst> <delta.zst> <out.ppm>\n",
                 argv[0]);
    return 1;
  }

  std::vector<std::uint8_t> rules_bytes(96, 0);
  rules_bytes[0] = 0x84;
  rules_bytes[1] = 1;
  rules_bytes[4] = 1000 & 255;
  rules_bytes[5] = 1000 >> 8;
  rules_bytes[6] = 600 & 255;
  rules_bytes[7] = 600 >> 8;
  rules_bytes[8] = 2;
  rules_bytes[12] = 128;

  paintboard::Board board;
  board.apply_rules(paintboard::protocol::parse_rules(rules_bytes));

  const auto snap_wire = slurp(argv[1]);
  const auto snap = paintboard::protocol::zstd_decode_one(snap_wire);
  if (!board.apply_snapshot(snap)) {
    std::puts("bad snapshot");
    return 2;
  }

  const auto delta_wire = slurp(argv[2]);
  const auto delta = paintboard::protocol::zstd_decode_one(delta_wire);
  std::span<const std::uint8_t> rest(delta);
  while (!rest.empty()) {
    const std::uint8_t type = rest[0];
    if (type == 0x82) {
      const std::size_t size = public_size(rest);
      if (size == 0) {
        std::puts("bad public size");
        return 3;
      }
      if (!board.apply_public(rest.first(size))) {
        std::puts("apply_public failed");
        return 3;
      }
      rest = rest.subspan(size);
    } else if (type == 0x83) {
      const std::size_t size = snapshot_size(rest);
      if (size == 0 || !board.apply_snapshot(rest.first(size))) {
        std::puts("bad snapshot");
        return 3;
      }
      rest = rest.subspan(size);
    } else {
      std::printf("unknown type 0x%02x\n", type);
      return 4;
    }
  }

  return write_ppm(argv[3], board) ? 0 : 5;
}
