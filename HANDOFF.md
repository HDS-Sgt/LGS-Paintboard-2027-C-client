# 交接摘要（2026-11-22 18:30，开新会话用）

> 本文件 + `AGENTS.md` + `docs/ONBOARDING.md` 是新会话的入口；不要凭记忆猜代码。
> 本项目已从单文件 `main.cpp` 重构为分层引擎（`include/` + `src/`），旧实现只在 `legacy/` 作对照。

## 0. 一分钟状态

- 仓库：`HDS-Sgt/LGS-Paintboard-2027-C-client`，`main` 与 `origin/main` 同步，HEAD `efa2a46`。
- 工作区：只有两个未跟踪文件 `main.cpp`（旧单文件副本）和 `targetII.png`（未被引用），是有意不提交的；其他干净。
- 构建：`make -q` 返回 0（`./main` 与源码同步）；`make test` 已构建全部 14 个测试二进制。
- 验证：14 个测试全部 rc=0；`replay_main` → `REPLAY_MATCH`；`pub_apply` → `PUB_APPLY_MATCH`（2026-11-22 18:30 复跑）。
- 调度器：默认 `ClassicScheduler`；`--scheduler newsch1122` 启用本轮新写的 `NewSch1122`（矩形分块 paint、菱形密铺 sonar、16 自由 repair + 绑定中心）。
- 网络：本机国际 IPv4 到 Cloudflare 被阻断，IPv6 抖动；首选用户已经运行的 `127.0.0.1:7890` 代理，**禁止自行启动 Clash**。
- Git 凭据：`credential.helper=store`，`~/.git-credentials` 存在且权限 600；`git push` 可直接用。SSH 公钥 `~/.ssh/id_ed25519.pub` 也在，但当前 remote 是 HTTPS。

## 0.5 下一个人先做（5 分钟）

```bash
cd ~/桌面/tcsmgg/paintboard
git status --short
git log --oneline -3
make -q || make -j4

# 需要网络时（代理由用户启动，别自己开）
export HTTPS_PROXY=http://127.0.0.1:7890
curl -sS --proxy "$HTTPS_PROXY" -o /dev/null -w '%{http_code}\n' https://pb.ika.zip/api/meta
```

`make -q` 没输出且 rc=0 表示二进制是最新的；若 mtime 异常（跨设备同步可能出现未来时间），先 `make fix-mtime` 再 `make`。

## 1. 项目是什么

对接 **2027 协议**的像素画板客户端：

- 公共流 `wss://<host>/ws?public=1`：`RULES` / `SNAPSHOT` / `PUBLIC`，维护当前画面与 L1/L2 历史层。
- 工作流 `wss://<host>/ws?public=0`：绑定槽位，发 `BATCH`，收 `TICK`（结果 / 画笔 / 声纳 / 释放槽位）。
- 分层：`Board`（画面）、`PaintQueue`（全局队列）、`TokenPainter`（单 token）、`WorkConnection`（工作连接）、`IScheduler`（策略）、`Engine`（生命周期）。
- 目标图支持 PNG/JPEG/PPM（`stb_image`）；`--draw` 画单像素；`--paint-loop` 反复重刷。

协议权威是上游 `repo/ikapb/docs/protocol.md`（2027 分支）；本地 `ikapb_protocol.md` 是逐字节偏移表，`ikapb_impl_guide.md` 是实现手册。

## 2. 构建与验证

```bash
make -j4
make test     # 只构建测试，不自动运行
```

14 个测试（全部应 rc=0）：

```bash
for t in protocol_test tick_selftest board_test queue_test fake_transport_test \
         classic_target_test classic_scheduler_test new_sch1122_test \
         new_sch1122_paint_test new_sch1122_sonar_test new_sch1122_repair_test \
         sonar_tiling_test cli_test alternate_scheduler_test; do
  ./testfiles/$t || echo "FAIL $t"
done
```

离线回归（改调度器/协议后必跑）：

```bash
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm && echo REPLAY_MATCH

./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm && echo PUB_APPLY_MATCH
```

`.protocol/` 抓包样本不入库，体积很大；若要重新采集，用仓库里的 `pull_pub.sh` / `snap_retry.sh`。

## 3. 网络与代理（最容易卡住）

