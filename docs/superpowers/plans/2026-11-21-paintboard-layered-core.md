# Paintboard Layered Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把单文件 `main.cpp` 拆成可独立测试的单进程分层库：独立的 Public 画面、全局 `PaintQueue`、单 token `TokenPainter`、可替换的 `IScheduler`，同时保持现有 CLI 和线上行为。

**Architecture:** 新代码在 `include/paintboard/` 与 `src/` 下并行构建，旧 `main.cpp` 在迁移期间保持可运行；先让协议/Board/队列/painter 各自通过离线测试，再接 `ClassicScheduler` 和 `Engine`，最后把 `main` 目标切到新入口并删除旧实现。

**Tech Stack:** C++20、g++ 15.2（WSL Ubuntu）、POSIX socket、OpenSSL、libzstd、pthread、GNU make。

**Spec:** `docs/superpowers/specs/2026-11-21-paintboard-layered-core-design.md`

## Global Constraints

- 只对接 2027 协议；BATCH/TICK/SNAPSHOT/PUBLIC/RULES 的字节格式不得改变。
- 公共头文件（`include/paintboard/*.hpp`）不得包含 `common_head.hpp`，不得使用 `$d/u$d/ret/pb` 宏。
- 队列计数语义固定：`push` +1；`take_due` 不减；`requeue` 不变；只有发送或丢弃时 `consume` -1。
- `TokenPainter` 主路径只提供异步 `submit`；阻塞适配器只能存在于测试/工具代码。
- 现有 CLI 参数、`main` 产物名、日志前缀（`[work]/[paint]/[repair]/[sonar]/[stat]`）保持不变。
- 不新增第三方依赖；构建只用 g++、OpenSSL、libzstd、pthread、make。
- 不提交 `tokens.json`、`.protocol/` 样本或二进制；`.gitignore` 继续生效。
- 所有离线验收必须在 WSL 下跑；线上 60s 验收只在最后切换后做一次。

## Review Focus

- **空目标 / 全空白目标图**：`build_target` 必须得到空 `known_idx`，不除零、不越界；Task 5 的 `classic_target_test` 空目标段覆盖。
- **RULES 尺寸变化 / 旧 SNAPSHOT**：`Board::apply_rules` 必须重建画布并丢弃旧层，随后 `apply_snapshot` 不能按旧尺寸写；Task 2 的 `rules_resize_test` 覆盖。
- **TICK 结果数与 pend 不一致**：少结果时保留剩余 pend，多结果时只消费现有数量且不越界；Task 4 的 `tick_short_and_long_test` 覆盖。
- **COOLING 重试同一像素**：不得换点、不得释放认领、不得重复减队列计数；Task 4 的 `cooling_keeps_op_test` 覆盖。
- **断线时未回结果的 bind/paint**：`take_pending` 只返回绘画 op，bind 不重复入队，重连后目标完成判定不误判；Task 4 的 `disconnect_pending_test` 覆盖。

---

### Task 1: 基础类型、协议层与构建骨架

**Files:**
- Create: `include/paintboard/types.hpp`
- Create: `include/paintboard/config.hpp`
- Create: `src/base/log.cpp`
- Create: `src/base/clock.cpp`
- Create: `src/protocol/wire.hpp`
- Create: `src/protocol/wire.cpp`
- Create: `testfiles/protocol_test.cpp`
- Create: `Makefile`
- Modify: `testfiles/tick_selftest.cpp`
- Commit: `docs/superpowers/specs/2026-11-21-paintboard-layered-core-design.md`

