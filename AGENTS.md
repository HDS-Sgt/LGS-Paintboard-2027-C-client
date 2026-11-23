# AGENTS.md — ikapb 2027 画板客户端

本文件是给 agent 协作用的入口：**先读这里，再读三份专题文档**。第一次接手先读 `docs/ONBOARDING.md`（10 分钟路径、旧名映射、改哪里跑什么），再回到本文件查不变量。只讲长期有效的约定与结构，变更流水看 git/mtime，不要往这里追加流水账。

## 0. 版本与权威来源（最重要）

- 本项目只对接 **2027 协议**。唯一权威是 `repo/ikapb/docs/protocol.md`（2027 分支）。
  - 截至 2026-11-18：HEAD `7b42411`；协议正文定稿于 `9a1aaa1`，之后 `b87edb9` 只改 README、`7b42411` 只改 protocol.md 里一句“敬意”的措辞。
  - 本地副本 `.protocol/protocol.md` 是 `9a1aaa1` 的快照，与当前 `repo/ikapb/docs/protocol.md` 仅差这一句措辞，**没有字节语义差异**。怀疑过期时先 `diff -u .protocol/protocol.md repo/ikapb/docs/protocol.md`；确认只有措辞差异再 `cp` 刷新，不要凭标题相似就覆盖。
- ⚠️ `repo/ikapb` 的 **`origin/2026` 分支是旧版后端**：线格式是逐条 `Paint{x,y,rgb,uid,token,id}` + `Pong`，没有槽位、没有 BATCH、ACK 带请求 id。**它与 2027 完全不兼容，禁止拿来当 2027 参考**，只能在明确讨论“2026 历史实现”时使用。
- 数值优先级：**实机 `RULES` / `GET /api/meta` > 官方文档 > 本地笔记**。文档里的数字都是默认值，不要硬编码。反例：`.protocol/meta_live.json`（2026-11-17 实机）里 `max_slots=32768`、`max_paints_per_message=1048576`，都高于文档默认的 4096 / 65536。
- 本地三份专题文档的分工（不要重复造第四份）：
  - `ikapb_protocol.md` — 五种消息的逐字节偏移表（已与官方 Python/TS 示例对拍）+ 实测差异
  - `ikapb_impl_guide.md` — 客户端实现手册（总体结构、数据模型、调度、踩坑表、落地顺序；
    **§12 是 `legacy/main_legacy.cpp`（旧单文件）的结构地图**，**§13 是调度策略接口草稿 + 三种策略**，
    按函数名查分层/职责/数据流）
  - `websock_guide.md` — `websock.hpp`（TLS + RFC6455）用法与重连模板

## 1. 连接结构（2027）

### 建立连接
```
wss://<host>/ws?public=1&since=<已持有的公共版本号>
```
- `public=1` 收公共画面流；纯绘画工作连接设 `public=0` 省带宽。
- `since` 只在重连时带，用于补发增量；不带或版本太旧会收到全图 `SNAPSHOT`。
- 不检查 Origin、不协商子协议。同一 IP 最多 4096 条 `/ws` 连接，新建过快返回 `429`。

### 传输层
- 只能是**二进制**消息；发文本 → 关闭码 `4000`。
- **每条消息恰好一个完整 Zstandard frame**：不能多 frame、不能有尾随字节、不能用字典；客户端帧窗口 ≤ 8 MiB。
- 解压后的载荷：`[0]` 消息类型、`[1]` 协议版本（当前 `1`），其后是**列**（同类定宽字段的连续数组）。
- 整数一律**小端**；`u8`/`u16` 列用 `pad4(n)=(n+3)&~3` 补齐，头部长度是 4 的倍数，所以每列都 4 字节对齐。
- 载荷长度必须等于头部计数推导出的长度；保留字段与补齐字节必须为 0。
- `max_message_bytes` / `max_payload_bytes` 是**服务器对客户端上行**的检查（压缩后 / 解压后都 ≤ 1 MiB）。`SNAPSHOT` 是下行，1000×600 codec 1 的载荷就是 `16 + 12·W·H = 7 200 016` 字节；本地解码上限不能按 1 MiB 设，`protocol::zstd_decode_one` 用的是 16 MiB。

### 上行：只有 `BATCH`（`0x01`）
```
16B 头: type u8=0x01 | ver u8=1 | rsv u16=0 | nb u32 | np u32 | rsv u32=0
列: bind.slot u16[nb] | bind.paint_token 16B[nb] | paint.slot u16[np] | paint.idx u32[np] | paint.color u32[np]
长度 L = 16 + pad4(2nb) + 16nb + pad4(2np) + 8np
```
- 先按数组序处理全部绑定，再按数组序处理全部绘画；同一连接的消息按到达顺序逐条处理。
- 每条记录恰好产生一个结果，**没有请求 id**，靠“第 k 条记录 ↔ 第 k 个结果”对应。
- 消息级校验失败（长度/保留位/槽位越界/idx 越界/alpha≠255）→ 整条丢弃并以 `4000` 断开。

