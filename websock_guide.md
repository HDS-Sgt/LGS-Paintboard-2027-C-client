# websock.hpp 操作指南

手写 WebSocket 客户端（RFC 6455 必要子集），无 Boost，只依赖 OpenSSL。
配套文件：

| 文件 | 作用 |
|---|---|
| `websock.hpp` | 全部实现，单头文件 |
| `common_head.hpp` | 码风宏板子（`$d`、`fn`、`ret`、`_Str` 等） |
| `testfiles/web_test.cpp` | 冒烟测试：连接 → Ping → 收一条 Binary |

---

## 1. 编译

```bash
g++ -std=c++20 -O2 -Wall -Wextra 你的文件.cpp -o 你的程序 \
    -lssl -lcrypto -pthread
```

依赖：OpenSSL 1.1.1+（本机 1.1.1f）、g++ 15+。不需要 libzstd——这一层只负责传输。

> **include 顺序**：`websock.hpp` 自己已经处理了“先包含 OpenSSL 头、再包含宏板子”的顺序。
> 如果你的 `.cpp` 里习惯先写 `#include "common_head.hpp"`，现在也没问题
> （`common_head.hpp` 里的 OpenSSL/sys 头已经挪到宏定义之前）。
> 如果哪天又出现 `expected ',' or '...' before 'return'`，就是有头文件把 `ret` 宏
> 在 OpenSSL 头之前定义了。

---

## 2. 最小可用示例

```cpp
#include "websock.hpp"

___main {
   tcSocket s;                                    // 一个对象 = 一条连接
   s.conn("pb.ika.zip", "443", "/ws?public=0");   // TCP + TLS + HTTP Upgrade
   puts("connected");

   s.sendping();                                  // 可选，验证链路

   while(true) {
      auto m = s.recvbin();                       // 阻塞收一条完整 Binary
      if(!m) break;                               // nullopt = 收到 Close
      printf("recv %zu bytes, first=%02x\n", m->size(), (*m)[0]);
   }
}
```

要点：

- `conn()` 是总入口，内部顺序：`tcpcot`（DNS + TCP）→ `set_timeouts` → `tlshand`（TLS）→
  `handsh`（HTTP Upgrade）。若设置了 `HTTPS_PROXY` / `https_proxy` / `ALL_PROXY`
  （`http://host:port` 形式），会先连代理并做 HTTP `CONNECT`，再在同一个 fd 上做 TLS；
  不设环境变量时行为与直连完全一致。
- `recvbin()` 返回 `std::optional<stream>`：`nullopt` 表示对端发了 Close（已经回过 Close）；
  抛异常表示链路出错。两种情况都该重连。
- `try_recvbin()` 是非阻塞版本：只切已经完整到达的帧，没凑齐返回 0，**不会卡在只收到 Ping
  的情况下**。需要在等冷却/定时发送的同时收包的客户端用它（见 `main.cpp` 的工作连接）。
- `stream` 就是 `vector<u$b>`，`u$b` 是 `unsigned char`。

---

## 3. 对外 API

| 方法 | 说明 |
|---|---|
| `conn(host, port, path)` | 建立完整连接。`host` 同时用于 SNI 和证书域名校验 |
| `sendbin(const stream &)` | 发一条 Binary Message（FIN=1，不分片） |
| `sendping(const stream & = {})` | 发 Ping，负载必须 ≤125 字节 |
| `recvbin()` | 收一条完整 Binary Message，自动回复 Ping、忽略 Pong、重组分片 |
| `try_recvbin(stream &)` | 非阻塞收包：1=收到 Binary，0=暂无完整消息，2=对端 Close；不消耗半个帧 |
| `sendframe(op, dat)` | 底层发帧（一般不用直接调） |
| `recvframe()` | 底层收一帧（一般不用直接调） |

对象**不可拷贝**（删了拷贝构造/赋值，避免重复 `close`/`SSL_free`）。
一条 `tcSocket` 只在一个线程里用；多连接就开多个对象。

---

## 4. 超时与生命周期

| 阶段 | 超时 |
|---|---|
| TLS 握手 + HTTP Upgrade | 30 s（`CONNECT_TIMEOUT_SEC`） |
| 握手完成后的读 | 15 s（`READ_TIMEOUT_SEC`） |
| 握手完成后的写 | 10 s（`WRITE_TIMEOUT_SEC`） |

实现方式是 `SO_RCVTIMEO` / `SO_SNDTIMEO`：底层 `recv/send` 超时后，OpenSSL 返回
`WANT_READ` / `WANT_WRITE`，由 `readbuf()` / `writstr()` 转成异常。
`conn()` 在握手前把超时临时放宽到 30 s，`handsh()` 结束后立刻调回 15 s / 10 s。

析构顺序：`SSL_shutdown` → `SSL_free` → `SSL_CTX_free` → `close(fd)`。
单次 `SSL_shutdown` 只发 close_notify，不等对端（实测静默对端 0.0 ms 返回）。

**没有 TCP connect 超时**：`getaddrinfo` 和 `::connect` 仍是阻塞调用，
DNS 卡住或某个候选地址不可达时，`conn()` 可能长时间不返回。这是当前已知限制。

---

## 5. 错误信息对照

