# ikapb 2027 客户端实现手册

> 依据：`docs/protocol.md`（2027 分支，commit `9a1aaa1b97`，2026-11-16 发布）+ 本机对 `pb.ika.zip` 的实测（2026-11-17/18）。
> 字段偏移表见 `ikapb_protocol.md`；传输层用法见 `websock_guide.md`。本手册讲**怎么把它们拼成一个能画画的客户端**。
> 协议数值以 `RULES` / `/api/meta` 为准，下文数字是实测时的值，不要硬编码。

---

## 0. 已有部件

| 文件 | 作用 | 状态 |
| --- | --- | --- |
| `websock.hpp` | TLS + HTTP Upgrade + RFC6455 帧（`tcSocket`） | 编译、连线上、Ping/Pong 实测通过 |
| `zstdwrap.hpp` | `zstddec` / `zstdenc`，强制“恰好一个 frame” | 实测通过 |
| `ikapb_protocol.md` | 五种消息的逐字节偏移表 | 与官方 Python/TS 逐字节对拍过 |
| `testfiles/web_test.cpp` | 最小连线上冒烟 | 通过 |
| `testfiles/pub_decode.cpp` / `testfiles/pub_apply.cpp` | PUBLIC/SNAPSHOT 解码、配方回放、出 PPM | 实测通过 |
| `snap_retry.sh` / `pull_pub.sh` | 快照+增量拉取（走 7890 代理，失败回退直连） | 通过 |

编译（`zstd.h` 来自无 root 解包的 dev 包）：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -I.deps/zstd-dev/usr/include \
    painter.cpp -o painter -lssl -lcrypto -pthread \
    /lib/x86_64-linux-gnu/libzstd.so.1
```

---

## 1. 总体结构

```
main
 ├── http       GET /api/meta、POST /api/paint-token（复用 OpenSSL 那套，或先 curl）
 ├── tcSocket   /ws?public=1&since=<V>，收发 Binary
 ├── zstd       zstddec / zstdenc
 ├── proto      parse_rules / parse_snapshot / parse_public / parse_tick / build_batch
 ├── board      cur / l1 / l2 + 声纳表 K + 目标图
 └── plan       每槽位画笔、冷却、选点、发 BATCH
```

一条连接足够：`public=1` 收公共画面，同一连接上绑定多个槽位绘画。**不要**为每个 token 开连接（IP 级限流共享）。工作连接想省下行可拆成 `public=0` 的绘画连接 + 一条观察连接，但多一条连接就多一份心跳和重连逻辑，第一版不必。

单线程模型（推荐）：

```
for (;;) {
    timeout = 到下一笔可落笔的毫秒数（没有就 1000）
    if (SSL_pending(ws.ssl) || poll(ws.file, POLLIN, timeout))
        handle(ws.recvbin());        // TICK / PUBLIC / SNAPSHOT / RULES
    if (now >= next_send)  send_batch();
}
```

注意 `recvbin()` 是阻塞的：`poll` 只保证有字节，完整一条消息仍可能等一会儿。启动时那条 1.1MB 的 SNAPSHOT 会卡几秒，属正常；稳态 TICK 很小。

---

## 2. 数据模型

```cpp
using vecu = vector<unsigned>;

struct rules_t {
   $d W, H, R, tps;
   $q c0, k;            // 微秒；cooldown_base_us / cooldown_per_px_us
   $q delay_ms, period_ms;
   unsigned max_slots, max_paints, max_message, max_payload;
   $q server_us;        // RULES 末尾的服务器时间，配合 steady_clock 校准
};

struct board_t {
   vecu cur, l1, l2;    // 各 W*H，u32 颜色（内存字节序 R,G,B,A）
   unsigned long long ver;   // 已应用到的公共版本（tick 号）
};

struct brush_t {          // 每个槽位一份
   $d idx = -1;           // 上次成功落笔的像素，-1 = 还没画过
   $q t = 0;              // 那次落笔的服务器时间（us）
};

struct slot_t {
   bool used = false;
   array<u$b,16> tok;     // paint token
   brush_t br;
   bool pending = false;  // 有已发出、未收到结果的绘画/绑定
};
```

声纳表按文档 §3.8：`Kc[p]`（颜色）+ `Kt[p]`（tick），配一个 `(p, tick)` FIFO；`V ≥ t` 时从队头弹出。渲染时 `K` 优先于公共画面。

---

## 3. 启动顺序

1. **`GET /api/meta`**：拿 `W/H`、`rules.*`、`limits.*`、`public_version`、`issuance.access_mode`。
2. **`POST /api/paint-token`**：脚本用 `{"access_key":"<43 字符>"}`；返回 `uid` 和 UUID 文本，转成 16 字节。
   - 重新申领会**立刻作废旧 token**，所有连接上的绑定被释放。
   - 不要把 token / access key 写进日志、仓库。
3. **连 `/ws?public=1&since=<V>`**：首连不带 `since` 或带 `meta.public_version` 均可；断线重连带自己最后应用的版本。
4. **收 `RULES`**：用它的值覆盖 meta（参数热调时服务器会重发）。
5. **收 `SNAPSHOT` 或 `PUBLIC`**：填 `cur/l1/l2`，记版本。
6. **发第一条 BATCH**：绑定槽位 `(slot, token)`；结果在下一 tick 的 TICK 里。
7. **开始画**：等画笔条目到达，再按冷却发绘画。

`RULES` 是连接后第一条消息；绑定和绘画可以同一条 BATCH 发，服务器按“先全部绑定、再全部绘画”处理。

---

## 4. 编解码

### 4.1 zstd 层

- 一条 WebSocket Binary Message = **恰好一个完整 zstd frame**。多 frame、尾随字节、字典都会 `4000`。
- 压缩后 ≤ `max_message_bytes`（1 MiB），解压后 ≤ `max_payload_bytes`（1 MiB）。
- `zstddec` 已检查 `ZSTD_findFrameCompressedSize` 等于输入长度，别绕过。

```cpp
stream raw;
zstddec(msg, raw, 1u << 20);     // 失败抛 runtime_error
u$b type = raw[0], ver = raw[1];
```

### 4.2 BATCH（唯一上行）

```cpp
fn put16(stream &s, unsigned v) { s.push_back(v & 255); s.push_back((v >> 8) & 255); }
fn put32(stream &s, unsigned v) { for ($d i = 0; i < 4; ++i) s.push_back((v >> (8 * i)) & 255); }
fn align4(stream &s) { while (s.size() & 3) s.push_back(0); }

