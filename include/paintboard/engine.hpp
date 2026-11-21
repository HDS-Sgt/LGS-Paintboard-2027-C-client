#pragma once

#include "paintboard/board.hpp"
#include "paintboard/config.hpp"
#include "paintboard/paint_queue.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/scheduler.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace paintboard {

class PublicClient;
class WorkConnection;

class Engine {
 public:
  explicit Engine(Config config);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  int run();
  void request_stop();

 private:
  void start_public();
  int run_work();
  void finish();

  Config config_;
  RuntimeStatus status_;
  Logger log_;
  Board board_;
  PaintQueue queue_;
  std::vector<std::unique_ptr<TokenPainter>> painters_;
  std::vector<IPainter*> painter_ptrs_;
  std::unique_ptr<WorkConnection> work_;
  std::unique_ptr<IScheduler> scheduler_;
  std::unique_ptr<PublicClient> public_client_;
  std::thread public_thread_;
  std::atomic<bool> stop_{false};
};

} // namespace paintboard
