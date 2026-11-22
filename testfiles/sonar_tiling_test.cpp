// 声纳密铺模块的离线自检：覆盖性、中心合法性、分块归属。
// 不联网，直接构造 Target。
#include "scheduler/sonar_tiling.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace paintboard;

namespace {

int g_fail = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

Target make_target(std::uint32_t W, std::uint32_t H, std::uint32_t x0,
                   std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
                   bool checkerboard) {
  Target t;
  t.built = true;
  t.W = W;
  t.H = H;
  t.pix.assign(static_cast<std::size_t>(W) * H, 0xFFFFFFFFu);
  t.known.assign(static_cast<std::size_t>(W) * H, 0);
  t.bb_x0 = x0;
  t.bb_y0 = y0;
  t.bb_x1 = x1;
  t.bb_y1 = y1;
  for (std::uint32_t y = y0; y <= y1; ++y) {
    for (std::uint32_t x = x0; x <= x1; ++x) {
      if (checkerboard && (((x + y) & 1u) != 0))
        continue;
      const std::uint32_t p = y * W + x;
      t.known[p] = 1;
      t.known_idx.push_back(p);
    }
  }
  return t;
}

void cov_diamond(std::uint32_t cx, std::uint32_t cy, std::int64_t R,
                 std::uint32_t W, std::uint32_t H,
                 std::vector<int>& cov) {
  const std::int64_t W64 = W, H64 = H;
  for (std::int64_t dy = -R; dy <= R; ++dy) {
    const std::int64_t span = R - (dy < 0 ? -dy : dy);
    const std::int64_t y = static_cast<std::int64_t>(cy) + dy;
    if (y < 0 || y >= H64)
      continue;
    for (std::int64_t dx = -span; dx <= span; ++dx) {
      const std::int64_t x = static_cast<std::int64_t>(cx) + dx;
      if (x < 0 || x >= W64)
        continue;
      ++cov[static_cast<std::size_t>(y * W64 + x)];
    }
  }
}

void check_case(std::uint32_t R, std::size_t slots, bool checkerboard) {
  const std::uint32_t W = 64, H = 48;
  const std::uint32_t x0 = 13, y0 = 7, x1 = 50, y1 = 38;
  const Target t = make_target(W, H, x0, y0, x1, y1, checkerboard);
  const SonarTiling tile = build_sonar_tiling(t, R, slots);
  const std::size_t n = static_cast<std::size_t>(W) * H;

  std::size_t known = 0;
  for (std::size_t p = 0; p < n; ++p)
    known += t.known[p] ? 1 : 0;
  CHECK(tile.known_total == known);
  CHECK(tile.W == W && tile.H == H && tile.radius == R && tile.slots == slots);
  CHECK(tile.bb_x0 == x0 && tile.bb_y0 == y0 && tile.bb_x1 == x1 &&
        tile.bb_y1 == y1);
  CHECK(tile.is_center.size() == n);
  CHECK(tile.slot_centers.size() == slots);
  CHECK(tile.covered_total == known);

  std::vector<std::uint8_t> seen(n, 0);
  std::vector<int> cov(n, 0);
  std::size_t total_centers = 0;
  for (std::size_t s = 0; s < tile.slot_centers.size(); ++s) {
    for (std::uint32_t p : tile.slot_centers[s]) {
      CHECK(p < n);
      CHECK(t.known[p] != 0);      // 中心必须落在目标像素上
      CHECK(tile.is_center[p] != 0);
      CHECK(seen[p] == 0);         // 同一个中心只能属于一个 slot
      seen[p] = 1;
      ++total_centers;
      cov_diamond(p % W, p / W, R, W, H, cov);
    }
  }
  CHECK(total_centers == tile.tiling_centers + tile.extra_centers);

  std::size_t marked = 0;
  for (std::size_t p = 0; p < n; ++p)
    marked += tile.is_center[p] ? 1 : 0;
  CHECK(marked == total_centers);

  // all_centers 是 is_center 的行优先展开，顺序必须严格递增。
  CHECK(tile.all_centers.size() == total_centers);
  for (std::size_t i = 0; i < tile.all_centers.size(); ++i) {
    const std::uint32_t p = tile.all_centers[i];
    CHECK(p < n);
    CHECK(tile.is_center[p] != 0);
    if (i > 0)
      CHECK(tile.all_centers[i - 1] < p);
  }

  // 每个 known 像素至少被一发菱形覆盖。
  std::size_t covered = 0;
  for (std::size_t p = 0; p < n; ++p) {
    if (!t.known[p])
      continue;
    CHECK(cov[p] > 0);
    if (cov[p] > 0)
      ++covered;
  }
  CHECK(covered == known);

  // 实心矩形内部离边超过 2R 的像素：边缘补射的菱形最远只能伸到 2R，
  // 所以这里只可能被密铺本身覆盖，应恰好一次。
  if (!checkerboard && R > 0) {
    std::size_t interior = 0;
    for (std::uint32_t y = y0 + 2 * R + 1; y + 2 * R + 1 <= y1; ++y) {
      for (std::uint32_t x = x0 + 2 * R + 1; x + 2 * R + 1 <= x1; ++x) {
        const std::size_t p = static_cast<std::size_t>(y) * W + x;
        CHECK(cov[p] == 1);
        ++interior;
      }
    }
    CHECK(interior > 0);
  }

  // R=0 时每个 known 像素都是中心，slot 分块必须把像素铺到多个槽位。
  if (R == 0 && slots > 1) {
    std::size_t nonempty = 0;
    for (const auto& list : tile.slot_centers)
      nonempty += list.empty() ? 0 : 1;
    CHECK(nonempty == slots);
  }
}

void check_partition() {
  for (std::size_t c : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                        std::size_t{5}, std::size_t{13}, std::size_t{40}}) {
    std::vector<std::uint32_t> centers;
    for (std::size_t i = 0; i < c; ++i)
      centers.push_back(static_cast<std::uint32_t>(100 + i * 3));

    for (std::size_t b : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                          std::size_t{3}, std::size_t{6}, std::size_t{13},
                          std::size_t{20}, std::size_t{60}}) {
      const auto parts = partition_sonar_centers(centers, b);
      CHECK(parts.size() == b);
      if (b == 0 || c == 0) {
        for (const auto& list : parts)
          CHECK(list.empty());
        continue;
      }

      if (b >= c) {
        // 每槽一个中心；中心被连续的一段槽共享。
        std::vector<std::size_t> count(c, 0);
        std::size_t total = 0;
        for (const auto& list : parts) {
          CHECK(list.size() == 1);
          if (list.size() != 1)
            continue;
          const std::uint32_t p = list[0];
          const std::size_t idx = (p - 100) / 3;
          CHECK(idx < c);
          if (idx < c)
            ++count[idx];
          ++total;
        }
        CHECK(total == b);
        for (std::size_t i = 0; i < c; ++i) {
          CHECK(count[i] == b / c || count[i] == (b + c - 1) / c);
          CHECK(count[i] > 0);
        }
      } else {
        // 连续块：不重不漏，块大小差不超过 1，顺序与 centers 一致。
        std::vector<std::uint8_t> seen(c, 0);
        std::size_t total = 0;
        std::size_t cursor = 0;
        for (const auto& list : parts) {
          CHECK(!list.empty());
          for (std::uint32_t p : list) {
            const std::size_t idx = (p - 100) / 3;
            CHECK(idx == cursor);
            CHECK(idx < c);
            if (idx < c)
              CHECK(seen[idx] == 0);
            if (idx < c)
              seen[idx] = 1;
            ++cursor;
            ++total;
          }
          CHECK(list.size() == (c + b - 1) / b ||
                list.size() == c / b);
        }
        CHECK(total == c);
        for (std::size_t i = 0; i < c; ++i)
          CHECK(seen[i] != 0);
      }
    }
  }
}

} // namespace

int main() {
  check_partition();
  for (std::uint32_t R : {0u, 1u, 2u, 3u}) {
    for (std::size_t slots : {std::size_t{1}, std::size_t{7},
                              std::size_t{29}}) {
      check_case(R, slots, false);
      check_case(R, slots, true);
    }
  }

  // 空目标 / 非法 slots 不应崩，也不应产生中心。
  {
    Target t = make_target(16, 16, 0, 0, 15, 15, false);
    for (auto& k : t.known)
      k = 0;
    t.known_idx.clear();
    const SonarTiling tile = build_sonar_tiling(t, 2, 4);
    CHECK(tile.known_total == 0);
    CHECK(tile.slot_centers.size() == 4);
    for (const auto& list : tile.slot_centers)
      CHECK(list.empty());
  }
  {
    Target t = make_target(16, 16, 0, 0, 15, 15, false);
    const SonarTiling tile = build_sonar_tiling(t, 2, 0);
    CHECK(tile.slot_centers.empty());
    CHECK(tile.tiling_centers == 0);
  }

  if (g_fail != 0) {
    std::fprintf(stderr, "sonar_tiling_test FAILED (%d)\n", g_fail);
    return 1;
  }
  std::printf("sonar_tiling_test OK\n");
  return 0;
}