### 下行
| 类型 | 名称 | 说明 |
| --- | --- | --- |
| `0x84` | `RULES` | 连接后第一条，定长 96 字节；参数热调后重发 |
| `0x83` | `SNAPSHOT` | 公共全图；`/ws` 上 codec=1（含历史层 L1/L2） |
| `0x82` | `PUBLIC` | 公共画面增量，含分段/差分键/颜色历史引用 |
| `0x81` | `TICK` | 本连接自己的结果、画笔、落笔声纳、释放槽位 |
| `0x85` | `ADMIN_TICK` | 仅 `/admin/ws`，本项目不用 |

未知类型必须忽略（前向兼容）。

### 连接内状态模型
- 每条连接有 `0 … max_slots-1` 号**槽位**（文档默认 4096，实机见 `RULES`），槽位绑 16 字节 paint token。
- **画笔按 uid** 记录（位置 + 时间），不按 token；绑定后服务器会在 `TICK` 里把画笔推给你，**不要自己猜**。
- 多条连接可绑同一 token；冷却按 uid 算，多处绑定不会更快。
- 每条 `TICK` 恰好描述一个 tick，从不合并/丢弃；结果、声纳、画笔、释放槽位四样全空时不发。

### 心跳与限制
- WS 协议层 Ping（约每 10 秒），20 秒无 Pong → `4004`；**没有应用层心跳**。
- 关闭码：`4000` malformed（计违规）、`4002` too_large（计违规）、`4003` backlog、`4004` heartbeat、`4005` ip_banned。
- 默认限制：`max_message_bytes = max_payload_bytes = 1 MiB`、`msg_rate=128/s`、`msg_burst=128`、`max_paints_per_message=65536`。
- 必须及时读下行（即使不关心内容），否则下行积压 → `4003`。

## 2. 编解码不变量（最容易写错）

- **颜色** `u32` 小端字节序是 `R,G,B,A`（`R | G<<8 | B<<16 | 0xFF000000`），`A` 必须 255，否则整条 BATCH 格式错误。
- **差分编码**只出现在服务端下发的“有序下标列”：存 `d0=i0`、`dj=ij-i(j-1)`，客户端做一次前缀和。
- **`PUBLIC` 的键**不是直接下标：`order=0` 时 `k=i`（行优先）；`order=1` 时 `k=(i mod W)·H + ⌊i/W⌋`，反算 `i=(k mod H)·W + ⌊k/H⌋`。每段**单独**前缀和；差值 255 是转义，真实差值按顺序从 `dbig` 取。
- **`PUBLIC` 头部 36 字节**：`type@0, ver@1, p@2(u16), n@4(u32), from@8(u64), to@16(u64), m@24(u32), e@28(u32), order@32(u32)`；完整列布局见 `ikapb_protocol.md §3`。`zst_file` 只解析第一条消息，对多条 PUBLIC 首尾相接的链会打印 `MISMATCH`，**那是它的局限，不是数据坏**；验证用 `pub_decode` / `pub_apply`。
- **颜色列是历史引用**：`0`→取 `l1[i]`，`1`→取 `l2[i]`，`2`→颜色不变（原地重绘），其他→直接是颜色。应用规则见 `ikapb_impl_guide.md §4.4`，写错会让画面“颜色倒退”。
- **`SNAPSHOT`**：`/ws` 发 codec 1，长度 `16 + 12·W·H`（当前画面 + L1 + L2，层里 0 表示空）；`/admin/ws` 发 codec 0。
- **`TICK` 声纳**：每个声纳是 `A = 2R²+2R+1` 个颜色，按菱形顺序（dy 从 −R 到 R，行内 dx 从 −(R−|dy|) 到 +(R−|dy|)），越界写 0；第 k 个声纳对应第 k 个 `OK` 的绘画结果。
- **`RULES` 偏移 2 的 u16** 文档写“保留”，实测为 `0x0020`（样本有限，不要依赖）；不要把它当成字段读，更不要据此判断协议版本（版本在偏移 1）。
- **解析入口约定**：消息统一是 `[0]=type`、`[1]=version`。写 `procXxx(readptr &P)` 时必须先说清传进来的 `P.ptr` 指向 type 还是 version，否则整条消息会错位；新协议层由调用方读掉 type，`parse_*` 内再读 version。
- **偏移表**以 `ikapb_protocol.md` 为准；不要在代码里复制粘贴表格而不留注释。

## 3. 本仓库现状与文件地图