// binds: (槽号, 16 字节 token)；paints: (槽号, idx, 颜色)
stream build_batch(const vector<pair<$d, array<u$b, 16>>> &binds,
                   const vector<pair<$d, pair<$d, unsigned>>> &paints) {
   stream s;
   s.push_back(0x01); s.push_back(1); put16(s, 0);          // 类型/版本/保留
   put32(s, binds.size()); put32(s, paints.size()); put32(s, 0);
   for (auto &b : binds) put16(s, b.first);                 // bind.slot
   align4(s);
   for (auto &b : binds) s.insert(s.end(), b.second.begin(), b.second.end());
   for (auto &p : paints) put16(s, p.first);                // paint.slot
   align4(s);
   for (auto &p : paints) put32(s, p.second.first);         // paint.idx
   for (auto &p : paints) put32(s, p.second.second);        // paint.color
   ret s;
}
```

长度 `L = 16 + pad4(2·nb) + 16·nb + pad4(2·np) + 8·np`。发送前本地校验：槽号 `< max_slots`、`idx < W·H`、`color >> 24 == 255`、补齐字节为 0。任何一项错都是 `4000`，**且计入 IP 违规（10 分钟 10 次自动封 IP）**。

### 4.3 下行分派

```cpp
switch (raw[0]) {
   case 0x84: parse_rules(raw); break;      // 96 字节定长
   case 0x83: parse_snapshot(raw); break;   // codec 0/1
   case 0x82: parse_public(raw); break;     // 必须 from == 当前 ver
   case 0x81: parse_tick(raw); break;       // 结果 + 声纳 + 画笔
   default: break;                          // 未知类型直接忽略
}
```

### 4.4 PUBLIC 应用（最容易写错）

```cpp
for (每段 j) {
   unsigned k = 0;
   for (i = 0; i < seg_n[j]; ++i) {
      unsigned d = dsmall[cursor++];
      if (d == 255) d = dbig[dcursor++];
      k += d;                       // 段内独立前缀和
      $d idx = (order == 0) ? $d(k) : $d((k % H) * W + k / H);
      unsigned w = color[cursor2++];
      unsigned old = cur[idx];
      unsigned nw = (w == 0) ? l1[idx] : (w == 1) ? l2[idx] : (w == 2) ? old : w;
      if (nw == old) continue;      // 原地重绘：层不动
      l2[idx] = (l1[idx] != old && l1[idx] != nw) ? l1[idx]
              : (l2[idx] != old && l2[idx] != nw) ? l2[idx] : nw;
      l1[idx] = old;
      cur[idx] = nw;
   }
}
ver = to;
```

实测（2026-11-18，3 秒窗口）：`n=202260` 条里 `w=0` 约 4.7 万、`w=1` 几百、`w=2` 约 1.4 万、绝对颜色 0——画板长期是两方抢同一个颜色，颜色列几乎全是配方。

### 4.5 TICK

头 32 字节：`R, nr, tick, nb, nk, ns`，然后 `brush.t[nb]`（u64）、`brush.idx[nb]`（u32）、`brush.slot[nb]`（u16，pad4）、`status[nr]`（u8，pad4）、`value[nr]`（u32）、`sonar.idx[nk]`（u32）、`sonar.color[nk·A]`、`lost.slot[ns]`（u16，pad4）。

结果按处理顺序排列、**没有请求 ID**：本地维护发送队列，收到 TICK 依次出队。`status`：

| 码 | 名称 | 处理 |
| --- | --- | --- |
| 0 | OK | 画笔更新；若带声纳，写 `K` |
| 1 | COOLING | `value` = 到该目标还要等多少微秒；按它等，别下一 tick 盲目重试 |
| 2 | NO_PAINT_TOKEN | 槽位没绑上，重新绑定 |
| 3 | BAD_PAINT_TOKEN | token 失效（被顶掉），重新申领 |
| 4 | BANNED | 停手，别刷 |
| 5 | CLOSED | 活动没开/已结束 |
| 6 | IP_LIMIT | 本 IP 同时绑定的 token 上限，`value` 是上限 |

槽位表的更新顺序必须照文档 §5：**先按结果顺序应用绑定/解绑 → 再清 `lost.slot` → 最后套画笔条目**。乐观更新画笔时，如果该 token 还有未回结果的绘画，画笔条目可能还没包含它：不要覆盖，记成“那次被拒时要恢复的值”。

---

## 5. 调度器

### 5.1 冷却公式（决定一切）

```
ready = brush.t + c0 + k · manhattan(brush.idx, target_idx)
```

实测 `c0 = 1s`、`k = 0.1s/格`。含义：

- 一个 uid **每 `c0` 只能成功落一笔**（同一 tick 内同一 uid 的第二笔必然 `COOLING`，因为 `now` 没变、`d` 再小也要等 `c0`）。
- 所以"多画几笔塞进一条 BATCH"对同一个 uid 没有意义；BATCH 的并行度来自**多个槽位/多个 uid**。
- 走远路很贵：10 格 = 多等 1 秒；100 格 = 多等 10 秒。**别全局按行扫描**，要就近成片地修。

### 5.2 选点

维护待修集合 `P = { p | 已知颜色(p) != target(p) }`（`已知` = 声纳优先，其次公共画面）。对每个可用槽位：

1. 由画笔算 `ready`；若 `now < ready`，这笔现在发必 `COOLING`，先跳过。
2. 在 `P` 里找 `manhattan` 最近的像素；距离相同取更早进入队列的。
3. 生成 `(slot, idx, target_color)`，按槽位聚成一条 BATCH。

改进（第二版再做）：

- 按连通块/矩形分块，块内蛇形走，块间才长途转移；总时间 ≈ `n·c0 + k·tour`，`tour` 用蛇形大约是最优的 1.1–1.3 倍。
- 对每个槽位维护独立的最近点队列，避免所有槽位挤同一块。
- 已知颜色是 60 秒延迟的公共画面，可能已经被人改掉；**不要因为"看起来已经对了"就跳过**——除非它出现在声纳表 `K` 里（那是实时的）。

### 5.3 发送节奏

- 一个 tick 一条消息就够（`msg_rate = 128/s`，`tps = 128`）。多发只会排队。
- 消息里可以混多个槽位的绘画；每条绘画 10 字节。
- 服务器按 IP 还有记录桶（`ip_record_rate`，默认 20 万/秒），正常单人远够。
- **必须持续读下行**：积压 >16 MiB 或最老一条 >10 秒 → `4003` 断开。公共流读得慢时服务器会自动改发快照，不会撑爆，但你要读。

### 5.4 结果处理

- `OK`：画笔已在 TICK 的 brush 列里（权威），本地只需把待修集合里的该像素按声纳颜色更新。
- `COOLING`：把这个槽位的 `next_try = now + value`；可换更近的目标立刻重试（`value` 只对那个目标有效）。
- `NO_PAINT_TOKEN`/`BAD_PAINT_TOKEN`：重新绑定/申领，绑定成功后同一条 TICK 里就带画笔。

---

## 6. 目标图与战争迷雾

- `stbi_load(..., 4)` 拿 RGBA；先按背景色合成 alpha，再转 `u32 = R | G<<8 | B<<16 | 0xFF000000`。**A 不是 255 的消息整条被拒**。
- 缩放到 `W×H`（最近邻就够；要好看就用面积平均）。
- 实时视野只有两处：自己落笔后的 13 格声纳（`R=2`），和 60 秒延迟的公共画面。
- 侦察要花一笔：原地同色重涂也算一次成功落笔，会给声纳，但同样吃满 `c0` 冷却。盯着某块区域时，让已经冷却好的槽位定期在那里落一笔。
- 渲染/调试：把 `cur`（或 `K` 覆盖后的画面）写 PPM，比 print 有用得多。

---

## 7. 连接管理

- **超时**：默认 15 秒读超时对公共流太短——实测服务器发大快照时会出现 10–40 秒空档。进公共流前调大：

  ```cpp
  ws.conn(host, "443", "/ws?public=1&since=" + to_string(ver));
  ws.set_timeouts(120, 10);      // 读 120s、写 10s
  ```

  超时/异常一律视为断线：销毁、退避、带 `since=ver` 重连。
- **`since` 的语义**：服务器保留 2 分钟的增量批次。太旧 → 直接回一条 `SNAPSHOT`（1.1–1.2MB，`0x83`），这是正常的，不是错误。
- **心跳**：服务器每 10 秒发 WebSocket Ping，`tcSocket` 自动回 Pong。别自己停读。
- **关闭码**：`4000` 格式错、`4002` 过大（这两种计违规，10 次/10 分钟封 IP）、`4003` 下行积压、`4004` 心跳、`4005` IP 封禁。收到就停手，别立刻重连刷。
- **TLS 握手慢**：实测 0.2–10 秒，偶发 >30 秒；握手期用 30 秒超时（`conn()` 内已如此）。本机可走 `127.0.0.1:7890` 代理，快照 1.1MB 约 2 秒。

---

## 8. 多 token

- 一条连接 `0 … max_slots-1` 个槽位（实测 65536），每个槽位绑一个 token；同一连接内一个 token 只占一个槽位。
- 冷却按 **uid** 算，不按槽位/连接：同一个 token 多开连接不会更快。
- 一个 uid 同时只有一个有效 token；重新申领会顶掉旧的并释放所有绑定。
- 同 IP 的 token 上限由 `abuse.max_paint_tokens_per_ip` 控制（默认 0 = 不限）；`IP_LIMIT` 的 `value` 就是上限。
- 解绑用 16 个 0 的 token；绑定新 token 会顶掉同连接同槽位的旧 token，换 token 时注意“先解绑再绑定”才能避开 IP 上限计数。

---

## 9. 验收清单

**离线**

1. `zstddec` 对“多 frame / 尾随字节 / 截断”都报错。
2. `build_batch` 与官方 Python `encode_batch` 对 4 组用例逐字节相同（已有 `testfiles/encbatch_cmp.cpp`）。
3. TICK 解析对 `testfiles/tick_probe3` 的两条真实样本：解绑 → `status=0`；坏 token → `status=3`；长度 40 与公式一致。
4. PUBLIC 解码对真实增量：`from` 等于上一个版本、每段前缀和、`order=1` 用例。

**线上**

5. `./testfiles/web_test` 连 `/ws?public=0`：101、Ping/Pong、收 71 字节 RULES。
6. 绑定槽位 → TICK 带画笔（`brush.t=0, idx=0`）。
7. 发一笔近距离 → `OK` + 13 格声纳；立刻再发同一 uid → `COOLING` 且 `value ≈ c0 + k·d`。
8. 等 `value` 后再发 → `OK`。
9. 快照 + 紧邻增量叠出来的图与 `snap_retry.sh` 出的图一致（同一版本）。

---

## 10. 踩坑表（都是实际踩过的）

| 坑 | 现象 | 处理 |
| --- | --- | --- |
| `Bytes payload(size_t(n));` | most vexing parse，编译报 `non-class type` | 加 `static_cast<size_t>` 或换 `{}` |
| `#define ret return` 与 OpenSSL 形参 `ret` | 头文件里 `expected ',' before 'return'` | 先 include OpenSSL 头，再 include 宏板子 |
| 全局 `struct bind` | 与 POSIX `::bind` 冲突，`expected a type` | 改名（`bt`/`pt`） |
| `___main` | 它不带参数，`signed main(int,char**)` 不能用宏 | 直接写 `signed main($d argc, char **argv)` |
| `stream` | 只在 `websock.hpp` 里 typedef | 单独文件顶部补 `using stream = vector<u$b>;` |
| zstd 多 frame | 服务器 `4000` | 解压前用 `ZSTD_findFrameCompressedSize` 校验“一个 frame、无尾随” |
| alpha ≠ 255 | 整条 BATCH 被拒 + 违规 | 发前断言 `color >> 24 == 255` |
| 补齐字节非 0 | 整条被拒 | 每列 pad4 手动补 0 |
| `order=1` | 像素错位 | `i = (k % H) * W + k / H` |
| 段内前缀和 | 颜色全乱 | 每段从 0 重新累加，段首是绝对键 |
| 配方当颜色 | 出现 0/1/2 的“黑色” | `0→l1, 1→l2, 2→不变`，其它才是 ARGB |
| 增量 `from` 比快照版本新 | 画面错色/错位 | 必须用快照自己的版本号当 `since`，或先补到同一版本 |
| `/api/board` 304 | 空 body，解压崩 | 304 视为“无变化”，不要解压 |
| `since` 过期 | 返回 1.1MB 快照 | 正常回退；不要慌 |
| 15s 读超时 | 大快照/公共流空档抛 `SSL_read timeout` | 公共连接读超时调到 120s |
| 只发不读 | 积压 >16MiB/10s 被 `4003` | 每轮都读下行 |
| 同一 uid 连发多笔 | 第二笔起全 `COOLING` | 一个 uid 每 `c0` 一笔；并行靠多槽位 |

