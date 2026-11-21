// ===========================================================================
// example.cpp
// 手写 HTTPS + WebSocket 客户端（POSIX socket + OpenSSL + RFC 6455 最小子集）
// ===========================================================================
//
// 这份文件面向「完全没接触过 HTTP / WebSocket」的读者。
// 代码本身只有 500 多行，但每往前走一步都对应一层协议，所以先把「层次」和
// 「一次连接的时间线」讲清楚，再逐函数对照代码。
//
// ---------------------------------------------------------------------------
// 零、先建立心智模型：四层东西叠在一起
// ---------------------------------------------------------------------------
//
//   你的程序（ikapb 应用层消息）
//        │  ↑
//        │  ↓
//   WebSocket（RFC 6455）：在一根 TCP 连接上收发「帧(frame)」，提供消息边界
//        │  ↑
//        │  ↓
//   HTTP/1.1：只在开头用一次，用来「升级(Upgrade)」成 WebSocket
//        │  ↑
//        │  ↓
//   TLS（OpenSSL）：把上面的字节加密，并验证对端证书
//        │  ↑
//        │  ↓
//   TCP（内核 socket）：可靠、有序的「字节流」，但没有消息边界
//        │  ↑
//        │  ↓
//   网卡 / 互联网
//
// 关键点：
//   1. TCP 只保证「字节按顺序到达」，不保证「一次 read 正好读到一条消息」。
//      你 write 1000 字节，对方可能分 3 次 read 到；也可能两条消息粘在一起。
//      —— 所以所有协议都要自己定义「消息边界」。WebSocket 用「帧头里的长度」
//      来定边界，HTTP 用「\r\n\r\n」和 Content-Length 来定边界。
//
//   2. TLS 是「加密的 TCP」。对代码而言，SSL_write/SSL_read 的语义和
//      send/recv 一样：你写明文，OpenSSL 负责加密、分片、握手、校验证书。
//      TLS 记录(record)的边界同样不可依赖，SSL_read 也可能只给你半条消息。
//
//   3. HTTP 是「文本协议」，报文就是一行行 ASCII，行尾固定是 CRLF（\r\n）。
//      它本身是「请求-响应」模型：客户端发一个请求，服务器回一个响应。
//      但 HTTP/1.1 允许在响应里说「我们换个协议聊」（状态码 101），
//      之后同一根 TCP 连接上就不再是 HTTP，而是 WebSocket 帧。
//      —— 这就是 WebSocket 能复用 443 端口、穿过防火墙的原因。
//
//   4. WebSocket 是「带消息边界的全双工字节通道」。握手完成后，双方都可以
//      随时主动发帧；帧有类型（文本/二进制/控制帧），有长度，有 FIN 标志
//      表示一条消息是否结束。它不关心你传的是 JSON、图片还是 zstd 压缩块。
//
// ---------------------------------------------------------------------------
// 一、一次连接到底发生了什么（时间线）
// ---------------------------------------------------------------------------
//
//   客户端                                              服务器(pb.ika.zip:443)
//     │                                                          │
//     │ 1. DNS: getaddrinfo("pb.ika.zip")  → IP 地址列表          │
//     │ 2. TCP: socket() + connect()  ──────────────────────────→ │  三次握手
//     │                                                          │
//     │ 3. TLS: SSL_connect()  ────────────────────────────────→ │  TLS 握手
//     │      ClientHello(含 SNI=pb.ika.zip) ───────────────────→ │
//     │      ←──────────── ServerHello + 证书链 + 密钥交换      │
//     │      验证证书链、验证证书里的域名、生成会话密钥            │
//     │      ←──────────────── 加密通道建立 ──────────────────→ │
//     │                                                          │
//     │ 4. HTTP Upgrade 请求（明文，走 TLS 加密）                 │
//     │      GET /ws?public=0 HTTP/1.1\r\n                       │
//     │      Host: pb.ika.zip\r\n                                │
//     │      Upgrade: websocket\r\n                              │
//     │      Connection: Upgrade\r\n                             │
//     │      Sec-WebSocket-Key: <16随机字节的base64>\r\n         │
//     │      Sec-WebSocket-Version: 13\r\n                       │
//     │      \r\n                                                │
//     │  ──────────────────────────────────────────────────────→ │
//     │                                                          │
//     │ 5. HTTP 101 响应                                         │
//     │      HTTP/1.1 101 Switching Protocols\r\n                │
//     │      Upgrade: websocket\r\n                              │
//     │      Connection: Upgrade\r\n                             │
//     │      Sec-WebSocket-Accept: <SHA1(key+GUID)的base64>\r\n  │
//     │      \r\n                                                │
//     │  ←────────────────────────────────────────────────────── │
//     │                                                          │
//     │ 6. 从这一刻起，同一根 TCP 连接上跑的全是 WebSocket 帧     │
//     │      [0x82 ...][payload...]  ──────────────────────────→ │  二进制帧
//     │      ←──────────────────── [0x82 ...][payload...]        │
//     │      ←──────────────────── [0x89 ...] Ping               │
//     │      [0x8A ...] Pong ──────────────────────────────────→ │
//     │                                                          │
//     │ 7. 任一方发 Close(0x88)，另一方回 Close，然后关 TCP      │
//     ▼                                                          ▼
//
// 注意第 5 步和第 6 步之间：服务器完全可能「把 101 响应和第一帧数据放在
// 同一次 TCP 到达里」。所以读 HTTP 头时不能多读一个字节就丢掉，必须把
// 多读到的字节留在缓冲区里给第 6 步用。代码里的 buffer/head 就是干这个的。
//
// ---------------------------------------------------------------------------
// 二、HTTP 报文长什么样（为什么代码里到处是 \r\n）
// ---------------------------------------------------------------------------
//
// HTTP/1.1 请求：
//
//     请求行 CRLF
//     头字段名: 值 CRLF
//     ...
//     CRLF                     ← 空行，表示「头结束了」
//     [可选的请求体]
//
// 具体到 WebSocket 握手：
//
//     GET /ws?public=0 HTTP/1.1
//     Host: pb.ika.zip
//     Upgrade: websocket
//     Connection: Upgrade
//     Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
//     Sec-WebSocket-Version: 13
//
// 规则细节：
//   - 行尾必须是 \r\n，不能只写 \n。最后的空行就是第二个 \r\n。
//   - 头字段名(field-name)大小写不敏感：Host / host / HOST 等价。
//     所以代码里统一 lower() 再比较。
//   - 字段值两边的空格是「可选空白(OWS)」，应当 trim 掉。
//   - 有些头允许出现多次，语义等于用逗号连接（RFC 7230 §3.2.2）。
//     代码里 `headers[name] += "," + value` 就是做这件事。
//   - 有些字段值是逗号分隔的列表，例如
//         Connection: keep-alive, Upgrade
//     所以判断「有没有 upgrade」不能写 ==，要按逗号切成 token 再比较。
//     这就是 has_token() 存在的原因。
//
// 状态行：
//
//     HTTP/1.1 101 Switching Protocols
//     └─版本─┘ └码┘ └────原因短语────┘
//
// 101 = Switching Protocols，服务器说「同意切换到你请求的协议」。
// 注意：101 响应没有 body。收到它之后，HTTP 语义就结束了。
//
// 为什么握手要算 SHA-1？
//   客户端随机生成 16 字节 key（base64 后发出去）。
//   服务器把 key 和固定 GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
//   拼起来，算 SHA-1，再 base64，放进 Sec-WebSocket-Accept 返回。
//   客户端自己算一遍并比对。
//   - 这不是加密，也不是身份认证（key 是明文发的，谁都能算 Accept）。
//   - 它的作用是证明「对端真的理解 WebSocket 握手」，而不是一个把请求
//     原样缓存后返回的代理/缓存服务器；同时 key 每次随机，能区分不同连接。
//
// ---------------------------------------------------------------------------
// 三、WebSocket 帧长什么样（逐位拆解）
// ---------------------------------------------------------------------------
//
// 每个帧至少 2 字节头：
//
//    字节 0：
//       bit 7   FIN   —— 1 表示这是「一条消息的最后一帧」
//       bit 6-4 RSV1-3—— 保留位，没协商扩展时必须为 0
//       bit 3-0 opcode—— 帧类型，见下表
//
//    字节 1：
//       bit 7   MASK  —— 1 表示 payload 被掩码过
//       bit 6-0 len7  —— 0..125 直接就是长度；
//                        126 表示「后面还有 2 字节大端长度」；
//                        127 表示「后面还有 8 字节大端长度」。
//
//    如果 MASK=1，再跟 4 字节掩码 key，然后才是 payload。
//
// opcode 表：
//     0x0  Continuation  续帧（一条消息的第 2 帧及以后）
//     0x1  Text          文本消息（payload 必须是合法 UTF-8）
//     0x2  Binary        二进制消息
//     0x8  Close         关闭
//     0x9  Ping          心跳请求
//     0xA  Pong          心跳应答
//     其余 保留，收到就是协议错误
//
// 控制帧(0x8/0x9/0xA)的特殊规则：
//   - 不能分片：FIN 必须为 1；
//   - payload 最多 125 字节（因为必须能塞进一个帧头，不能被长度扩展打断）；
//   - 可以插在一条分片数据消息的中间传输（因为它们不是数据消息的一部分）。
//
// 掩码(Masking)规则，RFC 6455 §5.3：
//   - 客户端 → 服务器：必须 MASK=1；
//   - 服务器 → 客户端：必须 MASK=0（收到 MASK=1 直接判协议错误）。
//   掩码算法：第 i 个字节 XOR key[i % 4]。key 是 4 字节随机数。
//   为什么要掩码？主要是防止「不懂 WebSocket 的中间代理/缓存」被攻击者
//   构造的字节流欺骗（缓存投毒）。浏览器里的 JS 不能直接控制 TCP 字节，
//   所以这个风险只在客户端→服务器方向，服务器→客户端就不需要掩码。
//
// 分片(Fragmentation)：
//   一条大消息可以拆成多帧：
//     第一帧：opcode=0x2, FIN=0
//     中间帧：opcode=0x0, FIN=0
//     最后帧：opcode=0x0, FIN=1
//   收到第二帧数据消息时如果上一条还没结束，是协议错误。
//   （本实现发送时永远 FIN=1，不主动分片；接收时支持重组。）
//
// 字节序：所有多字节长度都是「网络字节序」= 大端(big-endian)，
//   即高位字节先发。代码里 `(n << 8) | b` 和 `>> (8*i)` 就是在做大端编解码。
//
// 为什么长度有 126/127 这种「转义」？
//   为了让最常见的短帧只花 2 字节头（125 字节以内的消息很多），
//   长帧才额外付 2 或 8 字节。这是省带宽的设计。
//
// ---------------------------------------------------------------------------
// 四、代码结构（建议按这个顺序读）
// ---------------------------------------------------------------------------
//
//   WebSocket 类
//     ├─ tcp_connect()      DNS + TCP 连接
//     ├─ tls_connect()      TLS 握手 + 证书验证
//     ├─ write_all()        把一段字节完整写出去（处理部分写）
//     ├─ read_more()        从 TLS 读一块进缓冲区
//     ├─ read_exact()       从缓冲区凑够 n 字节（处理 TCP 粘包/半包）
//     ├─ read_http_header() 从缓冲区里切出 HTTP 头（找 \r\n\r\n）
//     ├─ base64/trim/lower/has_token  握手用的小工具
//     ├─ handshake()        HTTP Upgrade + 校验 101 + 校验 Accept
//     ├─ send_frame()       组帧、加掩码、写出
//     ├─ receive_frame()    读一个帧、拆头、去掩码
//     ├─ send_binary()      opcode=0x2 的便捷封装
//     ├─ send_ping()        opcode=0x9
//     └─ recv_binary()      循环收帧，重组分片，自动回 Pong，返回完整消息
//
//   main() 只是一个冒烟测试：连 pb.ika.zip，发 Ping，收第一条 Binary。
//
// ---------------------------------------------------------------------------
// 五、编译与运行
// ---------------------------------------------------------------------------
//
//   g++ -std=c++20 -O2 -Wall -Wextra example.cpp -o example -lssl -lcrypto -pthread
//
//   ./example
//
// 依赖：OpenSSL 开发头文件（libssl-dev）。本文件不需要 libzstd。
//
// ⚠ 传输层边界（已处理 / 已知取舍）：
//   - 读写超时：TCP 连上后立刻设置 SO_RCVTIMEO=15s、SO_SNDTIMEO=10s。
//     超时会让 SSL_read/SSL_write 返回 WANT_READ/WANT_WRITE（已实测
//     OpenSSL 1.1.1f），本实现把它转成异常，调用方应据此重连。
//     注意：DNS 解析和 connect() 本身仍可能长时间阻塞，没有单独超时。
//   - read_more() 用 while 循环而不是递归，超时不会加深调用栈。
//   - 析构里的单次 SSL_shutdown 只发 close_notify 就返回，不会等对端
//     （实测对端静默时 0.0ms 返回），因此没有阻塞风险。
//   - 收到 Close 后回一个 Close 并返回 nullopt；调用方应把它理解为
//     「连接结束了，该重连」，而不是「程序正常跑完」。严格按 RFC 还差
//     「等对端关 TCP」这一步，但析构的 close(fd) 已达到同样效果。
//   - 保留 opcode 0x3..0x7 / 0xB..0xF 在 receive_frame 里显式拒绝。
//   - 帧/消息大小上限 MAX_FRAME 是本地策略，不是协议规定。
// ===========================================================================

