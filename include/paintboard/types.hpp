#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace paintboard {

using SlotId = std::uint32_t;
using Tick = std::uint64_t;
using Token = std::array<std::uint8_t, 16>;

enum class Group : std::uint8_t { Paint, Repair, Sonar };
enum class PaintStatus : std::uint8_t { Ok, Cooling, SlotInvalid, Error };

struct PaintOp {
  std::uint64_t id = 0;
  std::uint32_t idx = 0;
  std::uint32_t color = 0;
  Group group = Group::Paint;
  Tick due = 0;
  std::uint64_t seq = 0;
  std::uint32_t priority = 0;
  std::optional<SlotId> affinity;
};

struct PaintResult {
  PaintOp op;
  PaintStatus status = PaintStatus::Error;
  std::uint64_t value = 0;
};

using TickResult = PaintResult;

struct SonarBatch {
  std::uint16_t radius = 0;
  Tick tick = 0;
  std::vector<std::uint32_t> center;
  std::vector<std::uint32_t> color;
};

struct BrushUpdate {
  SlotId slot = 0;
  std::uint32_t idx = 0;
  Tick t = 0;
};

struct TickEvent {
  Tick tick = 0;
  std::vector<TickResult> results;
  std::vector<SlotId> lost;
  std::vector<BrushUpdate> brushes;
  SonarBatch sonar;
};

struct Bind {
  SlotId slot = 0;
  Token token{};
};

struct Paint {
  SlotId slot = 0;
  std::uint32_t idx = 0;
  std::uint32_t color = 0;
};

struct Rules {
  std::uint32_t width = 0, height = 0;
  std::uint16_t radius = 0;
  std::uint64_t tps = 0;
  std::uint64_t cooldown_base = 0, cooldown_per_px = 0;
  std::uint32_t max_slots = 0;
  std::uint32_t max_message_bytes = 0;
  std::uint32_t max_paints_per_message = 0;
  std::uint64_t server_time = 0; // 服务器 UNIX 微秒；协议回落时用于校准
};

struct TokenEntry {
  Group group = Group::Paint;
  Token token{};
  bool follow = false;
};

struct Draw {
  std::uint32_t x = 0, y = 0;
  std::uint32_t color = 0; // R | G<<8 | B<<16 | 0xFF000000
};

} // namespace paintboard
