// WebSocket 客户端（RFC 6455 必要子集，只做客户端）
//
// 先包含 OpenSSL / sys 的头，再包含 common_head.hpp：
// common_head 里 #define ret return，而 OpenSSL 头文件里有形参就叫 ret
// （如 BN_bin2bn 的 BIGNUM *ret），顺序反了会报
// "expected ',' or '...' before 'return'"。
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <arpa/inet.h>

#include "common_head.hpp"

using stream = vector<u$b>;

using std::optional;
using std::unordered_map;

constexpr size_t KiB = 1024;
constexpr size_t MiB = 1048576;
constexpr size_t MAX_HEAD = 16 * KiB;

using std::runtime_error;

struct tcSocket {
   $d file = -1;                 // TCP fd
   SSL_CTX *context = nullptr;   // TLS 上下文
   SSL *ssl = nullptr;           // TLS 连接

   stream buf;                   // 收到但还没消费的字节
   size_t heat = 0;              // buf 里下一个未消费字节的下标

   static constexpr size_t MAX_FRAME = 128 * MiB;

   static constexpr int READ_TIMEOUT_SEC  = 15;
   static constexpr int WRITE_TIMEOUT_SEC = 10;
   // TLS + HTTP Upgrade 阶段网络可能很慢（到 pb.ika.zip 实测 3~10s），
   // 单独给一个更宽的超时，握手完成后再换回上面的读写超时。
   static constexpr int CONNECT_TIMEOUT_SEC = 30;
   // 纯 TCP connect 的超时：对端不可达时内核要重传 SYN 约 2 分钟，
   // 没有这个超时 main 会长时间 0 输出、看不到任何错误。
   static constexpr int TCP_CONNECT_TIMEOUT_SEC = 8;

   static constexpr int MAX_WANT_RETRY = 3;

   // 一帧：fin 表示最后一片，op 是 opcode，dat 是负载
   struct frame {
      bool fin;
      u$b op;
      stream dat;
   };

   // ---------- DNS ----------
   // 校园网/教育网的 DNS 经常抽风：getaddrinfo 返回 EAI_AGAIN（“Temporary
   // failure in name resolution”），而重连循环每次都要重新解析，一次失败
   // 就整轮跑不起来。这里做三件事：
   //   1. 缓存解析结果（默认 600 秒），重连不再反复查 DNS；
   //   2. 临时失败按 0.1/0.2/0.4 秒退避重试 4 次；
   //   3. 彻底失败时退回上次成功的地址，DNS 全挂也能继续连。
   // IKAPB_DNS_FAMILY=4/6 可强制只解析 IPv4/IPv6（校园网 IPv6 DNS 坏时有用）。
   struct addr_t {
      sockaddr_storage sa;
      socklen_t len;
      $d family, socktype, proto;
   };

   static inline std::mutex dns_mu;
   static inline std::unordered_map<_Str, std::pair<u$q, vector<addr_t>>> dns_cache;

   static u$q dns_now() {
      ret (u$q)std::chrono::duration_cast<std::chrono::seconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count();
   }

   static $d dns_family() {
      const char *s = getenv("IKAPB_DNS_FAMILY");
      if(s == nullptr) ret AF_UNSPEC;
      if(s[0] == '4') ret AF_INET;
      if(s[0] == '6') ret AF_INET6;
      ret AF_UNSPEC;
   }