**Interfaces:**
- Consumes: 现有 `main.cpp` 的 `readptr`、`procRule/procSnap/procPublic`、`enc_batch`、`procTick`、`zstddec/zstdenc` 实现。
- Produces:
```cpp
namespace paintboard {
using SlotId = std::uint32_t;
using Tick = std::uint64_t;
using Token = std::array<std::uint8_t, 16>;
enum class Group : std::uint8_t { Paint, Repair, Sonar };
enum class PaintStatus : std::uint8_t { Ok, Cooling, SlotInvalid, Error };

struct PaintOp {
  std::uint64_t id = 0;
  std::uint32_t idx = 0;
  std::uint32_t color = 0;
  Group group = Group::Paint;
  Tick due = 0;
  std::uint64_t seq = 0;
  std::uint32_t priority = 0;
  std::optional<SlotId> affinity;
};

struct SonarBatch {
  std::uint16_t radius = 0;
  Tick tick = 0;
  std::vector<std::uint32_t> center;
  std::vector<std::uint32_t> color;
};

struct TickResult { PaintOp op; PaintStatus status; std::uint64_t value; };
struct BrushUpdate { SlotId slot; std::uint32_t idx; Tick t; };
struct TickEvent {
  Tick tick = 0;
  std::vector<TickResult> results;
  std::vector<SlotId> lost;
  std::vector<BrushUpdate> brushes;
  SonarBatch sonar;
};

struct Bind { SlotId slot; Token token; };
struct Paint { SlotId slot; std::uint32_t idx; std::uint32_t color; };

struct Rules {
  std::uint32_t width = 0, height = 0;
  std::uint16_t radius = 0;
  std::uint64_t tps = 0;
  std::uint64_t cooldown_base = 0, cooldown_per_px = 0;
  std::uint32_t max_slots = 0;
  std::uint32_t max_message_bytes = 0;
  std::uint32_t max_paints_per_message = 0;
};
}

namespace paintboard::protocol {
Rules parse_rules(std::span<const std::uint8_t> payload);
void parse_tick(std::span<const std::uint8_t> payload, TickEvent& out);
std::vector<std::uint8_t> encode_batch(std::span<const Bind> binds,
                                       std::span<const Paint> paints);
std::vector<std::uint8_t> zstd_decode_one(std::span<const std::uint8_t> wire);
std::vector<std::uint8_t> zstd_encode(std::span<const std::uint8_t> payload);
}
```
- `Token` 是 16 字节，`Bind`/`Paint` 与现有 `enc_batch` 字段一致。

- [ ] **Step 1: 写失败的协议测试**

Create `testfiles/protocol_test.cpp`，至少包含：

```cpp
#include "paintboard/types.hpp"
#include "protocol/wire.hpp"
#include <cassert>

int main() {
  using namespace paintboard;
  std::array<std::uint8_t, 16> tok{};
  tok[0] = 0xAB; tok[15] = 0xCD;
  Bind b{7, tok};
  Paint p{7, 123, 0xFF0000FFu}; // R=255,G=0,B=0,A=255
  auto wire = protocol::encode_batch({&b, 1}, {&p, 1});
  // 16 字节头 + pad4(2*1)=4 + 16*1 + pad4(2*1)=4 + 8*1 = 48
  assert(wire.size() == 48);
  assert(wire[0] == 0x01 && wire[1] == 1);
  assert(wire[16] == 7);             // bind.slot 小端
  assert(wire[20] == 0xAB);          // bind token 起点
  assert(wire[40] == 7);             // paint.slot
  assert(wire[44] == 123);           // paint.idx 低字节
  assert(wire[48 - 4] == 0xFF);      // color 最低字节 R
  return 0;
}
```

