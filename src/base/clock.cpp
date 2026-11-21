#include "paintboard/config.hpp"

#include <chrono>

namespace paintboard {

Tick mono_us() {
  using namespace std::chrono;
  return static_cast<Tick>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch())
          .count());
}

void ServerClock::calibrate(Tick server_us) {
  base_mono_.store(mono_us());
  base_us_.store(server_us);
}

Tick ServerClock::now() const {
  const Tick base = base_mono_.load();
  if (base == 0)
    return 0;
  return base_us_.load() + (mono_us() - base);
}

} // namespace paintboard
