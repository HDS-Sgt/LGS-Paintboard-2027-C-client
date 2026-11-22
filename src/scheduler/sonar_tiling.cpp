#include "scheduler/sonar_tiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace paintboard {
namespace {

using i64 = std::int64_t;

struct Candidate {
  std::vector<std::uint32_t> centers;
  std::vector<std::uint8_t> cov;  // W*H, saturating
  std::size_t covered = 0;
};

struct LatticePoint {
  i64 x = 0, y = 0;
};

// 在 (cx,cy) 的密铺菱形上逐格标记；只统计画布内且 target.known 的像素。
template <class Fn>
void for_diamond(i64 cx, i64 cy, i64 R, i64 W, i64 H, Fn&& fn) {
  for (i64 dy = -R; dy <= R; ++dy) {
    const i64 span = R - (dy < 0 ? -dy : dy);
    const i64 y = cy + dy;
    if (y < 0 || y >= H)
      continue;
    const i64 row = y * W;
    for (i64 dx = -span; dx <= span; ++dx) {
      const i64 x = cx + dx;
      if (x < 0 || x >= W)
        continue;
      fn(static_cast<std::uint32_t>(row + x));
    }
  }
}

void mark_center(Candidate& cand, std::uint32_t idx, const Target& target,
                 i64 R, i64 W, i64 H) {
  const i64 cx = static_cast<i64>(idx % static_cast<std::uint32_t>(W));
  const i64 cy = static_cast<i64>(idx / static_cast<std::uint32_t>(W));
  for_diamond(cx, cy, R, W, H, [&](std::uint32_t p) {
    if (!target.known[p] || cand.cov[p] != 0)
      return;
    cand.cov[p] = 1;
    ++cand.covered;
  });
}

// 枚举一个平移下的全部密铺中心：anchor = (bb_x0,bb_y0)，格基
// e1=(R+1,R)、e2=(-R,R+1)，d 取遍 D（D 是格的完全剩余系）。
Candidate enumerate(const Target& target, i64 R, i64 W, i64 H, i64 bb_x0,
                    i64 bb_y0, i64 bb_x1, i64 bb_y1, i64 dx, i64 dy) {
  Candidate cand;
  cand.cov.assign(static_cast<std::size_t>(W * H), 0);

  const i64 N = 2 * R * R + 2 * R + 1;
  const i64 ex0 = bb_x0 - R, ex1 = bb_x1 + R;
  const i64 ey0 = bb_y0 - R, ey1 = bb_y1 + R;

  // E-d 的四角在 (u,v) 坐标下的包围盒，再各放宽 1 格。
  i64 lo_u = std::numeric_limits<i64>::max();
  i64 hi_u = std::numeric_limits<i64>::min();
  i64 lo_v = std::numeric_limits<i64>::max();
  i64 hi_v = std::numeric_limits<i64>::min();
  // 反变换的输入必须是相对 anchor 的坐标；之前这里直接用了绝对画布
  // 坐标，导致枚举范围整体平移，密铺中间会出现整片空洞。
  const i64 xs[2] = {ex0 - bb_x0 - dx, ex1 - bb_x0 - dx};
  const i64 ys[2] = {ey0 - bb_y0 - dy, ey1 - bb_y0 - dy};
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      const i64 rx = xs[i], ry = ys[j];
      const i64 u_num = (R + 1) * rx + R * ry;
      const i64 v_num = -R * rx + (R + 1) * ry;
      auto div_floor = [](i64 a, i64 b) {
        i64 q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0)))
          --q;
        return q;
      };
      auto div_ceil = [](i64 a, i64 b) {
        i64 q = a / b;
        if ((a % b != 0) && ((a < 0) == (b < 0)))
          ++q;
        return q;
      };
      lo_u = std::min(lo_u, div_floor(u_num, N));
      hi_u = std::max(hi_u, div_ceil(u_num, N));
      lo_v = std::min(lo_v, div_floor(v_num, N));
      hi_v = std::max(hi_v, div_ceil(v_num, N));
    }
  }
  lo_u -= 1;
  lo_v -= 1;
  hi_u += 1;
  hi_v += 1;

  for (i64 a = lo_u; a <= hi_u; ++a) {
    for (i64 b = lo_v; b <= hi_v; ++b) {
      const i64 rx = dx + (R + 1) * a - R * b;
      const i64 ry = dy + R * a + (R + 1) * b;
      const i64 x = bb_x0 + rx;
      const i64 y = bb_y0 + ry;
      if (x < ex0 || x > ex1 || y < ey0 || y > ey1)
        continue;
      if (x < 0 || x >= W || y < 0 || y >= H)
        continue;
      const std::uint32_t p = static_cast<std::uint32_t>(y * W + x);
      if (!target.known[p])
        continue;
      cand.centers.push_back(p);
    }
  }

  for (std::uint32_t p : cand.centers)
    mark_center(cand, p, target, R, W, H);
  return cand;
}