再把 `testfiles/tick_selftest.cpp` 的 include 从 `../main.cpp` 改为新头文件，保留其 BATCH 逐字节、TICK 列偏移、声纳顺序、颜色字节序断言。

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-protocol`
Expected: 编译失败，`paintboard/types.hpp` 或 `protocol::encode_batch` 不存在。

- [ ] **Step 3: 建骨架并搬协议代码**

- `Makefile` 变量：`CXX ?= g++`、`CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I. -Iinclude -I.deps/zstd-dev/usr/include`、`LDLIBS = -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1`。
- `make test-protocol` 编译 `testfiles/protocol_test.cpp testfiles/tick_selftest.cpp src/protocol/wire.cpp src/base/log.cpp src/base/clock.cpp`。
- `wire.cpp` 内保留现有 `readptr`/`proc*` 逻辑，外层加 `namespace paintboard::protocol`；所有 `parse_*` 的输入都从**完整载荷的 type 字节（偏移 0）**开始，函数内部再读 version（偏移 1）；`parse_rules` 返回 `Rules`，`parse_tick` 填 `TickEvent`，`encode_batch` 输出 `std::vector<uint8_t>`。
- `types.hpp` 只放类型；`config.hpp` 放 `Config`/`RuntimeStatus`/`Logger` 的完整声明（字段见 spec §6.8），后续任务只实现行为，不再改字段。

- [ ] **Step 4: 运行测试，确认通过**

Run: `make test-protocol && ./testfiles/protocol_test && ./testfiles/tick_selftest`
Expected: `protocol_test` 无输出且退出 0；`tick_selftest` 全部 PASS。

- [ ] **Step 5: 提交**

```bash
git add Makefile include src testfiles/protocol_test.cpp testfiles/tick_selftest.cpp docs/superpowers/specs/2026-11-21-paintboard-layered-core-design.md docs/superpowers/plans/2026-11-21-paintboard-layered-core.md
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 建立分层核心骨架与协议库"
```

---

### Task 2: Board 与 PublicClient

**Files:**
- Create: `include/paintboard/board.hpp`
- Create: `src/board/board.cpp`
- Create: `src/net/public_client.hpp`
- Create: `src/net/public_client.cpp`
- Create: `src/net/transport.hpp`
- Create: `src/net/transport.cpp`
- Modify: `testfiles/replay_main.cpp`
- Test: `testfiles/replay_main.cpp`（离线基准）

**Interfaces:**
- Consumes: Task 1 的 `protocol::parse_rules` / `zstd_decode_one`。
- Produces:
```cpp
namespace paintboard {
struct SonarSnapshot {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint32_t> color;
  std::vector<Tick> tick;
};

struct BoardView {
  Rules rules;
  Tick last_tick = 0;
  bool has_public_canvas = false;
};

class Board {
public:
  void apply_rules(const Rules&);
  bool apply_snapshot(std::span<const std::uint8_t> payload);
  bool apply_public(std::span<const std::uint8_t> payload);
  void apply_sonar(const SonarBatch&);
  Tick since() const;
  BoardView view() const;
  SonarSnapshot sonar_snapshot() const;
  bool copy_canvas_rgb(std::vector<std::uint8_t>& out) const;
  bool copy_sonar_rgb(std::vector<std::uint8_t>& out) const;
};

class ITransport {
public:
  virtual ~ITransport() = default;
  virtual void connect(const std::string& url) = 0;
  virtual void send(std::span<const std::uint8_t> bytes) = 0;
  virtual std::optional<std::vector<std::uint8_t>> recv(
      std::chrono::milliseconds timeout) = 0;
  virtual void close() = 0;
};

class TcpTransport final : public ITransport {
public:
  TcpTransport(std::string host, int port, std::string proxy);
  // connect/send/recv/close 用 tcSocket + zstd 单 frame
};

class PublicClient {
public:
  PublicClient(const Config&, Board&, RuntimeStatus&, Logger&);
  void run();
  void request_stop();
};
}
```

- [ ] **Step 1: 写 Board 尺寸变化测试**

Create `testfiles/board_test.cpp`：

```cpp
#include "paintboard/board.hpp"
#include <cassert>

int main() {
  paintboard::Board b;
  paintboard::Rules r;
  r.width = 4; r.height = 3; r.radius = 1;
  b.apply_rules(r);
  assert(b.view().rules.width == 4);
  assert(b.since() == 0);

  r.width = 2; r.height = 2;
  b.apply_rules(r);
  assert(b.view().rules.width == 2);
  std::vector<std::uint32_t> rgb;
  assert(b.copy_canvas_rgb(rgb));
  assert(rgb.size() == 2 * 2 * 3);

  paintboard::SonarBatch s;
  s.radius = 1; s.tick = 5;
  s.center = {0};
  s.color.assign(5, 0xFF0000FFu);
  b.apply_sonar(s);
  assert(b.sonar_snapshot().tick[0] == 5);
  return 0;
}
```

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-board`
Expected: 编译失败，`Board` 未定义。