---

## 11. 建议的落地顺序

1. `httpget` + `/api/meta`，打印真实规则。
2. `POST /api/paint-token`，拿到 uid + token。
3. 连 `/ws?public=0`，绑定一个槽位，打印画笔。
4. 画一个像素：`OK` + 声纳；再画一次看 `COOLING`。
5. 接公共流，收满 SNAPSHOT，用 `testfiles/pub_apply` 的同一套逻辑维护 `cur/l1/l2`。
6. 接 `stb_image`，读目标图，实现“最近点 + 冷却”调度。
7. 加断线重连（带 `since`）、多槽位、蛇形分块。
8. 最后才考虑多 token/多 IP 协同；单 uid 的物理上限就是 `1/c0` 笔/秒。

---

## 12. `main.cpp` 代码结构（当前实现地图）

> 2026-11-20 起，工作连接从“静态分段 + 蛇形”重构成
> **「带锁的全局操作时间优先队列 + 实施线程 + 三组 token」**。
> 行号会漂，以函数名为准；字节布局仍只看 `ikapb_protocol.md`。

### 12.1 分层与线程

```
main()
 ├ 解析 CLI / tokens.json / 图片(--ppm/--image/--draw)
 ├ 建 wk.sl：paint → repair → sonar，每个 token 一个槽位
 ├ RULES 到达后 build_target()：src_image + --draw → 画布尺寸的 target_image
 └ 线程：
     getWork()        实施线程（独占 public=0）：绑定 → 取队列 → 合批发 → 收 TICK
     paint_worker()   绘制组生产者：补“没画好”的像素
     repair_worker()  修复组生产者：补“声纳与目标差很大 / 声纳证据过期”的像素
     sonar_worker()   声纳组生产者：稀疏重画期望色，换取声纳读数
     getPublic()      public=1 公共流（--observe）
```

