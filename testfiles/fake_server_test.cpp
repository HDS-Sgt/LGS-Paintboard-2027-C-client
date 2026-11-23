// 本地假服务端 v1：只响应客户端，不注入竞争者。
//
// 目标：
//   1. 提供一个真实 TLS + WebSocket + zstd 的 /ws?public=0 服务端；
//   2. 严格校验 BATCH，维护共享画布、每 uid 画笔、冷却和槽位绑定；
//   3. 用 NewSch1122 跑完整小目标，最终服务端画布必须逐像素等于目标图。
//
// 这不是完整后端：没有 HTTP API、SNAPSHOT/PUBLIC、断线注入或竞争者。
// 证书由 `make test-fake-server` 生成，测试进程通过 SSL_CERT_FILE 信任它。
#include "paintboard/config.hpp"
#include "paintboard/engine.hpp"
#include "paintboard/types.hpp"
#include "protocol/wire.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace paintboard;

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

std::size_t pad4(std::size_t n) { return (n + 3) & ~std::size_t(3); }

std::uint16_t rd16(std::span<const std::uint8_t> p, std::size_t off) {
  return static_cast<std::uint16_t>(p[off]) |
         (static_cast<std::uint16_t>(p[off + 1]) << 8);
}

std::uint32_t rd32(std::span<const std::uint8_t> p, std::size_t off) {
  return static_cast<std::uint32_t>(p[off]) |
         (static_cast<std::uint32_t>(p[off + 1]) << 8) |
         (static_cast<std::uint32_t>(p[off + 2]) << 16) |
         (static_cast<std::uint32_t>(p[off + 3]) << 24);
}

void put16(std::vector<std::uint8_t>& out, std::size_t off,
           std::uint32_t value) {
  out[off] = static_cast<std::uint8_t>(value & 255);
  out[off + 1] = static_cast<std::uint8_t>((value >> 8) & 255);
}

void put32(std::vector<std::uint8_t>& out, std::size_t off,
           std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    out[off + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 255);
}

void put64(std::vector<std::uint8_t>& out, std::size_t off,
           std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    out[off + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 255);
}

