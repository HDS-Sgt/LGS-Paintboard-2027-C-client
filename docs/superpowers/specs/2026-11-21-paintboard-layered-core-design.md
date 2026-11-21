# Paintboard 客户端分层核心设计

- 日期：2026-11-21
- 状态：待批准
- 目标仓库：`HDS-Sgt/LGS-Paintboard-2027-C-client`
- 现有实现：单文件 `main.cpp`（2055 行）+ `websock.hpp` / `zstdwrap.hpp` / `common_head.hpp`

## 1. 意图

把当前“一个编译单元里塞下协议、画布、调度、线程、CLI”的客户端，拆成一组可独立测试、可替换策略的单进程分层库：

1. **Public 画面**独立：只负责 `public=1` 的 `RULES / SNAPSHOT / PUBLIC`，对外提供只读 `BoardView`。
2. **一个全局绘画队列**：paint / repair / sonar 的所有绘画操作都走同一个 `PaintQueue`；队列只按 `due + seq` 存取，不认像素归属、不认调度策略。
3. **单 token 绘画**：`TokenPainter` 是“一个 token / 一个槽位”的逻辑句柄，负责绑定、提交、冷却、画笔状态和在途计数；底层复用一条 `WorkConnection`，不要求每个 token 一条 socket。
4. **其余部分基于上面构建**：当前 paint / repair / sonar 调度器变成 `ClassicScheduler`，只依赖 `PaintQueue + IPainter + BoardView`；另写一个测试用 `RoundRobinScheduler`，证明换调度器不用改协议、队列、连接或 painter。

这不是协议改造，也不是性能优化；是一次**行为保持**的架构拆分。

## 2. 目标与非目标

### 目标

- `main` 的现有 CLI、日志语义、离线回归和线上行为保持不变。
- 公共画面可以在没有 token 的情况下独立运行（现有纯公共流用法）。
- 所有绘画操作经过同一个队列，队列计数规则与现在一致：
  - 取出到 `live` 不减计数；
  - 真正发送或丢弃才 `consume`；
  - 未派发 `requeue` 不改计数。
- 单 token 的绑定、冷却、在途、TICK 结果由 `TokenPainter` 自己维护。
- 换一套调度器只需实现 `IScheduler`，复用 `PaintQueue`、`IPainter`、`WorkConnection` 和协议层。
- 现有 `testfiles/replay_main.cpp`、`tick_selftest.cpp` 改为链接库，不再 `#include "../main.cpp"`。

### 非目标

- 不改变 2027 协议、BATCH/TICK 字节格式、压缩方式。
- 不把每个 token 拆成独立 WebSocket（方案 2 已否决）。
- 不引入 GUI、服务端或跨进程 IPC。
- 不在本次重写调度策略；`ClassicScheduler` 原样搬迁现有逻辑。
- 不追求比现在更高的吞吐；先保证等价，再谈优化。

### 反目标（即使能跑也算失败）

- 为了分层改变 `q_take_due` / `q_consumed` 的计数语义，导致队列再次超过软上限。
- 为了抽象把 `TokenPainter` 做成阻塞调用，破坏单连接批量发送。
- 新调度器必须复制 `WorkConnection` 或协议代码才能工作。
- 公共画面模块依赖 token 或调度器才能启动。

## 3. 约束

- C++20，Linux/POSIX；当前在 WSL Ubuntu 下用 g++ 15.2 编译。
- 服务器限制来自 `RULES`：`max_slots`、`max_message_bytes`、`max_paints_per_message`、冷却参数、`tps`、声纳半径。
- 一条工作连接可以承载多个槽位；TICK 结果按发送顺序与 `pend` 一一对应，没有请求 id。
- `common_head.hpp` 里有大量宏（`$d`、`u$d`、`ret`、`pb` 等）并引入 `<bits/extc++.h>`；**公共头文件不得依赖它**，实现文件可以继续使用。
- 现有测试直接 include 实现，拆分时必须同时改测试和构建方式。
- 现有 `.protocol/*.zst` + PPM 是回归基线，不能被覆盖。

## 4. 方案与选择

