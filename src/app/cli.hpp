#pragma once

#include "paintboard/config.hpp"

#include <string>

namespace paintboard {

struct CliResult {
  Config config;
  bool ok = false;
  bool help = false;
  std::string error;
};

CliResult parse_cli(int argc, char** argv);

void print_usage(const char* program);

} // namespace paintboard
