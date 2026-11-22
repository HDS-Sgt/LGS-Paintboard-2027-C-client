#include "net/work_connection.hpp"
#include "paintboard/config.hpp"
#include "paintboard/painter.hpp"
#include "paintboard/types.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

std::size_t pad4(std::size_t n) { return (n + 3) & ~std::size_t(3); }

std::vector<std::uint8_t> make_rules(std::uint16_t width,
                                     std::uint16_t height, Tick server_time) {
  std::vector<std::uint8_t> out(96, 0);
  out[0] = 0x84;
  out[1] = 1;
  put16(out, 2, 0x20);
  put16(out, 4, width);
  put16(out, 6, height);
  put16(out, 8, 1);
  put32(out, 12, 128);
  put64(out, 16, 8000);
  put64(out, 24, 300);
  put32(out, 64, 4);
  put32(out, 68, 1024);
  put32(out, 72, 128);
  put32(out, 76, 128);
  put32(out, 80, 1u << 20);
  put32(out, 84, 1u << 20);
  put64(out, 88, server_time);
  return out;
}

std::vector<std::uint8_t> make_tick(
    Tick tick, const std::vector<BrushUpdate>& brushes,
    const std::vector<std::uint8_t>& status,
    const std::vector<std::uint32_t>& value,
    const std::vector<SlotId>& lost = {}) {
  const std::size_t nb = brushes.size();
  const std::size_t nr = status.size();
  const std::size_t ns = lost.size();
  const std::size_t bt = 32;
  const std::size_t bi = bt + 8 * nb;
  const std::size_t bs = bi + 4 * nb;
  const std::size_t st = bs + pad4(2 * nb);
  const std::size_t va = st + pad4(nr);
  const std::size_t si = va + 4 * nr;
  const std::size_t ls = si;
  const std::size_t total = ls + pad4(2 * ns);

  std::vector<std::uint8_t> out(total, 0);
  out[0] = 0x81;
  out[1] = 1;
  put16(out, 2, 0);
  put32(out, 4, static_cast<std::uint32_t>(nr));
  put64(out, 8, tick);
  put32(out, 16, static_cast<std::uint32_t>(nb));
  put32(out, 20, 0);
  put32(out, 24, static_cast<std::uint32_t>(ns));

  for (std::size_t i = 0; i < nb; ++i) {
    put64(out, bt + 8 * i, brushes[i].t);
    put32(out, bi + 4 * i, brushes[i].idx);
    put16(out, bs + 2 * i, brushes[i].slot);
  }
  for (std::size_t i = 0; i < nr; ++i)
    out[st + i] = status[i];
  for (std::size_t i = 0; i < nr; ++i)
    put32(out, va + 4 * i, value[i]);
  for (std::size_t i = 0; i < ns; ++i)
    put16(out, ls + 2 * i, lost[i]);
  return out;
}

class FakeTransport final : public ITransport {
 public:
  void connect(const std::string& url) override {
    ++connects;
    last_url = url;
  }
  void send(std::span<const std::uint8_t> payload) override {
    if (fail_next_send) {
      fail_next_send = false;
      throw std::runtime_error("fake send failure");
    }
    sent.emplace_back(payload.begin(), payload.end());
  }
  std::optional<std::vector<std::uint8_t>> recv(
      std::chrono::milliseconds) override {
    if (inbox.empty())
      return std::nullopt;
    auto out = std::move(inbox.front());
    inbox.pop_front();
    return out;
  }
  void close() override {}

  int connects = 0;
  std::string last_url;
  bool fail_next_send = false;
  std::deque<std::vector<std::uint8_t>> inbox;
  std::vector<std::vector<std::uint8_t>> sent;
};

struct Harness {
  FakeTransport* fake = nullptr;
  std::vector<std::unique_ptr<TokenPainter>> painters;
  std::unique_ptr<WorkConnection> conn;
  RuntimeStatus status;
  Logger log;
};