### 方案 1：共享工作连接 + 逻辑 TokenPainter（已选）

一条 `WorkConnection` 持有多条 socket 级槽位；每个槽位一个 `TokenPainter`。`PaintQueue` 全局唯一；`ClassicScheduler` 是第一个 `IScheduler` 实现。

- 保持合批、253 槽位、单连接行为。
- 换调度器只需换 `IScheduler`。
- 代价：`WorkConnection` 负责 TICK 多路复用和结果路由。

### 方案 2：每 token 独立连接（否决）

每个 `TokenPainter` 自己开 `ws?public=0`。单 token 真正独立，但 253 条 TLS/WS、失去合批，和当前实测吞吐模型不同。

### 方案 3：只拆具体类，不抽象 `IScheduler`（否决）

最快，但另一套调度系统只能复制 `ClassicEngine`，不满足“基于已有接口构建另一套调度系统”。

## 5. 目标结构

```
include/paintboard/
  types.hpp              // SlotId, Tick, Token, Group, PaintOp, PaintResult
  board.hpp              // Rules, BoardView, SonarSnapshot, Board
  paint_queue.hpp        // PaintQueue
  painter.hpp            // SlotView, IPainter, TokenPainter
  scheduler.hpp          // SchedulerDeps, IScheduler, SchedulerStats
  config.hpp             // Config, RuntimeStatus
src/
  base/log.cpp
  base/clock.cpp
  protocol/wire.cpp      // readptr, proc*, enc_batch, tick 编解码
  board/board.cpp        // canvas + L1/L2 + sonar + rules + since
  paint/queue.cpp
  paint/token_painter.cpp
  net/work_connection.cpp
  net/public_client.cpp
  scheduler/classic_scheduler.cpp
  app/cli.cpp
  app/main.cpp
testfiles/
  replay_main.cpp        // 改为链接 libpaintboard
  tick_selftest.cpp
  queue_test.cpp
  alternate_scheduler_test.cpp
Makefile
```

`main` 仍然保留原文件名，构建产物仍叫 `main`，避免破坏脚本和 AGENTS.md 里的命令。

## 6. 公共接口

### 6.1 基础类型

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
  Tick due = 0;                  // 服务器 µs；0=立即
  std::uint64_t seq = 0;         // 由队列分配
  std::uint32_t priority = 0;    // repair: 0=坏色 1=复检；其他组恒 0
  std::optional<SlotId> affinity; // sonar 分块归属
};

struct PaintResult {
  PaintOp op;
  PaintStatus status = PaintStatus::Error;
  std::uint64_t value = 0;       // COOLING 时为冷却 µs
};

struct TokenEntry {
  Group group = Group::Paint;
  Token token{};
  bool follow = false;           // repair 常驻声纳网格点
};

struct Draw {
  std::uint32_t x = 0, y = 0;
  std::uint32_t color = 0;       // R | G<<8 | B<<16 | 0xFF000000
};

}
```

### 6.2 PaintQueue

```cpp
class PaintQueue {
public:
  struct Stats {
    std::size_t total = 0;
    std::array<std::size_t, 3> per_group{};
  };

  std::uint64_t push(PaintOp op);                 // 插入，补 seq
  void take_due(Tick now, std::vector<PaintOp>& out); // 取走，计数不变
  void requeue(PaintOp op);                       // 插回，计数不变
  void consume(const PaintOp& op);                // 已发送/已丢弃，计数 -1
  std::vector<PaintOp> remove_if(const std::function<bool(const PaintOp&)>&);
  void recount();
  std::size_t size() const;
  std::size_t size(Group group) const;
  Stats stats() const;
};
```

- 内部 `std::multiset<PaintOp, OpCmp>`，排序键 `(due, seq)`。
- `push` 原子递增 `count` 和 `per_group[group]`。
- `take_due` 只移动元素，不改计数；`consume` 才减。
- `remove_if` 返回被移除的 op，由调用方释放认领；内部减计数。
- 线程安全；不持有 `Board`/`Plan` 的锁。

### 6.3 Board

```cpp
struct Rules {
  std::uint32_t width = 0, height = 0;
  std::uint16_t radius = 0;
  std::uint64_t tps = 0;
  std::uint64_t cooldown_base = 0, cooldown_per_px = 0;
  std::uint32_t max_slots = 0;
  std::uint32_t max_message_bytes = 0;
  std::uint32_t max_paints_per_message = 0;
};