- **只有实施线程碰 websocket**。生产者只往全局队列里塞 `op_t`，不碰网络、不碰槽位。
- **槽位状态是单线程的**：`wk.sl` 只在 `getWork()/run_conn()/apply_tick()` 里读写，
  所以 `slot_t` 不带锁。跨线程共享的只有：`q_ops`（`q_mu`）、
  `g_enq/g_done`（`plan_mu`）、`g_target`（RULES 后不变）、`sonar_*`（`canvas_mu`）。
- **锁顺序**：`canvas_mu → plan_mu`。`apply_tick` 只拿 `plan_mu`，`apply_sonar` 只拿
  `canvas_mu`，`repair_worker` 先 `canvas_mu` 再在循环里 `plan_claim`（`plan_mu`）；
  没有任何反向路径。

### 12.2 数据模型

| 结构 / 全局 | 作用 | 要点 |
| --- | --- | --- |
| `op_t` | 队列里的一个操作 | `kind(OP_BIND/OP_PAINT)`、`grp`、`idx`、`color`、`due`、`seq`；`slot` 由实施线程派发时填 |
| `q_ops` + `q_mu` | 全局时间优先队列 | `std::multiset`，按 `(due, seq)` 排序；`q_push / q_take_due` |
| `q_count` | 队列长度 | `std::atomic<size_t>`，用于退出判断与日志 |
| `slot_t` | 一个 token 槽位 | `group`、`want_bind/used`、权威画笔 `a_*`、投影画笔 `p_*`、`not_before`、`inflight` |
| `work_t wk` | 实施线程状态 | `sl`、`pend`（在途 FIFO）、`W/H/c0/k/maxmsg/maxpaints`、`saw_tick` |
| `target_image g_target` | 画布尺寸目标表 | `pix[]` + `known[]`；`known=0` 表示“不想画/不知道” |
| `src_image g_src` | 解码后的源图 | `stb_image` 或 PPM 得到；按 `--offset` 贴到画布 |
| `g_raw` | `--draw` 列表 | 在 `build_target()` 里覆盖到 `g_target` |
| `g_enq` / `g_done` + `plan_mu` | 像素级去重/完成标记 | `enq=1` 表示已有操作在队列/在途；`done=1` 表示已确认达到目标色 |
| `sonar_col` / `sonar_tick` + `canvas_mu` | 声纳表 | `tick=0` 表示从未观测 |
| `g_gslots[3]` / `g_ok[3]` | 每组槽位数 / 每组 OK 数 | 原子量，生产者和 `main` 只读 |
| `g_ready` / `g_stop` | 绑定完成 / 全局停止 | 生产者等 `g_ready`；实施线程退出后置 `g_stop` |