   static vector<addr_t> dns_resolve(const _Str &host, const _Str &port,
                                    bool allow_pin = true) {
      const _Str key = host + ":" + port;
      const u$q now = dns_now();
      const u$q TTL = 600;
      {
         std::lock_guard<std::mutex> lk(dns_mu);
         auto it = dns_cache.find(key);
         if(it != dns_cache.end() && it->second.first + TTL > now)
            ret it->second.second;
      }

      // 校园网 IPv4 到 Cloudflare 被阻断、系统 DNS 又不返回 AAAA 时，
      // 可用 IKAPB_HOST_IP 直接钉地址（逗号分隔，v4/v6 都行），
      // TLS 的 SNI 与证书校验仍然用 host，不受影响。
      // 代理主机必须走真实 DNS：否则 IKAPB_HOST_IP 会把 127.0.0.1:7890
      // 解析成绘版服务器地址，表现为连本地代理也 TCP connect failed。
      if(allow_pin)
      if(const char *ips = getenv("IKAPB_HOST_IP"); ips != nullptr && ips[0] != 0) {
         vector<addr_t> out;
         _Str list(ips);
         size_t beg = 0;
         while(beg < list.size()) {
            size_t end = list.find(',', beg);
            if(end == _Str::npos) end = list.size();
            _Str one = list.substr(beg, end - beg);
            while(!one.empty() && (one.front() == ' ' || one.front() == '\t')) one.erase(one.begin());
            while(!one.empty() && (one.back() == ' ' || one.back() == '\t')) one.pop_back();

            addr_t a{};
            if(inet_pton(AF_INET, one.c_str(), &((sockaddr_in *)&a.sa)->sin_addr) == 1) {
               a.sa.ss_family = AF_INET;
               ((sockaddr_in *)&a.sa)->sin_family = AF_INET;
               ((sockaddr_in *)&a.sa)->sin_port = htons((u$w)atoi(port.c_str()));
               a.len = sizeof(sockaddr_in);
               a.family = AF_INET;
               a.socktype = SOCK_STREAM;
               out.push_back(a);
            } else if(inet_pton(AF_INET6, one.c_str(), &((sockaddr_in6 *)&a.sa)->sin6_addr) == 1) {
               a.sa.ss_family = AF_INET6;
               ((sockaddr_in6 *)&a.sa)->sin6_family = AF_INET6;
               ((sockaddr_in6 *)&a.sa)->sin6_port = htons((u$w)atoi(port.c_str()));
               a.len = sizeof(sockaddr_in6);
               a.family = AF_INET6;
               a.socktype = SOCK_STREAM;
               out.push_back(a);
            }
            beg = end + 1;
         }
         if(!out.empty()) ret out;
      }

      addrinfo hints{};
      hints.ai_family = dns_family();
      hints.ai_socktype = SOCK_STREAM;

      addrinfo *lst = nullptr;
      $d err = 0;
      for($d att = 0; att < 4; att ++) {
         err = getaddrinfo(host.c_str(), port.c_str(), &hints, &lst);
         if(err == 0) break;
         // 只有“临时失败”值得重试；名字不存在/参数错误立刻报错
         if(err != EAI_AGAIN && err != EAI_FAIL) break;
         usleep(100000 << att);   // 0.1s / 0.2s / 0.4s
      }

      if(err != 0) {
         std::lock_guard<std::mutex> lk(dns_mu);
         auto it = dns_cache.find(key);
         if(it != dns_cache.end())
            ret it->second.second;   // 退回 stale IP
         throw runtime_error(_Str("DNS resolve failed: ") + gai_strerror(err));
      }

      vector<addr_t> out;
      for(addrinfo *ptr = lst; ptr != nullptr; ptr = ptr->ai_next) {
         addr_t a{};
         memcpy(&a.sa, ptr->ai_addr, ptr->ai_addrlen);
         a.len = ptr->ai_addrlen;
         a.family = ptr->ai_family;
         a.socktype = ptr->ai_socktype;
         a.proto = ptr->ai_protocol;
         out.push_back(a);
      }
      freeaddrinfo(lst);

      {
         std::lock_guard<std::mutex> lk(dns_mu);
         dns_cache[key] = {now, out};
      }
      ret out;
   }

   // ---------- TCP ----------