struct SonarBatch {
  std::uint16_t radius = 0;
  Tick tick = 0;
  std::vector<std::uint32_t> center; // 每个声纳中心 idx
  std::vector<std::uint32_t> color;  // center.size() * (2R^2+2R+1)
};

struct BoardView {
  Rules rules;
  Tick last_tick = 0;
  bool has_public_canvas = false;
};

struct SonarSnapshot {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint32_t> color; // W*H
  std::vector<Tick> tick;           // W*H
};

class Board {
public:
  void apply_rules(const Rules&);
  bool apply_snapshot(std::span<const std::uint8_t> payload);
  bool apply_public(std::span<const std::uint8_t> payload);
  void apply_sonar(const SonarBatch&);   // 由工作连接 TICK 驱动
  Tick since() const;                    // 公共流重连用
  BoardView view() const;
  SonarSnapshot sonar_snapshot() const;
  bool copy_canvas_rgb(std::vector<std::uint8_t>& out) const;
  bool copy_sonar_rgb(std::vector<std::uint8_t>& out) const;
};
```

- `Board` 内部 `std::mutex`；`view()` 返回标量快照，`sonar_snapshot()` 整表复制。
- `apply_rules` 在 W/H 变化时重建 `cols/L1/L2/sonar`；和现在一样，旧画面会丢，公共流靠 `since` 回退。
- `since` 仍是 `Board` 私有，只由 public 流推进。
- 导出 PPM 从 `Board` 复制，不再直接访问全局 `cols/L1/L2`。

### 6.4 IPainter / TokenPainter

```cpp
struct SlotView {
  SlotId id = 0;
  Group group = Group::Paint;
  bool follow = false;           // repair 常驻声纳网格点
  bool bound = false;
  bool want_bind = true;
  int inflight = 0;
  bool has_brush = false;
  std::uint32_t brush_idx = 0;
  Tick brush_time = 0;
  bool has_projected = false;
  std::uint32_t projected_idx = 0;
  Tick projected_time = 0;
  Tick not_before = 0;
};

class IPainter {
public:
  virtual ~IPainter() = default;
  virtual SlotId id() const = 0;
  virtual Group group() const = 0;
  virtual SlotView view() const = 0;          // 线程安全快照
  virtual bool can_accept(Tick now) const = 0; // bound && inflight < 2
  virtual bool ready_for(std::uint32_t idx, Tick now,
                         std::uint64_t cooldown_base,
                         std::uint64_t cooldown_per_px) const = 0;
  virtual bool submit(const PaintOp& op, Tick now) = 0; // 标记投影+在途，进 outbox
};
```

`TokenPainter` 实现 `IPainter`，另外提供连接层使用的方法：

```cpp
class TokenPainter final : public IPainter {
public:
  TokenPainter(SlotId id, Group group, Token token, bool follow = false);

  void request_bind();
  void on_bind_result(bool ok);
  void on_paint_result(const PaintOp& op, PaintStatus st, std::uint64_t value, Tick now);
  void on_lost();
  void on_brush(std::uint32_t idx, Tick t);

  std::vector<PaintOp> take_outbox(); // 工作线程 flush 时取走
  bool wants_bind() const;
};
```

- `submit` 不阻塞、不发送；只更新 `p_*`、`inflight++`、把 op 放进 outbox。
- `ready_for` 使用 `p_t + c0 + k*曼哈顿距离` 与 `not_before` 取大。
- COOLING 时 `on_paint_result` 恢复投影到权威画笔 `a_*`，设 `not_before = now + value`；调度器从 `TickEvent.results` 拿到原 op 重排队，`TokenPainter` 自己不再保留它。
- 所有可变状态用一个小 mutex 保护，因为 `sonar_worker` 需要从生产者线程读槽位快照；不再出现现在 `sonar_worker` 无锁遍历 `wk.sl` 的竞争。

**单 token 绘画是异步句柄，不是阻塞 `paint()`。** 共享连接下，阻塞调用会占住唯一读 TICK 的线程。需要同步语义的工具可以在测试里用 `submit + 条件变量` 包一层，不进主路径。

### 6.5 WorkConnection

```cpp
struct TickResult {
  PaintOp op;
  PaintStatus status;
  std::uint64_t value;
};

