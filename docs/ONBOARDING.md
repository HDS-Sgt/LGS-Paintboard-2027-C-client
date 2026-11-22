# 接手须知：先看这一页（长期有效）

这是给“第一次打开这个仓库的 agent / 人”的 10 分钟入口。目标不是讲完协议，而是让你知道**东西在哪、数据怎么流、改哪里跑哪条命令**。

如果你是另一个 agent：先读本页，再读 [`AGENTS.md`](../AGENTS.md) 的 §0–§4，然后按下面的阅读路径走。协议字节不要凭记忆，以 `repo/ikapb/docs/protocol.md` 和 `ikapb_protocol.md` 为准。

## 1. 构建和单测先跑通

```bash
make                 # 生成 ./main
make test            # 构建 Makefile 里的全部测试二进制，不运行
./testfiles/protocol_test
./testfiles/tick_selftest
./testfiles/board_test
./testfiles/queue_test
./testfiles/fake_transport_test
./testfiles/classic_target_test
./testfiles/classic_scheduler_test
./testfiles/new_sch1122_test
./testfiles/new_sch1122_paint_test
./testfiles/new_sch1122_sonar_test
./testfiles/new_sch1122_repair_test
./testfiles/sonar_tiling_test
./testfiles/cli_test
./testfiles/alternate_scheduler_test
```

全部退出码为 0 就说明构建与核心逻辑没坏。`board_test` / `queue_test` 成功时可能没有输出。

离线 PPM 对比需要 `.protocol/` 抓包样本（仓库不附带）。如果没有样本，跳过下面两条，不要误以为项目坏了：

```bash
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm && echo "replay MATCH"

./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm && echo "pub_apply MATCH"
```

线上验证还需要 `tokens.json`（不随仓库分发）。没有 token 就只跑单测，不要用公共延迟画面判断“有没有画上”。

## 2. 职责分层

| 位置 | 负责什么 | 不该放什么 |
| --- | --- | --- |
| `include/paintboard/types.hpp` | 跨层数据类型：`PaintOp`、`TickEvent`、`Rules`、`TokenEntry` | 线程/策略 |
| `include/paintboard/config.hpp` | `Config`、`RuntimeStatus`（atomic 计数）、`Logger`、`ServerClock` | 画布状态 |
| `src/protocol/wire.cpp` | 字节 ↔ 结构：`parse_rules`、`parse_tick`、`encode_batch`、zstd 单帧 | 调度/重连 |
| `src/board/board.cpp` | 公共画面 `cols/L1/L2`、声纳表、`since`、RGB 导出 | 工作连接 |
| `src/paint/queue.cpp` | 全局 `PaintQueue`：`multiset` 按 `(due,seq)`，`take_due`/`consume`/`requeue` | 选择哪个 painter |
| `src/paint/token_painter.cpp` | 单槽位状态机：绑定、投影位置、冷却、outbox、inflight | 全局队列 |
| `src/net/work_connection.cpp` | 一条 `public=0` WS：绑定、组 BATCH、TICK 路由、`pend_` 顺序 | 像素该不该画 |
| `src/scheduler/target_builder.cpp` | 目标图 → `Target`（known/known_idx/bbox/sp_pts） | 派发 |
| `src/scheduler/result_handler.cpp` | `on_tick`/`on_disconnect` → `Plan`/`PaintQueue` 状态机 | 生产新 op |
| `src/scheduler/classic_scheduler.cpp` | `ClassicScheduler`：策略 + 三个生产者 + pass 控制；`Plan`/`target_complete` 也定义在这里 | 协议编解码 |
| `src/scheduler/new_sch1122.cpp` | `NewSch1122`：从 Classic 复制的实验调度器（`--scheduler newsch1122` 选用），声纳已改为密铺分块；共享 `Plan` 不在本文件 | 协议编解码 |
| `src/scheduler/sonar_tiling.cpp` | 声纳菱形密铺：中心枚举、known 补射、按块分给 sonar 槽位 | 目标图 / 派发 |
| `src/app/engine.cpp` | 组装、连接生命周期、公共线程、收尾 | 调度细节 |
| `src/app/cli.cpp` | 参数、token 文件、默认值 | 运行状态 |
| `legacy/main_legacy.cpp` | 旧单文件实现，只作历史对照，不参与构建 | 新功能 |

