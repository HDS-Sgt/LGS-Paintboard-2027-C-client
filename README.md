# paintboard —— ikapb 2027 画板客户端

对接 **2027 协议**的 C++20 像素画板客户端：一条连接维护公共画面（`RULES` / `SNAPSHOT` / `PUBLIC`），另一条工作连接用多组 Paint Token 通过 `BATCH` 落笔、读取 `TICK` 里的实时声纳并自动补画。

> 第一次接手先读 [`docs/ONBOARDING.md`](docs/ONBOARDING.md)（10 分钟走完构建、分层和一个像素的数据流），再读 [`AGENTS.md`](AGENTS.md) 的不变量与踩坑表；换会话时看 [`HANDOFF.md`](HANDOFF.md)。

- 公共流：`wss://<host>/ws?public=1`，维护当前画面与两层历史（`L1`/`L2`），断线用 `since` 续传。
- 工作流：`wss://<host>/ws?public=0`，绑定槽位、发送 `BATCH`、解析 `TICK`。
- 分层核心：`Board`（画面）/ `PaintQueue`（全局队列）/ `TokenPainter`（单 token 句柄）/ `WorkConnection`（工作连接）/ `IScheduler`（可替换调度策略）/ `Engine`（组装与生命周期）。
- 支持 PNG/JPG（`stb_image`）或 PPM 目标图、单像素 `--draw`、常驻 `--paint-loop`。