   fn tcpcot(const _Str &host, const _Str port, bool allow_pin = true) {
      if(getenv("IKAPB_NET_LOG"))
         fprintf(stderr, "[net] TCP connect %s:%s\n", host.c_str(), port.c_str());
      // 解析走缓存/重试；再逐个候选地址试，第一个连上的留下
      vector<addr_t> lst = dns_resolve(host, port, allow_pin);

      for(auto &ptr : lst) {
         $d tmp = socket(ptr.family, ptr.socktype, ptr.proto);
         if(tmp < 0) continue;

         // 非阻塞 connect + poll：否则对端丢 SYN 时要等内核重传 2 分钟
         $d flags = fcntl(tmp, F_GETFL, 0);
         fcntl(tmp, F_SETFL, flags | O_NONBLOCK);

         // 必须写 ::connect：成员名 connect 会隐藏全局函数
         $d rc = ::connect(tmp, (const sockaddr *)&ptr.sa, ptr.len);
         if(rc != 0 && errno == EINPROGRESS) {
            pollfd pf{tmp, POLLOUT, 0};
            rc = poll(&pf, 1, TCP_CONNECT_TIMEOUT_SEC * 1000);
            if(rc > 0) {
               $d so_err = 0;
               socklen_t so_len = sizeof(so_err);
               getsockopt(tmp, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
               rc = (so_err == 0) ? 0 : -1;
            } else {
               rc = -1;   // 超时或 poll 出错
            }
         }

         // 恢复阻塞模式（后续 TLS 依赖阻塞 socket + SO_RCVTIMEO）
         fcntl(tmp, F_SETFL, flags);

         if(rc == 0) {
            file = tmp;
            break;
         }
         close(tmp);
      }

      if(file < 0)
         throw runtime_error("TCP connect failed: " + host + ":" + port);
   }

   // 超时必须夹在 TCP 和 TLS 之间：握手本身也走 socket，
   // 对端接了 TCP 却一直不说话时 SSL_connect 同样会永久阻塞。
   // 超时后 SSL_read/SSL_write 返回 WANT_READ/WANT_WRITE，
   // 由 readbuf/writstr 转成异常。
   fn set_timeouts($d rd = READ_TIMEOUT_SEC, $d wr = WRITE_TIMEOUT_SEC) {
      timeval rv{rd, 0};
      timeval wv{wr, 0};

      if(setsockopt(file, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv)) != 0)
         throw runtime_error("set SO_RCVTIMEO failed");
      if(setsockopt(file, SOL_SOCKET, SO_SNDTIMEO, &wv, sizeof(wv)) != 0)
         throw runtime_error("set SO_SNDTIMEO failed");
   }

   // ---------- TLS ----------

   fn tlshand(const _Str &host) {
      context = SSL_CTX_new(TLS_client_method());
      if(context == nullptr)
         throw runtime_error("SSL_CTX_new failed");

      SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
      SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
      // 系统 CA 路径（Linux 一般 /etc/ssl/certs），加载失败不能继续
      if(SSL_CTX_set_default_verify_paths(context) != 1)
         throw runtime_error("Load CA certificates failed");

      ssl = SSL_new(context);
      if(ssl == nullptr)
         throw runtime_error("SSL_new failed");
      SSL_set_fd(ssl, file);

      // SNI：一个 IP 可能挂多个域名，服务器靠它决定给哪张证书
      if(SSL_set_tlsext_host_name(ssl, host.c_str()) != 1)
         throw runtime_error("SNI failed");

      // 校验证书里的 SAN/CN 是否等于 host
      if(SSL_set1_host(ssl, host.c_str()) != 1)
         throw runtime_error("Set hostname verification failed");

      $d rc = SSL_connect(ssl);
      if(rc != 1) {
         $d e = SSL_get_error(ssl, rc);
         ERR_print_errors_fp(stderr);
         if(e == SSL_ERROR_WANT_READ)
            throw runtime_error("TLS handshake timeout");
         throw runtime_error("TLS handshake failed");
      }
      if(SSL_get_verify_result(ssl) != X509_V_OK)
         throw runtime_error("TLS certificate verification failed");
   }

   // ---------- 读写 ----------

   fn writstr(const u$b *src, size_t siz) {
      $d jam = 0;
      while(siz) {
         $d k = SSL_write(ssl, src, $d(min(siz, size_t(INT_MAX))));
         if(k <= 0) {
            $d e = SSL_get_error(ssl, k);

            if(e == SSL_ERROR_WANT_WRITE)
               throw runtime_error("SSL_write timeout");

            if(e == SSL_ERROR_WANT_READ) {
               if(++jam > MAX_WANT_RETRY)
                  throw runtime_error("SSL_write stalled");
               continue;
            }
            throw runtime_error("SSL_write failed");
         }
         jam = 0;
         src += k;
         siz -= k;
      }
   }