struct BrushUpdate { SlotId slot; std::uint32_t idx; Tick t; };

struct TickEvent {
  Tick tick = 0;
  std::vector<TickResult> results;
  std::vector<SlotId> lost;
  std::vector<BrushUpdate> brushes;
  SonarBatch sonar;
};

class WorkConnection {
public:
  WorkConnection(std::vector<IPainter*> painters,
                 std::unique_ptr<ITransport> transport,
                 const Config& config,
                 RuntimeStatus& status,
                 Logger& log);

  void connect();                    // 连接 + 等 RULES
  Rules rules() const;
  void request_all_binds();
  void flush();                      // 先 bind，再 paint；按批发送
  std::optional<TickEvent> poll(std::chrono::milliseconds timeout);
  std::vector<PaintOp> take_pending(); // 断线时取回未回结果的 op
  bool all_bound() const;
};
```

- `flush` 从每个 painter 的 outbox 取 op，编码 BATCH，发送，按发送顺序进 `pend`。
- `poll` 解 TICK，按 `pend` 顺序路由到 `TokenPainter`；返回的 `TickEvent` 给调度器。
- `bind` 也走 `pend`，但 `PaintQueue` 不记录 bind。
- `take_pending` 只返回未回结果的绘画 op，不返回 bind。

连接层通过一个窄接口 `ITransport` 隔离 `tcSocket`，让 `WorkConnection` 可以在离线测试里接假 socket：

```cpp
class ITransport {
public:
  virtual ~ITransport() = default;
  virtual void connect(const std::string& url) = 0;
  virtual void send(std::span<const std::uint8_t> bytes) = 0;
  virtual std::optional<std::vector<std::uint8_t>> recv(
      std::chrono::milliseconds timeout) = 0; // 完整二进制帧；超时返回 nullopt
  virtual void close() = 0;
};
```

`TcpTransport` 用 `tcSocket` 实现；`FakeTransport` 只在测试里出现。

### 6.6 IScheduler

```cpp
struct SchedulerDeps {
  PaintQueue& queue;
  std::vector<IPainter*> painters;
  Board& board;
  Config& config;
  RuntimeStatus& status;
  Logger& log;
};

class IScheduler {
public:
  virtual ~IScheduler() = default;

  virtual void prepare() = 0;                 // RULES 后、bind 前：建目标/计划
  virtual void on_all_bound() = 0;            // 所有槽位绑定完成，启动生产者
  virtual void step(Tick now) = 0;            // 生产 + 派发（只在工作线程调用）
  virtual void on_tick(const TickEvent&) = 0; // 结果/释放，更新计划
  virtual void on_disconnect(std::vector<PaintOp> pending) = 0;
  virtual bool done() const = 0;
  virtual SchedulerStats stats() const = 0;
  virtual void stop() = 0;                    // 停生产者线程
};
```

- `ClassicScheduler` 持有 `Target`、`Plan`、三个生产者线程和 `follow` 槽位配置。
- `RoundRobinScheduler`（测试用）只用 `PaintQueue + IPainter`：按队列顺序轮流塞给空闲 painter，OK 即完成；证明接口可替换。
- 调度器不直接碰 `WorkConnection`；`submit` 进 painter outbox，由 `Engine` 在 `step` 后统一 `flush`。

### 6.7 Engine

```cpp
class Engine {
public:
  int run();
private:
  Config config;
  RuntimeStatus status;
  Board board;
  PaintQueue queue;
  std::vector<std::unique_ptr<TokenPainter>> painters;
  WorkConnection work;
  std::unique_ptr<IScheduler> scheduler;
  PublicClient public_client;
};
```

工作循环：

```
connect → 取 RULES → board.apply_rules → scheduler.prepare
        → request_all_binds → flush

