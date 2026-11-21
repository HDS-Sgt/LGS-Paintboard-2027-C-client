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

   static constexpr int MAX_WANT_RETRY = 3;

   // 一帧：fin 表示最后一片，op 是 opcode，dat 是负载
   struct frame {
      bool fin;
      u$b op;
      stream dat;
   };

   // ---------- TCP ----------

   fn tcpcot(const _Str &host, const _Str port) {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;      // v4 v6 都行
      hints.ai_socktype = SOCK_STREAM;  // TCP

      addrinfo *lst = nullptr;
      $d err = getaddrinfo(host.c_str(), port.c_str(), &hints, &lst);
      if(err != 0)
         throw runtime_error(gai_strerror(err));

      // 逐个候选地址试，第一个连上的留下
      for(addrinfo *ptr = lst; ptr != nullptr; ptr = ptr->ai_next) {
         $d tmp = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
         if(tmp < 0) continue;

         // 必须写 ::connect：成员名 connect 会隐藏全局函数
         if(::connect(tmp, ptr->ai_addr, ptr->ai_addrlen) == 0) {
            file = tmp;
            break;
         }
         close(tmp);
      }
      freeaddrinfo(lst);

      if(file < 0)
         throw runtime_error("TCP connect failed");
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

         throw runtime_error("SSL_read failed");
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
      throw runtime_error("SSL_read failed");
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
         tcpcot(ph, pp);
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
