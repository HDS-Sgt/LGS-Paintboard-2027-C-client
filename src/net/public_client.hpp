#pragma once

#include "paintboard/board.hpp"
#include "paintboard/config.hpp"

#include <atomic>

namespace paintboard {

class PublicClient {
 public:
  PublicClient(const Config& config, Board& board, RuntimeStatus& status,
               Logger& log);

  void run();
  void request_stop();

 private:
  const Config& config_;
  Board& board_;
  RuntimeStatus& status_;
  Logger& log_;
  std::atomic<bool> stop_{false};
};

} // namespace paintboard