   fn readbuf() {
      u$b tmp[8192];
      $d jam = 0;

      while(true) {
         $d n = SSL_read(ssl, tmp, sizeof(tmp));

         if(n > 0) {
            // 全部消费完了才清空，避免 buf 一直增长
            if(heat == buf.size()) {
               buf.clear();
               heat = 0;
            }
            buf.insert(buf.end(), tmp, tmp + n);
            return;
         }

         $d e = SSL_get_error(ssl, n);

         if(e == SSL_ERROR_WANT_READ)
            throw runtime_error("SSL_read timeout");

         if(e == SSL_ERROR_WANT_WRITE) {
            if(++jam > MAX_WANT_RETRY)
               throw runtime_error("SSL_read stalled");
            continue;
         }
         if(e == SSL_ERROR_ZERO_RETURN)
            throw runtime_error("TLS closed by peer");

         const int saved_errno = errno;
         throw runtime_error("SSL_read failed (ssl_err=" + std::to_string(e) +
                             ", errno=" + std::to_string(saved_errno) + ")");
      }
   }

   // 从 buf 里精确取 n 字节，不够就再读
   fn readstr(u$b *dst, size_t n) {
      while(n) {
         if(heat == buf.size())
            readbuf();

         size_t k = min(n, buf.size() - heat);
         memcpy(dst, buf.data() + heat, k);
         heat += k;
         dst += k;
         n -= k;
      }
   }

   // 读到 \r\n\r\n 为止，返回含空行的完整头。
   // 多读到的字节已经在 buf 里，它们就是 WebSocket 帧的开头，不能丢。
   _Str readhdr() {
      while(true) {
         for(size_t i = heat; i + 4 <= buf.size(); ++i)
            if(memcmp(buf.data() + i, "\r\n\r\n", 4) == 0) {
               _Str res((const char *)buf.data() + heat, i + 4 - heat);
               heat = i + 4;
               return res;
            }

         if(buf.size() - heat > MAX_HEAD)
            throw runtime_error("HTTP header too large");
         readbuf();
      }
   }

   // ---------- HTTP Upgrade ----------

   _Str b64enc(const u$b *p, size_t n) {
      _Str res(4 * ((n + 2) / 3), '\0');
      $d len = EVP_EncodeBlock((unsigned char *)res.data(), p, $d(n));
      res.resize(len);
      return res;
   }

   _Str low(_Str s) {
      for(char &c : s)
         c = tolower((unsigned char)c);
      return s;
   }

   _Str trm(_Str s) {
      size_t l = s.find_first_not_of(" \t\r\n");
      if(l == _Str::npos) return "";
      size_t r = s.find_last_not_of(" \t\r\n");
      return s.substr(l, r - l + 1);
   }

   // 头字段可能写成 "keep-alive, Upgrade"，按逗号切开逐个比
   bool hastok(_Str v, _Str t) {
      std::stringstream ss(v);
      _Str one;
      while(getline(ss, one, ','))
         if(low(trm(one)) == t) return true;
      return false;
   }