#include <bits/stdc++.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>   // struct timeval：setsockopt 设置读写超时要用

using namespace std;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// 裸字节数组。注意 vector<u8> 的 operator[] 返回引用，别和 string 混用：
// string 适合放 HTTP 这种文本，Bytes 适合放二进制帧。
using Bytes = vector<u8>;

class WebSocket {
    int fd = -1;              // TCP 文件描述符；-1 表示还没连
    SSL_CTX *ctx = nullptr;   // TLS「配置/工厂」：证书、协议版本、验证策略
    SSL *ssl = nullptr;       // TLS「一条具体连接」：握手状态、密钥、缓冲区

    // TLS 读取缓冲区。
    // HTTP 握手可能提前读入后续 WebSocket 数据，
    // 因此必须保留尚未消费的字节。
    //
    // 设计：buffer 是「已经读到但还没处理」的字节，head 是其中第一个
    // 未消费字节的下标。消费时只把 head 往后移，不立刻 erase——
    // 否则每读一次都要搬移整个数组，复杂度会退化成 O(n²)。
    // 只有 head 追上 size 时才整体 clear。
    Bytes buffer;
    size_t head = 0;

    // 单帧和单条重组后消息的大小上限。
    // 64MB 是本地防御性限制：防止对端声称一个 2^63 的长度把内存吃光。
    static constexpr size_t MAX_FRAME = 64 * 1024 * 1024;