std::string lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(std::string s) {
  const std::size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const std::size_t last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

std::string ssl_error_text() {
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return "unknown TLS error";
  char buf[256];
  ERR_error_string_n(code, buf, sizeof(buf));
  return buf;
}

struct ServerConfig {
  std::uint16_t width = 16;
  std::uint16_t height = 16;
  std::uint16_t radius = 2;
  std::uint64_t tps = 128;
  std::uint64_t cooldown_base = 2000;
  std::uint64_t cooldown_per_px = 100;
  std::uint32_t max_slots = 16;
  std::uint32_t max_paints = 1024;
  std::string cert = "testfiles/fake_server_cert.pem";
  std::string key = "testfiles/fake_server_key.pem";
};

class FakeServer {
 public:
  explicit FakeServer(ServerConfig config)
      : config_(std::move(config)),
        slots_(config_.max_slots),
        canvas_(static_cast<std::size_t>(config_.width) * config_.height, 0) {
    base_mono_ = std::chrono::steady_clock::now();
    base_us_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  ~FakeServer() { stop(); }

  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;

  bool start(std::string& error) {
    context_ = SSL_CTX_new(TLS_server_method());
    if (!context_) {
      error = "SSL_CTX_new: " + ssl_error_text();
      return false;
    }
    SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(context_, config_.cert.c_str()) !=
        1) {
      error = "load certificate: " + ssl_error_text();
      return false;
    }
    if (SSL_CTX_use_PrivateKey_file(context_, config_.key.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
      error = "load private key: " + ssl_error_text();
      return false;
    }
    if (SSL_CTX_check_private_key(context_) != 1) {
      error = "private key does not match certificate";
      return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      error = "socket failed";
      return false;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
      error = "bind failed";
      return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) !=
        0) {
      error = "getsockname failed";
      return false;
    }
    port_ = ntohs(addr.sin_port);
    if (::listen(listen_fd_, 4) != 0) {
      error = "listen failed";
      return false;
    }

    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    stop_.store(true);
    const int fd = client_fd_.exchange(-1);
    if (fd >= 0)
      ::shutdown(fd, SHUT_RDWR);
    if (thread_.joinable())
      thread_.join();
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (context_) {
      SSL_CTX_free(context_);
      context_ = nullptr;
    }
  }

  std::uint16_t port() const { return port_; }

  std::string error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
  }

  bool has_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !error_.empty();
  }

  std::vector<std::uint32_t> canvas() const {
    std::lock_guard<std::mutex> lock(mu_);
    return canvas_;
  }

  std::uint64_t bind_ok() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bind_ok_;
  }
  std::uint64_t ok_paints() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ok_paints_;
  }
  std::uint64_t cooling() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cooling_;
  }
  std::uint64_t invalid_paints() const {
    std::lock_guard<std::mutex> lock(mu_);
    return invalid_paints_;
  }
  std::uint64_t batches() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_;
  }
  std::uint64_t ticks() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ticks_;
  }

  // 直接调用状态机（不经过 socket），供确定性单测使用。
  bool process_batch(std::span<const std::uint8_t> payload, TickEvent& out,
                     std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    return process_batch_locked(payload, out, error);
  }

  std::vector<std::uint8_t> encode_tick(const TickEvent& event) const {
    const std::uint64_t cells =
        2ull * event.sonar.radius * event.sonar.radius +
        2ull * event.sonar.radius + 1;
    const std::size_t nr = event.results.size();
    const std::size_t nb = event.brushes.size();
    const std::size_t nk = event.sonar.center.size();
    const std::size_t ns = event.lost.size();
    const std::size_t total =
        32 + 12 * nb + pad4(2 * nb) + pad4(nr) + 4 * nr + 4 * nk +
        4 * nk * static_cast<std::size_t>(cells) + pad4(2 * ns);

    std::vector<std::uint8_t> out(total, 0);
    out[0] = 0x81;
    out[1] = 1;
    put16(out, 2, event.sonar.radius);
    put32(out, 4, static_cast<std::uint32_t>(nr));
    put64(out, 8, event.tick);
    put32(out, 16, static_cast<std::uint32_t>(nb));
    put32(out, 20, static_cast<std::uint32_t>(nk));
    put32(out, 24, static_cast<std::uint32_t>(ns));
    put32(out, 28, 0);

    const std::size_t bt = 32;
    const std::size_t bi = bt + 8 * nb;
    const std::size_t bs = bi + 4 * nb;
    const std::size_t st = bs + pad4(2 * nb);
    const std::size_t va = st + pad4(nr);
    const std::size_t si = va + 4 * nr;
    const std::size_t sc = si + 4 * nk;
    const std::size_t ls = sc + 4 * nk * static_cast<std::size_t>(cells);

    for (std::size_t i = 0; i < nb; ++i) {
      put64(out, bt + 8 * i, event.brushes[i].t);
      put32(out, bi + 4 * i, event.brushes[i].idx);
      put16(out, bs + 2 * i, event.brushes[i].slot);
    }
    for (std::size_t i = 0; i < nr; ++i) {
      out[st + i] = static_cast<std::uint8_t>(event.results[i].status);
      put32(out, va + 4 * i,
            static_cast<std::uint32_t>(
                std::min<std::uint64_t>(event.results[i].value, 0xFFFFFFFFu)));
    }
    for (std::size_t i = 0; i < nk; ++i)
      put32(out, si + 4 * i, event.sonar.center[i]);
    for (std::size_t i = 0; i < event.sonar.color.size(); ++i)
      put32(out, sc + 4 * i, event.sonar.color[i]);
    for (std::size_t i = 0; i < ns; ++i)
      put16(out, ls + 2 * i, event.lost[i]);
    return out;
  }

  std::vector<std::uint8_t> encode_rules() const {
    std::vector<std::uint8_t> out(96, 0);
    out[0] = 0x84;
    out[1] = 1;
    put16(out, 2, 0x20);
    put16(out, 4, config_.width);
    put16(out, 6, config_.height);
    put16(out, 8, config_.radius);
    put32(out, 12, static_cast<std::uint32_t>(config_.tps));
    put64(out, 16, config_.cooldown_base);
    put64(out, 24, config_.cooldown_per_px);
    put64(out, 32, 60000);
    put64(out, 40, 1000);
    put32(out, 64, config_.max_slots);
    put32(out, 68, config_.max_paints);
    put32(out, 72, 128);
    put32(out, 76, 128);
    put32(out, 80, 1u << 20);
    put32(out, 84, 1u << 20);
    put64(out, 88, now_us());
    return out;
  }

 private:
  struct Binding {
    bool used = false;
    Token token{};
    std::uint64_t uid = 0;
  };

  struct Brush {
    bool has = false;
    std::uint32_t idx = 0;
    std::uint64_t t = 0;
  };

  static std::uint64_t uid_for(const Token& token) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint8_t byte : token) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
  }

  static bool zero_token(const Token& token) {
    for (std::uint8_t byte : token)
      if (byte != 0)
        return false;
    return true;
  }

  std::uint64_t now_us() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - base_mono_);
    return base_us_ + static_cast<std::uint64_t>(elapsed.count());
  }

  void set_error_locked(const std::string& text) {
    if (error_.empty())
      error_ = text;
  }

  bool process_batch_locked(std::span<const std::uint8_t> payload,
                            TickEvent& out, std::string& error) {
    auto fail = [&](const std::string& text) {
      error = text;
      return false;
    };

    if (payload.size() < 16)
      return fail("BATCH shorter than 16-byte header");
    if (payload[0] != 0x01 || payload[1] != 1)
      return fail("BATCH type/version mismatch");
    if (rd16(payload, 2) != 0)
      return fail("BATCH reserved u16 not zero");
    const std::uint32_t nb = rd32(payload, 4);
    const std::uint32_t np = rd32(payload, 8);
    if (rd32(payload, 12) != 0)
      return fail("BATCH reserved u32 not zero");
    if (nb > config_.max_slots || np > config_.max_paints)
      return fail("BATCH count exceeds RULES limits");

    const std::size_t slot_end = 16 + 2 * static_cast<std::size_t>(nb);
    const std::size_t token_start = pad4(slot_end);
    const std::size_t token_end = token_start + 16 * static_cast<std::size_t>(nb);
    const std::size_t paint_slot_end =
        token_end + 2 * static_cast<std::size_t>(np);
    const std::size_t idx_start = pad4(paint_slot_end);
    const std::size_t idx_end = idx_start + 4 * static_cast<std::size_t>(np);
    const std::size_t color_start = idx_end;
    const std::size_t color_end = color_start + 4 * static_cast<std::size_t>(np);
    if (color_end != payload.size())
      return fail("BATCH length mismatch");

    for (std::size_t i = slot_end; i < token_start; ++i)
      if (payload[i] != 0)
        return fail("BATCH bind.slot padding not zero");
    for (std::size_t i = paint_slot_end; i < idx_start; ++i)
      if (payload[i] != 0)
        return fail("BATCH paint.slot padding not zero");

    std::vector<Bind> binds;
    binds.reserve(nb);
    for (std::uint32_t i = 0; i < nb; ++i) {
      Bind bind;
      bind.slot = rd16(payload, 16 + 2 * i);
      if (bind.slot >= config_.max_slots)
        return fail("BATCH bind.slot out of range");
      std::memcpy(bind.token.data(), payload.data() + token_start + 16 * i, 16);
      binds.push_back(bind);
    }

    std::vector<Paint> paints;
    paints.reserve(np);
    for (std::uint32_t i = 0; i < np; ++i) {
      Paint paint;
      paint.slot = rd16(payload, token_end + 2 * i);
      if (paint.slot >= config_.max_slots)
        return fail("BATCH paint.slot out of range");
      paint.idx = rd32(payload, idx_start + 4 * i);
      paint.color = rd32(payload, color_start + 4 * i);
      if (paint.idx >= canvas_.size())
        return fail("BATCH paint.idx out of range");
      if ((paint.color >> 24) != 255)
        return fail("BATCH paint.color alpha is not 255");
      paints.push_back(paint);
    }

    TickEvent event;
    event.tick = ++tick_;
    event.results.reserve(binds.size() + paints.size());

    for (const Bind& bind : binds) {
      ++bind_ok_;
      if (zero_token(bind.token)) {
        if (slots_[bind.slot].used) {
          token_slot_.erase(slots_[bind.slot].token);
          slots_[bind.slot] = Binding{};
        }
      } else {
        const auto used = token_slot_.find(bind.token);
        if (used != token_slot_.end() && used->second != bind.slot) {
          const SlotId old = used->second;
          slots_[old] = Binding{};
          token_slot_.erase(used);
          event.lost.push_back(old);
        }
        if (slots_[bind.slot].used)
          token_slot_.erase(slots_[bind.slot].token);
        Binding next;
        next.used = true;
        next.token = bind.token;
        next.uid = uid_for(bind.token);
        slots_[bind.slot] = next;
        token_slot_[bind.token] = bind.slot;
      }
      TickResult result;
      result.status = PaintStatus::Ok;
      event.results.push_back(result);
    }

    std::vector<std::uint32_t> ok_indices;
    ok_indices.reserve(paints.size());
    for (const Paint& paint : paints) {
      TickResult result;
      const Binding& bind = slots_[paint.slot];
      if (!bind.used) {
        result.status = PaintStatus::SlotInvalid;
        ++invalid_paints_;
        event.results.push_back(result);
        continue;
      }

      Brush& brush = brush_by_uid_[bind.uid];
      const std::uint64_t now = now_us();
      if (brush.has) {
        const std::uint64_t ax = brush.idx % config_.width;
        const std::uint64_t ay = brush.idx / config_.width;
        const std::uint64_t bx = paint.idx % config_.width;
        const std::uint64_t by = paint.idx / config_.width;
        const std::uint64_t distance =
            (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
        const std::uint64_t ready =
            brush.t + config_.cooldown_base + config_.cooldown_per_px * distance;
        if (now < ready) {
          result.status = PaintStatus::Cooling;
          result.value = std::min<std::uint64_t>(ready - now, 0xFFFFFFFFu);
          ++cooling_;
          event.results.push_back(result);
          continue;
        }
      }

      result.status = PaintStatus::Ok;
      ++ok_paints_;
      canvas_[paint.idx] = paint.color;
      brush.has = true;
      brush.idx = paint.idx;
      brush.t = now;
      event.brushes.push_back(BrushUpdate{paint.slot, paint.idx, now});
      ok_indices.push_back(paint.idx);
      event.results.push_back(result);
    }

    ++batches_;
    event.sonar.radius = config_.radius;
    event.sonar.tick = event.tick;
    const std::uint64_t cells =
        2ull * config_.radius * config_.radius + 2ull * config_.radius + 1;
    event.sonar.center.reserve(ok_indices.size());
    event.sonar.color.reserve(ok_indices.size() *
                              static_cast<std::size_t>(cells));
    for (std::uint32_t idx : ok_indices) {
      event.sonar.center.push_back(idx);
      const std::uint32_t cx = idx % config_.width;
      const std::uint32_t cy = idx / config_.width;
      const std::int32_t radius = config_.radius;
      for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        const std::int32_t span = radius - std::abs(dy);
        for (std::int32_t dx = -span; dx <= span; ++dx) {
          const std::int64_t x = static_cast<std::int64_t>(cx) + dx;
          const std::int64_t y = static_cast<std::int64_t>(cy) + dy;
          if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) {
            event.sonar.color.push_back(0);
          } else {
            event.sonar.color.push_back(
                canvas_[static_cast<std::size_t>(y) * config_.width + x]);
          }
        }
      }
    }

    ++ticks_;
    out = std::move(event);
    return true;
  }

  // ---------- WebSocket / TLS ----------

  static bool set_timeout(int fd, int seconds, int usec) {
    timeval tv{seconds, usec};
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
  }

  bool ssl_write_all(SSL* ssl, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
      const int chunk = static_cast<int>(std::min<std::size_t>(size, 1 << 20));
      const int wrote = SSL_write(ssl, data, chunk);
      if (wrote > 0) {
        data += wrote;
        size -= static_cast<std::size_t>(wrote);
        continue;
      }
      const int error = SSL_get_error(ssl, wrote);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        if (stop_.load())
          return false;
        continue;
      }
      return false;
    }
    return true;
  }

  bool send_frame(SSL* ssl, std::uint8_t op,
                  std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> frame;
    frame.reserve(data.size() + 10);
    frame.push_back(static_cast<std::uint8_t>(0x80 | op));
    if (data.size() < 126) {
      frame.push_back(static_cast<std::uint8_t>(data.size()));
    } else if (data.size() <= 65535) {
      frame.push_back(126);
      frame.push_back(static_cast<std::uint8_t>(data.size() >> 8));
      frame.push_back(static_cast<std::uint8_t>(data.size()));
    } else {
      frame.push_back(127);
      const std::uint64_t size = data.size();
      for (int i = 7; i >= 0; --i)
        frame.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    }
    frame.insert(frame.end(), data.begin(), data.end());
    return ssl_write_all(ssl, frame.data(), frame.size());
  }

  bool send_close(SSL* ssl, std::uint16_t code) {
    const std::uint8_t payload[2] = {
        static_cast<std::uint8_t>(code >> 8),
        static_cast<std::uint8_t>(code & 255),
    };
    return send_frame(ssl, 8, payload);
  }

  bool read_exact(SSL* ssl, std::uint8_t* dst, std::size_t size) {
    while (size > 0) {
      if (pre_pos_ < pre_.size()) {
        const std::size_t take =
            std::min(size, pre_.size() - static_cast<std::size_t>(pre_pos_));
        std::memcpy(dst, pre_.data() + pre_pos_, take);
        pre_pos_ += take;
        dst += take;
        size -= take;
        continue;
      }
      if (stop_.load())
        return false;
      const int got = SSL_read(ssl, dst, static_cast<int>(size));
      if (got > 0) {
        dst += got;
        size -= static_cast<std::size_t>(got);
        continue;
      }
      const int error = SSL_get_error(ssl, got);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        continue;
      if (error == SSL_ERROR_ZERO_RETURN ||
          (error == SSL_ERROR_SYSCALL && got == 0))
        peer_closed_ = true;
      return false;
    }
    return true;
  }

  // 返回 1=收到 Binary，0=暂无完整帧/停止，-1=错误，2=Close。
  int read_frame(SSL* ssl, std::vector<std::uint8_t>& message) {
    if (pre_pos_ >= pre_.size()) {
      const int fd = SSL_get_fd(ssl);
      if (SSL_pending(ssl) <= 0) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0)
          return -1;
        if (ready == 0)
          return 0;
      }
    }

    std::uint8_t head[2];
    if (!read_exact(ssl, head, 2))
      return stop_.load() ? 0 : -1;
    const bool fin = (head[0] & 0x80) != 0;
    const std::uint8_t op = head[0] & 0x0f;
    if (head[0] & 0x70)
      return -1;
    const bool masked = (head[1] & 0x80) != 0;
    std::uint64_t size = head[1] & 0x7f;
    if (!masked)
      return -1;
    if (size == 126) {
      std::uint8_t ext[2];
      if (!read_exact(ssl, ext, 2))
        return -1;
      size = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (size == 127) {
      std::uint8_t ext[8];
      if (!read_exact(ssl, ext, 8))
        return -1;
      if (ext[0] & 0x80)
        return -1;
      size = 0;
      for (std::uint8_t byte : ext)
        size = (size << 8) | byte;
    }
    if ((op & 8) && (!fin || size > 125))
      return -1;
    if (size > (2u << 20))
      return -1;

    std::uint8_t mask[4];
    if (!read_exact(ssl, mask, 4))
      return -1;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !read_exact(ssl, data.data(), data.size()))
      return -1;
    for (std::size_t i = 0; i < data.size(); ++i)
      data[i] ^= mask[i & 3];

    if (op == 0 || op == 1 || op == 3 || op > 10)
      return -1;
    if (op == 8)
      return 2;
    if (op == 9) {
      if (!send_frame(ssl, 10, data))
        return -1;
      return 0;
    }
    if (op == 10)
      return 0;
    if (op != 2)
      return -1;
    message = std::move(data);
    return 1;
  }

  static std::string header_value(const std::string& header,
                                  const std::string& name) {
    const std::string wanted = lower(name) + ":";
    std::size_t pos = 0;
    while (pos < header.size()) {
      const std::size_t end = header.find("\r\n", pos);
      const std::string line =
          header.substr(pos, end == std::string::npos ? std::string::npos
                                                      : end - pos);
      if (lower(line).rfind(wanted, 0) == 0)
        return trim(line.substr(wanted.size()));
      if (end == std::string::npos)
        break;
      pos = end + 2;
    }
    return {};
  }

  bool send_rules(SSL* ssl) {
    const auto payload = encode_rules();
    const auto wire = protocol::zstd_encode(payload);
    return send_frame(ssl, 2, wire);
  }

  void serve_client(int fd) {
    client_fd_.store(fd);
    peer_closed_ = false;
    if (!set_timeout(fd, 5, 0)) {
      ::close(fd);
      client_fd_.store(-1);
      std::lock_guard<std::mutex> lock(mu_);
      set_error_locked("setsockopt timeout failed");
      return;
    }

    SSL* ssl = SSL_new(context_);
    if (!ssl) {
      ::close(fd);
      client_fd_.store(-1);
      return;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) {
      if (!stop_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked("TLS accept: " + ssl_error_text());
      }
      SSL_free(ssl);
      ::close(fd);
      client_fd_.store(-1);
      return;
    }
    set_timeout(fd, 0, 200000);

    std::vector<std::uint8_t> raw;
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos && !stop_.load()) {
      std::uint8_t buf[2048];
      const int got = SSL_read(ssl, buf, sizeof(buf));
      if (got > 0) {
        raw.insert(raw.end(), buf, buf + got);
        const std::string text(raw.begin(), raw.end());
        header_end = text.find("\r\n\r\n");
        if (raw.size() > 16384)
          break;
        continue;
      }
      const int error = SSL_get_error(ssl, got);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        continue;
      break;
    }
    if (header_end == std::string::npos) {
      if (!stop_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked("HTTP upgrade header not received");
      }
      SSL_free(ssl);
      ::close(fd);
      client_fd_.store(-1);
      return;
    }

    const std::string header(raw.begin(), raw.begin() + header_end + 4);
    pre_.assign(raw.begin() + header_end + 4, raw.end());
    pre_pos_ = 0;
    const std::string key = header_value(header, "sec-websocket-key");
    if (key.empty()) {
      std::lock_guard<std::mutex> lock(mu_);
      set_error_locked("missing Sec-WebSocket-Key");
      SSL_free(ssl);
      ::close(fd);
      client_fd_.store(-1);
      return;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    const std::string source =
        key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    EVP_Digest(source.data(), source.size(), digest, &digest_len, EVP_sha1(),
               nullptr);
    std::string accept(4 * ((digest_len + 2) / 3), '\0');
    const int accept_len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(accept.data()), digest, digest_len);
    accept.resize(static_cast<std::size_t>(accept_len));

    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    if (!ssl_write_all(ssl, reinterpret_cast<const std::uint8_t*>(response.data()),
                       response.size()) ||
        !send_rules(ssl)) {
      std::lock_guard<std::mutex> lock(mu_);
      set_error_locked("failed to send WebSocket upgrade/RULES");
      SSL_free(ssl);
      ::close(fd);
      client_fd_.store(-1);
      return;
    }

    while (!stop_.load()) {
      std::vector<std::uint8_t> wire;
      const int status = read_frame(ssl, wire);
      if (status == 0)
        continue;
      if (status == 2)
        break;
      if (status < 0) {
        if (!stop_.load() && !peer_closed_) {
          std::lock_guard<std::mutex> lock(mu_);
          set_error_locked("WebSocket frame error");
        }
        break;
      }

      std::vector<std::uint8_t> payload;
      try {
        payload = protocol::zstd_decode_one(wire);
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked(std::string("zstd decode: ") + error.what());
        send_close(ssl, 4000);
        break;
      }
      if (payload.size() > (1u << 20)) {
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked("BATCH payload exceeds 1 MiB");
        send_close(ssl, 4002);
        break;
      }

      TickEvent event;
      std::string error;
      if (!process_batch(payload, event, error)) {
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked("BATCH: " + error);
        send_close(ssl, 4000);
        break;
      }
      if (!event.results.empty()) {
        const auto tick_payload = encode_tick(event);
        const auto tick_wire = protocol::zstd_encode(tick_payload);
        if (!send_frame(ssl, 2, tick_wire)) {
          std::lock_guard<std::mutex> lock(mu_);
          set_error_locked("failed to send TICK");
          break;
        }
      }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
    client_fd_.store(-1);
  }

  void run() {
    while (!stop_.load() && !has_error()) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 50);
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        std::lock_guard<std::mutex> lock(mu_);
        set_error_locked("listen poll failed");
        break;
      }
      if (ready == 0)
        continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0)
        continue;
      serve_client(fd);
    }
  }

  ServerConfig config_;
  mutable std::mutex mu_;
  std::vector<Binding> slots_;
  std::map<Token, SlotId> token_slot_;
  std::map<std::uint64_t, Brush> brush_by_uid_;
  std::vector<std::uint32_t> canvas_;
  std::string error_;

  std::uint64_t bind_ok_ = 0;
  std::uint64_t ok_paints_ = 0;
  std::uint64_t cooling_ = 0;
  std::uint64_t invalid_paints_ = 0;
  std::uint64_t batches_ = 0;
  std::uint64_t ticks_ = 0;
  std::uint64_t tick_ = 0;

  std::uint64_t base_us_ = 0;
  std::chrono::steady_clock::time_point base_mono_{};

  std::atomic<bool> stop_{false};
  std::atomic<int> client_fd_{-1};
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  SSL_CTX* context_ = nullptr;
  std::thread thread_;

  std::vector<std::uint8_t> pre_;
  std::size_t pre_pos_ = 0;
  bool peer_closed_ = false;
};