   fn handsh(const _Str &host, const _Str &path) {
      u$b rnd[16];
      if(RAND_bytes(rnd, 16) != 1)
         throw runtime_error("RAND_bytes failed");
      _Str key = b64enc(rnd, 16);

      _Str req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
         "\r\nSec-WebSocket-Version: 13\r\n\r\n";
      writstr((const u$b *)req.data(), req.size());

      _Str res = readhdr();
      std::stringstream ss(res);
      _Str line;
      getline(ss, line);
      if(line.find(" 101 ") == _Str::npos)
         throw runtime_error("WebSocket upgrade rejected: " + line);

      // 头名大小写不敏感；重复字段按 RFC 用逗号合并
      unordered_map<_Str, _Str> hdr;
      while(getline(ss, line)) {
         size_t p = line.find(':');
         if(p == _Str::npos) continue;

         _Str name = low(trm(line.substr(0, p)));
         _Str val = trm(line.substr(p + 1));
         if(hdr.count(name)) hdr[name] += "," + val;
         else hdr[name] = val;
      }

      if(!hdr.count("upgrade") || low(hdr["upgrade"]) != "websocket")
         throw runtime_error("Invalid Upgrade header");
      if(!hdr.count("connection") || !hastok(hdr["connection"], "upgrade"))
         throw runtime_error("Invalid Connection header");

      // Sec-WebSocket-Accept = base64(sha1(key + GUID))
      u$b dig[EVP_MAX_MD_SIZE];
      unsigned dlen = 0;
      _Str src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
      EVP_Digest(src.data(), src.size(), dig, &dlen, EVP_sha1(), nullptr);

      if(!hdr.count("sec-websocket-accept") ||
         hdr["sec-websocket-accept"] != b64enc(dig, dlen))
         throw runtime_error("Invalid Sec-WebSocket-Accept");
   }

   // ---------- 帧 ----------

   // 客户端帧必须 mask；FIN=1，不分片。长度分 7/16/64 位三档。
   fn sendframe(u$b op, const stream &dat) {
      size_t n = dat.size();
      if(n > MAX_FRAME)
         throw runtime_error("Frame too large");

      stream f;
      f.push_back(0x80 | op);
      if(n < 126) {
         f.push_back(0x80 | u$b(n));
      } else if(n <= 65535) {
         f.push_back(0x80 | 126);
         f.push_back(u$b(n >> 8));
         f.push_back(u$b(n));
      } else {
         f.push_back(0x80 | 127);
         uint64_t x = n;
         for($d i = 7; i >= 0; --i)
            f.push_back(u$b(x >> (8 * i)));
      }

      u$b mask[4];
      if(RAND_bytes(mask, 4) != 1)
         throw runtime_error("RAND_bytes failed");
      f.insert(f.end(), mask, mask + 4);

      for(size_t i = 0; i < n; ++i)
         f.push_back(dat[i] ^ mask[i & 3]);

      writstr(f.data(), f.size());
   }

   frame recvframe() {
      u$b h[2];
      readstr(h, 2);

      bool fin = h[0] & 0x80;
      u$b rsv = h[0] & 0x70;
      u$b op = h[0] & 0x0f;
      bool mask = h[1] & 0x80;
      size_t n = h[1] & 0x7f;

      if(rsv) throw runtime_error("Unexpected RSV bits");
      if(!(op <= 2 || op == 8 || op == 9 || op == 10))
         throw runtime_error("Reserved opcode");
      if(mask) throw runtime_error("Masked server frame");

      if(n == 126) {
         u$b b[2];
         readstr(b, 2);
         n = (b[0] << 8) | b[1];
      } else if(n == 127) {
         u$b b[8];
         readstr(b, 8);
         if(b[0] & 0x80) throw runtime_error("Invalid frame length");
         n = 0;
         for($d i = 0; i < 8; ++i)
            n = (n << 8) | b[i];
      }

      // 控制帧必须 FIN 且负载 ≤125
      if((op & 8) && (!fin || n > 125))
         throw runtime_error("Invalid control frame");
      if(n > MAX_FRAME)
         throw runtime_error("Frame too large");

      stream dat(n);
      if(n) readstr(dat.data(), n);

      return frame{fin, op, std::move(dat)};
   }

   optional<stream> recvbin() {
      stream msg;
      bool frag = false;

      while(true) {
         frame f = recvframe();

         if(f.op == 0) {                    // continuation
            if(!frag) throw runtime_error("Unexpected continuation");
            if(msg.size() + f.dat.size() > MAX_FRAME)
               throw runtime_error("Message too large");
            msg.insert(msg.end(), f.dat.begin(), f.dat.end());
            if(!f.fin) continue;
            return msg;
         }
         if(f.op == 2) {                    // binary
            if(frag) throw runtime_error("Nested fragmented message");
            if(f.fin) return std::move(f.dat);
            msg = std::move(f.dat);
            frag = true;
            continue;
         }
         if(f.op == 1) throw runtime_error("Unexpected text message");
         if(f.op == 8) {                    // close：回一个 close 就结束
            sendframe(8, f.dat);
            return std::nullopt;
         }
         if(f.op == 9) {                    // ping -> pong
            sendframe(10, f.dat);
            continue;
         }
         // op == 10: 忽略 pong
      }
   }