```
docs/ONBOARDING.md  10 分钟接手页（先读这个）
include/paintboard/  公共接口（无 common_head.hpp 宏依赖）
  types.hpp          SlotId/Tick/Token/Group/PaintOp/TickEvent/Rules...
  config.hpp         Config/RuntimeStatus/Logger/ServerClock
  board.hpp          Board：RULES/SNAPSHOT/PUBLIC/声纳 + RGB 导出
  paint_queue.hpp    全局 PaintQueue（multiset 按 due,seq）
  painter.hpp        IPainter / SlotView / TokenPainter 的逻辑接口
  scheduler.hpp      Target/Plan/build_target/IScheduler/SchedulerDeps
  engine.hpp         Engine：组装 Board/Queue/Painters/WorkConnection/Scheduler
src/
  base/              clock.cpp/log.cpp
  protocol/wire.cpp  parse_rules/parse_tick/encode_batch/zstd 单帧
  board/board.cpp
  paint/             queue.cpp/token_painter.cpp
  net/               transport.cpp/public_client.cpp/work_connection.cpp
  scheduler/         classic_scheduler.cpp（策略/生产者，Plan/target_complete 定义在此）
                     new_sch1122.cpp（从 Classic 复制的实验调度器；--scheduler
                     newsch1122 选用，声纳已改为密铺分块，只放本类方法，勿重复定义 Plan）
                     sonar_tiling.cpp（声纳菱形密铺：中心枚举、known 补射、按块分槽位）
                     target_builder.cpp（目标图构建）
                     result_handler.cpp（TICK/断线 → Plan/Queue 状态机）
  app/               cli.cpp/engine.cpp/main.cpp（新入口）
legacy/main_legacy.cpp 旧单文件实现，仅作历史参考，不参与构建
websock.hpp         TLS + HTTP Upgrade + RFC6455 帧（tcSocket：
                    connect/recvbin/try_recvbin/sendbin；HTTPS_PROXY CONNECT）
zstdwrap.hpp        zstddec/zstdenc，强制“恰好一个 frame”
common_head.hpp     旧宏板子（$d/u$d/_Str/rep/...；仅 legacy 与少量工具用）
tokens.example.json token 配置模板；复制为 tokens.json 后填真实 token（已忽略）
.deps/stb/          stb_image.h（target_builder.cpp 相对路径包含，无需 -I）
.deps/zstd-dev/     zstd.h（无 root 解包）
ikapb_protocol.md   2027 逐字节偏移表
ikapb_impl_guide.md 2027 客户端实现手册（§12 旧单文件结构，§13 调度策略）
websock_guide.md    websock.hpp 操作指南
repo/ikapb/         上游仓库（2027 分支检出；本地克隆，不入库）
.protocol/          实机抓包样本（本地数据，不入库）
testfiles/          探针与对拍程序；详见 testfiles/README.md
  replay_main.cpp   离线回放：Board + 协议层，快照/增量 → PPM
  tick_selftest.cpp BATCH/TICK/声纳/颜色自检
  board_test/queue_test/fake_transport_test/classic_target_test/
  classic_scheduler_test/new_sch1122_test/new_sch1122_paint_test/
  new_sch1122_sonar_test/new_sch1122_repair_test/sonar_tiling_test/
  cli_test/alternate_scheduler_test
  fake_server_test.cpp  本地假服务端 v1（TLS/WS/zstd、严格 BATCH、画布/冷却/声纳；
                        仅响应客户端、无竞争者；测试显式用 NewSch1122）
  pub_decode/pub_apply/web_test 等探针
pull_pub.sh         走 7890 代理拉公共画面（HTTP），失败回退直连
snap_retry.sh       快照重试（HTTP，成功后出 PPM/PNG）
```

### 分层核心的现状/坑（扩展前先看）