- 本机到 Cloudflare 的 **IPv4 443 全超时**（`curl -4`、`ping -4` 都不通），不是 DNS 问题。
- IPv6 能通但丢包/抖动明显：同一主机不同边缘 0.9–15s 都可能，`curl` 可能出现 `200` 响应头但 body 0 字节。
- 首选用户已运行的 Clash：`export HTTPS_PROXY=http://127.0.0.1:7890`。**不要自己启动 Clash**。
- 直连时才用 `IKAPB_HOST_IP` 钉 IP（逗号分隔，v4/v6 均可）；`websock.hpp` 已保证它只作用于目标主机，不会劫持代理主机，TLS SNI 仍用域名。
- `/etc/hosts` 里不再有 `pb.ika.zip` 的写死 IP（2026-11-22 已清掉）；不要再加回去。
- 代理链路检查：

```bash
curl -sS --proxy http://127.0.0.1:7890 -o /dev/null -w '%{http_code} %{time_total}\n' https://pb.ika.zip/api/meta
```

正常是 200，约 0.5s。直连 `curl` 5s 超时是当前环境的预期现象，不是客户端 bug。

## 4. 调度器现状

### ClassicScheduler（默认，线上基线）

`src/scheduler/classic_scheduler.cpp`。paint / repair / sonar 三组生产者 + 全局 `Plan` + 时间优先队列；`Plan` 与 `target_complete` 的定义也在这里。行为保持原样，不要为了实验改它。

### NewSch1122（实验，`--scheduler newsch1122`）

`src/scheduler/new_sch1122.cpp` + `src/scheduler/sonar_tiling.cpp`。

- **paint**：目标 bbox 切成 `kx*ky` 个近方形矩形，块号行优先；`blocks<=slots` 时一块一个 paint token，否则每槽连续负责多块；块内逐行蛇形（奇数行反向）。op 带 `affinity=槽 id`，`step` 只让该槽接，FIFO 保持相邻两笔只差一格。`--paint-loop` 重刷只重画本块。
- **paint 预算**：每波 `paint_budget=max(round_cap, blocks)`、队列上限 `max(queue_cap, blocks+256)`；`blocks>round_cap` 不再截断前缀块。设 `IKAPB_PAINT_LOG=1` 会打印 grid、块矩形、每槽首末像素、每次入队和每轮计数。
- **sonar**：菱形密铺，`π=(R+1)+Ri`，一个中心覆盖 `N=2R²+2R+1` 格；中心必须落在 `known` 像素上，漏覆盖的 known 像素用自身中心补射；每槽负责连续中心、块内蛇形。
- **repair**：末尾 16 个 repair 槽自由，按原 damaged/stale 全局窗口扫描；其余 repair 槽绑定声纳中心（`partition_sonar_centers`，槽少时连续块、槽多时多槽共享中心），每轮主动重画自己的中心。
- **必须注意**：`Plan::claim` 对 `done_` 像素返回 false，repair 必须用 `Plan::claim_repair`（只查 `enq_`），否则修复永远入不了队。
- 派发：Paint/Sonar 按 affinity + FIFO；Repair 按 priority（坏色优先）+ 曼哈顿距离；未派发的 Paint/Sonar op 重排队 `due` 保持 0，Repair 才用 `now+20ms`。

示例：

```bash
export HTTPS_PROXY=http://127.0.0.1:7890
./main --scheduler newsch1122 --image target.png --offset 880,0 --paint-loop 1 --daemon
```

## 5. 本轮完成 / 关键修复