void test_state_machine() {
  ServerConfig config;
  config.width = 16;
  config.height = 16;
  config.radius = 2;
  config.cooldown_base = 2000;
  config.cooldown_per_px = 100;
  FakeServer server(config);

  std::string error;
  TickEvent event;

  Token token{};
  token[0] = 0x11;
  token[15] = 0x5A;
  std::vector<Bind> binds{Bind{0, token}};
  std::vector<Paint> no_paints;
  auto batch = protocol::encode_batch(binds, no_paints);
  check(server.process_batch(batch, event, error),
        "direct bind BATCH accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::Ok,
        "direct bind returns OK");

  const std::uint32_t color = 0xFF112233u;
  std::vector<Paint> paints{Paint{0, 5, color}};
  batch = protocol::encode_batch(std::vector<Bind>{}, paints);
  check(server.process_batch(batch, event, error),
        "direct paint BATCH accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::Ok,
        "first paint is OK");
  check(event.brushes.size() == 1 && event.brushes[0].idx == 5,
        "first paint carries brush update");
  check(event.sonar.center.size() == 1 && event.sonar.center[0] == 5,
        "first paint carries sonar");
  check(event.sonar.color.size() == 13 && event.sonar.color[6] == color,
        "sonar center color matches canvas");
  check(server.canvas()[5] == color, "server canvas updated");

  std::vector<Paint> cooling_paint{Paint{0, 6, color}};
  batch = protocol::encode_batch(std::vector<Bind>{}, cooling_paint);
  check(server.process_batch(batch, event, error),
        "cooling BATCH accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::Cooling,
        "same uid immediate paint is COOLING");
  check(event.results[0].value > 0, "COOLING carries remaining microseconds");
  check(server.canvas()[6] == 0, "COOLING does not change canvas");

  const auto tick_payload = server.encode_tick(event);
  TickEvent parsed;
  protocol::parse_tick(tick_payload, parsed);
  check(parsed.results.size() == 1 &&
            parsed.results[0].status == PaintStatus::Cooling,
        "server TICK encodes/decodes with client parser");

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  batch = protocol::encode_batch(std::vector<Bind>{}, cooling_paint);
  check(server.process_batch(batch, event, error),
        "retry after cooldown accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::Ok,
        "retry after cooldown is OK");
  check(server.canvas()[6] == color, "retry updates canvas");

  Token zero{};
  std::vector<Bind> unbind{Bind{0, zero}};
  batch = protocol::encode_batch(unbind, std::vector<Paint>{});
  check(server.process_batch(batch, event, error),
        "unbind BATCH accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::Ok,
        "unbind returns OK");

  std::vector<Paint> unbound_paint{Paint{0, 7, color}};
  batch = protocol::encode_batch(std::vector<Bind>{}, unbound_paint);
  check(server.process_batch(batch, event, error),
        "unbound paint BATCH accepted");
  check(event.results.size() == 1 &&
            event.results[0].status == PaintStatus::SlotInvalid,
        "paint on unbound slot is rejected");
  check(server.canvas()[7] == 0, "unbound paint does not change canvas");

  // 严格线格式：保留位非 0 必须拒绝，不能静默接受。
  std::vector<Paint> bad_paint{Paint{0, 8, 0xFF445566u}};
  auto bad_batch = protocol::encode_batch(std::vector<Bind>{}, bad_paint);
  bad_batch[2] = 1;
  std::string bad_error;
  TickEvent bad_event;
  check(!server.process_batch(bad_batch, bad_event, bad_error),
        "malformed BATCH reserved field is rejected");
  check(!bad_error.empty(), "malformed BATCH reports an error");
}