更细的“为什么这么拆”见 [`2026-11-21-paintboard-layered-core-design.md`](superpowers/specs/2026-11-21-paintboard-layered-core-design.md)。

## 3. 一个像素的旅程

1. `TargetBuilder::build` 把 `--image` / `--draw` 变成 `Target.known_idx` 和 bbox。
2. `ClassicScheduler` 的 `paint_loop` / `repair_loop` / `sonar_loop` 把 `PaintOp` 放进全局 `PaintQueue`。
3. `ClassicScheduler::step(now)` 用 `PaintQueue::take_due` 取到期 op，按组规则挑一个 `TokenPainter`，调 `submit`。
4. `WorkConnection::flush` 先收集 bind，再收集各 painter 的 `outbox_snapshot()`，编码成一条 BATCH 发出；发送成功后才 `take_outbox()` 清空。
5. 服务端回 TICK；`WorkConnection::handle_tick` 按 `pend_` 顺序把结果分给对应 `TokenPainter`，并把绘画结果放进 `TickEvent.results`。
6. `Engine` 调 `ClassicScheduler::on_tick`；`ClassicResultHandler` 处理 OK / COOLING / 失败：OK 标记 `Plan::mark_done`，COOLING 用同 op 重排。
7. `--paint-loop` 在 `target_complete()` 后清空 `Plan` 的 done 表、撤掉残留 repair/sonar op，然后重新生产。

### 队列语义（最容易改错）

- `take_due` 把 op 取到调度器 `live` 缓冲区时**不**减计数。
- 只有真正发送进 `WorkConnection::pend_`，或过滤丢弃时，才 `consume` 减计数。
- `requeue` 只插回，不动计数。
- COOLING 必须重试**同一个 op**，不能丢掉换一个新像素。
- 断线时 `restore_consumed` + `queue.recount()`；不要用普通 `requeue` 恢复已消费的计数。

## 4. 旧名 → 新名映射

`AGENTS.md` 的“调度器不变量”是 2026-11-20 旧单文件时代写的，保留历史结论，但名字已经变了。看到旧名时按下表对照：

| 旧名（legacy） | 新分层核心 |
| --- | --- |
| `g_target` / `g_known_idx` / `g_sp_pts` / `g_bb_*` | `Target` / `Target::known_idx` / `sp_pts` / `bb_*` |
| `g_enq` / `g_enq_s` | `Plan::claim` / `Plan::claim_sonar` |
| `g_done` | `Plan::done` / `Plan::mark_done` |
| `g_need_total` / `g_done_total` | `Plan::need_total` / `Plan::done_total` |
| `plan_claim` / `plan_release` / `plan_done` | `Plan::claim` / `Plan::release` / `Plan::done` |
| `q_take_due` / `q_consumed` / `q_requeue` | `PaintQueue::take_due` / `consume` / `requeue` |
| `q_count` / `g_qc` | `PaintQueue::size` / `PaintQueue::stats` |
| `dispatch_ops` | `ClassicScheduler::step` |
| `apply_tick` | `ClassicScheduler::on_tick` → `ClassicResultHandler` |
| `paint_worker` / `repair_worker` / `sonar_worker` | `ClassicScheduler::paint_loop` / `repair_loop` / `sonar_loop` |
| `slot_t` / `work_t` / `wk.pend` | `TokenPainter` + `SlotView` / `WorkConnection` / `WorkConnection::pend_` |
| `procRule` / `procSnap` / `procPublic` / `procTick` | `protocol::parse_rules` / `Board::apply_snapshot` / `Board::apply_public` / `protocol::parse_tick` |
| `enc_batch` | `protocol::encode_batch` |
| `unstick_leaks` / `q_drop_nonpaint` | 已移除；发送失败靠 outbox 保留，pass 切换在 `paint_loop` 内联处理 |

