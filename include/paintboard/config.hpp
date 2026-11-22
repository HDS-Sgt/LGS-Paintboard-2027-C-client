#pragma once

#include "paintboard/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace paintboard {

enum class SchedulerKind { Classic, NewSch1122 };

struct Config {
  std::vector<TokenEntry> tokens;
  std::string image_path;
  std::vector<Draw> draws;

  std::string host = "pb.ika.zip";
  std::string port = "443";
  std::string proxy;

  std::int32_t offset_x = 0, offset_y = 0;
  std::uint32_t exec_ms = 20;
  std::uint32_t paint_ms = 250, repair_ms = 500, sonar_ms = 800;
  std::uint32_t round_cap = 256, queue_cap = 2048;
  std::uint64_t repair_verify_ms = 60000;
  std::uint32_t repair_thresh = 96, sonar_stride = 5;
  std::uint32_t paint_loop_ms = 0;
  std::uint32_t repair_follow = 2;
  std::uint64_t early_us = 0;

  SchedulerKind scheduler = SchedulerKind::Classic;
  bool observe = false, daemon = false, dry_run = false;
  std::uint64_t limit = 0;
  std::string sonar_out, board_out;
};

struct RuntimeStatus {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> abort{false};

  std::array<std::atomic<std::uint64_t>, 3> ok{};
  std::array<std::atomic<std::uint64_t>, 3> sent{};
  std::array<std::atomic<std::uint32_t>, 3> slots{};

  std::atomic<std::uint64_t> painted{0};
  std::atomic<std::uint64_t> passes{0};
  std::atomic<std::uint64_t> last_tick{0};
  std::atomic<std::uint64_t> help_sent{0};
  std::atomic<std::uint64_t> drop_np{0};
  std::atomic<std::uint64_t> live_np{0};
};

inline void format_into(std::ostringstream& out, const char* fmt) {
  out << fmt;
}

template <class T, class... Rest>
void format_into(std::ostringstream& out, const char* fmt, const T& value,
                 const Rest&... rest) {
  const char* hole = std::strstr(fmt, "{}");
  if (!hole) {
    out << fmt;
    return;
  }
  out.write(fmt, hole - fmt);
  out << value;
  format_into(out, hole + 2, rest...);
}

class Logger {
 public:
  void line(const std::string& text);

  template <class... Args>
  void logf(const char* fmt, const Args&... args) {
    std::ostringstream out;
    format_into(out, fmt, args...);
    line(out.str());
  }

 private:
  std::mutex mu_;
};

Tick mono_us();

class ServerClock {
 public:
  void calibrate(Tick server_us);
  Tick now() const;

 private:
  std::atomic<Tick> base_us_{0};
  std::atomic<Tick> base_mono_{0};
};

} // namespace paintboard