### 12.3 实施线程一次循环（`run_conn`）

```
等 RULES → refresh_cfg() → build_target()（只一次）→ 清槽位态
loop:
  should_stop() ?         // --limit / --dry-run / 目标完成且无修复声纳组
  send_binds(ws)          // 只发 want_bind && !used 的槽位，结果进 pend
  dispatch_ops(ws)        // 取走所有 due<=now，挑槽位、合批、发送、投影前进
  try_recvbin(wire)
    ├ 1 → zstddec → handle_msg → apply_tick；全部绑定后 g_ready=true
    ├ 2 → 抛异常（外层重连；在途操作 requeue_pending）
    └ 0 → poll(WS, g_exec_ms)   // 默认 20ms，即“每段时间执行一次”
```

**`dispatch_ops` 的派发规则**：

1. `q_take_due(now)` 拿到所有到期操作。
2. 去重：`g_enq[idx]` 为 0 直接丢；绘制组跳过 `g_done[idx]=1` 的像素，
   **修复/声纳组允许复查已完成像素**；`--paint-loop` 开启时绘制组也放行（循环重刷）。
3. `pick_slot()`：在同组内挑 `used && !want_bind && inflight<kMaxInflight` 且
   `slot_ready_us() <= now` 的槽位；没有就按最早可重试时间重新入队。
4. 选中后 **立即“释放画笔”**：`p_t=now; p_idx=idx; p_has=true`，同槽位下一条操作
   就会按 `p_t + c0 + k·d` 重新排队，不必等 TICK。
5. 每 4096 条（且不超过 `max_paints_per_message`）合批成一条 BATCH。

**声纳组的固定扫描**（2026-11-20 实机修复）：生产者按 bbox 网格行优先顺序认领
（`k=0..bw*bh-1`，不旋转起点、不再按“离画笔最近”挑）；派发时**严格按 `seq` 队头服务**——
队头没到 `slot_ready_us` 就让槽位等，不允许后入队的近点插队。原因是近点每 `c0` 就绪、
远点要 `c0+k·d` 才就绪，按“就绪中取最小 seq”会让远点永远差一步（实测 4 个远点 30 秒内
一次都没发出）。声纳不再要求 `g_done=1`：它重涂的就是目标色，扫未完成像素也推进度；
跨组占位饿死已由队头 FIFO 消除。实测 OK 序列为
`465534 → 465539 → 470534 → 470539 → 475534 → 475539` 循环。

**`slot_ready_us`**：

```
ready = p_has ? p_t + c0 + k · manh(idx, p_idx) : 0
ready = max(ready, not_before)          // COOLING 给的硬下限
```

### 12.4 结果应用（`apply_tick`）

顺序固定：**结果（按 `pend` 出队）→ `lost.slot` → 权威 `brush` 列 → 声纳**。

| 结果 | 处理 |
| --- | --- |
| `OK` | `inflight--`、计数；`g_enq[idx]=0`；若 `color == target[idx]` 则 `g_done[idx]=1`、`g_done_total++` |
| `COOLING` | `inflight--`；投影回退到权威画笔（`p=a`）；`not_before = srv_now()+value`；释放 `g_enq` |
| `NO_PAINT_TOKEN` | 清 `used`、`want_bind=true` |
| 其它错误 | `g_abort=true`（绑定失败/非法 token 时直接退出） |

`brush` 列是权威值，最后覆盖 `a_*` 并同步 `p_*`；这就是“乐观投影”的校正点。

### 12.5 三个生产者

| 线程 | 选择规则 | 周期参数 |
| --- | --- | --- |
| `paint_worker` | 游标扫 `g_target.known && !g_done && !g_enq`，用目标色入队；每轮最多 `--round-cap` 条。`--paint-loop N >0` 时无视 `g_done`，整图一轮接一轮循环刷，每轮结束等 N 秒 | `--paint-ms`（默认 250ms） / `--paint-loop N` |
| `repair_worker` | 扫 `known` 像素：声纳看到坏色（`colordiff > --repair-thresh`）就修；声纳证据超过 `--repair-verify-ms`（默认 20000，0=关闭）没更新也复检一次。用目标色入队 | `--repair-ms`（500ms） / `--repair-verify-ms N` |
| `sonar_worker` | 在目标包围盒内按 `--sonar-stride`（0=自动 `2R+1`）采样，重画目标色换声纳 | `--sonar-ms`（800ms） |

- 三者都用 `plan_claim()` 做“一个像素同时只有一个操作”的去重。
- `--queue-cap`（默认 2048）是全局队列软上限：生产者看到 `q_count >= cap` 就暂停补货，
  避免一张 5 万像素的图在启动几秒内把整图塞进队列、导致实施线程每秒重排几万条操作。
