#pragma once

#include "paintboard/types.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace paintboard {

struct SlotView {
  SlotId id = 0;
  Group group = Group::Paint;
  bool follow = false;
  bool bound = false;
  bool want_bind = false;
  std::uint32_t inflight = 0;
  bool has_brush = false;
  std::uint32_t brush_idx = 0;
  Tick brush_time = 0;
  bool has_projected = false;
  std::uint32_t projected_idx = 0;
  Tick projected_time = 0;
  Tick not_before = 0;
};

// 一个逻辑画笔句柄。它只描述“这条连接上的这个槽位现在处于什么状态”，
// 不拥有 socket、不读 TICK、不做队列策略；这些都属于 WorkConnection/调度器。
class IPainter {
 public:
  virtual ~IPainter() = default;

  virtual SlotId id() const = 0;
  virtual Group group() const = 0;
  virtual Token token() const = 0;
  virtual SlotView view() const = 0;

  virtual bool can_accept(Tick now) const = 0;
  virtual bool ready_for(std::uint32_t idx, Tick now, std::uint64_t cooldown_base,
                         std::uint64_t cooldown_per_px,
                         std::uint32_t width) const = 0;
  virtual bool submit(const PaintOp& op, Tick now) = 0;

  // 连接层回调：由 WorkConnection 按 TICK/断线顺序调用。
  virtual void request_bind() = 0;
  virtual bool wants_bind() const = 0;
  virtual void on_bind_result(bool ok) = 0;
  virtual void on_paint_result(const PaintOp& op, PaintStatus status,
                               std::uint64_t value, Tick now) = 0;
  virtual void on_brush(std::uint32_t idx, Tick t) = 0;
  virtual void on_lost() = 0;
  // flush 先取快照发送，成功后再清空；发送失败时 outbox 保持原样，避免丢 op。
  virtual std::vector<PaintOp> outbox_snapshot() const = 0;
  virtual std::vector<PaintOp> take_outbox() = 0;
};

class TokenPainter final : public IPainter {
 public:
  TokenPainter(SlotId id, Group group, Token token, bool follow = false);

  SlotId id() const override;
  Group group() const override;
  Token token() const override;
  SlotView view() const override;

  bool can_accept(Tick now) const override;
  bool ready_for(std::uint32_t idx, Tick now, std::uint64_t cooldown_base,
                 std::uint64_t cooldown_per_px,
                 std::uint32_t width) const override;
  bool submit(const PaintOp& op, Tick now) override;

  void request_bind();
  bool wants_bind() const;
  void on_bind_result(bool ok);

  // 结果、画笔与断线都由 WorkConnection 按服务器顺序喂进来。
  void on_paint_result(const PaintOp& op, PaintStatus status, std::uint64_t value,
                       Tick now);
  void on_brush(std::uint32_t idx, Tick t);
  void on_lost();

  std::vector<PaintOp> outbox_snapshot() const;
  std::vector<PaintOp> take_outbox();

 private:
  const SlotId id_;
  const Group group_;
  const Token token_;
  const bool follow_;

  mutable std::mutex mu_;
  bool bound_ = false;
  bool want_bind_ = true;
  std::uint32_t inflight_ = 0;

  // 服务器在 TICK.brush 里给出的权威画笔位置。
  bool has_brush_ = false;
  std::uint32_t brush_idx_ = 0;
  Tick brush_time_ = 0;

  // 客户端对“下一笔落点”的投影；冷却时回退到权威值。
  bool has_projected_ = false;
  std::uint32_t projected_idx_ = 0;
  Tick projected_time_ = 0;
  Tick not_before_ = 0;

  std::vector<PaintOp> outbox_;
};

} // namespace paintboard