void test_end_to_end(const std::string& cert, const std::string& key) {
  constexpr std::uint32_t W = 16;
  constexpr std::uint32_t H = 16;
  constexpr std::uint32_t kTokens = 4;

  ServerConfig server_config;
  server_config.width = W;
  server_config.height = H;
  server_config.radius = 2;
  server_config.cooldown_base = 2000;
  server_config.cooldown_per_px = 100;
  server_config.max_slots = 16;
  server_config.cert = cert;
  server_config.key = key;

  FakeServer server(server_config);
  std::string error;
  if (!server.start(error)) {
    std::cerr << "server start failed: " << error << "\n";
    ++failures;
    return;
  }

  Config config;
  config.host = "127.0.0.1";
  config.port = std::to_string(server.port());
  config.proxy.clear();
  config.scheduler = SchedulerKind::NewSch1122;
  config.paint_ms = 2;
  config.repair_ms = 2;
  config.sonar_ms = 2;
  config.exec_ms = 2;
  config.round_cap = 256;
  config.queue_cap = 2048;
  config.observe = false;

  std::vector<std::uint32_t> expected(static_cast<std::size_t>(W) * H, 0);
  for (std::uint32_t y = 0; y < H; ++y) {
    for (std::uint32_t x = 0; x < W; ++x) {
      const std::uint32_t color = 0xFF000000u | (y << 8) | x;
      expected[static_cast<std::size_t>(y) * W + x] = color;
      config.draws.push_back(Draw{x, y, color});
    }
  }

  for (std::uint32_t i = 0; i < kTokens; ++i) {
    Token token{};
    token[0] = static_cast<std::uint8_t>(i + 1);
    token[15] = 0x5A;
    config.tokens.push_back(TokenEntry{Group::Paint, token, false});
  }

  Engine engine(config);
  std::atomic<bool> finished{false};
  int rc = -1;
  std::thread engine_thread([&] {
    rc = engine.run();
    finished.store(true);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!finished.load() && !server.has_error() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!finished.load())
    engine.request_stop();
  engine_thread.join();
  server.stop();

  const auto canvas = server.canvas();
  check(finished.load(), "engine finished without forced stop");
  check(rc == 0, "engine returned 0");
  check(server.error().empty(), "server reported no protocol/TLS error");
  check(server.bind_ok() == kTokens, "all tokens bind OK");
  check(server.invalid_paints() == 0, "no paint on unbound slots");
  check(server.batches() > 0 && server.ticks() > 0,
        "server processed BATCH and emitted TICK");
  check(server.ok_paints() == static_cast<std::uint64_t>(W) * H,
        "exactly one OK paint per target pixel");
  check(canvas == expected, "final server canvas equals target image");

  std::cout << "fake_server_test: bound=" << server.bind_ok()
            << " ok=" << server.ok_paints()
            << " cooling=" << server.cooling()
            << " invalid=" << server.invalid_paints()
            << " batches=" << server.batches()
            << " ticks=" << server.ticks() << "\n";
}

} // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);

  const std::string cert =
      argc > 1 ? argv[1] : "testfiles/fake_server_cert.pem";
  const std::string key =
      argc > 2 ? argv[2] : "testfiles/fake_server_key.pem";

  setenv("SSL_CERT_FILE", cert.c_str(), 1);
  unsetenv("HTTPS_PROXY");
  unsetenv("https_proxy");
  unsetenv("ALL_PROXY");
  unsetenv("all_proxy");
  unsetenv("IKAPB_HOST_IP");
  unsetenv("IKAPB_DNS_FAMILY");

  test_state_machine();
  test_end_to_end(cert, key);

  if (failures == 0) {
    std::cout << "fake_server_test OK (state machine + NewSch1122 end-to-end)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