    // 读写超时（秒）。对端假死/半开时，没有超时就会永久阻塞。
    // 读 15s 足够覆盖服务器的心跳/TICK 间隔；写 10s 对控制帧绰绰有余。
    static constexpr int READ_TIMEOUT_SEC  = 15;
    static constexpr int WRITE_TIMEOUT_SEC = 10;

    // 一次读/写里允许连续出现多少次「非本方向的 WANT_*」。
    // 例如 SSL_read 返回 WANT_WRITE（TLS 1.3 KeyUpdate）时需要重试；
    // 但本方向的 WANT（读时 WANT_READ）在阻塞 socket + 超时下就是超时，
    // 立即报错，不再空转。
    static constexpr int MAX_WANT_RETRY = 3;

    // ========================================================
    // TCP
    // ========================================================

    // 建立 TCP 连接。
    //
    // getaddrinfo 同时做两件事：
    //   1. DNS 解析（域名 → IP）；
    //   2. 把结果填成一组 addrinfo，每个对应一种「地址族+协议」组合
    //      （IPv4 / IPv6，SOCK_STREAM / TCP）。
    // AF_UNSPEC 表示「IPv4、IPv6 都行，由系统给出顺序」。
    //
    // 为什么要遍历 res？一个域名可能解析出多个 IP（多 A/AAAA 记录），
    // 也可能同时有 IPv6 和 IPv4。前一个连不上就试下一个。
    //
    // connect() 是阻塞的：成功返回 0，失败返回 -1 并设置 errno。
    // 这里没有设置超时（SO_SNDTIMEO / 非阻塞 connect），所以网络不通时
    // 可能卡在 connect 里很久。
    void tcp_connect(const string &host, const string &port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;   // 流式 socket = TCP

        addrinfo *res = nullptr;

        // 注意：getaddrinfo 的返回值不是 errno，要用 gai_strerror 解释。
        int err = getaddrinfo(
            host.c_str(), port.c_str(), &hints, &res
        );

        if (err)
            throw runtime_error(gai_strerror(err));

        // 逐个候选地址尝试连接，第一个成功的留下。
        for (auto p = res; p; p = p->ai_next) {
            int s = socket(
                p->ai_family, p->ai_socktype, p->ai_protocol
            );

            if (s < 0) continue;   // 例如系统没开 IPv6，跳过即可

            if (::connect(s, p->ai_addr, p->ai_addrlen) == 0) {
                fd = s;
                break;
            }

            // 连接失败必须关掉这个 socket，否则泄漏文件描述符。
            close(s);
        }

        // res 是 getaddrinfo 用 malloc 分配的链表，必须 freeaddrinfo。
        freeaddrinfo(res);

        if (fd < 0)
            throw runtime_error("TCP connect failed");
    }