- 声纳/修复组不再要求 `g_done`：修复组靠“坏色或证据过期”，声纳组靠固定扫描；`dispatch_ops` 对这两组一律放行。派发上只有声纳组按 `seq` 队头 FIFO（固定网格）；绘制/修复组按距离贪心，避免行优先队列跨行时 `k·d` 把槽位卡住（修复每像素只认领一次，不会被饿死）。
- 槽位的本组没有到期 op 时，`dispatch_ops` 允许 repair/sonar 槽位**代发队列里的 G_PAINT op**（同样按距离最近）。这些 token 是独立 uid，受同一套 `c0 + k·d` 约束，但能把闲置槽位变成整图吞吐；`[stat]` 里的 `help=` 是代发笔数（需 `IKAPB_STAT=1`）。
- `--repair-follow N`（默认 2）：把 repair 组前 N 个槽位钉在声纳网格点上，只接这些点的修复 op，空闲不代画；`build_target` 会把网格点算进 `g_sp_pts`（与 `sonar_worker` 的 stride/行优先一致），日志 `[target] sonar posts=…`。`IKAPB_FOLLOW_LOG=1` 时每次派发打印 `[follow] slot=… idx=…`，可核对 idx 是否都在网格点上。
- 目标完成（`g_done_total == g_need_total`）只影响绘制组的补货；有修复/声纳组时进程默认常驻
  （`--daemon` 可显式常驻；`--limit` 是最硬的退出条件）。`--paint-loop` 开启时绘制组不因
  “画完了”而停手，且 `should_stop()` 也不会因此自动退出。

### 12.6 `--limit` 的准确含义

`--limit N` = **所有组累计收到 `OK` 的绘画记录数达到 N 后退出**，包含：

- 绘制组的正常落笔
- 修复组的修复落笔
- 声纳组的探测落笔（重画期望色也会被服务器判 `OK`）

退出时 `[main] painted=... ok[paint=.. repair=.. sonar=..]` 会分组打印，便于区分。
如果只想限制“真正画目标”的次数，目前请只用绘制组 token（或看分组日志自行停止）。

### 12.7 图片与 token 入口

| 入口 | 说明 |
| --- | --- |
| `--image <path>` / `--ppm <path>` | 先试 PPM，再走 `stb_image`（png/jpg/bmp/...）；统一转成 `R\|G<<8\|B<<16\|0xFF000000`，半透明按白底混合 |
| `--offset x,y` | 源图贴到画布的左上角 |
| `--draw x,y,RRGGBB` | 单像素覆盖，优先级高于图片 |
| `--tokens <file.json>` / 默认 `./tokens.json` | 读 `{"paint":[...],"repair":[...],"sonar":[...]}`；测试服务允许明文 |
| `--token* / IKAPB_TOKEN` | CLI/env 入口，仍可用；与 tokens.json 叠加 |

> 图片解码头文件是仓库内 `.deps/stb/stb_image.h`（无 root 下载的单头文件），
> `main.cpp` 用相对路径 include，不需要额外 `-I`。

### 12.8 必须记住的不变量

1. `procRule` 从 **version**（偏移 1）读，其余 `procXxx(readptr&)` 由调用方先读掉 type。
2. `TICK` 没有请求 id：`status[k]` ↔ `pend` 第 k 条；结果不足时保留队列。
3. 结果应用顺序：结果 → `lost.slot` → 画笔。
4. 画笔权威值只来自 `TICK`；本地 `p_*` 只是调度投影，`COOLING`/brush 列负责校正。
5. `--draw` 的十六进制是 RGB；内部低字节是 R（`R,G,B,A` 内存序）。
6. 工作连接必须用 `try_recvbin` + `poll`，否则会在冷却期间卡住。
7. 公共画面落后 `public_delay_ms`，不能用来验收刚落笔的像素；实时证据只有声纳。
8. `g_enq`/`g_done` 必须成对维护；漏清 `g_enq` 会让像素永久消失（生产者不再入队）。

### 12.9 已知边界

- **运行中 `W/H` 变化**：`procRule` 会重建画布，但 `g_target`、`wk.W/H` 不会重建。
  真要支持需重新 `build_target()` 并处理 `--ppm` 尺寸。
- **同 token 多槽位是同一个 uid**：三组里不要重复放同一个 token，否则会互相抢冷却。
- **声纳组会重画目标色**：如果目标图本身是错的，探测会把像素“按目标修回去”；
  这是设计使然，不想改图就别把该像素标成 `known`。
- **修复阈值越高越保守**：`--repair-thresh` 是 RGB 曼哈顿和（0..765），默认 96。
- **`wk.sl.size() > max_slots`** 直接 `g_abort`；`max_slots` 以最新 `RULES` 为准。

---

## 13. 调度策略参考（接口 + 三种策略）

> **状态：部分已被 §12 的新实现取代。** 2026-11-20 的 `main.cpp` 已经落地了
> 「全局操作队列 + 实施线程 + 三组 token」：`dispatch_ops` 负责派发、`pick_slot`
> 按组选槽、`slot_ready_us` 现算冷却、投影画笔即“释放画笔”。本节保留策略 A/B/C
> 作为**进一步替换 `pick_slot` 的参考**，不再描述当前代码。

### 13.1 先明确：策略能改什么、不能改什么

**不能改（协议硬约束）**

| 约束 | 出处 |
| --- | --- |
| 接受条件 `τ ≥ t_u + c0 + k·|i − p_u|₁` | §1.3 |
| 冷却按 uid，不按槽位；同 token 多槽位不加速 | §1.3 / §3.5 |
| 被拒（`COOLING`）不移动画笔 | §1.3 |
| 一笔一个 tick，同 tick 多笔只有第一笔能成 | §1.4 |
| 画笔只能由 `TICK` 的 `brush` 条目更新 | §3.8 |
| `OK` 的绘画才更新 `brush`，`--limit` 按 `OK` 计 | §3.6 / §1.3 |

**能改（策略层）**

1. 挑哪个作业（像素 + 颜色）
2. 用哪个槽位 / 哪个 uid
3. 现在就发，还是等多久再发

### 13.2 接口草稿

目标：把“决策/发送”分层。**当前代码已用 `dispatch_ops()`（发送）与 `pick_slot()`（选人）
实现了这个分层**；下面保留一版更纯粹的 `policy_plan()` 形状，供继续替换策略时参考。

