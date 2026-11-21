# paintboard —— ikapb 2027 画板客户端

对接 **2027 协议**的 C++20 像素画板客户端：一边维护公共画面（`RULES` / `SNAPSHOT` / `PUBLIC`），一边用多组 Paint Token 通过 `BATCH` 落笔、读取 `TICK` 里的实时声纳并自动补画。

- 公共流：连接 `wss://<host>/ws?public=1`，维护当前画面与两层历史（`L1`/`L2`），断线用 `since` 续传。
- 工作流：连接 `wss://<host>/ws?public=0`，绑定槽位、发送 `BATCH`、解析 `TICK`。
- 调度器：绘制 / 修复 / 声纳三组 token 由三个生产者补货，共享一个带锁的时间优先队列；声纳按槽位分块蛇形扫描，修复带跨轮游标。
- 支持 PNG/JPG（`stb_image`）或 PPM 目标图、单像素 `--draw`、常驻刷图 `--paint-loop`。

> 本项目只对接 2027 协议。协议权威来源是上游 [`Ikaleio/ikapb`](https://github.com/Ikaleio/ikapb) 的 `docs/protocol.md`；本仓库里的 `ikapb_protocol.md` 是逐字节偏移表，`ikapb_impl_guide.md` 是实现手册。

## 构建

需要 Linux / WSL（`websock.hpp` 使用 POSIX socket）、支持 C++20 的 g++、OpenSSL、libzstd 和 pthread。

仓库自带 `stb_image.h` 与 zstd 头文件，Linux 上直接链接系统的 `libzstd.so.1`：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -I.deps/zstd-dev/usr/include \
    main.cpp -o main -lssl -lcrypto -pthread \
    /lib/x86_64-linux-gnu/libzstd.so.1
```

如果系统装了 zstd 开发包，最后一项可以换成 `-lzstd`。编译时 `common_head.hpp` 里的 `rep` 宏会产生一些 `-Wsign-compare` 警告，属于已知噪音。

## 使用

```bash
# 纯公共流：打印 RULES，随后接收 SNAPSHOT / PUBLIC
./main

# 画一个像素，收到 1 次 OK 后退出，并导出声纳表
./main --draw 100,100,FF0000 --limit 1 --sonar-out /tmp/sonar.ppm

# 把图片刷到画布（左上角默认 950,8）
./main --image target_40.png --limit 1000

# 只绑定并打印第一帧 TICK，不落笔
./main --dry-run

# 直连不通时走本地 HTTP 代理（tcSocket 支持 HTTPS_PROXY 的 CONNECT）
HTTPS_PROXY=http://127.0.0.1:7890 ./main --dry-run
```

常用参数（完整列表见 `./main --help`）：

| 参数 | 说明 |
|---|---|
| `--token <uuid>` / `--token-paint <uuid>` | 绘制组 token，可重复；也可用 `IKAPB_TOKEN` |
| `--token-repair <uuid>` / `--token-sonar <uuid>` | 修复组 / 声纳组 token，可重复 |
| `--tokens <file.json>` | 从 JSON 读取三组 token，默认自动读 `./tokens.json` |
| `--draw x,y,RRGGBB` | 增加一个目标像素，可重复 |
| `--ppm <file>` / `--image <file>` | 用 PPM / PNG / JPG 作目标图 |
| `--offset x,y` | 目标图左上角偏移，默认 `950,8` |
| `--limit N` | 所有组累计收到 `OK` 的绘画数达到 N 后退出 |
| `--dry-run` | 只绑定并打印第一帧 TICK，不绘画 |
| `--paint-loop N` | 整图刷完后等 N 秒再刷一遍（0=关闭） |
| `--observe` | 同时跑一条 `public=1` 观察连接 |
| `--sonar-out <file.ppm>` / `--board-out <file.ppm>` | 退出时导出声纳表 / 公共画面（后者需 `--observe`） |

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

## 离线回归

`testfiles/` 里是探针、解码器和自检程序。编译方式：

```bash
# 不带 zstd 的传输层冒烟
g++ -std=c++20 -O2 -Wall -Wextra -I. testfiles/web_test.cpp \
    -o testfiles/web_test -lssl -lcrypto -pthread

# 带 zstd 的探针 / 工具
g++ -std=c++20 -O2 -Wall -Wextra -I. -I.deps/zstd-dev/usr/include \
    testfiles/pub_apply.cpp -o testfiles/pub_apply \
    /lib/x86_64-linux-gnu/libzstd.so.1

# 直接跑 main.cpp 解析路径的离线 harness
g++ -std=c++20 -O2 -Wall -Wextra -I. -I.deps/zstd-dev/usr/include \
    testfiles/replay_main.cpp -o testfiles/replay_main \
    -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1

# BATCH 编码 / TICK 解析 / 声纳顺序 / 颜色字节序自检
g++ -std=c++20 -O2 -Wall -Wextra -I. -I.deps/zstd-dev/usr/include \
    testfiles/tick_selftest.cpp -o testfiles/tick_selftest \
    -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1
./testfiles/tick_selftest
```

仓库**不附带** `.protocol/` 抓包样本（体积达数百 MB），离线回放需要自己用脚本采集：

```bash
./pull_pub.sh      # 拉公共增量 → .protocol/live.zst + changes.csv
./snap_retry.sh    # 反复拉完整快照 → .protocol/snap_full.zst + public_board.ppm
./heat2m.sh        # 2 分钟逐像素变更采集
./heat10m.sh       # 10 分钟公共画面活动采集
```

脚本默认访问 `https://pb.ika.zip`，会优先尝试 `http://127.0.0.1:7890` 代理；可用 `PROXY` 环境变量覆盖。各探针的详细说明见 [`testfiles/README.md`](testfiles/README.md)。

## 目录结构

```
main.cpp             客户端主程序：公共流 + 工作流 + 调度器
websock.hpp          TLS / HTTP Upgrade / RFC6455 帧（POSIX + OpenSSL）
zstdwrap.hpp         单 frame Zstandard 解压 / 压缩
common_head.hpp      宏与基础类型
example.cpp          带完整注释的参考实现（教学用）
tokens.example.json  token 配置模板
ikapb_protocol.md    2027 协议逐字节偏移表
ikapb_impl_guide.md  客户端实现手册
websock_guide.md     websock.hpp 使用与重连模板
AGENTS.md            开发约定、踩坑记录与接手清单
testfiles/           探针、离线解码器、自检程序
.deps/               stb_image.h 与 zstd 头文件
pull_pub.sh          拉公共增量
snap_retry.sh        拉完整快照并回放成 PPM/PNG
heat2m.sh / heat10m.sh  公共画面活动采集
target_*.png         示例目标图
```

## 验收提示

- **不要用公共画面验证刚画的像素**：`GET /api/board`、`/ws?public=1` 都是延迟画面（`public_delay_ms`，实机默认 60 秒），刚画完看不到属于正常现象。
- 实时证据只有两处：本连接 `TICK` 里 `OK` 结果附带的**声纳**，以及后续 `brush` 是否推进。
- 声纳是逐 tick 的实时读数，后续落笔可能覆盖早先的格子，`--sonar-out` 看到的不一定仍是自己画的样子。
- 服务器关闭码：`4000` 格式错误、`4002` 超限、`4003` 下行积压、`4004` 心跳超时、`4005` IP 封禁。先按 `ikapb_protocol.md` 对字节，不要盲目重试。

## 许可证

本仓库尚未指定开源许可证。`repo/` 下的上游 `ikapb` 项目使用 MIT 许可证（Copyright © 2025 Ikaleio）；第三方头文件 `stb_image.h`、zstd 头文件遵循各自许可证。