loop:
  poll TICK → board.apply_sonar → scheduler.on_tick
  scheduler.step(now)
  work.flush()
  如果刚全部绑定 → scheduler.on_all_bound()
  检查 stop/limit/dry/signal
```

公共线程独立：`PublicClient` 只调用 `Board::apply_*`，不接触队列或调度器。

### 6.8 PublicClient / Config

```cpp
class PublicClient {
public:
  PublicClient(const Config& config, Board& board, RuntimeStatus& status, Logger& log);
  void run();          // public=1 重连循环，直到 status.stop
  void request_stop();
};

struct Config {
  std::vector<TokenEntry> tokens; // group + token
  std::string image_path;         // 空 = 只跑 --draw
  std::vector<Draw> draws;
  std::int32_t offset_x = 0, offset_y = 0;
  std::uint32_t exec_ms = 20;
  std::uint32_t paint_ms = 250, repair_ms = 500, sonar_ms = 800;
  std::uint32_t round_cap = 256, queue_cap = 2048;
  std::uint64_t repair_verify_ms = 60000;
  std::uint32_t repair_thresh = 96, sonar_stride = 5;
  std::uint32_t paint_loop_ms = 0;
  bool observe = false, daemon = false;
  std::uint64_t limit = 0;
  std::string sonar_out, board_out;
};
```

- `Config` 只由 CLI 在启动时写一次；运行期只读。
- `RuntimeStatus` 持有 `std::atomic<bool> stop/ready/abort`、`ok[3]`、`sent[3]`、`passes`、`last_tick` 等诊断计数。
- `Logger` 保留现有 `logp` 的格式和 `[work]/[paint]/[sonar]/[stat]` 前缀，便于脚本继续 grep。

### 6.9 Target / Plan（ClassicScheduler 私有）

```cpp
struct Target {
  bool built = false;
  std::uint32_t W = 0, H = 0;
  std::vector<std::uint32_t> pix;   // W*H，目标色
  std::vector<std::uint8_t> known;  // W*H，是否目标像素
  std::vector<std::uint32_t> known_idx; // 紧凑目标下标表
  std::uint32_t bb_x0 = 0, bb_y0 = 0, bb_x1 = 0, bb_y1 = 0;
  std::vector<std::uint32_t> sp_pts; // 声纳网格点
};