    // 给底层 socket 设置读写超时。
    //
    // 关键点：必须在 TLS 握手之前调用。TLS 握手本身也要用 socket 收发，
    // 否则对端接了 TCP 却永远不说话时，SSL_connect 同样会永久阻塞。
    //
    // SO_RCVTIMEO / SO_SNDTIMEO 只影响「阻塞的 recv/send 最多等多久」，
    // 不改变 socket 的阻塞属性。对 OpenSSL 来说，底层 recv 超时会让
    // SSL_read 返回 WANT_READ（写方向则返回 WANT_WRITE），由
    // read_more() / write_all() 负责把这种「超时」转成异常。
    void set_timeouts() {
        timeval rv{READ_TIMEOUT_SEC, 0};
        timeval wv{WRITE_TIMEOUT_SEC, 0};

        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv)) != 0)
            throw runtime_error("set SO_RCVTIMEO failed");

        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wv, sizeof(wv)) != 0)
            throw runtime_error("set SO_SNDTIMEO failed");
    }

    // ========================================================
    // TLS
    // ========================================================

    // 在已建立的 TCP 连接上做 TLS 握手。
    //
    // SSL_CTX 是「上下文/工厂」：保存证书验证策略、TLS 版本范围、
    //   密码套件、CA 证书库等，可以被多条 SSL 连接复用。
    // SSL 是「一条连接」：保存这条连接自己的握手状态和密钥。
    //
    // 验证链条：
    //   SSL_CTX_set_verify(SSL_VERIFY_PEER)  —— 要求服务端出示证书并验证
    //   SSL_CTX_set_default_verify_paths()   —— 加载系统 CA 根证书
    //   SSL_connect()                        —— 握手中完成证书链验证
    //   SSL_get_verify_result()              —— 再确认一次结果
    //   SSL_set1_host()                      —— 校验证书里的域名是否匹配
    //
    // 只验证「证书链可信」不够：攻击者可以拿一张对自己域名有效的合法证书
    // 来冒充 pb.ika.zip。必须同时验证「证书里的 CN/SAN 与你连的域名一致」，
    // 所以 SSL_set1_host 不能省。两者都通过，才能防中间人。
    void tls_connect(const string &host) {
        ctx = SSL_CTX_new(TLS_client_method());

        if (!ctx)
            throw runtime_error("SSL_CTX_new failed");

        // 最低 TLS 1.2。TLS 1.0/1.1 已被弃用。
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

        SSL_CTX_set_verify(
            ctx, SSL_VERIFY_PEER, nullptr
        );

        // 从系统默认路径加载 CA 根证书（Linux 一般是 /etc/ssl/certs）。
        // 加载失败就没法验证服务端，必须直接报错，不能「继续但不验证」。
        if (SSL_CTX_set_default_verify_paths(ctx) != 1)
            throw runtime_error("Load CA certificates failed");

        ssl = SSL_new(ctx);

        if (!ssl)
            throw runtime_error("SSL_new failed");

        // 把 SSL 对象绑定到已连好的 TCP fd 上。
        SSL_set_fd(ssl, fd);

        // SNI (Server Name Indication)：
        // 一个 IP 上可能挂很多网站，服务器需要知道客户端想要哪个域名，
        // 才能返回对应的证书。没有 SNI，服务器可能返回默认证书，
        // 接着域名验证就会失败。
        if (SSL_set_tlsext_host_name(
                ssl, host.c_str()) != 1)
            throw runtime_error("SNI failed");

        // 证书域名验证：握手时会检查证书的 SAN/CN 是否包含 host。
        if (SSL_set1_host(ssl, host.c_str()) != 1)
            throw runtime_error("Set hostname verification failed");

        // 执行 TLS 握手。失败时把 OpenSSL 错误队列打印出来，
        // 否则只会看到一句没有信息量的 "TLS handshake failed"。
        if (SSL_connect(ssl) != 1) {
            ERR_print_errors_fp(stderr);
            throw runtime_error("TLS handshake failed");
        }

        // 双保险：显式再查一次验证结果。
        if (SSL_get_verify_result(ssl) != X509_V_OK)
            throw runtime_error("TLS certificate verification failed");
    }

    // ========================================================
    // TLS I/O
    // ========================================================

    // 把 p 指向的 n 个字节「完整」写出。
    //
    // SSL_write 的返回值是「这次实际接受并加密了多少字节」，
    // 可能小于 n（尤其是大块数据），所以必须循环直到写完。
    // 直接调用一次 SSL_write 就以为写完了，是新手最常见的 bug 之一。
    //
    // 出错处理（配合 set_timeouts() 设的 SO_SNDTIMEO）：
    //   WANT_WRITE —— 写方向等超时了。阻塞 socket 上出现它，说明对端
    //                 10 秒都没把数据收走，继续重试只会一直卡住，
    //                 直接抛异常让上层重连。
    //   WANT_READ  —— TLS 1.3 KeyUpdate 等场景下 OpenSSL 需要先读一点
    //                 数据；少量重试正常，连续多次没有进展就放弃。
    void write_all(const u8 *p, size_t n) {
        int stalled = 0;   // 连续「一个字节都没写出去」的次数

        while (n) {
            int k = SSL_write(
                ssl, p, int(min(n, size_t(INT_MAX)))
            );

            if (k <= 0) {
                int e = SSL_get_error(ssl, k);

                if (e == SSL_ERROR_WANT_WRITE)
                    throw runtime_error("SSL_write timeout");

                if (e == SSL_ERROR_WANT_READ) {
                    if (++stalled > MAX_WANT_RETRY)
                        throw runtime_error("SSL_write stalled");
                    continue;
                }

                throw runtime_error("SSL_write failed");
            }

            stalled = 0;
            p += k;
            n -= k;
        }
    }

    // 从 TLS 连接读「一块」数据追加到缓冲区。
    //
    // 注意 SSL_read 的语义：它返回「这次解密出来可用的字节数」，
    // 可能是 1 字节，可能是 16KB，也可能把多条 WebSocket 帧粘在一起。
    // 它不保证返回你请求的那么多，也不保证对齐任何协议边界。
    //
    // 出错处理（配合 SO_RCVTIMEO=15s）：
    //   WANT_READ  —— 阻塞 socket 的 recv 超时了，即「15 秒没收到任何
    //                 数据」，连接已不可用，直接抛异常让上层重连。
    //                 ⚠ 这里曾经写成 return read_more() 递归：每超时一次
    //                 栈就加深一层（已实测），约 1000 次后 SIGSEGV。
    //   WANT_WRITE —— TLS 1.3 KeyUpdate 等场景需要先写一点数据，
    //                 少量重试；连续多次仍无进展就放弃。
    void read_more() {
        u8 temp[8192];
        int stalled = 0;

        while (true) {
            int n = SSL_read(ssl, temp, sizeof(temp));

            if (n > 0) {
                // 如果缓冲区里的数据已经全部消费完，直接清空复用，
                // 避免 buffer 无限增长。
                if (head == buffer.size()) {
                    buffer.clear();
                    head = 0;
                }

                // 追加新读到的字节。注意这里不 erase 已消费的前缀，
                // 因为 head 还指向后面未消费的部分；等 head 追上 size 再清。
                buffer.insert(buffer.end(), temp, temp + n);
                return;
            }

            int e = SSL_get_error(ssl, n);

            if (e == SSL_ERROR_WANT_READ)
                throw runtime_error("SSL_read timeout");

            if (e == SSL_ERROR_WANT_WRITE) {
                if (++stalled > MAX_WANT_RETRY)
                    throw runtime_error("SSL_read stalled");
                continue;
            }

            // e == SSL_ERROR_ZERO_RETURN：对端发了 TLS close_notify，
            // 是「正常关闭」；但本实现统一当异常处理。
            if (e == SSL_ERROR_ZERO_RETURN)
                throw runtime_error("TLS closed by peer");

            throw runtime_error("SSL_read failed / connection closed");
        }
    }

    // 从缓冲区里「凑够」n 字节，复制到 p。
    //
    // 这是处理 TCP 字节流的核心函数：上层要 n 字节，但缓冲区里可能
    // 一个字节都没有、只有一部分、或者远远多于 n。循环负责：
    //   1. 先从已有缓冲区消费；
    //   2. 不够就 read_more() 继续读。
    // 多读到的字节留在 buffer 里，等下一次调用再消费，不会丢。
    void read_exact(u8 *p, size_t n) {
        while (n) {
            size_t available = buffer.size() - head;

            if (available) {
                size_t k = min(n, available);

                memcpy(p, buffer.data() + head, k);

                head += k;
                p += k;
                n -= k;

                continue;
            }

            read_more();
        }
    }

    // 从缓冲区里读出一个完整的 HTTP 响应头。
    //
    // HTTP 头以空行结束，也就是字节序列 "\r\n\r\n"。
    // 这里用 std::search 在 [head, end) 里找这个四字节串。
    //
    // 找到后：
    //   - 返回 [head, 结尾位置+4) 的字符串（包含最后的空行）；
    //   - head 推到响应头之后，指向可能已经读到的 WebSocket 帧数据。
    //     ← 这一步非常关键，绝对不能把剩余字节丢掉。
    //
    // 找不到就继续 read_more()，同时限制头部总大小，防止恶意对端
    // 发一个永远没有 \r\n\r\n 的超长头把内存撑爆。
    string read_http_header() {
        constexpr string_view END = "\r\n\r\n";

        while (true) {
            auto it = search(
                buffer.begin() + head,
                buffer.end(),
                END.begin(), END.end()
            );

            if (it != buffer.end()) {
                size_t end = size_t(it - buffer.begin()) + 4;

                string result(
                    buffer.begin() + head,
                    buffer.begin() + end
                );

                head = end;
                return result;
            }

            if (buffer.size() - head > 16384)
                throw runtime_error("HTTP header too large");

            read_more();
        }
    }

    // ========================================================
    // WebSocket handshake helpers
    // ========================================================

    // Base64 编码。用 OpenSSL 的 EVP_EncodeBlock，避免自己写查表。
    //
    // 输出长度公式：每 3 字节输入 → 4 字节输出，向上取整：
    //   4 * ((n + 2) / 3)
    // 然后按实际写入长度 resize（EVP_EncodeBlock 返回实际字符数，
    // 不含结尾 '\0'）。
    static string base64(const u8 *p, size_t n) {
        string out(4 * ((n + 2) / 3), '\0');

        int len = EVP_EncodeBlock(
            reinterpret_cast<unsigned char *>(out.data()),
            p, int(n)
        );

        out.resize(len);
        return out;
    }

    // ASCII 转小写。HTTP 头字段名大小写不敏感，比较前统一小写。
    // 注意：tolower 的参数必须是 unsigned char 或 EOF，
    // 否则对 >=128 的字节是未定义行为。
    static string lower(string s) {
        for (char &c : s)
            c = char(tolower((unsigned char)c));

        return s;
    }

    // 去掉两端空白（空格、tab、CR、LF）。
    // 为什么需要？getline 按 '\n' 切行，行尾的 '\r' 会留在字符串里；
    // 头字段值两边也可能有空格。
    static string trim(string s) {
        size_t l = s.find_first_not_of(" \t\r\n");
        if (l == string::npos) return "";

        size_t r = s.find_last_not_of(" \t\r\n");
        return s.substr(l, r - l + 1);
    }

    // 判断一个逗号分隔的头部值里是否含有某个 token（忽略大小写）。
    // 例：has_token("keep-alive, Upgrade", "upgrade") == true
    static bool has_token(string value, string token) {
        value = lower(value);
        token = lower(token);

        stringstream ss(value);
        string part;

        while (getline(ss, part, ',')) {
            if (trim(part) == token)
                return true;
        }

        return false;
    }

    // 完成 WebSocket 的 HTTP Upgrade 握手。
    //
    // 分三步：
    //   1. 生成随机 key，拼出 HTTP 请求并发出去；
    //   2. 读响应头，确认状态码是 101；
    //   3. 校验 Upgrade / Connection / Sec-WebSocket-Accept 三个头。
    //
    // 为什么要随机 key？它是 nonce，让每条连接的 Accept 都不同，
    // 防止重放/缓存；同时证明对端按协议计算了 Accept。
    void handshake(const string &host, const string &path) {
        // 16 字节 = RFC 6455 建议的 nonce 长度。
        // 必须用密码学安全随机数（RAND_bytes），不能用 rand()，
        // 否则 key 可预测，虽然 WebSocket 握手本身不靠它保密，
        // 但安全相关的地方一律用 CSPRNG 是纪律。
        u8 random_key[16];

        if (RAND_bytes(random_key, 16) != 1)
            throw runtime_error("RAND_bytes failed");

        string key = base64(random_key, 16);

        // 请求行 + 头 + 空行。
        // 注意每行结尾都是 \r\n，最后再加一个 \r\n 表示头结束。
        //
        // Host 在 HTTP/1.1 里是必填的（一台服务器可能托管多个站点）。
        // 严格来说非 443 端口时 Host 应写成 host:port，这里连的是 443，
        // 默认端口可以省略。
        string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        write_all(
            reinterpret_cast<const u8 *>(request.data()),
            request.size()
        );

        // 读到的是完整的 HTTP 响应头（含空行），后面可能已经跟着帧数据。
        string response = read_http_header();

        // ---- 解析状态行 ----
        stringstream ss(response);
        string line;

        getline(ss, line);

        // 简单判断状态码 101。注意这里用 " 101 " 而不是行首匹配，
        // 对 "HTTP/1.1 101 Switching Protocols" 足够；更严格的实现
        // 应按空格切分并精确比较第 2 个 token。
        if (line.find(" 101 ") == string::npos)
            throw runtime_error("WebSocket upgrade rejected: " + line);

        // ---- 解析头字段 ----
        // 头字段名统一小写后存进 map。
        // 重复字段按 RFC 7230 的规则用逗号合并（对列表型头是对的；
        // 对 Set-Cookie 这类不可合并的头不适用，但握手响应里没有）。
        unordered_map<string, string> headers;

        while (getline(ss, line)) {
            size_t pos = line.find(':');

            if (pos == string::npos) continue;

            string name = lower(trim(line.substr(0, pos)));
            string value = trim(line.substr(pos + 1));

            if (headers.count(name))
                headers[name] += "," + value;
            else
                headers[name] = value;
        }

        // 服务器必须确认升级到 websocket。
        if (!headers.count("upgrade") ||
            lower(headers["upgrade"]) != "websocket")
            throw runtime_error("Invalid Upgrade header");

        // Connection 头可能是 "Upgrade"，也可能是 "keep-alive, Upgrade"，
        // 所以不能用 ==，要按 token 判断。
        if (!headers.count("connection") ||
            !has_token(headers["connection"], "upgrade"))
            throw runtime_error("Invalid Connection header");

        // ---- 校验 Sec-WebSocket-Accept ----
        // 算法：Base64( SHA1( client_key + 固定GUID ) )
        //
        // 固定 GUID 是 RFC 6455 写死的常量，不是密钥，公开可见。
        // 它的作用是给 SHA-1 输入加一个固定后缀，避免与其他协议
        // 对同一 key 的哈希结果撞车。
        string source =
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        u8 digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len = 0;

        if (EVP_Digest(
                source.data(), source.size(),
                digest, &digest_len,
                EVP_sha1(), nullptr
            ) != 1)
            throw runtime_error("SHA1 failed");

        string expected = base64(digest, digest_len);

        // 服务器算错/没算，说明它没真正完成 WebSocket 握手（可能是个
        // 普通 HTTP 服务器，或中间有代理改写了响应），必须拒绝。
        if (!headers.count("sec-websocket-accept") ||
            headers["sec-websocket-accept"] != expected)
            throw runtime_error("Invalid Sec-WebSocket-Accept");
    }

    // ========================================================
    // WebSocket frame sending
    // ========================================================

    // 发送一帧。
    //
    // 本实现永远发「不拆分的单帧」：FIN=1，RSV=0。
    // 组帧顺序：
    //   [字节0: FIN|RSV|opcode]
    //   [字节1: MASK|len7]
    //   [可选 2 或 8 字节扩展长度，大端]
    //   [4 字节掩码 key]      ← 客户端必须带
    //   [payload XOR key]
    //
    // 这里先把整个帧拼进 frame 再一次性 write_all，原因是：
    //   - 避免多次小 write 造成 Nagle 延迟/更多系统调用；
    //   - 但注意 TCP 仍可能把这一次 write 拆成多个包，接收方靠帧头
    //     里的长度自行重组，所以「一次 write」不代表「一个 TCP 包」。
    void send_frame(u8 opcode, const Bytes &payload) {
        size_t n = payload.size();

        if (n > MAX_FRAME)
            throw runtime_error("Frame too large");

        Bytes frame;

        // 字节 0：FIN=1（0x80），RSV 全 0，低 4 位放 opcode。
        frame.push_back(0x80 | opcode);

        // 字节 1：MASK=1（0x80），低 7 位是长度选择。
        //   0..125  : 直接放长度
        //   126     : 长度放后面 2 字节
        //   127     : 长度放后面 8 字节
        // 注意 n == 126 和 n == 127 这两个值本身不会被直接编码，
        // 它们是「扩展长度」的哨兵值。
        if (n < 126) {
            frame.push_back(0x80 | u8(n));
        }
        else if (n <= 65535) {
            frame.push_back(0x80 | 126);
            frame.push_back(u8(n >> 8));   // 高字节先发（大端）
            frame.push_back(u8(n));
        }
        else {
            frame.push_back(0x80 | 127);

            // 8 字节大端：从最高字节往最低字节依次 push。
            for (int i = 7; i >= 0; --i)
                frame.push_back(u8(u64(n) >> (8 * i)));
        }

        // 4 字节掩码 key，必须随机。RFC 要求「足够不可预测」，
        // 否则掩码就失去防止代理缓存投毒的意义。
        u8 mask[4];

        if (RAND_bytes(mask, 4) != 1)
            throw runtime_error("Mask generation failed");

        frame.insert(frame.end(), mask, mask + 4);

        size_t start = frame.size();

        frame.resize(start + n);

        // 逐字节 XOR：第 i 个 payload 字节 XOR mask[i % 4]。
        for (size_t i = 0; i < n; ++i)
            frame[start + i] = payload[i] ^ mask[i & 3];

        write_all(frame.data(), frame.size());
    }

    // ========================================================
    // WebSocket frame receiving
    // ========================================================

    struct Frame {
        bool fin;        // 是否是本条消息的最后一帧
        u8 opcode;       // 帧类型
        Bytes payload;   // 已去掉掩码的负载
    };

    // 读取并解析一个帧。
    //
    // 每一步都在做「字节 → 位」的解包：
    //   h[0] & 0x80  → FIN
    //   h[0] & 0x70  → RSV1..3
    //   h[0] & 0x0f  → opcode
    //   h[1] & 0x80  → MASK
    //   h[1] & 0x7f  → 长度或长度哨兵
    Frame receive_frame() {
        u8 h[2];
        read_exact(h, 2);

        bool fin = h[0] & 0x80;
        u8 rsv = h[0] & 0x70;
        u8 opcode = h[0] & 0x0f;

        bool masked = h[1] & 0x80;
        u64 n = h[1] & 0x7f;

        // 没有协商任何扩展（本实现不发 Sec-WebSocket-Extensions），
        // 所以 RSV 必须全 0；非 0 说明对端用了我们没同意的扩展。
        if (rsv)
            throw runtime_error("Unexpected RSV bits");

        // 0x3..0x7 与 0xB..0xF 是保留 opcode。本实现没有协商扩展，
        // 收到它们只能是对端实现错误或探测，直接判协议错误。
        // 合法值只有：0x0 Continuation、0x1 Text、0x2 Binary、
        //             0x8 Close、0x9 Ping、0xA Pong。
        if (!(opcode <= 0x2 || opcode == 0x8 || opcode == 0x9 || opcode == 0xA))
            throw runtime_error("Reserved opcode");

        // RFC 6455: 服务端→客户端的帧不得 Mask。
        // 如果这里为 1，要么对端实现错误，要么中间有东西在伪装/篡改。
        if (masked)
            throw runtime_error("Masked server frame");

        if (n == 126) {
            u8 b[2];
            read_exact(b, 2);
            n = (u16(b[0]) << 8) | b[1];
        }
        else if (n == 127) {
            u8 b[8];
            read_exact(b, 8);

            // 64 位长度只有 63 位有效，最高位必须为 0。
            // 这一位为 1 是协议错误，也可能是恶意的超大长度。
            if (b[0] & 0x80)
                throw runtime_error("Invalid frame length");

            n = 0;

            for (u8 x : b)
                n = (n << 8) | x;   // 大端拼接
        }

        // opcode >= 8 是控制帧。控制帧的两条硬性规则：
        //   1. 不允许分片，所以 FIN 必须为 1；
        //   2. payload 必须 <= 125 字节。
        bool control = opcode >= 8;

        if (control && (!fin || n > 125))
            throw runtime_error("Invalid control frame");

        if (n > MAX_FRAME)
            throw runtime_error("Frame too large");

        // ⚠ 这里曾经写成 Bytes payload(size_t(n));
        //   C++ 会把 size_t(n) 解析成「参数类型 size_t、参数名 n」，
        //   于是整行变成函数声明 Bytes payload(size_t)，即
        //   "most vexing parse"，后面 payload.data() 就编译失败。
        //   加 static_cast 或再加一层括号即可消除歧义。
        Bytes payload(static_cast<size_t>(n));

        if (n)
            read_exact(payload.data(), size_t(n));

        return {fin, opcode, move(payload)};
    }

    // 发送控制帧的统一入口。
    // 控制帧必须 FIN=1 且 <=125 字节；本类只通过 send_ping() 和
    // Close 响应调用它，调用方已经保证长度。
    void send_control(u8 opcode, const Bytes &payload) {
        send_frame(opcode, payload);
    }

