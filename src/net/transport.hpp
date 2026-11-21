#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace paintboard {

// 一条完整的 WebSocket 二进制消息，已解压为协议载荷。
class ITransport {
 public:
  virtual ~ITransport() = default;
  virtual void connect(const std::string& url) = 0;
  virtual void send(std::span<const std::uint8_t> payload) = 0;
  virtual std::optional<std::vector<std::uint8_t>> recv(
      std::chrono::milliseconds timeout) = 0;
  virtual void close() = 0;
};

class TcpTransport final : public ITransport {
 public:
  TcpTransport(std::string host, std::string port, std::string proxy);
  ~TcpTransport() override;

  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  void connect(const std::string& url) override;
  void send(std::span<const std::uint8_t> payload) override;
  std::optional<std::vector<std::uint8_t>> recv(
      std::chrono::milliseconds timeout) override;
  void close() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace paintboard