std::unique_ptr<Harness> make_harness(std::size_t count) {
  auto h = std::make_unique<Harness>();
  auto fake = std::make_unique<FakeTransport>();
  fake->inbox.push_back(make_rules(8, 8, mono_us()));
  h->fake = fake.get();

  std::vector<IPainter*> raw;
  for (std::size_t i = 0; i < count; ++i) {
    Token token{};
    token[0] = static_cast<std::uint8_t>(i + 1);
    h->painters.push_back(std::make_unique<TokenPainter>(
        static_cast<SlotId>(i), Group::Paint, token));
    raw.push_back(h->painters.back().get());
  }

  Config config;
  h->conn = std::make_unique<WorkConnection>(raw, std::move(fake), config,
                                             h->status, h->log);
  h->conn->connect();
  return h;
}

void test_bind_and_all_bound() {
  auto h = make_harness(2);
  h->conn->request_all_binds();
  h->conn->flush();
  check(h->fake->sent.size() == 1, "bind batch sent");
  check(!h->conn->all_bound(), "not bound before result");

  h->fake->inbox.push_back(make_tick(1, {}, {0, 0}, {0, 0}));
  auto event = h->conn->poll(std::chrono::milliseconds(10));
  check(event.has_value(), "bind result tick parsed");
  check(event->results.empty(), "bind results are not paint results");
  check(h->conn->all_bound(), "all painters bound");
}

void test_failed_bind_settles_without_retry() {
  auto h = make_harness(2);
  h->conn->request_all_binds();
  h->conn->flush();
  check(!h->conn->binds_settled(), "not settled while binds pending");

  // slot 0 OK, slot 1 BAD_PAINT_TOKEN (3)。
  h->fake->inbox.push_back(make_tick(1, {}, {0, 3}, {0, 0}));
  auto event = h->conn->poll(std::chrono::milliseconds(10));
  check(event.has_value(), "mixed bind result tick parsed");
  check(h->conn->binds_settled(), "failed bind still settles the batch");
  check(h->conn->bind_fail_count() == 1, "failure counted");
  check(h->painters[0]->view().bound, "good token bound");
  check(!h->painters[1]->view().bound, "bad token stays unbound");
  check(!h->painters[1]->view().want_bind, "bad token does not retry");

  const std::size_t sent_before = h->fake->sent.size();
  h->conn->flush();
  check(h->fake->sent.size() == sent_before, "no rebind flush for bad token");
}