- **旧字段名只在 `legacy/main_legacy.cpp`**：`procRule` 的 `glob.edit`/`glob.remain` 是历史遗留；新代码一律走 `protocol::parse_rules`，字段名按协议。
- **RULES 可重发**：`Board::apply_rules` 每次都更新；`W/H` 变化时重建 `cols/L1/L2/sonar`（旧画面会丢，公共流靠 `since` 不匹配回退到 `SNAPSHOT`）。
- **PUBLIC 断链即重连**：`Board::apply_public` 要求 `from == since`，不匹配返回 false，`PublicClient` 用旧 `since` 重连；成功应用时**每段**推进 `since = from + tick[i]`。
- **两条连接分工**：`PublicClient` 跑 `public=1` 只画画面；`WorkConnection` 跑 `public=0` 绑定槽位、发 BATCH、解析 TICK。无 token 时 `Engine` 只跑公共流。
- **工作连接必须用 `try_recvbin`**：`recvbin()` 收到 Ping 后会继续等“下一条 Binary”，会在冷却期间卡住、无法按时发下一笔。`TcpTransport::recv` 只用 `try_recvbin` 切完整帧。
- **TICK 没有请求 ID**：`WorkConnection::pend_` 按发送顺序记录 bind/paint；`result[k]` 对应第 k 条记录，结果不足时保留 pend 到下一个 TICK，不要清空。
- **时间域必须统一**：TICK 的 `brush.t`、cooling 值是**服务器微秒**，调度器 `step(now)` 必须传 `WorkConnection::now()`（`ServerClock`），不能传 `mono_us()`（开机单调时钟）。初版混用导致 `ready_for` 永假、单像素不落笔。
- **颜色文本**：`--draw x,y,RRGGBB` 是给人看的 RGB 顺序，内部要转成 `R | G<<8 | B<<16 | 0xFF000000`（R 在最低字节），不能直接把 `0xRRGGBB` 当 u32。
- **声纳才是实时视野**：`TICK` 里第 k 个声纳对应第 k 个 `OK` 的**绘画**；绑定成功没有声纳。`--sonar-out` 导出 `sonar_col/tick` 便于核对。
- **不要用公共画面验证刚画的像素**：`GET /api/board`、`/ws?public=1`、`x-board-version` 都属于**公共延迟画面**，只包含提交时间 ≤ `now − D` 的 tick（`D = public_delay_ms`；文档默认 10 分钟，实机 2026-11-20 `/api/meta` 为 **60000**）。刚画完去拉快照一定看不到，会误判成“没画上”。实时证据只有两处：本连接 `OK` 结果里的声纳、以及 `brush.t` 是否推进。
- **声纳是逐 tick 的实时读数，会被别人覆盖**：每笔 `OK` 的声纳读的是该 tick 提交后的画面。一次画很多笔时，早先的格子可能在后续 tick 被别人写掉，`--sonar-out` 里看到的不一定还是你画的样子——这不是编码问题。
- **发送失败不能丢 op**：`WorkConnection::flush` 先用 `outbox_snapshot()` 组 BATCH，`transport_->send` 成功后才 `take_outbox()` 清空。发送抛异常时 op 留在 painter，重连后重发；否则 `ClassicScheduler::step` 已 `queue.consume(op)`，op 却不在队列/outbox/pend 任何一处，`Plan` 认领会永久泄漏（历史 `target_done = need − 4` 的同类症状）。`fake_transport_test` 有对应场景。
- **绑定失败不能阻塞启动**：bind 的 TICK 结果 `3 = BAD_PAINT_TOKEN`，表示该 token 已不是有效 paint token（同一 uid 重新申领会立刻让旧 token 失效，删除/吊销同理；`tokens.json` 里常混着旧 token）。`Engine` 只在 `WorkConnection::binds_settled()`（没有待回执 bind、也没有 painter 还想重绑）后启动生产者；失败的 `TokenPainter` 置 `want_bind_=false`，不再每轮 50ms 重绑，成功绑定的子集继续画。只有 bound=0 才 abort。旧行为等待 `all_bound()`（要求全部成功）会让少数坏 token 拖死整条连接：`target_90` 表现为 `painted=0 sent=0`。
- **token 必须去重**：同一个 uid 绑到两个槽位时，服务器会把先绑的槽位判 `lost`；`TokenPainter::on_lost()` 会置回 `want_bind_=true`，于是每 50ms 重绑一次，`binds_settled()` 永远为 false、调度器不启动。症状是只打印 `RULES` + `[paint] target ...` 后静默、`painted=0`。`--tokens`/`tokens.json` 加载时已按 16 字节 token 去重（保留首次出现）；实测 1760 条目只有 1233 唯一、527 个重复，去重后 25s `painted=10449`。
- **冷却/断线重排用 `restore_consumed`**：发送时已经 `consume`，不能再用“只插回、不动计数”的 `requeue`；断线后还要 `queue.recount()` 按真实 multiset 校正。`done()` 只在纯 paint 组、非 `--daemon`/`--paint-loop` 时按 `target_complete` 收尾；带 repair/sonar 的会话继续值守，Engine 另要求 `pending_paint_count()==0`。

### 调度器不变量（2026-11-20 踩坑后确认）

> 下面这段是**旧单文件时代**的结论，保留是因为坑本身仍然成立；名字已经换了。读之前先按 [`docs/ONBOARDING.md`](docs/ONBOARDING.md) §4 的“旧名 → 新名映射”对照：`g_enq`→`Plan::claim`、`g_enq_s`→`Plan::claim_sonar`、`g_done`→`Plan::done`、`g_known_idx`→`Target::known_idx`、`q_take_due`→`PaintQueue::take_due`、`dispatch_ops`→`ClassicScheduler::step`、`apply_tick`→`ClassicScheduler::on_tick`。

**症状**：`--image ... --paint-loop 1 --daemon` 跑起来后，paint token “缩在某个位置不动”，退出汇总 `target_done = need − 4` 且 `enq_stuck > 0`。当时证据在 `.protocol/sched_repro.log`、`.protocol/sched_diag.log`。这**不是**网络、冷却或 token 问题，先查调度器。

**当时的根因（历史，别重犯）**：`plan_claim` 在**共享**的 `g_enq` 上占位（paint/repair/sonar 三组共用）；`dispatch_ops` 只从“本组已就绪”的 op 里挑**曼哈顿距离最近**的；sonar 每 800ms 重新认领固定网格点、且不看 `g_done`。于是 sonar 槽的画笔停在网格下排时，永远挑中近点，上排 4 个远点的 op 一直差一步、从未发送，却始终占着 `g_enq`；paint 组因此无法认领这 4 个像素 → `target_complete()` 永远 false → `--paint-loop` 不重置 `g_done`、不进下一轮 → paint 全组空转。