1. **NewSch1122 落地**：复制 Classic 后只改策略；`--scheduler` CLI/Config/Engine 选择；`classic-scheduler_test` 宏复用出 `new_sch1122_test`。
2. **声纳菱形密铺**：`sonar_tiling.cpp` 枚举中心、known 补射、按块分槽；修复了“反变换用绝对坐标导致中段空洞/重复覆盖”的坐标 bug。
3. **repair 匀配**：16 个自由槽做全局 damaged/stale，其余绑定声纳中心；修复了 `Plan::claim` 对 done 像素永远失败、repair 一笔都发不出的问题（`claim_repair`）。
4. **paint 矩形分块**：块内蛇形、块内重刷、affinity 派发；修复了“未派发 op 统一 `due=now+20ms` 导致 FIFO 被插队、蛇形提交序列错乱”的 bug。
5. **paint 预算自适应**：`max(round_cap, blocks)`，避免 `blocks>round_cap` 时只画最上面几行块。
6. **代理 pin IP bug**：`websock.hpp` 的 `IKAPB_HOST_IP` 原来在 `dns_resolve()` 里无条件劫持任意 host，导致连代理 `127.0.0.1:7890` 被改成连 Cloudflare:7890；已加 `allow_pin`，代理路径传 false。
7. **诊断开关**：`IKAPB_PAINT_LOG`（paint 细节）、`IKAPB_NET_LOG`（TCP 目标）、`IKAPB_BIND_WAIT`（dry-run 等 bind 全部 settle）。
8. **README / HANDOFF 更新**；`README.md` 已提交并推送（`efa2a46`）。

## 6. 已知限制 / 建议下一步

1. **最新代码还没有完整线上长跑**：paint budget、日志、proxy pin 修复后只做了离线回归和 dry-run；建议用代理跑一次 120s 的 `newsch1122`，确认 `passes` 增长、`[paint]` 日志无前缀饥饿、`ok[paint/repair/sonar]` 都有数。
2. **paint 上/下分批**：代码层已经取消 `round_cap` 前缀截断；若线上仍看到“从上到下分批”，先看 `IKAPB_PAINT_LOG=1` 的 enqueue/block 日志，确认是客户端入队顺序还是服务端 tick 呈现，不要直接改分块。
3. **`--observe`（public=1）在分层引擎上未重新验证**；大 SNAPSHOT 有 10–40s 空档，读超时要足够长。
4. **SIGTERM 优雅退出**未验证；目前 Ctrl-C 汇总正常，`timeout -k` 可兜底。
5. **长跑稳定性**：建议观察 `[stat] q= qc[p= r= s=]`，确认队列不持续上涨。
6. **未跟踪文件**：根目录 `main.cpp`（旧单文件，2051 行）与 `targetII.png` 未提交；若不需要就删除，若要保留先确认与 `legacy/main_legacy.cpp` 的差异。
7. **token**：`tokens.json` 含 1760 条、唯一 1233 个；加载时会去重。破坏性测试可用 `./gettoken 6789/1900/2011`（该二进制被 `.gitignore` 忽略，别提交）。

## 7. 日志口径

- `[work] bind settled: N/N bound, M failed`
- `[paint] grid kx=... ky=... blocks=... slots=...` / `[paint] rect blocks slots=...`
- `[paint] note blocks=... > round_cap=...: per-wave paint budget is raised to blocks`
- `[sonar] dense-tiling R=... slots=... known=... centers=... tiling=... extra=... covered=...`
- `[stat] q=<队列> done=<确认>/<目标> painted=<累计OK> passes=<完整轮数>`
- `timeout` 的 rc=124/137 是到点被杀，不是程序错误。

## 8. 快速定位

- 调度：`classic_scheduler.cpp`（默认基线）、`new_sch1122.cpp`（实验）、`sonar_tiling.cpp`（密铺几何）、`round_robin.cpp`（测试用）、`result_handler.cpp`、`target_builder.cpp`。
- 网络：`src/net/transport.cpp`、`work_connection.cpp`、`public_client.cpp`；TLS/WS 在根目录 `websock.hpp`。
- 绘画：`src/paint/queue.cpp`、`token_painter.cpp`。
- 协议/板/应用：`src/protocol/wire.cpp`、`src/board/board.cpp`、`src/app/{cli,engine}.cpp`。
- 离线样本：`.protocol/t_snap.zst` / `t_delta.zst` / `ref_t.ppm`（回放基线，别覆盖）。

## 9. 接手清单

1. `git status --short`、`git log --oneline -3`，确认在 `main` 且干净（除两个未跟踪文件）。
2. `make -q || make -j4`；需要时 `make fix-mtime`。
3. 跑 §2 的 14 个测试 + 两条离线回归，确认全绿。
4. 读 `AGENTS.md` §3/§4 与 `docs/ONBOARDING.md`，再动 `src/scheduler/`。
5. 线上验证前先确认代理 7890 可用；不要自行启动 Clash。
6. 改完至少跑 `make` + `replay_main` + `cmp`，再考虑线上。