void test_submit_and_ok() {
  auto h = make_harness(1);
  h->conn->request_all_binds();
  h->conn->flush();
  h->fake->inbox.push_back(make_tick(1, {}, {0}, {0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "bind tick consumed");

  PaintOp op;
  op.id = 7;
  op.idx = 9;
  op.color = 0xFF0000FFu;
  op.group = Group::Paint;
  check(h->painters[0]->submit(op, mono_us()), "submit accepted");
  h->conn->flush();
  check(h->fake->sent.size() == 2, "paint batch sent");

  h->fake->inbox.push_back(make_tick(2, {}, {0}, {0}));
  auto event = h->conn->poll(std::chrono::milliseconds(10));
  check(event.has_value() && event->results.size() == 1,
        "one paint result returned");
  check(event->results[0].op.id == 7, "result carries op id");
  check(event->results[0].status == PaintStatus::Ok, "result is OK");
  check(h->status.ok[0].load() == 1, "status OK counted");
  check(h->painters[0]->view().inflight == 0, "inflight cleared");
}

void test_cooling_keeps_same_op() {
  auto h = make_harness(1);
  h->conn->request_all_binds();
  h->conn->flush();
  h->fake->inbox.push_back(make_tick(1, {}, {0}, {0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "bind tick consumed");

  PaintOp op;
  op.id = 8;
  op.idx = 11;
  op.color = 0xFF00FF00u;
  check(h->painters[0]->submit(op, mono_us()), "submit cooling op");
  h->conn->flush();

  const Tick before = mono_us();
  h->fake->inbox.push_back(make_tick(2, {}, {1}, {500}));
  auto event = h->conn->poll(std::chrono::milliseconds(10));
  check(event.has_value() && event->results.size() == 1, "cooling result");
  check(event->results[0].op.idx == 11, "cooling keeps same op idx");
  check(event->results[0].status == PaintStatus::Cooling, "cooling status");
  const SlotView view = h->painters[0]->view();
  check(view.not_before >= before + 400, "not_before set from cooldown");
  check(!view.has_projected, "projection falls back to authoritative brush");
}

void test_insufficient_results_keep_pending() {
  auto h = make_harness(1);
  h->conn->request_all_binds();
  h->conn->flush();
  h->fake->inbox.push_back(make_tick(1, {}, {0}, {0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "bind tick consumed");

  PaintOp a;
  a.id = 1;
  a.idx = 20;
  PaintOp b;
  b.id = 2;
  b.idx = 21;
  check(h->painters[0]->submit(a, mono_us()), "submit a");
  check(h->painters[0]->submit(b, mono_us()), "submit b");
  h->conn->flush();

  h->fake->inbox.push_back(make_tick(2, {}, {0}, {0}));
  auto event = h->conn->poll(std::chrono::milliseconds(10));
  check(event.has_value() && event->results.size() == 1,
        "only one result this tick");
  auto pending = h->conn->take_pending();
  check(pending.size() == 1 && pending[0].idx == 21,
        "unanswered paint stays pending");
}

void test_disconnect_pending_and_rebind() {
  auto h = make_harness(2);
  h->conn->request_all_binds();
  h->conn->flush();
  h->fake->inbox.push_back(make_tick(1, {}, {0, 0}, {0, 0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "bind tick consumed");

  PaintOp op;
  op.id = 3;
  op.idx = 30;
  check(h->painters[0]->submit(op, mono_us()), "submit before disconnect");
  h->conn->flush();

  auto pending = h->conn->take_pending();
  check(pending.size() == 1 && pending[0].idx == 30,
        "take_pending returns paints only");

  h->fake->inbox.push_back(make_rules(8, 8, mono_us()));
  h->conn->connect();
  check(h->fake->connects == 2, "reconnected");
  h->conn->request_all_binds();
  h->conn->flush();
  check(!h->conn->all_bound(), "needs bind after reconnect");

  h->fake->inbox.push_back(make_tick(3, {}, {0, 0}, {0, 0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "rebind tick consumed");
  check(h->conn->all_bound(), "all bound after reconnect");
}

void test_send_failure_keeps_outbox() {
  auto h = make_harness(1);
  h->conn->request_all_binds();
  h->conn->flush();
  h->fake->inbox.push_back(make_tick(1, {}, {0}, {0}));
  check(h->conn->poll(std::chrono::milliseconds(10)).has_value(),
        "bind tick consumed");

  PaintOp op;
  op.id = 9;
  op.idx = 42;
  op.color = 0xFF0000FFu;
  check(h->painters[0]->submit(op, mono_us()), "submit before failed send");
  h->fake->fail_next_send = true;

  bool threw = false;
  try {
    h->conn->flush();
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "send failure propagates");
  check(h->fake->sent.size() == 1, "failed batch not recorded as sent");
  check(h->painters[0]->outbox_snapshot().size() == 1,
        "failed send keeps op in outbox");

  h->conn->flush();
  check(h->fake->sent.size() == 2, "retry sends the kept op");
  check(h->painters[0]->outbox_snapshot().empty(), "outbox cleared on success");
}

} // namespace

int main() {
  test_bind_and_all_bound();
  test_failed_bind_settles_without_retry();
  test_submit_and_ok();
  test_cooling_keeps_same_op();
  test_insufficient_results_keep_pending();
  test_disconnect_pending_and_rebind();
  test_send_failure_keeps_outbox();

  if (failures == 0) {
    std::cout << "fake_transport_test OK (7 scenarios)\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