   // ---------- 非阻塞收包 ----------
   // recvbin() 在只收到 Ping 时会一直等“下一条 Binary”，调用方就无法在冷却结束时
   // 发下一笔。try_recvbin() 只切已经完整到达的帧，没凑齐就返回 0，不消耗半个帧。
   // 返回：1 = 收到一条完整 Binary；0 = 暂无完整消息；2 = 对端关闭。
   stream rx_msg;
   bool rx_frag = false;

   fn pump_once() {
      if(!ssl) ret;
      if(SSL_pending(ssl) <= 0) {
         pollfd pfd{file, POLLIN, 0};
         if(poll(&pfd, 1, 0) <= 0) ret;
      }
      u$b tmp[16384];
      $d n = SSL_read(ssl, tmp, sizeof(tmp));
      if(n > 0) {
         if(heat == buf.size()) { buf.clear(); heat = 0; }
         buf.insert(buf.end(), tmp, tmp + n);
         ret;
      }
      $d e = SSL_get_error(ssl, n);
      if(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ret;
      if(e == SSL_ERROR_ZERO_RETURN) throw runtime_error("TLS closed by peer");
      const int saved_errno = errno;
      throw runtime_error("SSL_read failed (ssl_err=" + std::to_string(e) +
                          ", errno=" + std::to_string(saved_errno) + ")");
   }

   $d try_recvbin(stream &out) {
      pump_once();
      while(true) {
         size_t avail = buf.size() - heat;
         if(avail < 2) ret 0;

         u$b h0 = buf[heat], h1 = buf[heat + 1];
         bool fin = h0 & 0x80;
         u$b rsv = h0 & 0x70;
         u$b op = h0 & 0x0f;
         bool mask = h1 & 0x80;
         size_t n = h1 & 0x7f;
         size_t hdr = 2;

         if(rsv) throw runtime_error("Unexpected RSV bits");
         if(!(op <= 2 || op == 8 || op == 9 || op == 10))
            throw runtime_error("Reserved opcode");
         if(mask) throw runtime_error("Masked server frame");

         if(n == 126) {
            if(avail < 4) ret 0;
            n = ((size_t)buf[heat + 2] << 8) | buf[heat + 3];
            hdr = 4;
         } else if(n == 127) {
            if(avail < 10) ret 0;
            if(buf[heat + 2] & 0x80) throw runtime_error("Invalid frame length");
            n = 0;
            for($d i = 0; i < 8; i ++)
               n = (n << 8) | buf[heat + 2 + i];
            hdr = 10;
         }

         if((op & 8) && (!fin || n > 125))
            throw runtime_error("Invalid control frame");
         if(n > MAX_FRAME)
            throw runtime_error("Frame too large");
         if(avail < hdr + n) ret 0;      // 半帧，等下次

         stream dat(buf.begin() + heat + hdr, buf.begin() + heat + hdr + n);
         heat += hdr + n;
         if(heat == buf.size()) { buf.clear(); heat = 0; }

         if(op == 0) {
            if(!rx_frag) throw runtime_error("Unexpected continuation");
            if(rx_msg.size() + dat.size() > MAX_FRAME)
               throw runtime_error("Message too large");
            rx_msg.insert(rx_msg.end(), dat.begin(), dat.end());
            if(fin) {
               out = std::move(rx_msg);
               rx_msg.clear();
               rx_frag = false;
               ret 1;
            }
            continue;
         }
         if(op == 2) {
            if(rx_frag) throw runtime_error("Nested fragmented message");
            if(fin) { out = std::move(dat); ret 1; }
            rx_msg = std::move(dat);
            rx_frag = true;
            continue;
         }
         if(op == 1) throw runtime_error("Unexpected text message");
         if(op == 8) { sendframe(8, dat); ret 2; }
         if(op == 9) { sendframe(10, dat); continue; }
         // op == 10: pong，忽略
      }
   }

   // ---------- HTTP CONNECT 代理（可选，不设环境变量就不启用） ----------
   // 支持 HTTPS_PROXY / https_proxy / ALL_PROXY / all_proxy，
   // 形式：http://host:port 或 host:port（可选 user@）。
   static _Str envstr(const char *n) {
      const char *v = getenv(n);
      ret v ? _Str(v) : _Str();
   }

   static _Str envproxy() {
      const char *names[] = {"HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"};
      for(const char *n : names) {
         _Str v = envstr(n);
         if(!v.empty()) ret v;
      }
      ret _Str();
   }

   static bool parseproxy(_Str u, _Str &h, _Str &p) {
      if(u.rfind("http://", 0) == 0) u = u.substr(7);
      elif(u.rfind("https://", 0) == 0) u = u.substr(8);
      size_t slash = u.find('/');
      if(slash != _Str::npos) u = u.substr(0, slash);
      size_t at = u.rfind('@');
      if(at != _Str::npos) u = u.substr(at + 1);
      size_t c = u.rfind(':');
      if(c == _Str::npos) { h = u; p = "80"; }
      else { h = u.substr(0, c); p = u.substr(c + 1); }
      ret !h.empty() && !p.empty();
   }

   fn sendraw(const char *src, size_t n) {
      while(n) {
         ssize_t k = ::send(file, src, n, 0);
         if(k <= 0) throw runtime_error("proxy write failed");
         src += k;
         n -= (size_t)k;
      }
   }

   // HTTP CONNECT：纯 TCP 阶段完成，之后才在同一个 fd 上做 TLS
   fn proxyhsh(const _Str &host, const _Str &port) {
      _Str req = "CONNECT " + host + ":" + port + " HTTP/1.1\r\nHost: " + host + ":" + port +
         "\r\nProxy-Connection: keep-alive\r\n\r\n";
      sendraw(req.data(), req.size());

      _Str res;
      char tmp[1024];
      while(res.find("\r\n\r\n") == _Str::npos) {
         ssize_t k = ::recv(file, tmp, sizeof(tmp), 0);
         if(k <= 0) throw runtime_error("proxy handshake failed");
         res.append(tmp, (size_t)k);
         if(res.size() > MAX_HEAD) throw runtime_error("proxy header too large");
      }
      size_t sp = res.find(' ');
      if(sp == _Str::npos || res.substr(sp + 1, 3) != "200")
         throw runtime_error("proxy CONNECT rejected: " + res.substr(0, res.find("\r\n")));
   }

   // ---------- 对外 ----------

   fn sendbin(const stream &dat) {
      sendframe(2, dat);
   }

   fn sendping(const stream &dat = {}) {
      if(dat.size() > 125)
         throw runtime_error("Ping payload too large");
      sendframe(9, dat);
   }

   fn connect(const _Str &host, const _Str &port, const _Str &path) {
      _Str ph, pp;
      if(parseproxy(envproxy(), ph, pp)) {
         // 先连代理，再用 CONNECT 打开到目标的字节流；TLS 仍是对目标做的
         tcpcot(ph, pp, false);
         set_timeouts(CONNECT_TIMEOUT_SEC, CONNECT_TIMEOUT_SEC);
         proxyhsh(host, port);
      } else {
         tcpcot(host, port);
         // 握手阶段先给宽超时，连上之后收回正常的读写超时
         set_timeouts(CONNECT_TIMEOUT_SEC, CONNECT_TIMEOUT_SEC);
      }
      tlshand(host);
      handsh(host, path);
      set_timeouts();
   }

   ~tcSocket() {
      if(ssl) {
         SSL_shutdown(ssl);
         SSL_free(ssl);
      }
      if(context) SSL_CTX_free(context);
      if(file >= 0) close(file);
   }

   tcSocket() = default;
   tcSocket(const tcSocket &) = delete;             // 避免重复 close/free
   tcSocket &operator=(const tcSocket &) = delete;
};