| 报错 | 含义 / 处理 |
|---|---|
| `TCP connect failed` | 所有候选地址都连不上；检查网络/端口 |
| `TLS handshake timeout` | 30 s 内没完成 TLS 握手；通常是网络慢或被限流 |
| `TLS handshake failed` | 握手失败，先看 stderr 里 `ERR_print_errors_fp` 打的详情 |
| `TLS certificate verification failed` | 证书链或域名校验失败 |
| `Load CA certificates failed` | 系统 CA 路径不可用（`/etc/ssl/certs`） |
| `SNI failed` / `Set hostname verification failed` | 参数或内存问题，基本不会出现 |
| `SSL_read timeout` | 15 s 没收到数据：对端半死、被限流或服务端太慢 |
| `TLS closed by peer` | 对端发了 TLS close_notify |
| `WebSocket upgrade rejected: <状态行>` | HTTP 不是 101；检查 path、Host、是否被反代改成普通 HTTP |
| `Invalid Upgrade` / `Connection` / `Sec-WebSocket-Accept` | 对端不是 WebSocket，或中间有代理改写 |
| `Unexpected RSV bits` | 对端用了扩展（如 permessage-deflate），本实现不支持 |
| `Reserved opcode` | 对端发了 0x3~0x7 / 0xB~0xF |
| `Masked server frame` | 服务器帧带了 mask，违反 RFC 6455 |
| `Invalid frame length` / `Invalid control frame` | 帧格式非法 |
| `Frame too large` / `Message too large` | 超过 128 MiB 上限（本地策略） |
| `Unexpected continuation` / `Nested fragmented message` | 分片序列非法 |
| `Unexpected text message` | 对端发了 text 帧；ikapb 只用 Binary |
| `HTTP header too large` | Upgrade 响应头超过 16 KiB |
| `Ping payload too large` | `sendping` 负载 >125 |

---

## 6. 协议实现要点（对应代码）

| RFC 6455 要求 | 代码位置 |
|---|---|
| 24 字节随机 Key，base64 | `handsh` → `RAND_bytes` + `b64enc` |
| `Sec-WebSocket-Accept = base64(sha1(key + GUID))` | `handsh` 末尾，GUID `258EAFA5-E914-47DA-95CA-C5AB0DC85B11` |
| 头名大小写不敏感、重复头逗号合并、`Connection` 按 token 匹配 | `low` / `trm` / `hastok` |
| 101 之后可能已到达帧数据，必须保留 | `readhdr` 只移动 `heat`，不丢 `buf` 尾部 |
| 客户端帧必须 mask，mask 用安全随机数 | `sendframe` → `RAND_bytes(mask,4)` |
| 服务器帧不得 mask | `recvframe` 检查 `mask` 位 |
| 长度 7 / 16 / 64 位三档，大端 | `sendframe` / `recvframe` |
| 控制帧必须 FIN=1 且 ≤125 字节 | `recvframe` 的 `(op & 8)` 检查 |
| Ping 自动回 Pong、Pong 忽略、Close 回 Close | `recvbin` 状态机 |
| 分片消息重组 | `recvbin` 的 `frag` 标志 |

---

## 7. 调试与自测

```bash
# 1) 先确认站点和 API 通
curl -s https://pb.ika.zip/api/meta | head -c 300

# 2) 编译并跑自带冒烟测试
g++ -std=c++20 -O2 -Wall -Wextra -I. testfiles/web_test.cpp -o testfiles/web_test -lssl -lcrypto -pthread
./testfiles/web_test
```

正常输出（实测）：

```
connected
ping sent
got 71 bytes, first=28
```

`first=28` 是 ZSTD 帧魔数 `28 B5 2F FD` 的首字节，说明 Binary 负载是压缩过的应用层数据。

调试建议：

- 用 `std::chrono::steady_clock` 包住 `conn()`，能看到 DNS/TCP/TLS/Upgrade 各阶段耗时
  （本机实测 1.4~10 s，网络差时会超过 30 s 而触发握手指令超时）。
- 想复现读超时，可用 `testfiles/probe_shutdown/server_silent.py`（TLS 握手后静默）+
  `SSL_CERT_FILE` 指向 `testfiles/probe_shutdown/cert.pem` 的自签证书。
- 证书问题先看 stderr，`tlshand` 失败时会调 `ERR_print_errors_fp(stderr)`。

---

## 8. 已知限制

1. **不做 ZSTD**：`recvbin()` 返回的是压缩数据，先解压再看 ikapb 消息类型，
   不能把首字节当消息类型。
2. 不支持 text 帧、permessage-deflate、代理、HTTP/2。
3. 没有 TCP connect 超时（见第 4 节），也没有自动重连。
4. `sendbin` 固定 FIN=1 单帧发送，不会主动分片（接口层简化）。
5. 对端 Close 后只回 Close，不等待 TCP 完全关闭；析构里的 `close` 完成收尾。

---

## 9. 重连模板

传输层遇到任何错误都抛 `std::exception`，重连是应用层的事：

```cpp
for(;;) {
   try {
      tcSocket s;
      s.conn("pb.ika.zip", "443", "/ws?public=0");
      while(auto m = s.recvbin()) {
         // 处理 m：zstd 解压 → ikapb 协议解析
      }
      // nullopt：服务器正常关闭
   } catch(const std::exception &e) {
      fprintf(stderr, "disconnected: %s\n", e.what());
   }
   sleep(3);   // 退避，避免被限流
}
```

注意 `tcSocket` 每轮循环重新构造；不要在 catch 之后复用同一个对象。

---

## 10. 下一步（ikapb 应用层）

传输层到这里就够用了。接着要做的：

1. 装 libzstd（`/usr/include/zstd.h` 当前不存在），先解压 `recvbin()` 的负载；
2. 对照 [2027 docs/protocol.md](https://github.com/Ikaleio/ikapb/blob/2027/docs/protocol.md)
   列出 RULES / SNAPSHOT / PUBLIC / TICK / BATCH 的真实字段，**不要沿用旧回答里的偏移**；
3. 用 Python/TS 示例对拍编码；
4. 再做图片读取、距离冷却调度、战争迷雾下的局部视野。
