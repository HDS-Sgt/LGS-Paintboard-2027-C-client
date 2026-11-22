# 交接摘要（2026-11-22，开新会话用）

> 旧会话上下文 ≈ 283K tok，本文件是新会话的唯一入口。读完这份 + `AGENTS.md` 即可开工，
> 不要再凭记忆猜代码；本项目已从单文件 `main.cpp` 重构为**分层引擎**（`include/` + `src/`）。

## 0. 一句话状态

2027 洛谷冬日绘板客户端（手写 C++：OpenSSL TLS + RFC6455 + zstd + ikapb 协议）已经能跑：
`make` 构建通过、14 个单测 + 离线回放全绿、实机 25 秒 `painted=7508`（target.png，offset 880,0）。
当前网络：本机 IPv4 到 Cloudflare 被封，推荐走已运行的代理（`HTTPS_PROXY=http://127.0.0.1:7890`）；直连时才用 `IKAPB_HOST_IP` 钉 IPv6。

## 0.5 只换一张图画（不用重新构建）

换图是**运行时参数**，不需要 `make`。唯一要做的是连上（见 §2）后直接跑：

```bash
cd ~/桌面/tcsmgg/paintboard
export HTTPS_PROXY=http://127.0.0.1:7890   # 代理已由用户启动，禁止自行启动

./main --image new.png --offset 0,0 --paint-loop 1 --daemon
```

- `--image` 支持 PNG/JPEG/PPM（stb_image），不用转格式、不用重编译。
- `--offset` 是左上角，默认 `0,0`；画布 1000×600，**越界像素被静默裁剪**，所以要求
  `图宽 ≤ 1000−x`、`图高 ≤ 600−y`（例如 offset 880 只剩 120 列）。
- 画完就停用 `--limit N`；`--paint-loop 1` 会每轮完成后等 1 秒重画。
- `Ctrl-C` 打印 `painted/passes/ok[...]` 汇总；不需要动 `tokens.json`（加载时自动去重，1233 个唯一 token）。

## 1. 必读文档（按顺序）

| 文件 | 用途 |
|---|---|
| `AGENTS.md` | 工作区权威入口：构建/验证命令、协议不变量、调度器不变量、踩坑表、接手清单 |
| `ikapb_protocol.md` | 五种消息逐字节偏移表（已与官方 Python/TS 对拍）+ 实测差异 |
| `ikapb_impl_guide.md` | 实现手册：结构、数据模型、调度、验收 |
| `websock_guide.md` | `websock.hpp`（TLS + RFC6455）用法与重连模板 |
| `docs/ONBOARDING.md` | 分层重构后的代码导读（另一台设备写的） |

协议唯一权威：`repo/ikapb/docs/protocol.md`（2027 分支）；本地 `.protocol/protocol.md` 是快照。

## 2. 环境与网络前提（最容易卡住）

- 构建：`make -j4`（`g++`，`-I. -Iinclude -Isrc -I.deps/zstd-dev/usr/include`，链接 `/lib/x86_64-linux-gnu/libzstd.so.1`）。
- **IPv4 到 Cloudflare 整段被封**；系统 DNS 对 `pb.ika.zip` 的 AAAA 也时好时坏。
- 首选已运行的代理（用户明确禁止自行启动 Clash）：
  ```bash
  export HTTPS_PROXY=http://127.0.0.1:7890
  ```
  直连时才用 `IKAPB_HOST_IP` 钉 IP（逗号分隔、v4/v6 都行，TLS SNI/证书仍用域名）；
  该变量只对目标主机生效，不会劫持代理主机 `127.0.0.1`（否则连代理会失败）。
- DNS 本身也抖（冷查询 5–13s），`websock.hpp` 已加 600s 缓存 + 退避重试 + stale IP 回落；
  `TCP connect` 已改非阻塞 + `poll` 8s 超时。