public:

    WebSocket() = default;

    // 禁止拷贝：这个类持有 fd / SSL* 等独占资源，
    // 拷贝会导致 double close / double free。
    WebSocket(const WebSocket &) = delete;
    WebSocket &operator=(const WebSocket &) = delete;

    // 析构顺序必须与构造相反：先 TLS，再 TCP。
    //
    // SSL_shutdown 会尝试发送 TLS close_notify（「我要正常关闭加密层」）。
    // 单次调用在发送成功后立刻返回 0，表示「已发出，但还没收到对方的
    // close_notify」——它不会在这里等待对端（实测：对端保持静默时
    // 0.0ms 返回）。真正会阻塞的是「第二次 SSL_shutdown」或发送缓冲区
    // 已满时的写操作，本代码都没有走到。
    // 所以这里实际风险很小；若想彻底避免，可以只 SSL_free + close，
    // 不发 close_notify（代价是 TLS 层面不算优雅关闭）。
    ~WebSocket() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }

        if (ctx)
            SSL_CTX_free(ctx);

        if (fd >= 0)
            close(fd);
    }

    // ========================================================
    // Public API
    // ========================================================

    // 连接 = TCP + 设置超时 + TLS + HTTP Upgrade，四步缺一不可。
    //
    // 超时必须夹在 TCP 和 TLS 之间：TLS 握手、HTTP 握手、后续收帧
    // 都依赖它，否则任何一步被对端挂住都会永久阻塞。
    void connect(
        const string &host,
        const string &port,
        const string &path
    ) {
        tcp_connect(host, port);
        set_timeouts();
        tls_connect(host);
        handshake(host, path);
    }

    // 发送一个 Binary Message。
    //
    // 注意：WebSocket 层完全不知道 payload 里是什么。
    // 对 ikapb 来说，这里放的是 ZSTD 压缩后的应用层消息；
    // 但本类不负责压缩/解压，也不应该负责。
    void send_binary(const Bytes &payload) {
        send_frame(0x2, payload);
    }

    // 主动发送 Ping（opcode 0x9）。
    // 控制帧 payload 最多 125 字节；服务器收到后应原样回一个 Pong，
    // payload 必须与 Ping 相同。Ping 也常被用作「连接还活着吗」的心跳。
    void send_ping(const Bytes &payload = {}) {
        if (payload.size() > 125)
            throw runtime_error("Ping payload too large");

        send_control(0x9, payload);
    }

    // 接收一个完整 Binary Message。
    // 返回 nullopt 表示收到了 Close 帧（连接正在正常关闭）。
    //
    // 这是本类最像「状态机」的地方。它把多个帧归并成一条消息，
    // 同时处理穿插在其中的控制帧：
    //
    //   state: fragmented = 当前是否处在一条分片消息的中间
    //          message    = 已经收到的分片内容
    //
    //   收到 Binary(FIN=1)      → 直接返回
    //   收到 Binary(FIN=0)      → 开始分片，message = payload
    //   收到 Continuation       → 必须 fragmented 为真，否则是协议错误
    //      ├ FIN=0             → 继续等
    //      └ FIN=1             → 拼接完成，返回 message
    //   收到 Ping               → 立即回 Pong（payload 原样带回），继续循环
    //   收到 Pong               → 忽略（心跳应答，不需要应用层处理）
    //   收到 Close              → 回一个 Close，返回 nullopt
    //   收到 Text               → ikapb 不使用文本帧，直接报错
    //   其他 opcode             → 协议错误
    //
    // 为什么 Ping 可以出现在分片中间？
    //   控制帧不算「数据消息的一部分」，RFC 允许它们插队，
    //   这样长消息传输期间也能及时做心跳。所以这里不能因为
    //   fragmented 就拒绝 Ping。
    optional<Bytes> recv_binary() {
        Bytes message;
        bool fragmented = false;

        while (true) {
            Frame f = receive_frame();

            switch (f.opcode) {
                case 0x0: {
                    // Continuation
                    if (!fragmented)
                        throw runtime_error("Unexpected continuation");

                    if (message.size() + f.payload.size() > MAX_FRAME)
                        throw runtime_error("Message too large");

                    message.insert(
                        message.end(),
                        f.payload.begin(),
                        f.payload.end()
                    );

                    if (f.fin) {
                        fragmented = false;
                        return message;
                    }

                    break;
                }

                case 0x2: {
                    // Binary
                    // 上一条分片消息还没结束，又来了一个新的数据帧：
                    // 这是非法交错，RFC 不允许。
                    if (fragmented)
                        throw runtime_error("Nested fragmented message");

                    message = move(f.payload);

                    if (f.fin)
                        return message;

                    fragmented = true;
                    break;
                }

                case 0x1:
                    // Text：payload 必须是合法 UTF-8。ikapb 只用二进制，
                    // 这里直接拒绝，避免把文本帧误当应用数据。
                    throw runtime_error("Unexpected text message");

                case 0x8: {
                    // Close：按 RFC 应该原样回送 Close（回显状态码/原因），
                    // 完成关闭握手。
                    //
                    // ⚠ 这里回送后直接返回，没有等待对端关 TCP。严格流程是：
                    //   收到 Close → 回 Close → 等对端关 TCP（或自己关）。
                    //   本实现靠析构的 close(fd) 完成关闭；上层应把 nullopt
                    //   当作「连接结束，需要重连」，而不是「程序跑完了」。
                    send_control(0x8, f.payload);
                    return nullopt;
                }

                case 0x9: {
                    // Ping -> Pong。payload 必须原样带回，
                    // 这样对端可以用 payload 匹配请求/响应。
                    send_control(0xA, f.payload);
                    break;
                }

                case 0xA:
                    // Pong：忽略。也可能是对端主动发来的（不一定是回应
                    // 我们的 Ping），同样忽略。
                    break;

                default:
                    throw runtime_error("Unknown WebSocket opcode");
            }
        }
    }
};