> 本项目只对接 2027 协议。协议权威来源是上游 [`Ikaleio/ikapb`](https://github.com/Ikaleio/ikapb) 的 `docs/protocol.md`；本仓库的 `ikapb_protocol.md` 是逐字节偏移表，`ikapb_impl_guide.md` 是实现手册。

## 调度器

默认 `ClassicScheduler` 保留原来的「绘制 / 修复 / 声纳三组生产者 + 时间优先队列」，行为作为线上基线。

`--scheduler newsch1122` 启用实验调度器 `NewSch1122`，它复用同一套 `Plan` / 队列 / 结果处理，但把三个组的策略改成了分块维护：

| 组 | NewSch1122 的策略 |
|---|---|
| paint | 目标 bbox 切成近方形矩形块，块内逐行蛇形；`blocks <= slots` 时一块一个 paint token，否则每槽连续负责多块；`--paint-loop` 重刷也只重画本块。 |
| sonar | 菱形声纳按 `π=(R+1)+Ri` 的格密铺（每个中心覆盖 `2R²+2R+1` 个格），每个 sonar 槽负责连续块并蛇形扫；中心必须落在目标像素上，漏覆盖的 known 像素用自身中心补射。 |
| repair | 末尾 16 个 repair 槽保持自由，按原来的 damaged/stale 全局扫描；其余 repair 槽绑定声纳中心（槽少时连续分块，槽多时多槽共享中心），每轮主动重画自己的中心。 |

`NewSch1122` 的 paint 每波预算会自动抬到块数（`max(round_cap, blocks)`），避免 `blocks > round_cap` 时只有最上面几行块被服务。设 `IKAPB_PAINT_LOG=1` 可打印 grid、块矩形、每槽首末像素、入队顺序和每轮计数。

## 构建

需要 Linux / WSL（`websock.hpp` 使用 POSIX socket）、支持 C++20 的 g++、OpenSSL、libzstd 和 pthread。

仓库自带 `stb_image.h` 与 zstd 头文件，Linux 上直接链接系统的 `libzstd.so.1`：

```bash
make            # 生成 ./main
make test       # 只构建测试二进制，不自动运行
```

旧单文件实现保留在 `legacy/main_legacy.cpp`，只作历史对照，不参与构建。若系统装了 zstd 开发包，可把 `Makefile` 里的 `/lib/x86_64-linux-gnu/libzstd.so.1` 换成 `-lzstd`。

## 使用

```bash
# 纯公共流：打印 RULES，随后接收 SNAPSHOT / PUBLIC
./main

# 画一个像素，收到 1 次 OK 后退出，并导出声纳表
./main --draw 100,100,FF0000 --limit 1 --sonar-out /tmp/sonar.ppm

# 把图片刷到画布（偏移默认 0,0）
./main --image target_40.png --limit 1000

# 实验调度器：矩形分块 paint + 密铺 sonar + 匀配 repair
./main --scheduler newsch1122 --image target.png --offset 880,0 --paint-loop 1 --daemon

# 只绑定并打印第一帧 TICK，不落笔
./main --dry-run

# 本机 IPv4 到 Cloudflare 不通时，走已运行的本地代理
HTTPS_PROXY=http://127.0.0.1:7890 ./main --dry-run
```

常用参数（完整列表见 `./main --help`）：

| 参数 | 说明 |
|---|---|
| `--scheduler <name>` | `classic`（默认）或 `newsch1122` |
| `--token <uuid>` / `--token-paint <uuid>` | 绘制组 token，可重复；也可用 `IKAPB_TOKEN` |
| `--token-repair <uuid>` / `--token-sonar <uuid>` | 修复组 / 声纳组 token，可重复 |
| `--tokens <file.json>` | 从 JSON 读取三组 token，默认自动读 `./tokens.json` |
| `--draw x,y,RRGGBB` | 增加一个目标像素，可重复 |
| `--ppm <file>` / `--image <file>` | 用 PPM / PNG / JPG 作目标图 |
| `--offset x,y` | 目标图左上角偏移，默认 `0,0` |
| `--limit N` | 所有组累计收到 `OK` 的绘画数达到 N 后退出 |
| `--dry-run` | 只绑定并打印第一帧 TICK，不绘画 |
| `--paint-loop N` | 整图刷完后等 N 秒再刷一遍（0=关闭） |
| `--paint-ms N` / `--repair-ms N` / `--sonar-ms N` | 三组补货周期 |
| `--round-cap N` / `--queue-cap N` | 生产 / 队列上限 |
| `--repair-follow N` | 常驻声纳网格点的 repair 槽位数 |
| `--observe` | 同时跑一条 `public=1` 观察连接 |
| `--sonar-out <file.ppm>` / `--board-out <file.ppm>` | 退出时导出声纳表 / 公共画面（后者需 `--observe`） |

### 网络与代理

`./main` 没有 `--proxy` 参数，只读环境变量 `HTTPS_PROXY`（或小写 `https_proxy`）。直连时才可用 `IKAPB_HOST_IP` 把目标主机钉到指定 IP（逗号分隔，v4/v6 都行）；该变量**只对目标主机生效，不会劫持代理主机**，TLS 的 SNI/证书仍使用域名。

如果 `IKAPB_HOST_IP` 指向的 IPv6 地址失效，删掉这个环境变量改成走代理，或按当前 `dig +short AAAA pb.ika.zip` 换新地址。

### Token 配置

测试期可以把 token 放进 `./tokens.json`（程序会自动读取）：

```bash
cp tokens.example.json tokens.json
# 编辑 tokens.json，填入 paint / repair / sonar 三组 UUID
```

`tokens.json`、`tokens*.json`、`tokens.out` 和所有 `*_token_bk.json` 都已写进 `.gitignore`，**不要提交真实 token**。也可以完全不用文件：

```bash
IKAPB_TOKEN=<uuid> ./main --image target_40.png
./main --token-paint <uuid> --token-repair <uuid> --token-sonar <uuid>
```

同一个 uid 的 token 加载时会自动去重；重复槽位会触发 `lost → 重绑` 风暴。

## 离线回归

`make test` 会构建全部测试二进制；其中 14 个自测程序逐个运行（`replay_main` 另行用于离线回放）：

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

# 公开画面基准：期望 since=24144972 且与 ref_t.ppm 逐字节一致
./testfiles/replay_main .protocol/t_snap.zst .protocol/t_delta.zst /tmp/replay.ppm
cmp /tmp/replay.ppm .protocol/ref_t.ppm

# 快照 + 增量基准：期望与 public_board.ppm 一致
./testfiles/pub_apply .protocol/snap_full.zst .protocol/catchup.zst /tmp/board.ppm
cmp /tmp/board.ppm .protocol/public_board.ppm
```

`pub_apply`、`pub_decode`、`web_test` 等独立探针的编译方式见 [`testfiles/README.md`](testfiles/README.md)。

仓库**不附带** `.protocol/` 抓包样本（体积达数百 MB），离线回放需要自己用脚本采集：

```bash
./pull_pub.sh      # 拉公共增量 → .protocol/live.zst + changes.csv
./snap_retry.sh    # 反复拉完整快照 → .protocol/snap_full.zst + public_board.ppm
./heat2m.sh        # 2 分钟逐像素变更采集
./heat10m.sh       # 10 分钟公共画面活动采集
```

脚本默认访问 `https://pb.ika.zip`，会优先尝试 `http://127.0.0.1:7890` 代理；可用 `PROXY` 环境变量覆盖。

## 目录结构

```
docs/ONBOARDING.md   10 分钟接手页：分层、数据流、旧名映射、改哪里跑什么
HANDOFF.md           跨会话交接摘要（状态、网络、已修 bug、待办）
include/paintboard/  公共接口：types/config/board/paint_queue/painter/scheduler/engine
src/                 实现：base/protocol/board/paint/net/scheduler/app
  scheduler/classic_scheduler.cpp   ClassicScheduler + Plan/target_complete
  scheduler/new_sch1122.cpp         NewSch1122（密铺 sonar / 匀配 repair / 分块 paint）
  scheduler/sonar_tiling.cpp        菱形密铺中心枚举与分块
legacy/main_legacy.cpp  旧单文件客户端，仅作历史参考，不参与构建
main                 `make` 生成的客户端
Makefile             构建 `main` 与全部测试
websock.hpp          TLS / HTTP Upgrade / RFC6455 帧（POSIX + OpenSSL）
zstdwrap.hpp         单 frame Zstandard 解压 / 压缩
common_head.hpp      旧宏与基础类型（仅 legacy / 少量工具）
example.cpp          带完整注释的参考实现（教学用）
tokens.example.json  token 配置模板
ikapb_protocol.md    2027 协议逐字节偏移表
ikapb_impl_guide.md  客户端实现手册
websock_guide.md     websock.hpp 使用与重连模板
AGENTS.md            开发约定、踩坑记录与接手清单
testfiles/           探针、离线解码器、自检程序
.deps/               stb_image.h 与 zstd 头文件
target_*.png         示例目标图
```

## 验收提示

- **不要用公共画面验证刚画的像素**：`GET /api/board`、`/ws?public=1` 都是延迟画面（`public_delay_ms`，实机默认 60 秒），刚画完看不到属于正常现象。
- 实时证据只有两处：本连接 `TICK` 里 `OK` 结果附带的**声纳**，以及后续 `brush` 是否推进。
- 声纳是逐 tick 的实时读数，后续落笔可能覆盖早先的格子，`--sonar-out` 看到的不一定仍是自己画的样子。
- 服务器关闭码：`4000` 格式错误、`4002` 超限、`4003` 下行积压、`4004` 心跳超时、`4005` IP 封禁。先按 `ikapb_protocol.md` 对字节，不要盲目重试。

## 许可证

本仓库使用 **BSD 3-Clause** 许可证，全文见 [`LICENSE`](LICENSE)。`repo/` 下的上游 `ikapb` 项目使用 MIT 许可证（Copyright © 2025 Ikaleio）；第三方头文件 `stb_image.h`、zstd 头文件遵循各自许可证。