- `tokens.json`：1760 条，**唯一 1233 个**（paint 1044 / repair 408 / sonar 308 里有 527 个重复）。
  加载时会自动跳过重复 token（重复 uid 绑多个槽位会触发 lost→重绑风暴，见 §4）。
- 跨设备同步可能带来**未来 mtime**，导致 `make` 说“无需做任何事”：
  先 `make fix-mtime` 再 `make`。

## 3. 构建 / 测试 / 运行（均实测通过）

```bash
make -j4                 # 构建 ./main
make fix-mtime           # 只在 make 不重编 / 报“时钟错误”时用

# 单测：make test 只编译测试，不执行！需手动跑：
make test && for t in queue_test fake_transport_test classic_target_test \
  classic_scheduler_test new_sch1122_test new_sch1122_paint_test new_sch1122_sonar_test new_sch1122_repair_test sonar_tiling_test \
  cli_test alternate_scheduler_test \
  protocol_test tick_selftest board_test; do ./testfiles/$t || echo "FAIL $t"; done
# 实测：全部 rc=0（输出 OK / 无输出）

# 离线回放回归（改 scheduler/protocol 后必跑）
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/r.ppm
cmp /tmp/r.ppm .protocol/ref_t.ppm && echo REPLAY_MATCH   # 实测 REPLAY_MATCH

# 实机（首选已运行的代理；直连才用 IKAPB_HOST_IP）
export HTTPS_PROXY=http://127.0.0.1:7890
timeout -k 5 25 ./main --image target.png --offset 880,0 --paint-loop 1 --daemon
# 实测：bind settled 1233/1233、painted=7508、ok[paint=7412 repair=0 sonar=96]、0 cooling
```

## 4. 本会话修掉的 bug（都有实测数字，别再踩）

1. **重复 token → 绑定风暴（RULES 后“卡住”）**：1760 条 token 里 527 个重复，同一 uid 绑两个槽位
   → 服务器把先绑的判 `lost` → `TokenPainter::on_lost()` 置 `want_bind_=true` → 每 50ms 重绑，
   `binds_settled()` 永远 false，调度器永不启动。修复：`src/app/cli.cpp` 的 `add_token`/`load_token_file`
   用 `token_seen()` 去重（保留首次出现的组别）。修复后 `flush binds=1233 → settle=1`。
2. **跨组代画删除**：`dispatch_ops` 曾在“本组没有到期 op”时让 repair/sonar 槽位代发 G_PAINT，
   导致 sonar/repair 实际 0 出活（`sent[p=109 r=0 s=2]`）。已整段删除，`help` 恒 0。
3. **声纳独立认领表 `g_enq_s`**：sonar 不再和 paint 共用 `g_enq`（`plan_claim_s` / `plan_release_s`），
   释放路径：`apply_tick` 的 OK/解绑/失败三处按 `rec.grp==G_SONAR` 分别清。
4. **声纳按槽位分块 + 蛇形扫描**：`sonar_worker` 给每个 sonar 槽位分一段列区间、块内蛇形、跨轮保留游标
   `scur[t]`；`dispatch_ops` 只把 op 派给 `o.slot` 对应的槽位。60s 实测 sonar 252 笔、覆盖 184 个不同 k。
5. **repair 游标 `rcur`**：原先每轮从 `p=0` 扫且每轮只认领 16 个，repair 永远只在画布开头工作。
   改为跨轮游标后，45s 实测 repair 252 笔、idx 覆盖 8974..42973。
6. **repair 复检阈值 60s + 坏色优先**：`g_repair_verify_ms=60000`；`op_t.prio`（0=坏色，1=复检），
   `repair_worker` 先收 `dmg` 再收 `chk`，`dispatch_ops` 先比 prio 再比距离。
7. **队列计数不配平**：`q_take_due` 原来在“取到 live 缓冲区”时就减计数，生产者以为队列空又补满，
   真实队列涨到 4000+。改为 `q_consumed()` 只在真正发送/丢弃时减、`q_requeue()` 不动计数、
   `q_recount()` 在重连后按实际内容重置。修复后 `q` 最大 1792（= 2048−256 预留）。