class Plan {
public:
  void alloc(std::size_t n);
  bool claim(std::uint32_t idx);       // paint/repair 共享 g_enq
  bool claim_sonar(std::uint32_t idx); // 独立 g_enq_s
  bool done(std::uint32_t idx) const;
  void mark_done(std::uint32_t idx);
  void release(std::uint32_t idx);
  void release_sonar(std::uint32_t idx);
  void reset();
  std::size_t need_total() const, done_total() const;
};
```

`Target` 与 `Plan` 只给 `ClassicScheduler` 用，不暴露给 `IScheduler`；另一套调度器可以完全不用它们。

## 7. 迁移顺序

采用“新核心并行构建，最后切换”的方式，保证任何一步都能编译和回退。

1. **骨架 + 基础层**：建 `include/paintboard/`、`src/`、Makefile；抽出 `types/log/clock`。
2. **协议层**：把 `readptr`、`procRule/procSnap/procPublic`、`enc_batch`、`procTick`、PPM 读写搬到 `protocol/`；`tick_selftest` 先链接新库。
3. **Board**：抽 `cols/L1/L2/sonar/rules/since` 和公共流应用；`PublicClient`；纯公共模式先用新代码跑通。
4. **队列 + painter**：实现 `PaintQueue`、`TokenPainter`、`WorkConnection`；用假 socket/单元测试验证计数、绑定、TICK 路由。
5. **ClassicScheduler**：搬 `target/plan/build_target`、三个生产者、`dispatch_ops`、`apply_tick` 的调度部分；接上 Engine。
6. **切换 main**：新 `app/main.cpp` 保持所有 CLI 参数；旧 `main.cpp` 删除或降为薄 wrapper；更新 `AGENTS.md` / `README.md` / `testfiles/README.md`。
7. **替代调度器测试**：`alternate_scheduler_test` 用 `RoundRobinScheduler` + fake painter 验证接口；不要求线上可用。

每一步的验收：

- 步骤 1-3：`make` + 纯公共 smoke（`timeout 20 ./main --observe`）。
- 步骤 4：`queue_test` 通过，计数不变量成立。
- 步骤 5-6：离线 `replay_main` + `cmp`、`tick_selftest`、60s 线上刷图。
- 步骤 7：`alternate_scheduler_test` 通过。

## 8. 验收标准

1. `make` 生成 `main` 和测试二进制；无新增编译错误（`rep` 宏的 sign-compare 噪音可保留）。
2. `./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm` 与 `.protocol/ref_t.ppm` 逐字节一致。
3. `./testfiles/tick_selftest` 通过（BATCH 逐字节、TICK 列偏移、声纳菱形、颜色字节序）。
4. `./testfiles/queue_test` 通过：`take_due` 不减计数、`consume` 才减、`requeue` 不变。
5. `./testfiles/alternate_scheduler_test` 通过：只依赖 `PaintQueue + IPainter` 就能把一批 op 派完。
6. 线上 60s：`timeout 60 ./main --image target.png --paint-loop 1 --daemon` 能连接、绑定、持续 `OK`，`ok[paint]` 增长，无 `abort`；repair/sonar 组不饿死。
7. 纯公共：`timeout 20 ./main --observe` 能打印 RULES 并收到 SNAPSHOT/PUBLIC。
8. `git diff` 不包含 token、`.protocol` 样本或二进制；`tokens.json` 仍被忽略。

## 9. 风险与应对

| 风险 | 应对 |
| --- | --- |
| 搬全局变量导致行为漂移 | 先并行构建，逐步切换；每步跑离线回归 |
| `common_head.hpp` 宏污染公共头 | 公共头只用标准类型；宏只在 `.cpp` 内使用 |
| `TokenPainter` 加锁后与调度器锁顺序冲突 | 规定锁顺序：Board → Plan → Queue 不嵌套；painter 锁只包自身字段，不回调 |
| TICK 与 `pend` 顺序错位 | `WorkConnection` 单线程 flush/poll；`pend` 统一保存 `(slot, op, is_bind)` |
| 生产者读槽位再次竞争 | 生产者只调 `IPainter::view()` 快照，不直接读字段 |
| 测试仍 include main.cpp | 步骤 1 起就改测试链接库；旧 include 只作为临时兼容 |
| 构建系统变化影响现有脚本 | 保留 `main` 产物名和 g++ 兼容的 Makefile；AGENTS.md 同步更新 |

## 10. 已定的实现选择

- 命名空间 `paintboard`；公共 API 不使用 `common_head.hpp` 宏。
- `TokenPainter` 异步；不为单 token 开独立 socket。
- 一个 `WorkConnection` 持有全部 painter；`PaintQueue` 全局唯一。
- 先加 Makefile，不引入 CMake（保持依赖最少；需要时再加）。
- `ClassicScheduler` 保持现有策略；替代调度器先作为测试实现，不暴露 CLI。
- 二进制仍叫 `main`，CLI 参数不变。

## 11. 需要你确认的三个选择

这三项不会改变核心分层，只影响外围 API/构建方式。下面是我的默认建议；你不同意再改。

1. **单 token 阻塞 API**：核心只提供异步 `TokenPainter::submit`；另加一个仅测试/工具用的 `BlockingPainter` 适配器（`submit` + 条件变量），不进主路径。理由：共享连接下阻塞会占住唯一读 TICK 的线程。
2. **替代调度器**：先只做测试用 `RoundRobinScheduler`，不暴露 CLI；接口稳定后再加 `--scheduler`。理由：先证明可替换，不增加线上分支。
3. **构建系统**：用 Makefile，不引入 CMake。理由：当前依赖只有 g++/OpenSSL/libzstd/pthread，Makefile 足够，且能保留 `main` 产物名。

如果你希望第 2 项现在就加 CLI 切换，或第 1 项必须提供阻塞式主 API，请在批准前说明；否则我按默认建议进入实现计划。