```cpp
// ===================== 调度策略接口（草稿） =====================
static const size_t NPOS = (size_t)-1;

struct plan_item { $d slot; size_t job; };     // job 是 wk.jobs 的下标

struct sched_view {
   u$q now;                    // srv_now()，0 = 服务器时钟尚未校准
   u$d W, H;
   u$q c0, k;                  // 当前 RULES 的冷却参数
   vector<slot_t> &sl;
   vector<job_t>  &jobs;
   vector<char>   &done;       // 1 = 已经 OK 过
   vector<char>   &inflight;   // 1 = 已发出，结果未回
};

static u$q manh(u$d a, u$d b, u$d W) {
   $d ax = a % W, ay = a / W, bx = b % W, by = b / W;
   ret (u$q)(ax > bx ? ax - bx : bx - ax) + (u$q)(ay > by ? ay - by : by - ay);
}

// 画笔为空 → 一定就绪；否则按目标现算，不读任何 absolute wait 状态
static bool ready_for(const sched_view &V, const slot_t &s, size_t j) {
   if(!s.has_brush) ret true;
   u$q r = s.brush_t + V.c0 + V.k * manh(V.jobs[j].idx, s.brush_idx, V.W);
   ret V.now && V.now >= r;
}

// 轮的：k > 0 时，现在最多能画多远；k == 0 视为无限
static u$q max_dist(const sched_view &V, const slot_t &s) {
   if(!s.has_brush) ret ~(u$q)0;
   if(V.k == 0) ret ~(u$q)0;
   if(!V.now || V.now < s.brush_t + V.c0) ret 0;
   ret (V.now - s.brush_t - V.c0) / V.k;
}

static void policy_plan(const sched_view &V, vector<plan_item> &out);

// 可替换的策略入口
using policy_fn = void (*)(const sched_view &, vector<plan_item> &);
static policy_fn g_policy = policy_segmented;   // 运行时由 --strategy 选
```

**执行端（改 `try_send`，逻辑不变，只是不再自己挑）**

```cpp
static void try_send(tcSocket &ws) {
   sched_view V{srv_now(), wk.W, wk.H, wk.c0, wk.k,
                wk.sl, wk.jobs, wk.done, wk.inflight};
   vector<plan_item> plan;
   g_policy(V, plan);

   for(auto &p : plan) {
      if(g_sig || g_abort) ret;
      slot_t &s = wk.sl[p.slot];
      if(s.busy || s.cur_job != NPOS) continue;
      if(V.done[p.job] || V.inflight[p.job]) continue;

      job_t &j = wk.jobs[p.job];
      stream pl = enc_batch({}, {{p.slot, j.idx, j.color}});
      stream wire; zstdenc(pl, wire, 1);
      if(wire.size() > wk.maxmsg) { g_abort = true; ret; }
      ws.sendbin(wire);

      s.busy = true;
      s.cur_job = p.job;
      V.inflight[p.job] = 1;
      wk.pend.pb({K_PAINT, p.slot, j.idx, j.color, p.job});
   }
}
```

**配套的 5 处改动（清单）**

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | `slot_t` | 加 `size_t cur_job = NPOS;`；`lo/hi/cur` 仅 `policy_segmented` 用，可保留 |
| 2 | `pending_t` | 加 `size_t job;`（绑定记录填 `NPOS`） |
| 3 | `work_t` | 加 `vector<char> done, inflight;` 和 `u$d remaining;` |
| 4 | `apply_tick` | `OK` → `done[job]=1; inflight[job]=0; remaining--; cur_job=NPOS;`<br>`COOLING` → `cur_job=NPOS;`（**不再写 `wait_until`**） |
| 5 | `run_conn` / `getWork` 的退出判断 | 由「所有槽位 `cur>=hi`」改成 `wk.remaining == 0` |

> 关键点：**删掉 `wait_until` 这类绝对时间**。它记录的是「针对上一次目标」的值，
> 换目标或别人用同一 token 画了之后就会偏长，导致空等（见 §12.5）。

### 13.3 策略 A：分段蛇形（基线，复刻现状）

```cpp
static void policy_segmented(const sched_view &V, vector<plan_item> &out) {
   for($d sid = 0; sid < ( $d )V.sl.size(); sid ++) {
      slot_t &s = V.sl[sid];
      if(!s.used || s.busy || s.cur_job != NPOS) continue;
      while(s.cur < s.hi && V.done[s.cur]) s.cur ++;     // 跳过已完成的
      if(s.cur >= s.hi) continue;
      if(!ready_for(V, s, s.cur)) continue;
      out.pb({sid, s.cur});
   }
}
```

- 优点：零开销、路径局部最优、多槽位天然均衡（等分）。
- 缺点：不能跨槽位换活、不能重排、`done` 判据要线性跳过。
- 用途：**回归基线**。任何新策略必须证明不慢于它。

### 13.4 策略 B：波前 / 最近邻（单 token 的推荐起点）

核心观察：就绪条件可以反解成「距离预算」——

```
d ≤ (now − brush_t − c0) / k  = max_dist()
```

所以「挑最近的未画作业，且距离在预算内」就是全部逻辑；找不到就等
「最近那个作业变得可画」的时间。

