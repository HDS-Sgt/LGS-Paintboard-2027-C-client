# testfiles —— 测试与探针程序

这里只放**测试/探针/一次性验证**用的源文件和它们编译出的二进制。交付用的库和文档仍在上一层工作区根目录：

- `common_head.hpp`、`websock.hpp`、`zstdwrap.hpp`：库
- `example.cpp`：带完整注释的参考实现（`example` 二进制在本目录）
- `ikapb_protocol.md`、`ikapb_impl_guide.md`、`websock_guide.md`：文档
- `snap_retry.sh`、`pull_pub.sh`：数据拉取脚本（**从仓库根运行**，内部调用 `testfiles/pub_apply` / `testfiles/pub_decode`）
- `.protocol/`、`.deps/`：测试数据与无 root 解包的 zstd 头文件

## 编译（在仓库根目录执行，`-I.` 指向上一层的头文件）

```bash
# 传输层冒烟
g++ -std=c++20 -O2 -Wall -Wextra -I. testfiles/web_test.cpp -o testfiles/web_test \
    -lssl -lcrypto -pthread

# 带 zstd 的探针/工具（zstd.h 来自 .deps 解包，链接系统运行库）
g++ -std=c++20 -O2 -Wall -Wextra -I. -I.deps/zstd-dev/usr/include \
    testfiles/pub_apply.cpp -o testfiles/pub_apply \
    /lib/x86_64-linux-gnu/libzstd.so.1
```

## 文件清单

| 文件 | 作用 |
|---|---|
| `web_test.cpp` | 最小连线上冒烟：连接 → Ping → 收一条 Binary |
| `test_timeout.cpp` | 读超时复现（`#define main example_main` 后 include `example.cpp`） |
| `t_zstd.cpp` | `ZSTD_versionNumber()` 自检 |
| `zstd_probe.cpp` | 连 `/ws?public=0`，解 RULES 并打印全部字段 |
| `dump_rules.cpp` | 打印 RULES 96 字节 hex |
| `zst_file.cpp` | 整帧解压 + 按类型打印头（PUBLIC/SNAPSHOT/RULES） |
| `zst_stream.cpp` | `ZSTD_decompressStream` 解半截文件 |
| `zst_pub.cpp` | 流式解压后切 PUBLIC 记录 |
| `encbatch_cmp.cpp` | BATCH 编码 vs 官方 Python `encode_batch` 逐字节对比 |
| `tick_selftest.cpp` | 离线自检：`enc_batch`/`procTick`/声纳菱形顺序/颜色字节序/`ppm_load` |
| `tick_probe.cpp` / `tick_probe2.cpp` / `tick_probe3.cpp` | 发 BATCH 观察 TICK（3 为最终可用版） |
| `pub_probe.cpp` | 连 `/ws?public=1` 等 SNAPSHOT/PUBLIC |
| `pub_decode.cpp` | `usage: pub_decode <board.zst> [changes.csv] [W] [H]`，输出 `tick,idx,w` |
| `pub_apply.cpp` | `usage: pub_apply <snapshot.zst> <delta.zst|-> <out.ppm>`，快照 + 配方回放出图 |
| `heat_accum.cpp` | `usage: heat_accum <W> <H> <out.ppm> <file.zst>...`，把一批 `/api/board?since=` 增量离线累积成逐像素活动热力图（log 色阶） |
| `probe_shutdown/` | 自签证书 + 静默 TLS 服务（`server_silent.py`/`server.py`）、`SSL_shutdown` 与超时探针 `cli.cpp`/`cli2.cpp` |

`probe_shutdown/` 的 `cert.pem` / `key.pem` 是本地自签证书，已被 `.gitignore` 忽略。首次运行前在该目录生成：

```bash
cd testfiles/probe_shutdown
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -keyout key.pem -out cert.pem -subj "/CN=127.0.0.1"
```

`.protocol/` 里的 Python/JS 探针（`ws_raw.py`、`ws_frames.py`、`ws_probe.py`、`ws_node.mjs`）依赖该目录里的数据和脚本，保持原位。