## 5. 改哪里，跑什么

| 想改的东西 | 先看 | 最小验证 |
| --- | --- | --- |
| 线格式 / RULES / TICK 字段 | `src/protocol/wire.cpp`、`ikapb_protocol.md` | `protocol_test`、`tick_selftest` |
| 公共画面 / 历史层 / 声纳 | `src/board/board.cpp` | `board_test`、`replay_main` + `pub_apply` |
| 队列计数 / 冷却重排 | `src/paint/queue.cpp`、`classic_scheduler.cpp` | `queue_test`、`classic_scheduler_test` |
| 单 token 状态 / 绑定 | `src/paint/token_painter.cpp` | `fake_transport_test` |
| 一条 WS 的发送/接收 | `src/net/work_connection.cpp` | `fake_transport_test`、线上 `--dry-run` |
| 目标图 / bbox / known_idx | `src/scheduler/target_builder.cpp` | `classic_target_test` |
| 派发策略 / 三个生产者 | `src/scheduler/classic_scheduler.cpp` | `classic_scheduler_test`、线上 60s |
| 新调度器实验 / paint 矩形分块 / 密铺声纳 / repair 匀配 | `src/scheduler/new_sch1122.cpp` + `sonar_tiling.cpp`（`--scheduler newsch1122`） | `new_sch1122_test`、`new_sch1122_paint_test`、`new_sch1122_sonar_test`、`new_sch1122_repair_test`、`sonar_tiling_test`、线上 60s |
| 新调度器接口 | 实现 `IScheduler` | `alternate_scheduler_test` |
| CLI / token 文件 | `src/app/cli.cpp` | `cli_test` |
| 生命周期 / 线程 | `src/app/engine.cpp` | `make test` + 线上冒烟 |

## 6. 当前已知限制（别当成 bug 重新查）

- **RULES 热重发没有完整生效**：`WorkConnection` 会更新 `rules_` 和时钟，但 `ClassicScheduler::prepare` 只在一开始读取 `c0_/k_`，`Board` 的尺寸也不会在运行中重建。主办方目前不热调参，所以先不处理。
- **`RoundRobinScheduler` 只用于测试**，没有 `--scheduler` CLI；它证明 `IScheduler` 可替换，不是生产默认。
- **`--early-us` 已解析但未使用**，不要以为它会改变派发时序。
- **坏 token 会被跳过**：绑定返回 `BAD_PAINT_TOKEN` 的槽位不再重试，`Engine` 用成功的子集继续；日志 `[work] bind settled: X/Y bound, Z failed`。彻底清理需要重新申领或删掉 `tokens.json` 里的旧 token。
- **`.protocol/` 与 `tokens.json` 不在仓库**：没有它们可以跑单测，但跑不了离线 PPM 对比和线上绘画。不要为了“让测试过”去伪造基线。
- **`ClassicScheduler` 仍偏大**（约 570 行）：`TargetBuilder` 和 `ClassicResultHandler` 已拆出；`ProducerRunner` / `PassController` 还没拆，改生产者逻辑时仍要在这个文件里看。

## 7. 推荐阅读顺序

1. 本页。
2. [`README.md`](../README.md) 的构建/使用/目录结构。
3. [`AGENTS.md`](../AGENTS.md) §0–§4（协议不变量与验收）。
4. `include/paintboard/{types,config,engine,scheduler,painter}.hpp`。
5. `src/app/engine.cpp` 的 `run_work()`：连接 → RULES → prepare → bind → 循环。
6. `src/scheduler/classic_scheduler.cpp` 的 `step` + 三个 loop。
7. 任选一个测试反推行为：`classic_scheduler_test.cpp` 或 `fake_transport_test.cpp`。

按这个顺序，通常 10–15 分钟能画出主链路；要动调度器，再读 `AGENTS.md` 的调度器不变量和 `classic_scheduler_test`。