```cpp
// 桶网格：把画布切成 BS×BS 像素的格子，桶里存该格的作业下标
static const u$d BS = 16;
static vector<vector<size_t>> buckets;   // 尺寸 bh * bw
static u$d bw, bh;

// 桶内挑距离最短且未完成、未在途的
static size_t best_in_bucket(const sched_view &V, $d bx, $d by,
                             u$d cx, u$d cy, u$q &bestd) {
   if(bx < 0 || by < 0 || bx >= ( $d )bw || by >= ( $d )bh) ret NPOS;
   size_t best = NPOS;
   for(size_t j : buckets[by * bw + bx]) {
      if(V.done[j] || V.inflight[j]) continue;
      u$q d = manh(V.jobs[j].idx, cy * V.W + cx, V.W);
      if(best == NPOS || d < bestd) { bestd = d; best = j; }
   }
   ret best;
}

// 从 (cx,cy) 向外扫桶环，第一圈命中即返回
static size_t pick_nearest(const sched_view &V, $d sid, u$d &out_d) {
   slot_t &s = V.sl[sid];
   u$d cx = s.brush_idx % V.W, cy = s.brush_idx / V.W;
   u$q budget = max_dist(V, s);

   $d rmax = ( $d )((V.W + V.H) / BS) + 2;
   for($d r = 0; r <= rmax; r ++) {
      size_t best = NPOS; u$q bestd = ~(u$q)0;
      $d bx0 = ( $d )(cx / BS), by0 = ( $d )(cy / BS);
      for($d by = by0 - r; by <= by0 + r; by ++)
         for($d bx = bx0 - r; bx <= bx0 + r; bx ++) {
            if(abs(bx - bx0) != r && abs(by - by0) != r) continue;  // 只扫最外圈
            u$q d = ~(u$q)0;
            size_t j = best_in_bucket(V, bx, by, cx, cy, d);
            if(j != NPOS && d < bestd) { bestd = d; best = j; }
         }
      if(best != NPOS) {
         if(bestd > budget) ret NPOS;        // 最近的在预算外 → 等（见 next_delay）
         $d best_sid = sid;
         if(!ready_for(V, s, best)) ret NPOS;
         out_d = (u$d)bestd;
         ret best;
      }
   }
   ret NPOS;
}

static void policy_wavefront(const sched_view &V, vector<plan_item> &out) {
   vector<char> used(V.sl.size(), 0);        // 同一轮里别把一个作业派两次
   for($d sid = 0; sid < ( $d )V.sl.size(); sid ++) {
      slot_t &s = V.sl[sid];
      if(!s.used || s.busy || s.cur_job != NPOS) continue;
      u$d d = 0;
      size_t j = pick_nearest(V, sid, d);
      if(j == NPOS || used[j]) continue;
      used[j] = 1;
      out.pb({sid, j});
   }
}
```

**配套的 `next_delay_ms`**：不再看 `wait_until`，而是取所有空闲槽位的「最近作业」中
最早可画的时间：

```cpp
// 对每个空闲槽位：找最近作业 j*，ready = brush_t + c0 + k·d(j*)
// 返回 min(ready) − now，夹在 1..200ms
```

- 优点：任意目标分布下 `d` 都是当前可选的最小值，天然「哪里近画哪里」。
- 缺点：桶需要维护（作业完成后不删除，靠 `done` 跳过，桶会逐渐全是垃圾，需定期重建）。
- 注意：`k == 0` 时预算无限，等价于「每次都挑最近」。

### 13.5 策略 C：空间分区（多 token / 多 uid）

多 token 的瓶颈不是路径，而是**分区**。等分「蛇形序列」在稀疏目标上会跨很远；
按平面切分则把跨区跳跃降到 0。

```
预处理（build_jobs 之后）：
  1. 取每个 token 的首个目标点（或 k-means 迭代几轮）作为中心
  2. 每个作业分配给曼哈顿距离最近的中心
  3. 每个槽位只在自己的分区里跑策略 B

运行时：
  policy_partition(V, out) {
     for each idle slot s:
        j = pick_nearest_in_partition(V, s)
        if j != NPOS: out.pb({s, j})
  }
```

- 优点：每个 uid 的路径长度近似最小，且负载可控（分区大小可调）。
- 缺点：分区边界会来回跳（可用「按列切片 + 每片内部蛇形」的简化版）；
  目标图动态变化时需要重新分配。
- 何时值得：**token 数 ≥ 2 且目标稀疏/成簇**。整图填充时与 A 差异很小。

### 13.6 两个非策略优化（不改决策，只省时间）

1. **绑定 + 首笔合进同一条 BATCH**。协议保证「先处理全部 bind，再处理全部 paint」
   （§3.4），所以 `enc_batch({..bind..}, {{同槽位, 第一笔}})` 合法。当前实现是
   「绑定 → 等 TICK → 才敢画」，白等一个 tick。
2. **多槽位合批**。多个 uid 同时要画时，把它们的绘画记录放进同一条 BATCH
   （`np > 1`），省消息配额（`msg_rate=128/s`）和压缩开销。注意同一 uid 在同一
   消息里只能有一条能成。

### 13.7 怎么验证（不联网）

```
testfiles/sched_sim.cpp
  输入: 目标 PPM、token 数、c0、k、策略名、随机种子
  模型: 维护每 uid 的 (p, t)；每步按策略选作业
        τ = max(now, t + c0 + k·d)；t ← τ；p ← i
  输出: 总耗时、总路程、平均 d、每 uid 负载、COOLING 尝试比例
```

同一份目标图跑 A/B/C，比较总耗时即可。它和 `replay_main` / `pub_apply` 无关，
不会污染公共画面的回归基线。

线上对照指标：

- `wk.painted / 秒`
- `COOLING` 次数 ÷ `OK` 次数（偏高 = 发早了，或预算算错）
- 平均 `d`（`apply_tick` 里 `ready in … ms` 的日志已带）

### 13.8 坑（写策略时逐条对）

1. **别把 `wait_until` 当状态**：它就是「上次那个目标」的 `ready − now`，会过期。
2. **别提前发**：`COOLING` 不移动画笔，却会占住 `busy` 到结果回来。
3. **同 tick 多笔只有第一笔能成**；`np > 1` 只在「不同 uid」时有意义。
4. **多槽位同 token 是同一个 uid**，不能当两个并发通道。
5. **`done/inflight` 必须成对维护**：漏置 `inflight=0` 会让作业永久消失。
6. **作业去重要保留**（`build_jobs` 里对同 `idx` 取最后一个）。
7. **`k == 0` 时 `max_dist` 必须返回无限**，否则除法出 0。
8. **`now == 0`（时钟未校准）时不要判就绪**，退化成「等 RULES 校准后再发」。
