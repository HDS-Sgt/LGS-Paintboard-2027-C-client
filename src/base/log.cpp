#include "paintboard/config.hpp"

#include <iostream>

namespace paintboard {

void Logger::line(const std::string& text) {
  std::lock_guard<std::mutex> lock(mu_);
  std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
  std::cout.flush();
}

} // namespace paintboard
