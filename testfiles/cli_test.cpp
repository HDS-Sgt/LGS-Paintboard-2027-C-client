#include "app/cli.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace paintboard;

namespace {

CliResult parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args)
    argv.push_back(arg.data());
  return parse_cli(static_cast<int>(argv.size()), argv.data());
}

void test_draw_and_flags() {
  CliResult result = parse({"paintboard_new", "--tokens", "/nonexistent",
                            "--draw", "1,2,FF0000", "--limit", "10",
                            "--paint-loop", "2", "--observe"});
  assert(result.ok);
  assert(result.config.draws.size() == 1);
  assert(result.config.draws[0].x == 1);
  assert(result.config.draws[0].y == 2);
  assert(result.config.draws[0].color == 0xFF0000FFu);
  assert(result.config.limit == 10);
  assert(result.config.paint_loop_ms == 2000);
  assert(result.config.observe);
}

void test_unknown_and_help() {
  CliResult unknown = parse({"paintboard_new", "--not-an-option"});
  assert(!unknown.ok);
  assert(!unknown.error.empty());

  CliResult help = parse({"paintboard_new", "--help"});
  assert(help.ok);
  assert(help.help);
}

} // namespace

int main() {
  test_draw_and_flags();
  test_unknown_and_help();
  std::puts("cli_test OK");
  return 0;
}
