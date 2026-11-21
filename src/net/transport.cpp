#include "net/transport.hpp"

#include "protocol/wire.hpp"

#include "websock.hpp"

#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace paintboard {
namespace {

struct Url {
  std::string host;
  std::string port;
  std::string path;
};

Url parse_url(const std::string& url, const std::string& fallback_host,
              const std::string& fallback_port) {
  Url out{fallback_host, fallback_port, "/"};
  std::string rest = url;
  const std::string prefix = "wss://";
  if (rest.rfind(prefix, 0) == 0) {
    rest = rest.substr(prefix.size());
  } else if (rest.rfind("https://", 0) == 0) {
    rest = rest.substr(8);
  }

  const std::size_t slash = rest.find('/');
  std::string authority = rest.substr(0, slash);
  if (slash != std::string::npos) out.path = rest.substr(slash);
  if (authority.empty()) return out;

  const std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    out.host = authority.substr(0, colon);
    out.port = authority.substr(colon + 1);
  } else {
    out.host = authority;
  }
  return out;
}

} // namespace

struct TcpTransport::Impl {
  std::string host;
  std::string port;
  std::string proxy;
  std::unique_ptr<tcSocket> socket;
};

TcpTransport::TcpTransport(std::string host, std::string port, std::string proxy)
    : impl_(std::make_unique<Impl>()) {
  impl_->host = std::move(host);
  impl_->port = std::move(port);
  impl_->proxy = std::move(proxy);
}

TcpTransport::~TcpTransport() = default;

void TcpTransport::connect(const std::string& url) {
  close();
  if (!impl_->proxy.empty()) {
    setenv("HTTPS_PROXY", impl_->proxy.c_str(), 1);
  }
  const Url target = parse_url(url, impl_->host, impl_->port);
  impl_->socket = std::make_unique<tcSocket>();
  impl_->socket->connect(target.host, target.port, target.path);
}

void TcpTransport::send(std::span<const std::uint8_t> payload) {
  if (!impl_->socket) throw std::runtime_error("transport not connected");
  const auto wire = protocol::zstd_encode(payload);
  impl_->socket->sendbin(wire);
}

std::optional<std::vector<std::uint8_t>> TcpTransport::recv(
    std::chrono::milliseconds timeout) {
  if (!impl_->socket) throw std::runtime_error("transport not connected");

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    stream frame;
    const int got = impl_->socket->try_recvbin(frame);
    if (got == 1) {
      return protocol::zstd_decode_one(frame);
    }
    if (got == 2) {
      throw std::runtime_error("websocket closed by peer");
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void TcpTransport::close() {
  if (impl_) impl_->socket.reset();
}

} // namespace paintboard