// 把中心按 bbox 划成 kx*ky 个近似方形的块，每块交给一个 slot。
// 优先用满更多块（覆盖更多 slot），块数相同时优先接近方形。
// 每个 slot 的列表按行蛇形排列，相邻两发只差一个格步。
void assign_blocks(SonarTiling& out, const std::vector<std::uint32_t>& centers,
                   i64 W, std::size_t slots) {
  out.slot_centers.assign(slots, {});
  if (centers.empty())
    return;

  const i64 bb_x0 = out.bb_x0, bb_y0 = out.bb_y0;
  const i64 bw = static_cast<i64>(out.bb_x1) - bb_x0 + 1;
  const i64 bh = static_cast<i64>(out.bb_y1) - bb_y0 + 1;

  std::size_t best_blocks = 0;
  i64 best_kx = 1, best_ky = 1;
  double best_aspect = std::numeric_limits<double>::max();
  const i64 kx_cap = std::min<i64>(static_cast<i64>(slots), bw);
  for (i64 kx = 1; kx <= kx_cap; ++kx) {
    i64 ky = static_cast<i64>(slots) / kx;
    if (ky > bh)
      ky = bh;
    if (ky <= 0)
      continue;
    const std::size_t blocks = static_cast<std::size_t>(kx * ky);
    const double aspect =
        std::fabs(std::log(static_cast<double>(kx) * bh /
                           (static_cast<double>(ky) * bw)));
    const bool better =
        blocks > best_blocks ||
        (blocks == best_blocks && aspect < best_aspect);
    if (better) {
      best_blocks = blocks;
      best_kx = kx;
      best_ky = ky;
      best_aspect = aspect;
    }
  }
  if (best_blocks == 0) {
    best_kx = best_ky = 1;
  }

  for (std::uint32_t p : centers) {
    const i64 x = static_cast<i64>(p % static_cast<std::uint32_t>(W));
    const i64 y = static_cast<i64>(p / static_cast<std::uint32_t>(W));
    i64 bx = (x - bb_x0) * best_kx / bw;
    i64 by = (y - bb_y0) * best_ky / bh;
    bx = std::clamp<i64>(bx, 0, best_kx - 1);
    by = std::clamp<i64>(by, 0, best_ky - 1);
    const std::size_t block =
        static_cast<std::size_t>(by * best_kx + bx);
    out.slot_centers[block % slots].push_back(p);
  }

  for (std::vector<std::uint32_t>& list : out.slot_centers) {
    std::sort(list.begin(), list.end(),
              [W](std::uint32_t a, std::uint32_t b) {
                const i64 ay = a / static_cast<std::uint32_t>(W);
                const i64 by = b / static_cast<std::uint32_t>(W);
                return ay != by ? ay < by : a < b;
              });
    for (std::size_t i = 0; i < list.size();) {
      std::size_t j = i;
      const i64 row = list[i] / static_cast<std::uint32_t>(W);
      while (j < list.size() &&
             list[j] / static_cast<std::uint32_t>(W) == row)
        ++j;
      if (((row - bb_y0) & 1) != 0)
        std::reverse(list.begin() + static_cast<std::ptrdiff_t>(i),
                     list.begin() + static_cast<std::ptrdiff_t>(j));
      i = j;
    }
  }
}

// 中心列表按画布行优先重排，repair 分块时相邻块在空间上连续。
void fill_all_centers(SonarTiling& out) {
  out.all_centers.clear();
  out.all_centers.reserve(out.is_center.size());
  for (std::size_t p = 0; p < out.is_center.size(); ++p)
    if (out.is_center[p])
      out.all_centers.push_back(static_cast<std::uint32_t>(p));
}

} // namespace