**三条不变量（改调度器前必须保持）**：
1. **认领按组语义**：paint 认领 `!g_done`；repair 认领前必须 `plan_done(idx)`（只修已经画好的像素）；**sonar 用独立的 `g_enq_s` 认领表**（2026-11-21 起），可以扫 bbox 网格上的任意点（包括还没画的，反正声纳画的也是目标色），不占 paint 的 `g_enq`，也不再受 `plan_done` 限制。2026-11-21 实测反例：sonar 不看 `g_done` 时会先认领未画像素并占住共享 `g_enq`，paint 再也认领不了，29 个 paint token 空转，整轮最后 ~90 个像素只有 ~0.6/s（全靠 sonar 慢慢挪过去），用户看到的就是“paint token 不动、只有声纳在动”。加上守卫后同一场景（97 槽位/40px）120s 内 pass 1 完整、paint 3424 笔（≈28/s）。**副作用**：paint 会把队列打到 `q_queue_cap`(2048)，sonar/repair 生产者被 `q_count < g_queue_cap` 挡住，只在队列低时才能认领；因此 paint 生产者的入队条件改成 `q_count + 256 < q_queue_cap`，给 sonar/repair 预留 256 个位置（否则 paint 独占队列时 sonar 一笔也发不出）。
2. **派发规则**：G_SONAR 严格按 `seq` 取**队头** FIFO（固定网格扫描，禁止近点插队）；G_PAINT/G_REPAIR 按距离最近挑（repair 每像素只认领一次，不会被近点饿死）。**不做跨组代画**：repair/sonar 槽位只发本组的操作，本组没活就空等。曾试过“本组没有*到期* op 就代发 G_PAINT”，后收紧到“本组在 `live` 里一个 op 都没有（`lc[group]==0`）才代发”；2026-11-21 用 97 槽位/target_90 实测仍是 `ok[paint=21448 repair=710 sonar=309]`——代画让声纳/修复槽位被 paint 的活占满、扫描进度被拖慢（用户看到声纳仍聚集在少数网格点），因此把整段兜底**移除了**（`help=` 诊断保留，恒为 0）。`--repair-follow` 常驻槽位只接网格点 G_REPAIR、不代画。
3. **`g_enq` 的跨组共享只发生在 paint↔repair**：sonar 已拆到独立 `g_enq_s`；所有释放路径（OK/解绑/失败/`q_drop_nonpaint`/`unstick_leaks`）都必须同步清 `g_enq_s`，否则声纳网格点会被永久占住。2026-11-21 实测 60s（97 槽位/target_90）：声纳认领覆盖 k=0..76（拆表前只到 7），`ok[paint=2466 repair=16 sonar=77]`、paint ≈42/s。
4. **声纳按槽位分块、块内蛇形扫**：每个 sonar 槽位分到一段连续的列条带（`sbw=ceil(bw/sk)` 列），槽位内按行蛇形（奇数行反向）依次认领，相邻两点只差一个网格步（`stride` 像素），避免 FIFO 让槽位跨行跳（~50 px ⇒ `c0+k·d` 要 5–10 秒）。op 上带 `o.slot` 表示分块归属：`dispatch_ops` 的 sonar 分支只让它自己那个槽位接，槽位之间不抢；`COOLING` 重排队保留 `o.slot`，重连/槽位失效时丢弃并 `plan_release_s`。每槽 pending 上限 `sk*4`。2026-11-21 实测（97 槽位/target_90）：60s `ok[paint=2392 repair=215 sonar=252]`、sonar 认领覆盖 184 个不同 k（拆表前 81、未拆表 69 笔/60s）。`IKAPB_SC_LOG=1` 打印 `[sc] slot= k= idx=`。 **repair 同理**：`repair_worker` 只遍历 `build_target` 建好的 `g_known_idx`（目标像素的紧凑下标表，6150 个约 12s 一圈），**不再按画布下标 `0..N` 扫**——120x123 的目标在 1000x600 画布上只占右侧 50 列，按画布扫时 95% 的窗口是空白，一圈要 ~19.5 分钟，repair 看起来完全不动。pending 上限按 repair 槽位数放宽（`max(16, g_gslots[G_REPAIR])`），否则 49 个 repair 槽位最多只有 16 个有活。2026-11-21 实测（253 槽位/target.png）：单轮 100s `ok[paint=5935 repair=569 sonar=470]`；`--paint-loop 1` 120s `passes=1 ok[paint=6852 repair=114]`——绘制期间 repair 仍会空闲，因为每笔 `OK` 都自带该像素的新鲜声纳、`dmg=0`，而 pass 完成后 1s 就清 `g_done`，repair 没有复检窗口。 **NewSch1122 例外（2026-11-22）**：`--scheduler newsch1122` 的声纳已不再用列条带/`stride` 采样，而是用 `src/scheduler/sonar_tiling.cpp` 把菱形声纳密铺到 bbox（π=(R+1)+Ri 的格，N=2R²+2R+1），每槽按块负责连续中心、块内蛇形；中心必须落在 `known` 像素上，漏覆盖的 known 像素用自身中心补射。Classic 行为未变，仍是本条描述的条带扫描。

