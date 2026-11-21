#include "net/public_client.hpp"

#include "net/transport.hpp"
#include "protocol/wire.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace paintboard {

PublicClient::PublicClient(const Config& config, Board& board,
                           RuntimeStatus& status, Logger& log)
    : config_(config), board_(board), status_(status), log_(log) {}

void PublicClient::request_stop() { stop_.store(true); }

void PublicClient::run() {
  while (!stop_.load() && !status_.stop.load() && !status_.abort.load()) {
    try {
      TcpTransport transport(config_.host, config_.port, config_.proxy);
      std::string path = "/ws?public=1";
      const Tick since = board_.since();
      if (since != 0) path += "&since=" + std::to_string(since);
      const std::string url =
          "wss://" + config_.host + ":" + config_.port + path;
      transport.connect(url);
      log_.logf("[pub] connected {}", path);

      while (!stop_.load() && !status_.stop.load() && !status_.abort.load()) {
        auto payload = transport.recv(std::chrono::milliseconds(200));
        if (!payload) continue;
        if (payload->empty()) continue;

        switch ((*payload)[0]) {
          case 0x84:
            board_.apply_rules(protocol::parse_rules(*payload));
            break;
          case 0x83:
            if (!board_.apply_snapshot(*payload)) {
              log_.logf("[pub] bad snapshot");
            }
            break;
          case 0x82:
            if (!board_.apply_public(*payload)) {
              log_.logf("[pub] public version mismatch; resync");
              break;
            }
            continue;
          default:
            // 未知类型必须忽略，前向兼容。
            break;
        }
        if ((*payload)[0] == 0x82) break;
      }
    } catch (const std::exception& e) {
      if (!stop_.load() && !status_.stop.load() && !status_.abort.load()) {
        log_.logf("[pub] {}", e.what());
      }
    }

    if (!stop_.load() && !status_.stop.load() && !status_.abort.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

} // namespace paintboard