std::vector<std::vector<std::uint32_t>> partition_sonar_centers(
    const std::vector<std::uint32_t>& centers, std::size_t blocks) {
  std::vector<std::vector<std::uint32_t>> out(blocks);
  const std::size_t n = centers.size();
  if (blocks == 0 || n == 0)
    return out;

  if (blocks >= n) {
    // 槽位够：每个槽位认领一个中心；中心 i 会被连续的一段槽位共享。
    for (std::size_t s = 0; s < blocks; ++s)
      out[s].push_back(centers[(s * n) / blocks]);
    return out;
  }

  // 槽位不够：切成 blocks 个连续块，每块至少一个中心。
  for (std::size_t s = 0; s < blocks; ++s) {
    const std::size_t begin = (s * n) / blocks;
    const std::size_t end = ((s + 1) * n) / blocks;
    out[s].assign(centers.begin() + static_cast<std::ptrdiff_t>(begin),
                  centers.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return out;
}

SonarTiling build_sonar_tiling(const Target& target, std::uint32_t radius,
                               std::size_t slots) {
  SonarTiling out;
  out.W = target.W;
  out.H = target.H;
  out.radius = radius;
  out.slots = slots;
  out.bb_x0 = target.bb_x0;
  out.bb_y0 = target.bb_y0;
  out.bb_x1 = target.bb_x1;
  out.bb_y1 = target.bb_y1;

  const i64 W = target.W, H = target.H;
  if (!target.built || W <= 0 || H <= 0 || slots == 0)
    return out;

  const std::size_t n = static_cast<std::size_t>(W * H);
  out.is_center.assign(n, 0);
  out.slot_centers.resize(slots);
  for (std::size_t p = 0; p < n; ++p)
    if (target.known[p])
      ++out.known_total;
  if (out.known_total == 0)
    return out;

  if (radius == 0) {
    // 菱形退化成单点：每个 known 像素都必须自己发一发。
    std::vector<std::uint32_t> centers;
    centers.reserve(out.known_total);
    for (std::size_t p = 0; p < n; ++p) {
      if (!target.known[p])
        continue;
      out.is_center[p] = 1;
      centers.push_back(static_cast<std::uint32_t>(p));
    }
    out.tiling_centers = centers.size();
    out.covered_total = centers.size();
    assign_blocks(out, centers, W, slots);
    fill_all_centers(out);
    return out;
  }

  const i64 R = radius;
  const i64 bb_x0 = out.bb_x0, bb_y0 = out.bb_y0;
  const i64 bb_x1 = out.bb_x1, bb_y1 = out.bb_y1;

  // D 里的每个偏移都试一次；D 是格的完全剩余系，所以这就是所有不同的平移。
  Candidate best;
  bool have_best = false;
  for (i64 dy = -R; dy <= R; ++dy) {
    const i64 span = R - (dy < 0 ? -dy : dy);
    for (i64 dx = -span; dx <= span; ++dx) {
      Candidate cand = enumerate(target, R, W, H, bb_x0, bb_y0, bb_x1, bb_y1,
                                 dx, dy);
      const bool better =
          !have_best || cand.covered > best.covered ||
          (cand.covered == best.covered &&
           cand.centers.size() < best.centers.size());
      if (better) {
        best = std::move(cand);
        have_best = true;
      }
    }
  }
  if (!have_best)
    return out;

  std::vector<std::uint32_t>& centers = best.centers;
  std::vector<std::uint8_t>& cov = best.cov;
  out.tiling_centers = centers.size();
  for (std::uint32_t p : centers)
    out.is_center[p] = 1;

  // 边缘补射：唯一覆盖中心在 known/画布之外的目标像素，以自身为中心补一发。
  for (i64 y = bb_y0; y <= bb_y1; ++y) {
    for (i64 x = bb_x0; x <= bb_x1; ++x) {
      const std::uint32_t p = static_cast<std::uint32_t>(y * W + x);
      if (!target.known[p] || cov[p] != 0)
        continue;
      out.is_center[p] = 1;
      centers.push_back(p);
      ++out.extra_centers;
      mark_center(best, p, target, R, W, H);
    }
  }

  for (std::size_t p = 0; p < n; ++p)
    if (target.known[p] && best.cov[p] != 0)
      ++out.covered_total;

  assign_blocks(out, centers, W, slots);
  fill_all_centers(out);
  return out;
}

} // namespace paintboard
