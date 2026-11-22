#pragma once

#include "paintboard/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace paintboard {

// 声纳密铺分块（NewSch1122 专用，ClassicScheduler 不用）。
//
// 半径为 R 的声纳菱形 D={|dx|+|dy|<=R} 共 2R^2+2R+1 格。取高斯整数
// π=(R+1)+Ri（|π|^2=R^2+(R+1)^2=2R^2+2R+1=|D|），D 是格 πZ[i] 的
// 基本域：{D + πZ[i]} 恰好密铺整张平面，每格被覆盖一次。
//
// 声纳那一笔会把中心像素画成目标色，所以中心必须是 target.known 且在
// 画布内；这会让贴着目标边缘、唯一覆盖中心落在 known/画布之外的像素
// 漏掉。这里对漏掉的像素补一发“以该像素自身为中心”的声纳，保证每个
// known 像素每轮至少被观测一次。
struct SonarTiling {
  std::uint32_t W = 0, H = 0;
  std::uint32_t radius = 0;
  std::size_t slots = 0;
  std::uint32_t bb_x0 = 0, bb_y0 = 0, bb_x1 = 0, bb_y1 = 0;

  // W*H；标记哪些像素被选作声纳中心。
  std::vector<std::uint8_t> is_center;
  // 每个 slot 负责的块内中心，已按行蛇形排序（相邻发次只差一个格步）。
  std::vector<std::vector<std::uint32_t>> slot_centers;
  // 全部中心，按画布行优先排序；repair 匀配用它切块。
  std::vector<std::uint32_t> all_centers;

  std::size_t known_total = 0;    // 目标 known 像素总数
  std::size_t covered_total = 0;  // 被至少一发菱形覆盖的 known 像素数
  std::size_t tiling_centers = 0; // 密铺选出的中心数
  std::size_t extra_centers = 0;  // 边缘补射中心数
};

SonarTiling build_sonar_tiling(const Target& target, std::uint32_t radius,
                               std::size_t slots);

// 把声纳中心匀配给 B 个 repair 槽位，返回 B 个列表：
// - B >= centers.size()：每个槽位分到 1 个中心，多个槽位可以共享同一个
//   声纳点（“够就多配”）；
// - B < centers.size()：按 all_centers 的顺序切 B 个连续块，每槽一块
//   （“不够就分块”）。
// 每个列表内保持 centers 的相对顺序。
std::vector<std::vector<std::uint32_t>> partition_sonar_centers(
    const std::vector<std::uint32_t>& centers, std::size_t blocks);

} // namespace paintboard