- [ ] **Step 3: 实现 Board/PublicClient**

- 把 `cols/L1/L2/sonar_col/sonar_tick/glob/since/canvas_mu` 迁入 `Board`；`apply_rules` 在 W/H 变化时重建。
- `apply_snapshot`/`apply_public` 用 Task 1 协议层解码，保持现有颜色引用/差分键/`since` 规则。
- `apply_sonar` 按现有菱形顺序写 `sonar_col/tick`。
- `PublicClient::run` 用 `ITransport` 连接 `wss://.../ws?public=1&since=...`，断线重连；只调 `Board`。
- `TcpTransport` 包装 `tcSocket`；URL/代理解析沿用 `main.cpp` 现有逻辑。

- [ ] **Step 4: 改造 replay_main 并跑离线基准**

`testfiles/replay_main.cpp` 不再 `#include "../main.cpp"`；改为 `paintboard::Board` + `protocol::parse_rules` + `zstd_decode_one`。命令：

```bash
make test-replay
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm && echo "replay_main MATCH"
```

Expected: `replay_main MATCH`，`since=24144972`。

- [ ] **Step 5: 跑 Board 测试与公共回归**

```bash
make test-board && ./testfiles/board_test
./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm && echo "pub_apply MATCH"
```

Expected: 两个 MATCH。

- [ ] **Step 6: 提交**

```bash
git add include/paintboard/board.hpp src/board src/net src/base testfiles/board_test.cpp testfiles/replay_main.cpp Makefile
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 抽出 Board 与公共画面客户端"
```

---

### Task 3: PaintQueue

**Files:**
- Create: `include/paintboard/paint_queue.hpp`
- Create: `src/paint/queue.cpp`
- Create: `testfiles/queue_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `PaintOp`。
- Produces: `PaintQueue`，签名见 spec §6.2；`Stats` 含 `total` 与 `per_group[3]`。

- [ ] **Step 1: 写队列计数测试**

```cpp
#include "paintboard/paint_queue.hpp"
#include <cassert>

int main() {
  paintboard::PaintQueue q;
  paintboard::PaintOp a{1, 10, 0xFF0000FFu, paintboard::Group::Paint, 0};
  paintboard::PaintOp b{2, 11, 0xFF0000FFu, paintboard::Group::Repair, 0};
  paintboard::PaintOp c{3, 12, 0xFF0000FFu, paintboard::Group::Sonar, 100};
  q.push(a); q.push(b); q.push(c);
  assert(q.size() == 3);

  std::vector<paintboard::PaintOp> due;
  q.take_due(0, due);
  assert(due.size() == 2);
  assert(q.size() == 3);            // 取出不减
  q.consume(due[0]);
  assert(q.size() == 2);            // 发送/丢弃才减
  q.requeue(due[1]);
  assert(q.size() == 2);            // 重排队不变

  auto removed = q.remove_if([](const auto& o){ return o.group != paintboard::Group::Paint; });
  assert(removed.size() == 2);
  assert(q.size() == 0);
  return 0;
}
```

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-queue`
Expected: 编译失败，`PaintQueue` 未定义。

- [ ] **Step 3: 实现 PaintQueue**

`multiset` 按 `(due, seq)` 排序；`push` 补 `seq` 并原子递增计数；`take_due` 只移动；`requeue` 只插回；`consume` 减计数；`remove_if` 在锁内删除并减计数，返回被删 op。

- [ ] **Step 4: 运行测试**

Run: `make test-queue && ./testfiles/queue_test`
Expected: 退出 0。

- [ ] **Step 5: 提交**

```bash
git add include/paintboard/paint_queue.hpp src/paint/queue.cpp testfiles/queue_test.cpp Makefile
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 抽出全局 PaintQueue"
```

---

### Task 4: TokenPainter、Transport 与 WorkConnection

