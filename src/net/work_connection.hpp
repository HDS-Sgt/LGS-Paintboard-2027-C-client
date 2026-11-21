#pragma once

#include "paintboard/config.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/types.hpp"
#include "net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace paintboard {

// 一条 public=0 的工作连接：拥有 socket、绑定/绘画的发送顺序、TICK 的路由。
// 它不知道队列策略，也不知道哪些像素还没画；只把结果送回各自的 TokenPainter。
class WorkConnection {
 public:
  WorkConnection(std::vector<IPainter*> painters,
                 std::unique_ptr<ITransport> transport, const Config& config,
                 RuntimeStatus& status, Logger& log);

  void connect();
  void request_all_binds();
  void flush();
  std::optional<TickEvent> poll(std::chrono::milliseconds timeout);
  void close();

  Rules rules() const;
  bool all_bound() const;

  // 断线时把已发出但没等到结果的绘画交回调度器；bind 不计入。
  std::vector<PaintOp> take_pending();

 private:
  struct Pend {
    bool bind = false;
    SlotId slot = 0;
    PaintOp op;
  };

  IPainter* find(SlotId slot) const;
  void handle_tick(TickEvent& event);
  static std::size_t group_index(Group group);

  std::vector<IPainter*> painters_;
  std::unique_ptr<ITransport> transport_;
  Config config_;
  RuntimeStatus& status_;
  Logger& log_;

  Rules rules_{};
  ServerClock clock_;
  std::deque<Pend> pend_;
  std::set<SlotId> bind_pending_;
};

} // namespace paintboard