8. **队列预留 256**：paint 入队条件 `q_count + 256 < g_queue_cap`，给 repair/sonar 留位置。
9. **DNS/连接加固**：见 §2。
10. **Makefile**：`HDRS` 依赖（改头文件会重编）；`fix-mtime` 目标。

## 5. 已知限制 / 待办（按优先级）

1. **自动 IPv6 兜底**：有代理时不需要；直连仍要手动 `IKAPB_HOST_IP` 或写 hosts。可在 `dns_resolve()` 内置 AAAA 兜底。
2. **`--observe`（public=1 观察连接）未在分层引擎上验证**：大 SNAPSHOT 传输有 10–40s 空档，
   读超时需 ≥120s；`public_client.cpp` 的超时策略要确认。
3. **SIGTERM 优雅退出**：旧引擎曾因 public 线程 180s SSL_read + join 卡住（`timeout -k` 才能杀）；
   分层引擎未验证，Ctrl-C 汇总正常说明基本路径 OK。
4. **吞吐上限**：受 `c0 + k·d`（1s + 0.1s/格）约束，实测 15–25 笔/s；想更快只能加 token
   （每个 token 是独立 uid）或减少画笔位移（按行扫描）。不是 bug。
5. **干净画板上 `repair=0` 属正常**：repair 只修“声纳看到坏色”或“证据过期”的像素。
6. 队列/内存在长时间运行下是否稳定，建议长跑观察 `[stat] q= qc[p= r= s=]`。

## 6. 日志口径（看日志时别误判）

- `[stat] q=<队列> done=<已确认>/<目标> painted=<累计OK> passes=<完整轮数> free[p=r=s=] sent[p=r=s=] help=`
  - `painted` 是累计 OK（含重画），`passes` 才是“整图完成了几轮”。
- `[main] painted=... passes=... ok[paint=... repair=... sonar=...] sent[...]`
- `[work] bind settled: N/N bound, M failed`
- `[schedule] released N stuck pixel(s)`：g_enq 泄漏回收触发（正常应为 0）。
- `[paint] pass N complete -> next pass after ... (dropped M queued repair/sonar op(s))`
- `timeout` 的 rc=124/137 是“到点被杀”，不是程序错误。

## 7. 新会话开工清单

1. **改代码才需要** `cd ~/桌面/tcsmgg/paintboard && make -j4`（不重编先 `make fix-mtime`）；
   只换绘画图直接跳到 §0.5，不需要构建。
2. `make test` + 手动跑测试二进制（见 §3），确认基线绿（同样是改代码后）。
3. 读 `AGENTS.md` §3 文件地图、§4 构建验证，再看要改的模块（`src/scheduler/`、`src/net/`）。
4. 实机前先 `export HTTPS_PROXY=http://127.0.0.1:7890`（或按需 `IKAPB_HOST_IP`）。
5. 改动后至少跑：`make` + 离线 `replay_main` + `cmp .protocol/ref_t.ppm`，再考虑实机。

## 8. 快速定位

- 调度：`src/scheduler/classic_scheduler.cpp`（主用，基线）、`src/scheduler/new_sch1122.cpp`（从 Classic 复制的实验调度器，`--scheduler newsch1122` 选用，声纳已改成密铺分块，实验改动只放这里）、`src/scheduler/sonar_tiling.cpp`（密铺几何）、`round_robin.cpp`、`result_handler.cpp`、`target_builder.cpp`
- 网络：`src/net/transport.cpp`（TLS/WS）、`work_connection.cpp`（工作连接 + TICK）、`public_client.cpp`
- 绘画：`src/paint/queue.cpp`（PRB 队列/计数）、`token_painter.cpp`（bind/lost/重绑）
- 协议：`src/protocol/wire.cpp`；板：`src/board/board.cpp`；应用：`src/app/{cli,engine}.cpp`
- 离线样本：`.protocol/*.zst`（`t_snap/t_delta` 是回放基线，别覆盖）