**Files:**
- Create: `include/paintboard/painter.hpp`
- Create: `src/paint/token_painter.cpp`
- Create: `src/net/work_connection.hpp`
- Create: `src/net/work_connection.cpp`
- Create: `testfiles/fake_transport_test.cpp`
- Modify: `testfiles/protocol_test.cpp`（如需 TICK 构造辅助）

**Interfaces:**
- Consumes: Task 1 `protocol::parse_tick/encode_batch`，Task 2 `ITransport`，Task 3 `PaintQueue`。
- Produces:
```cpp
namespace paintboard {
struct SlotView { /* 见 spec §6.4，含 follow/bound/inflight/brush/projected */ };

class IPainter {
public:
  virtual ~IPainter() = default;
  virtual SlotId id() const = 0;
  virtual Group group() const = 0;
  virtual SlotView view() const = 0;
  virtual bool can_accept(Tick now) const = 0;
  virtual bool ready_for(std::uint32_t idx, Tick now,
                         std::uint64_t cooldown_base,
                         std::uint64_t cooldown_per_px) const = 0;
  virtual bool submit(const PaintOp& op, Tick now) = 0;
};

class TokenPainter final : public IPainter {
public:
  TokenPainter(SlotId id, Group group, Token token, bool follow = false);
  void request_bind();
  void on_bind_result(bool ok);
  void on_paint_result(const PaintOp&, PaintStatus, std::uint64_t value, Tick now);
  void on_lost();
  void on_brush(std::uint32_t idx, Tick t);
  std::vector<PaintOp> take_outbox();
  bool wants_bind() const;
};

class WorkConnection {
public:
  WorkConnection(std::vector<IPainter*>, std::unique_ptr<ITransport>,
                 const Config&, RuntimeStatus&, Logger&);
  void connect();
  Rules rules() const;
  void request_all_binds();
  void flush();
  std::optional<TickEvent> poll(std::chrono::milliseconds timeout);
  std::vector<PaintOp> take_pending();
  bool all_bound() const;
};
}
```

- [ ] **Step 1: 写假 transport 测试**

`testfiles/fake_transport_test.cpp` 用 `FakeTransport` 记录发送的 BATCH，向 `WorkConnection` 注入 TICK：