// ============================================================
// Test
// ============================================================
//
// 冒烟测试的目的只有一个：验证「TCP 能连、TLS 能过、Upgrade 能成、
// 能收到一条 Binary」。它不解析 ikapb 应用层协议。
//
// 关于 ikapb 的消息封装（务必以 docs/protocol.md 为准，不要照抄注释）：
//   目前的理解是每条应用消息先被 ZSTD 压缩，再放进一条 WebSocket
//   Binary Message。所以这里拿到的 msg 是压缩数据，它的第一个字节
//   不是 ikapb 消息类型，必须先解压。压缩发生在消息层还是字段层、
//   以及消息头格式，实现前都要重新核对协议文档和官方 Python/TS 示例。
int main() {
    try {
        WebSocket ws;

        ws.connect(
            "pb.ika.zip",
            "443",
            "/ws?public=0"
        );

        cerr << "WebSocket connected!\n";

        // 主动 Ping，服务器应该回复 Pong。
        // recv_binary() 内部会自动吃掉 Pong 并继续等 Binary，
        // 所以这里不需要单独处理。
        ws.send_ping();

        // 收到第一条 Binary Message。
        // ikapb 应该首先发送 RULES。
        auto msg = ws.recv_binary();

        if (!msg) {
            cerr << "Server closed connection.\n";
            return 0;
        }

        cerr << "Received binary message: "
             << msg->size() << " bytes\n";

        if (msg->empty())
            throw runtime_error("Empty message");

        // 此时拿到的还是 ZSTD 压缩数据。
        // 打印首字节只是冒烟检查，没有协议含义。
        cerr << "First compressed byte: 0x"
             << hex << int((*msg)[0]) << dec << '\n';

        // 后续可以：
        //
        // Bytes raw = zstd_decompress(*msg);
        // parse_rules(raw);
        //
        // ws.send_binary(zstd_compress(batch));
        //
        // auto tick = ws.recv_binary();
        // parse_tick(zstd_decompress(*tick));

    }
    catch (const exception &e) {
        cerr << "Error: " << e.what() << '\n';

        // 传输层异常（TLS 读写超时、对端异常关闭、帧格式错误）都到这里。
        // 真实绘图客户端应在外层套一个「销毁连接 → 退避 → 重连」的循环，
        // 而不是像这个冒烟测试一样直接退出。
        return 1;
    }

    return 0;
}