5. **NewSch1122 repair 匀配（2026-11-22）**：`repair_loop` 把末尾 16 个 repair 槽当自由槽，仍按原来的 damaged/stale 全局窗口扫描；其余 repair 槽按 `partition_sonar_centers` 绑定声纳中心（槽少时连续分块，槽多时多个槽共享同一中心），每轮主动重画自己负责的中心。绑定 op 带 `affinity`，`step` 只让该槽接；自由 op 也在自由槽间轮转 affinity。**`Plan::claim` 对 `done_` 像素返回 false，repair 必须用新增的 `Plan::claim_repair`（只查 `enq_`）**，否则修复永远入不了队。Classic 仍用旧 claim，本处行为只在 NewSch1122 生效。

6. **NewSch1122 paint 矩形分块（2026-11-22）**：paint 不再全局行优先扫画布，而是把目标 bbox 划成 `kx*ky` 个近似方形矩形（`paint_loop` 里的 `choose_paint_grid`/`build_paint_blocks`），每块交给一个 paint slot，块内逐行蛇形（奇数行反向），op 带 `affinity=槽 id`；块号 `block=by*kx+bx` 行优先，块→槽由 `paint_slot_for_block` 决定（`blocks<=slots` 一块一槽，否则每槽一段连续块号）。`step` 的 Paint 分支只让该槽接，并按队列顺序取（FIFO），保持相邻两笔只差一个像素。Paint/Sonar 未派发的 op 重排队时 `due` 保持 0，不能用 `now+20ms`，否则新 op 的 due=0 会插到重排 op 前面，蛇形顺序被打乱。`--paint-loop` 重刷时 `plan_.reset()` + 所有块 cursor 归零，每个槽仍只重画自己的块。捆绑变化触发 `drop_paint_queue()` 并按新槽位重建分块。**paint 预算已自适应（2026-11-22 起）**：每波 `paint_budget=max(round_cap, blocks)`、队列上限 `max(queue_cap, blocks+256)`，`blocks>round_cap` 不再只服务前缀块；日志会打 `[paint] note ... budget is raised to blocks`。同波内仍按块号行优先入队，所以上排块先入队（微秒级）；若线上仍看到“从上到下分批”，先用 `IKAPB_PAINT_LOG=1` 看 grid/block/enqueue 日志，确认是客户端顺序还是服务端 tick 呈现，不要直接改分块。设 `IKAPB_PAINT_LOG=1` 会打印 grid/每块矩形/每槽首末像素/每次入队/每轮计数。

**冷却结果必须重试同一像素**：`apply_tick` 收到 `COOLING`（st=1）时，不能丢弃 op 并释放 `g_enq`；要保留 op、`due = now + value + 20ms` 重新入队。丢掉的后果是生产者立刻换一个（更远的）像素重新认领，而客户端并不知道画笔真实位置（`p_has=false` 时 d 按 0 估），整条连接会在「发送→冷却→丢弃→重认领」上空转：2026-11-21 实测修复前 20px 图 240s 只有 715 笔 OK（冷却日志每 50 条打 1 条，共 411 行 ≈ 2 万次冷却），修复后 70s 就有 581 笔 OK、队列从 2000+ 降到 23。

**配套过滤**：`dispatch_ops` 里 `valid = (grp == G_PAINT) ? !done : true`。paint-loop 重置 `g_done` 后残留的 sonar/repair op 不能丢（它们画的就是目标色，扫未完成像素也有益）；只有 paint 组自己的 op 要按 `done` 丢弃并释放 `g_enq`。

**整轮重画必须让 paint 先起跑**：`--paint-loop` 在 `target_complete()` 后、清 `g_done` 之前先调 `q_drop_nonpaint()`，把队列里还没发出的 G_REPAIR/G_SONAR op 全部撤掉并释放它们的 `g_enq`。不这么做的话，上一轮末尾/复检留下的修复和声纳操作会一直占着共享 `g_enq`；paint 生产者又受 `q_queue_cap` 限制，表现就是“整轮画完了，却要等修复/声纳跑完一轮才重新开始画”。撤掉的操作不用补——声纳/修复生产者下一轮会重新认领（2026-11-21 起生效，日志里 `pass N complete ... (dropped M queued repair/sonar op(s))`）。

