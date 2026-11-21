#include "paintboard/engine.hpp"

#include "app/cli.hpp"
#include "net/public_client.hpp"
#include "net/transport.hpp"
#include "net/work_connection.hpp"
#include "scheduler/classic_scheduler.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace paintboard {
namespace {

void sleep_ms(std::uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool write_ppm(const std::string& path, const std::vector<std::uint8_t>& rgb,
               std::uint32_t width, std::uint32_t height) {
  if (path.empty() || rgb.size() !=
                          static_cast<std::size_t>(width) * height * 3) {
    return false;
  }
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  const std::size_t written =
      std::fwrite(rgb.data(), 1, rgb.size(), file);
  std::fclose(file);
  return written == rgb.size();
}

std::size_t group_index(Group group) {
  return static_cast<std::size_t>(group);
}

} // namespace

Engine::Engine(Config config) : config_(std::move(config)) {
  SlotId slot = 0;
  for (const TokenEntry& entry : config_.tokens) {
    painters_.push_back(std::make_unique<TokenPainter>(
        slot++, entry.group, entry.token, entry.follow));
  }
  for (const auto& painter : painters_)
    painter_ptrs_.push_back(painter.get());
}

Engine::~Engine() {
  request_stop();
  if (public_thread_.joinable())
    public_thread_.join();
  if (scheduler_)
    scheduler_->stop();
  if (work_)
    work_->close();
}

void Engine::request_stop() {
  stop_.store(true);
  status_.stop.store(true);
}

void Engine::start_public() {
  if (public_thread_.joinable())
    return;
  public_client_ = std::make_unique<PublicClient>(config_, board_, status_, log_);
  public_thread_ = std::thread([this] { public_client_->run(); });
}

int Engine::run() {
  if (config_.tokens.empty())
    config_.observe = true;

  if (config_.observe || config_.tokens.empty())
    start_public();

  int rc = 0;
  if (!config_.tokens.empty())
    rc = run_work();

  if (config_.tokens.empty()) {
    while (!stop_.load() && !status_.stop.load())
      sleep_ms(50);
  }

  finish();
  return rc;
}

int Engine::run_work() {
  work_ = std::make_unique<WorkConnection>(
      painter_ptrs_,
      std::make_unique<TcpTransport>(config_.host, config_.port, config_.proxy),
      config_, status_, log_);

  bool prepared = false;
  bool started = false;
  bool connected = false;
  int failures = 0;

  while (!stop_.load() && !status_.stop.load()) {
    try {
      if (!connected) {
        work_->connect();
        const Rules rules = work_->rules();
        board_.apply_rules(rules);
        log_.logf("[work] RULES {}x{} radius={} tps={} slots={}\n",
                  rules.width, rules.height, rules.radius, rules.tps,
                  rules.max_slots);

        if (!config_.dry_run && config_.image_path.empty() &&
            config_.draws.empty() && !config_.daemon) {
          log_.logf("[main] 没有目标图（--image/--draw），仅绑定后退出\n");
          config_.dry_run = true;
        }

        if (!config_.dry_run && !prepared) {
          scheduler_ = std::make_unique<ClassicScheduler>(SchedulerDeps{
              queue_, painter_ptrs_, board_, config_, status_, log_});
          scheduler_->prepare();
          prepared = true;
        }

        work_->request_all_binds();
        work_->flush();
        connected = true;
        failures = 0;
      }

      while (!stop_.load() && !status_.stop.load()) {
        if (scheduler_ && !started && work_->all_bound()) {
          scheduler_->on_all_bound();
          started = true;
        }

        auto event = work_->poll(std::chrono::milliseconds(50));
        if (event) {
          board_.apply_sonar(event->sonar);
          if (scheduler_)
            scheduler_->on_tick(*event);
          if (config_.dry_run) {
            log_.logf("[work] dry-run: first TICK received\n");
            return 0;
          }
        }

        if (scheduler_) {
          scheduler_->step(work_->now());
          work_->flush();
          if (config_.limit && status_.painted.load() >= config_.limit)
            return 0;
          if (scheduler_->done()) {
            log_.logf("[work] scheduler done painted={} limit={}\n",
                      status_.painted.load(), config_.limit);
            return 0;
          }
        } else {
          work_->flush();
        }
      }
      return 0;
    } catch (const std::exception& error) {
      ++failures;
      if (!stop_.load() && !status_.stop.load()) {
        log_.logf("[work] {} (retry {})\n", error.what(), failures);
        if (work_) {
          auto pending = work_->take_pending();
          if (scheduler_)
            scheduler_->on_disconnect(std::move(pending));
        }
        connected = false;
        started = false;
        sleep_ms(1000);
      }
    }
  }
  return 0;
}

void Engine::finish() {
  request_stop();
  if (public_client_)
    public_client_->request_stop();
  if (public_thread_.joinable())
    public_thread_.join();
  if (scheduler_)
    scheduler_->stop();
  if (work_)
    work_->close();

  if (!config_.sonar_out.empty()) {
    std::vector<std::uint8_t> rgb;
    const BoardView view = board_.view();
    if (board_.copy_sonar_rgb(rgb) &&
        write_ppm(config_.sonar_out, rgb, view.rules.width,
                  view.rules.height)) {
      log_.logf("[ppm] wrote {} ({}x{})\n", config_.sonar_out,
                view.rules.width, view.rules.height);
    } else {
      log_.logf("[ppm] cannot write sonar {}\n", config_.sonar_out);
    }
  }

  if (!config_.board_out.empty() && config_.observe) {
    std::vector<std::uint8_t> rgb;
    const BoardView view = board_.view();
    if (board_.copy_canvas_rgb(rgb) &&
        write_ppm(config_.board_out, rgb, view.rules.width,
                  view.rules.height)) {
      log_.logf("[ppm] wrote {} ({}x{})\n", config_.board_out,
                view.rules.width, view.rules.height);
    } else {
      log_.logf("[ppm] cannot write board {}\n", config_.board_out);
    }
  }

  const SchedulerStats stats =
      scheduler_ ? scheduler_->stats() : SchedulerStats{};
  log_.logf(
      "[main] painted={} passes={} ok[paint={} repair={} sonar={}] "
      "sent[paint={} repair={} sonar={}] {}\n",
      status_.painted.load(),
      stats.passes ? stats.passes : status_.passes.load(),
      status_.ok[group_index(Group::Paint)].load(),
      status_.ok[group_index(Group::Repair)].load(),
      status_.ok[group_index(Group::Sonar)].load(),
      status_.sent[group_index(Group::Paint)].load(),
      status_.sent[group_index(Group::Repair)].load(),
      status_.sent[group_index(Group::Sonar)].load(),
      status_.abort.load() ? "aborted" : (config_.dry_run ? "dry-run" : "done"));
}

} // namespace paintboard