1. 两个 painter 各 `request_bind`；`flush()` 后 fake 收到 bind 批；fake 回两个 `st=0` 的 TICK；`all_bound()` 为真。
2. `painter[0].submit(op, now)`；`flush()`；fake 回 `st=0`；`poll()` 返回 `results[0].status == Ok`，painter inflight 回到 0。
3. 冷却：fake 对同一 op 回 `st=1, value=500`；`poll()` 返回 `Cooling`；painter `view().not_before >= now+500`；该 op 不能被替换成别的 idx。
4. 结果不足：fake 只回 1 条而 pend 有 2 条；`poll()` 后 pend 仍剩 1 条。
5. 断线：`take_pending()` 只返回绘画 op，不返回 bind；重连后 bind 重新请求。

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-fake-transport`
Expected: 编译失败，painter/WorkConnection 未定义。

- [ ] **Step 3: 实现 TokenPainter**

按 spec §6.4：`mutex` 保护字段；`submit` 更新 `p_*`、`inflight++`、outbox；`on_paint_result` 处理 OK/COOLING/SlotInvalid/Error；COOLING 恢复 `p_* = a_*` 并设 `not_before`；`on_lost` 清绑定/在途；`on_brush` 更新 `a_*` 与 `p_*`。

- [ ] **Step 4: 实现 WorkConnection**

`flush` 先发所有 `wants_bind`，再发 outbox；`pend` 保存 `{slot, op, is_bind}`；`poll` 解析 TICK，按 pend 顺序把结果分给 painter；`take_pending` 只取绘画 op 并清空对应 pend；`all_bound` 全 painter `view().bound`。

- [ ] **Step 5: 运行测试**

Run: `make test-fake-transport && ./testfiles/fake_transport_test`
Expected: 五个场景全部断言通过。

- [ ] **Step 6: 提交**

```bash
git add include/paintboard/painter.hpp src/paint/token_painter.cpp src/net/work_connection.* testfiles/fake_transport_test.cpp Makefile
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 抽出 TokenPainter 与工作连接"
```

---

### Task 5: ClassicScheduler（Target/Plan/生产者）

**Files:**
- Create: `include/paintboard/scheduler.hpp`
- Create: `src/scheduler/classic_scheduler.hpp`
- Create: `src/scheduler/classic_scheduler.cpp`
- Create: `testfiles/classic_target_test.cpp`

**Interfaces:**
- Consumes: Task 1 `PaintOp/Group`，Task 3 `PaintQueue`，Task 4 `IPainter`，Task 2 `Board`。
- Produces:
```cpp
namespace paintboard {
struct Target {
  bool built = false;
  std::uint32_t W = 0, H = 0;
  std::vector<std::uint32_t> pix;
  std::vector<std::uint8_t> known;
  std::vector<std::uint32_t> known_idx;
  std::uint32_t bb_x0 = 0, bb_y0 = 0, bb_x1 = 0, bb_y1 = 0;
  std::vector<std::uint32_t> sp_pts;
};

class Plan {
public:
  void alloc(std::size_t n);
  bool claim(std::uint32_t idx);
  bool claim_sonar(std::uint32_t idx);
  bool done(std::uint32_t idx) const;
  void mark_done(std::uint32_t idx);
  void release(std::uint32_t idx);
  void release_sonar(std::uint32_t idx);
  void reset();
  std::size_t need_total() const;
  std::size_t done_total() const;
};

Target build_target(const Config&, const Rules&, const Board&);
bool target_complete(const Target&, const Plan&);

class ClassicScheduler final : public IScheduler {
public:
  ClassicScheduler(const SchedulerDeps&, Config&);
  // IScheduler 的 7 个方法
};
}
```

- [ ] **Step 1: 写目标构建测试**

`testfiles/classic_target_test.cpp`：

```cpp
#include "scheduler/classic_scheduler.hpp"
#include <cassert>

int main() {
  paintboard::Config cfg;
  cfg.image_path = "";                 // 只测 --draw
  cfg.draws = {{2, 1, 0xFF0000FFu}};
  paintboard::Rules r; r.width = 4; r.height = 3;
  paintboard::Board b; b.apply_rules(r);

  auto t = paintboard::build_target(cfg, r, b);
  assert(t.built);
  assert(t.known_idx.size() == 1);
  assert(t.known_idx[0] == 1 * 4 + 2);
  assert(t.bb_x0 == 2 && t.bb_y0 == 1);

  paintboard::Plan p; p.alloc(12);
  assert(p.claim(6));
  assert(!p.claim(6));
  p.mark_done(6);
  assert(p.done(6));
  p.release(6);
  assert(p.claim(6)); // 释放后可再次认领

  // 空目标：不崩溃、size 0
  paintboard::Config empty; paintboard::Board b2; b2.apply_rules(r);
  auto t2 = paintboard::build_target(empty, r, b2);
  assert(t2.known_idx.empty());
  return 0;
}
```

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-classic-target`
Expected: 编译失败。

- [ ] **Step 3: 搬 Target/Plan/生产者**

- `build_target` 复用现有 `build_target`：读图片/`--draw`、裁剪到画布、算 bbox、`known_idx`、`sp_pts`。
- `Plan` 复用 `g_enq/g_enq_s/g_done` 语义。
- `ClassicScheduler` 搬 `paint_worker/repair_worker/sonar_worker`、`dispatch_ops` 的过滤/选槽/冷却/入队规则；生产者只通过 `IPainter::view()` 读槽位，不再直接读 `wk.sl`。
- `step` 调 `q_take_due` + 派发；`on_tick` 处理 OK/COOLING/释放；`on_disconnect` 处理 `take_pending` 的 op。

- [ ] **Step 4: 运行测试**

Run: `make test-classic-target && ./testfiles/classic_target_test`
Expected: 退出 0。