**队列计数只在“消费”时释放**：`q_take_due` 把 op 取到 `dispatch_ops` 的 `live` 缓冲区时**不能**减 `q_count`/`g_qc`；只有真正发送（放进 `wk.pend`）或过滤丢弃时才 `q_consumed()`。操作取出后仍在派发中，`requeue` 用 `q_requeue()` 只插回、不动计数。2026-11-21 实测反例：旧实现一取出就减计数，paint 生产者（另一线程）看到队列近乎清空就补到 1792，第 4 步又把未派发的 op 塞回去，真实队列一路涨到 4000+ （软上限 2048 失效）；改为“消费才减”后 30s 实机 `q` 最大 1792、`qc[p]+qc[r]+qc[s] == q`。重连时用 `q_recount()` 按真实队列重置，覆盖发送异常导致的只减不消失。

**验收**：改完跑 `timeout 120 ./main --image target_10.png --offset 534,465 --paint-loop 1 --daemon`，`passes` 应持续增长（120s 内 ≥3 轮）、无 `error`；再跑离线 `replay_main` + `cmp`（PPM 必须一致）。只看 `target_done` 接近 `need` 不够——退回 `need − 4` 就是饿死复发。声纳侧：30s 内 `[sonar]` 的 OK 下标应循环覆盖网格全部 6 个点；若只在 2 个点之间交替，说明又被“就绪过滤”卡住了。

### 可用的离线基准（都已实测）

| 快照 | 增量 | 回放结果 | 期望 |
| --- | --- | --- | --- |
| `.protocol/t_snap.zst`（codec1，ver=24144588） | `.protocol/t_delta.zst`（3 条 PUBLIC，到 24144972） | `.protocol/ref_t.ppm` | `replay_main` 输出逐字节相同 |
| `.protocol/snap_full.zst`（codec1，ver=24149324） | `.protocol/catchup.zst`（2 条 PUBLIC，到 24149580） | `.protocol/public_board.ppm` | `pub_apply` 输出逐字节相同 |
| — | `.protocol/delta_proxy.zst`（3 条 PUBLIC） | `/tmp/changes.csv` | `records=3 entries=203322` |

- `.hdr` 是当时的 HTTP 响应头，`.zst` 是响应体；配对的版本号看 `x-board-version`。`meta_live.json` 是当时的 `/api/meta`。
- `.protocol/board.zst` 是**多条 zstd frame 拼接**的抓包，`zstddec` 强制“恰好一个 frame”，直接解会报 `zstd: trailing bytes after frame`——先按 frame 拆开再解。单 frame 的离线样本用 `t_snap.zst` / `t_delta.zst` / `snap_full.zst` / `catchup.zst` / `delta_proxy.zst`。
- `ref_t.ppm`、`public_board.ppm` 是回归基准，不要覆盖；新样本另起文件名。

## 4. 构建与验证（改代码前后都按这里跑）

### 编译 `main`
```bash
make            # 生成 ./main（src/app/main.cpp + 全部 src/）
make test       # 构建全部测试二进制（不自动运行）
make test-fake-server  # 本地 TLS/WS 假服务端 v1：状态机 + NewSch1122 完整小目标
```
- `zstd.h` 来自无 root 解包的 `.deps/zstd-dev/usr/include`（Makefile 已带 `-I`）；运行时链接系统 `libzstd.so.1`（dev 包装出来的 `.so` 软链可能不存在，别改成 `-lzstd` 后不检查）。
- `stb_image.h` 已放在 `.deps/stb/`，由 `src/scheduler/target_builder.cpp` 相对包含，**不需要额外 `-I`**。
- 旧单文件实现已移到 `legacy/main_legacy.cpp`，不参与构建；需要对照历史行为时直接读它，不要把新代码改回去。

### 离线回归（不联网，改 `src/` 后必跑）
```bash
make test
./testfiles/protocol_test
./testfiles/tick_selftest
./testfiles/board_test
./testfiles/queue_test
./testfiles/fake_transport_test
./testfiles/classic_target_test
./testfiles/classic_scheduler_test
./testfiles/new_sch1122_test
./testfiles/new_sch1122_sonar_test
./testfiles/new_sch1122_paint_test
./testfiles/new_sch1122_repair_test
./testfiles/sonar_tiling_test
./testfiles/cli_test
./testfiles/alternate_scheduler_test

# 公开画面基准：期望 since=24144972 且 PPM 逐字节一致
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm && echo "replay MATCH"

# 快照 + 增量基准：期望与 public_board.ppm 一致
./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm && echo "pub_apply MATCH"
```
- `pub_apply` / `pub_decode` 是独立探针，编译方式见 `testfiles/README.md`。
- `pub_apply` 的第二个参数可以是 `-`；304 的空 body 不能丢给 `zstddec`。
- 多 frame 样本的反例：`./testfiles/pub_decode .protocol/board.zst /tmp/x.csv 1000 600` 会以 `decompress: zstd: trailing bytes after frame` 失败，这是预期行为。

