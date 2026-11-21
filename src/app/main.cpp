#include "app/cli.hpp"
#include "paintboard/engine.hpp"

#include <csignal>
#include <cstdio>
#include <utility>

namespace {

paintboard::Engine* g_engine = nullptr;

extern "C" void handle_stop_signal(int) {
  if (g_engine)
    g_engine->request_stop();
}

} // namespace

int main(int argc, char** argv) {
  paintboard::CliResult parsed = paintboard::parse_cli(argc, argv);
  if (!parsed.ok) {
    std::fprintf(stderr, "%s\n", parsed.error.c_str());
    paintboard::print_usage(argv[0]);
    return 2;
  }
  if (parsed.help) {
    paintboard::print_usage(argv[0]);
    return 0;
  }

  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);

  paintboard::Engine engine(std::move(parsed.config));
  g_engine = &engine;
  const int rc = engine.run();
  g_engine = nullptr;
  return rc;
}