- [ ] **Step 5: 提交**

```bash
git add src/scheduler/classic_scheduler.* testfiles/classic_target_test.cpp Makefile
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 抽出 ClassicScheduler 与目标计划"
```

---

### Task 6: Engine 与新 main 入口

**Files:**
- Create: `include/paintboard/log.hpp`（如 Task 1 未建）
- Create: `src/app/cli.cpp`
- Create: `src/app/main.cpp`
- Modify: `Makefile`（新增 `paintboard_new` 目标）
- Modify: `include/paintboard/config.hpp`（补全字段）

**Interfaces:**
- Consumes: Task 1-5 全部。
- Produces:
```cpp
namespace paintboard {
class Engine {
public:
  explicit Engine(Config);
  int run();
};
}

int main(int argc, char** argv); // 解析 CLI，构造 Config，调用 Engine
```

- [ ] **Step 1: 写 CLI 解析冒烟测试**

`testfiles/cli_test.cpp` 断言：`--draw 1,2,FF0000` 解析为 `Draw{1,2,0xFF0000FF}`；`--limit 10`、`--paint-loop 2`、`--observe` 字段正确；未知参数返回非 0。

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-cli`
Expected: 编译失败。

- [ ] **Step 3: 实现 CLI 与 Engine**

- 搬现有参数解析，保持 `--token*`、`--tokens`、`--image/--ppm`、`--offset`、`--limit`、`--dry-run`、`--paint-loop`、`--observe`、`--sonar-out`、`--board-out`。
- `Engine::run` 按 spec §6.7：connect → RULES → Board → scheduler.prepare → bind → loop(poll/step/flush) → stop。
- 公共线程可选；纯公共模式只跑 PublicClient。
- `Makefile` 增加 `paintboard_new`，链接全部 `src/**/*.cpp`；旧 `main` 仍由 `main.cpp` 构建。

- [ ] **Step 4: 跑 CLI 测试与纯公共冒烟**

```bash
make test-cli && ./testfiles/cli_test
timeout 20 ./paintboard_new --observe
```

Expected: `cli_test` 退出 0；`paintboard_new` 打印 `RULES` 并收到 `SNAPSHOT/PUBLIC`。

- [ ] **Step 5: 跑一次绑定/绘画冒烟**

```bash
HTTPS_PROXY=http://127.0.0.1:7890 timeout 30 ./paintboard_new --dry-run
HTTPS_PROXY=http://127.0.0.1:7890 timeout 30 ./paintboard_new --draw 100,100,FF0000 --limit 1
```

Expected: 绑定成功、`OK` 至少 1 条、退出 0。

- [ ] **Step 6: 提交**

```bash
git add src/app src/net include/paintboard/config.hpp Makefile testfiles/cli_test.cpp
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 接入 Engine 与新 main 入口"
```

---

### Task 7: 切换 main 并跑全量回归

**Files:**
- Modify: `Makefile`（`main` 改为新目标；旧 `main.cpp` 不再编译）
- Move: `main.cpp` → `legacy/main_legacy.cpp`（保留历史，不参与构建）
- Modify: `AGENTS.md`、`README.md`、`testfiles/README.md`
- Modify: `.gitignore`（如需忽略 `build/`）

**Interfaces:**
- Consumes: Task 6 的 `Engine`。
- Produces: 最终 `main` 二进制，CLI 与旧版一致。

- [ ] **Step 1: 切换构建目标**

`make` 生成 `main` 来自 `src/app/main.cpp` + 全部 `src/`；`make legacy` 可选编译 `legacy/main_legacy.cpp` 作对照，不默认构建。

- [ ] **Step 2: 跑离线全量回归**

```bash
make clean && make
make test
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm && echo "replay MATCH"
./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm && echo "pub_apply MATCH"
./testfiles/tick_selftest
```

Expected: 全部 MATCH/PASS。

- [ ] **Step 3: 跑 60s 线上验收**

```bash
HTTPS_PROXY=http://127.0.0.1:7890 timeout 60 ./main --image target.png --paint-loop 1 --daemon
```

Expected: `passes` 持续增长或至少完成 1 轮；`ok[paint]` 增长；无 `abort`；repair/sonar 组不饿死。

- [ ] **Step 4: 更新文档**

- `AGENTS.md` §3 文件地图改为新目录；§4 构建命令改为 `make`；保留离线回归与调度器不变量。
- `README.md` 构建/测试命令改为 `make`，目录结构更新。
- `testfiles/README.md` 增加 `queue_test/fake_transport_test/classic_target_test/cli_test/alternate_scheduler_test`。

- [ ] **Step 5: 提交**

```bash
git add Makefile legacy AGENTS.md README.md testfiles/README.md .gitignore
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "refactor: 切换 main 到分层核心并更新文档"
```

---

### Task 8: RoundRobinScheduler 与替代调度器测试

**Files:**
- Create: `src/scheduler/round_robin.hpp`
- Create: `src/scheduler/round_robin.cpp`
- Create: `testfiles/alternate_scheduler_test.cpp`

**Interfaces:**
- Consumes: Task 3 `PaintQueue`，Task 4 `IPainter`，Task 5 `IScheduler`。
- Produces: `RoundRobinScheduler final : IScheduler`，只按队列顺序轮流给空闲 painter；不依赖 `Target/Plan`。

- [ ] **Step 1: 写替代调度器测试**

`testfiles/alternate_scheduler_test.cpp` 用 `FakePainter`（实现 `IPainter`，`submit` 记录 op）和 `PaintQueue`：

1. 推入 5 个 op；`RoundRobinScheduler::step(0)` 后 3 个 painter 各拿到 1–2 个，顺序为队列顺序。
2. 全部 painter 都 `can_accept=false` 时，`step` 不再派发，op 留在队列。
3. 换回 `ClassicScheduler` 的接口不需要改 `PaintQueue`/`FakePainter`（编译期证明）。

- [ ] **Step 2: 运行测试，确认失败**

Run: `make test-alternate-scheduler`
Expected: 编译失败。

- [ ] **Step 3: 实现 RoundRobinScheduler**

`step` 从 `PaintQueue::take_due` 取 op，循环 painter 列表，找 `can_accept` 的槽位 `submit`；没派发的 `requeue`。`on_tick` 只从结果里把 OK 的 op 标记完成（测试不需要 Target）。

- [ ] **Step 4: 运行测试并做最终检查**

```bash
make test-alternate-scheduler && ./testfiles/alternate_scheduler_test
make clean && make && make test
git status --short
```

Expected: 测试通过；`git status` 只显示预期改动；无 token/二进制进暂存区。

- [ ] **Step 5: 提交**

```bash
git add src/scheduler/round_robin.* testfiles/alternate_scheduler_test.cpp Makefile
git -c user.name="HDS-Sgt" -c user.email="zhuliqinF1@163.com" commit -m "test: 增加可替换调度器与 RoundRobin 验证"
```

---

## Self-Review

- **Spec coverage:** 设计文档 §6.1–§6.9 的接口分别落在 Task 1（types/protocol）、Task 2（Board/PublicClient/ITransport）、Task 3（Queue）、Task 4（Painter/WorkConnection）、Task 5（Target/Plan/ClassicScheduler）、Task 6（Engine/CLI）；§7 的 7 步迁移对应 Task 1–8；§8 验收分别落在 Task 2/4/5/7/8。
- **Type consistency:** `SlotId`、`Tick`、`PaintOp`、`TickEvent`、`Rules` 只在 Task 1 定义；后续任务只引用，不重复定义。`IPainter*` 在所有 scheduler 接口中一致，避免 `unique_ptr<TokenPainter>` 绑定具体类型。
- **Review Focus:** 五个风险分别由 Task 5（空目标）、Task 2（尺寸变化）、Task 4（TICK 数不齐/COOLING/断线）覆盖。
- **Proportion:** 计划只固定文件、签名、测试断言和命令；函数体留给实现者，算法细节引用 spec。