### 线上冒烟（只在需要时）
```bash
# 直连不通时用代理（tcSocket 认 HTTPS_PROXY，走 HTTP CONNECT）
HTTPS_PROXY=http://127.0.0.1:7890 ./testfiles/web_test

timeout 30 ./main             # 纯公共流：打印 RULES，随后 SNAPSHOT/PUBLIC

# 绘画：默认自动读 ./tokens.json（测试服务明文）；也可用 --token / IKAPB_TOKEN 覆盖
HTTPS_PROXY=http://127.0.0.1:7890 ./main --dry-run        # 绑定 + 打印画笔
./main --draw 100,100,FF0000 --limit 1 --sonar-out /tmp/s.ppm
./main --image target.png --limit 1000                   # png/jpg 走 stb；PPM 也行
./main --token-paint <uuid> --token-repair <uuid> --token-sonar <uuid> --ppm t.ppm
```
- `main` / `web_test` 默认**直连** `pb.ika.zip:443`；本机实测直连会 `TLS handshake timeout`，设 `HTTPS_PROXY` 后 `tcSocket` 先 `CONNECT` 再做 TLS。只支持 `http://host:port` 形式的代理，不支持代理认证。
- `--dry-run` 绑定后只读一帧 TICK 就退出，适合确认 token / 槽位 / 画笔；不带 token 时 `main` 行为与旧的纯公共流一致。
- `--limit N` = **所有组累计收到 `OK` 的绘画数**（含绘制/修复/声纳探测）；分组统计见退出日志。
- **验证落笔只看两样**：`[work] OK ... group=...` 是否持续增加、以及随后的 `brush` 是否推进；要颜色证据就设 `--sonar-out` 看声纳中心。`--board-out` / `GET /api/board` 是公共延迟画面，`public_delay_ms` 之内看不到刚画的像素（实机 60 秒），别拿它当验收。
- 服务器 `4003` 表示下行积压（没及时读），`4002` 表示消息/载荷超限，`4000` 表示格式错误；先按 `ikapb_protocol.md §6` 对字节，不要盲目重试。

## 5. 协作约定

1. **单一权威**：协议字节表只改 `ikapb_protocol.md`；实现建议只改 `ikapb_impl_guide.md`；本文件只放入口与不变量。
2. **区分“文档写的”与“实测的”**：实测差异必须带样本/命令（如 `.protocol/*.hdr`），不要静默改文档数值。
3. 用户会手改 `src/`/`include/paintboard/`/`websock.hpp`：**编辑前必须重新读文件**（mtime 会变），不要用旧快照；`old_string` 必须来自最新内容。
4. 验证优先用离线样本 `.protocol/*.zst` 和 `testfiles/`，不要为每次改动重连线上；线上冒烟用 `testfiles/web_test`。
5. 测试期用户明确要求把 token 明文放在 `./tokens.json`（该文件已被 `.gitignore` 忽略，不会入库）；除此之外不要把 token 写进其他提交文件或脚本。正式/公开环境应删除 `tokens.json`，改用 `--token` / `IKAPB_TOKEN`。
6. 声称完成前必须重新编译并跑最小验证（至少 §4 的 `replay_main` + `cmp`，见 `verification-before-completion` 纪律）。
7. 新增/删除 `testfiles/` 程序时同步更新 `testfiles/README.md` 和本文件地图，避免下一个人找不到入口。
8. `.protocol/` 里的 `ref_t.ppm`、`public_board.ppm`、`snap_full.zst`、`t_snap.zst` 是回归基线；要重新抓样本就换新文件名，不要覆盖。

## 6. 快速命令（均在本工作区实测通过）

```bash
# 离线解码 PUBLIC 链 → 变更 CSV（delta_proxy.zst：3 条 PUBLIC，entries=203322）
./testfiles/pub_decode .protocol/delta_proxy.zst /tmp/changes.csv 1000 600

# SNAPSHOT + PUBLIC 增量 → 回放成 PPM（验证历史层/颜色引用逻辑）
./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm

# 看样本的 HTTP 头 / 版本
grep -i 'x-board-version\|content-length' .protocol/t_snap.hdr

# 只读参考 2026 后端（禁止当 2027 用）
git -C repo/ikapb show origin/2026:paintboard/src/ws/codec.rs

# 看本地协议原文（RULES/SNAPSHOT/PUBLIC/TICK/BATCH）
sed -n '203,545p' repo/ikapb/docs/protocol.md
```

## 7. 接手/续写清单（隔几天回来先做这四步）

1. `git -C repo/ikapb log --oneline -3` + `git -C repo/ikapb status --short`：确认还在 2027 分支、有没有别人改过上游。
2. 重读 `include/paintboard/` 与 `src/`（mtime 可能已变）和本文件 §3 的分层核心说明。
3. 跑 §4 的离线回归三条，确认基线仍绿。
4. 看 `.protocol/*.log`（`main_run.log` / `replay.log` / `ref_t.log` / `snap_job.log`）确认上次停在哪一步，再决定动哪里。
