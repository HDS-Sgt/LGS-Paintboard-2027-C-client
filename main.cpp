// stb_image 是单头文件实现；放在 common_head.hpp 之前展开，避免与它的
// `ret`/`pb`/`min`/`max` 等宏冲突。编译命令不需要额外的 -I 参数。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#define STB_IMAGE_IMPLEMENTATION
#include ".deps/stb/stb_image.h"
#pragma GCC diagnostic pop

#include "common_head.hpp"
#include "websock.hpp"
#include "zstdwrap.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <array>
#include <deque>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <poll.h>
#include <signal.h>

using std::array;
using std::deque;

static const char *ikapb_host = "pb.ika.zip";
static const char *ikapb_port = "443";

// ===================== 全局状态 =====================

vector<vector<std::atomic<u$d> > >cols, L1, L2;

std::atomic<bool> init_finished = false;
u$q since = 0;                      // 公共画面已应用到的版本（只有公共线程用）

// 保护 cols / L1 / L2 / sonar_* 与 glob 的维度；procRule 可能来自任意一条连接
std::mutex canvas_mu;

// 声纳表：像素下标 -> (颜色, tick)。协议 §3.8，退出时导出 PPM 方便核对
vector<u$d> sonar_col;
vector<u$q> sonar_tick;

std::mutex log_mu;
template<class... A>
static void logp(const char *s, const A &...a) {
   std::lock_guard<std::mutex> lk(log_mu);
   print(s, a...);
}

// SIGINT/SIGTERM 用 sa_flags=0（不要 SA_RESTART），让阻塞的 recv/poll 以 EINTR
// 返回，线程就能及时退出并把 PPM 写出来。
volatile sig_atomic_t g_sig = 0;
std::atomic<bool> g_abort{false};

static void on_signal(int) { g_sig = 1; }

// ===================== 读取游标 =====================

struct readptr {
   stream *str;
   $d ptr;
   bool bad = false;

   bool end() { ret ptr >= ( $d )str->size(); }
   u$b readb() {
      if(bad || ptr >= ( $d )str->size()) { bad = true; ret 0; }
      ret (*str)[ptr ++];
   }
   u$w readw() { ret readb() | (((u$w)readb()) << 8); }
   u$d readd() { ret readw() | (((u$d)readw()) << 16); }
   u$q readq() { ret readd() | (((u$q)readd()) << 32); }
};

// 服务器时间校准：RULES 末尾的 server_time + 本地 steady_clock
static std::atomic<u$q> srv_base_us{0};
static std::atomic<u$q> srv_base_mono{0};

static u$q mono_us() {
   using namespace std::chrono;
   ret (u$q)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
fn srv_calibrate(u$q st) {
   srv_base_mono = mono_us();
   srv_base_us = st;
}
static u$q srv_now() {
   u$q b = srv_base_mono.load();
   if(!b) ret 0;
   ret srv_base_us.load() + (mono_us() - b);
}

/*
| 偏移 | 类型 | 字段                     | 语义                                |
| ---- | ---- | ------------------------ | ----------------------------------- |
| 0    | u8   | 类型                     | `0x84`                              |
| 1    | u8   | 版本                     | `1`                                 |
| 2    | u16  | 保留（文档）             | 实测为 `0x0020`，见 §6 未解释的差异 |
| 4    | u16  | `W`                      | 画布宽                              |
| 6    | u16  | `H`                      | 画布高                              |
| 8    | u16  | `sonar_radius_px`        | 声纳半径 `R`（默认 2）              |
| 10   | u16  | 保留                     | 实测 0                              |
| 12   | u32  | `tps`                    | 服务器 tick 频率（默认 128）        |
| 16   | u64  | `cooldown_base_us`       | 冷却基数 `c0`，微秒                 |
| 24   | u64  | `cooldown_per_px_us`     | 每格曼哈顿距离附加冷却 `k`，微秒/格 |
| 32   | u64  | `public_delay_ms`        | 公共画面延迟 `D`，毫秒              |
| 40   | u64  | `public_period_ms`       | 公共流推进周期 `P`，毫秒            |
| 48   | u64  | `event_start_unix_ms`    | 活动开始，0 = 不限                  |
| 56   | u64  | `event_end_unix_ms`      | 活动结束，0 = 不限                  |
| 64   | u32  | `max_slots`              | 每条连接槽位数                      |
| 68   | u32  | `max_paints_per_message` | 单条 BATCH 绘画上限                 |
| 72   | u32  | `msg_rate`               | 每连接消息令牌桶补充速率            |
| 76   | u32  | `msg_burst`              | 每连接消息令牌桶容量                |
| 80   | u32  | `max_message_bytes`      | 压缩后/WS 消息上限                  |
| 84   | u32  | `max_payload_bytes`      | 解压后载荷上限                      |
| 88   | u64  | 服务器时间               | UNIX 微秒，用于校准本地时钟         |

*/

struct rule {
   u$b edit;
   u$w remain;
   u$w W, H;
   u$w rad;
   u$w remain2;
   u$d tps;
   u$q cooldown_base, cooldown_per_px;
   u$q public_delay, public_period;
   u$q event_start, event_end;
   u$d max_slots, max_paints_per_message;
   u$d msg_rate, msg_burst;
   u$d max_message_bytes, max_payload_bytes;
   u$q server_time;
}glob;

// 调用方必须持有 canvas_mu
static void canvas_alloc(u$d W, u$d H) {
   cols.clear(); cols.resize(H);
   for(auto &c : cols) c = vector<std::atomic<u$d> >(W);

   L1.clear(); L1.resize(H);
   for(auto &c : L1) c = vector<std::atomic<u$d> >(W);

   L2.clear(); L2.resize(H);
   for(auto &c : L2) c = vector<std::atomic<u$d> >(W);

   sonar_col.assign((size_t)W * H, 0xFFFFFFFFu);
   sonar_tick.assign((size_t)W * H, 0);
}

// P 指向 RULES 的“版本”字节（调用方已经读掉 type），与旧实现一致。
fn procRule(readptr &P) {
   u$b ver = P.readb();
   u$w rsv = P.readw();
   u$w W = P.readw(), H = P.readw();
   u$w rad = P.readw(), rsv2 = P.readw();
   u$d tps = P.readd();
   u$q c0 = P.readq(), k = P.readq();
   u$q pdelay = P.readq(), pperiod = P.readq();
   u$q estart = P.readq(), eend = P.readq();
   u$d mslots = P.readd(), mpaints = P.readd();
   u$d mrate = P.readd(), mburst = P.readd();
   u$d mmsg = P.readd(), mpay = P.readd();
   u$q stime = P.readq();

   if(P.bad || W == 0 || H == 0 || (u$q)W * H > (16u << 20)) {
      logp("[rules] malformed (bad={})\n", (int)P.bad);
      ret;
   }

   bool alloc = false;
   {
      std::lock_guard<std::mutex> lk(canvas_mu);
      bool empty = cols.empty();
      bool dimchg = (glob.W != W || glob.H != H);

      glob.edit = ver;
      glob.remain = rsv;
      glob.W = W;
      glob.H = H;
      glob.rad = rad;
      glob.remain2 = rsv2;
      glob.tps = tps;
      glob.cooldown_base = c0;
      glob.cooldown_per_px = k;
      glob.public_delay = pdelay;
      glob.public_period = pperiod;
      glob.event_start = estart;
      glob.event_end = eend;
      glob.max_slots = mslots;
      glob.max_paints_per_message = mpaints;
      glob.msg_rate = mrate;
      glob.msg_burst = mburst;
      glob.max_message_bytes = mmsg;
      glob.max_payload_bytes = mpay;
      glob.server_time = stime;

      if(empty || dimchg) { canvas_alloc(W, H); alloc = true; }
   }

   srv_calibrate(stime);
   init_finished = true;

   logp("[rules] proto={} canvas={}x{} R={} tps={} c0={}us k={}us/px slots={} paints/msg={} rate={}/{} bytes={}/{} {}\n",
        (int)ver, (u$d)W, (u$d)H, (u$d)rad, tps, c0, k, mslots, mpaints,
        mrate, mburst, mmsg, mpay, alloc ? "alloc" : "update");
}
/*
## 2. `SNAPSHOT` `0x83`（服务端 → 客户端，全图）

| 偏移       | 类型      | 字段                                                  |
| ---------- | --------- | ----------------------------------------------------- |
| 0          | u8        | 类型 `0x83`                                           |
| 1          | u8        | 版本 `1`                                              |
| 2          | u8        | codec：`0` 或 `1`                                     |
| 3          | u8        | 保留                                                  |
| 4          | u16       | `W`                                                   |
| 6          | u16       | `H`                                                   |
| 8          | u64       | 画面版本号（tick）                                    |
| 16         | u32 × W·H | 颜色，按像素下标 `idx = y·W + x` 顺序                 |
| 16 + 4·W·H | u32 × W·H | **仅 codec 1**：历史层 L1（等于当前颜色表示空，写 0） |
| 16 + 8·W·H | u32 × W·H | **仅 codec 1**：历史层 L2，同上                       |

长度：codec 0 为 `16 + 4·W·H`；codec 1 为 `16 + 12·W·H`。`/ws` 发 codec 1，`/admin/ws` 发 codec 0。
颜色 `u32` 内存字节序为 `R, G, B, A`，即 `color = R | G<<8 | B<<16 | 0xFF000000`，`A` 必须 255（§1.1）。
*/
fn procSnap(readptr &P) {
   std::lock_guard<std::mutex> lk(canvas_mu);
   if(cols.empty() || L1.empty()) { logp("[snap] no canvas (RULES first)\n"); ret; }
   P.readb();
   u$b codec = P.readb();
   P.readb();
   P.readw();
   P.readw();
   since = P.readq();
   logp("[snap] {}x{} ver={} codec={}\n", (u$d)glob.W, (u$d)glob.H, since, (int)codec);
   rep(i, 0, glob.H - 1)
      rep(j, 0, glob.W - 1)
         cols[i][j] = P.readd();
   if(codec) {
   rep(i, 0, glob.H - 1)
      rep(j, 0, glob.W - 1)
         L1[i][j] = P.readd();

   rep(i, 0, glob.H - 1)
      rep(j, 0, glob.W - 1)
         L2[i][j] = P.readd();
   }
}

// public
$d procPublic(readptr &P) {
   std::lock_guard<std::mutex> lk(canvas_mu);
   if(cols.empty()) ret -1;

   P.readb();
   P.readw();
   u$d cont = P.readd();
   u$q from, to;
   from = P.readq();
   to = P.readq();
   if(from != since) ret -1;
   u$d seg = P.readd();
   u$d dlt = P.readd();
   u$d order = P.readd();
   if(P.bad || seg == 0) ret -1;
   logp("[public] {} -> {} n={} seg={} order={}\n", from, to, cont, seg, order);

   vector<u$d>tick, count, dbig, col;
   vector<u$b> dlit;

   rep(i, 1, seg)
      tick.pb(P.readd());
   rep(i, 1, seg)
      count.pb(P.readd());

   rep(i, 1, cont)
      dlit.pb(P.readb());
   P.ptr += (4 - cont % 4) % 4;
   
   rep(i, 1, dlt)
      dbig.pb(P.readd());

   rep(i, 1, cont)
      col.pb(P.readd());

   if(P.bad) { logp("[public] truncated\n"); ret -1; }
   u$q total = 0;
   rep(i, 0, seg - 1) total += count[i];
   if(total != cont) { logp("[public] count mismatch {} != {}\n", total, cont); ret -1; }

   $d ptrn = 0, ptre = 0;

   rep(i, 0, seg - 1) {
      since = from + tick[i];
      u$d pos = 0;
      rep(j, 0, count[i] - 1) {
         if(ptrn >= ( $d )dlit.size()) ret -1;
         if(dlit[ptrn] == 255) {
            if(ptre >= ( $d )dbig.size()) ret -1;
            pos += dbig[ptre ++];
         } else
            pos += dlit[ptrn];
         $d I, J;
         if(order == 0)
            I = pos / glob.W, J = pos % glob.W;
         else {
            I = pos % glob.H * glob.W + pos / glob.H;
            J = I % glob.W;
            I /= glob.W;
         }
         if(I < 0 || J < 0 || I >= ( $d )glob.H || J >= ( $d )glob.W) ret -1;
         u$d c = col[ptrn], old = cols[I][J], nw;
         if(c == 0)    nw = (u$d)L1[I][J];
         elif(c == 1)  nw = (u$d)L2[I][J];
         elif(c == 2)  nw = old;
         else          nw = c;

         if(nw != old) {
            u$d h1 = (u$d)L1[I][J], h2 = (u$d)L2[I][J], nl2;
            if(h1 != old && h1 != nw)      nl2 = h1;
            elif(h2 != old && h2 != nw)    nl2 = h2;
            else                           nl2 = nw;
            L2[I][J] = nl2;
            L1[I][J] = old;
            cols[I][J] = nw;
         }
         ptrn ++;
      }
   }
   

   ret 0;
}


// ===================== BATCH 编码（上行唯一消息 0x01） =====================
// 布局：16B 头 + bind.slot u16[nb] + pad + bind.paint_token 16B[nb]
//       + paint.slot u16[np] + pad + paint.idx u32[np] + paint.color u32[np]
struct bt { $d slot; array<u$b,16> tok; };
struct pt { $d slot; u$d idx, color; };

static stream enc_batch(const vector<bt> &bs, const vector<pt> &ps) {
   stream v;
   auto p16 = [&](u$d x) { v.pb(u$b(x & 255)); v.pb(u$b((x >> 8) & 255)); };
   auto p32 = [&](u$d x) { for($d i = 0; i < 4; i ++) v.pb(u$b((x >> (8 * i)) & 255)); };
   auto pad = [&]() { while(v.size() & 3) v.pb(0); };

   v.pb(0x01); v.pb(1); p16(0);
   p32((u$d)bs.size()); p32((u$d)ps.size()); p32(0);

   for(auto &b : bs) p16((u$d)b.slot);
   pad();
   for(auto &b : bs) for($d i = 0; i < 16; i ++) v.pb(b.tok[i]);

   for(auto &p : ps) p16(p.slot);
   pad();
   for(auto &p : ps) p32(p.idx);
   for(auto &p : ps) p32(p.color);
   ret v;
}

// ===================== TICK 解析（下行 0x81） =====================
static size_t pad4sz(size_t n) { ret (n + 3) & ~size_t(3); }

static u$d rd16(const stream &v, size_t o) {
   ret (u$d)v[o] | ((u$d)v[o + 1] << 8);
}
static u$d rd32(const stream &v, size_t o) {
   ret (u$d)v[o] | ((u$d)v[o + 1] << 8) | ((u$d)v[o + 2] << 16) | ((u$d)v[o + 3] << 24);
}
static u$q rd64(const stream &v, size_t o) {
   u$q r = 0;
   for($d i = 0; i < 8; i ++) r |= (u$q)v[o + i] << (8 * i);
   ret r;
}

struct tick_info {
   $d R = 0;
   u$q tick = 0;
   u$d nr = 0, nb = 0, nk = 0, ns = 0;
   vector<u$b> status;     // nr
   vector<u$d> value;      // nr
   vector<u$q> brt;        // nb
   vector<u$d> bridx;      // nb
   vector<u$d> brslot;     // nb
   vector<u$d> sonidx;     // nk
   vector<u$d> soncol;     // nk * A
   vector<u$d> lost;       // ns
};

// p 是完整载荷，p[0] = 0x81。返回 0 表示成功。
static $d procTick(const stream &p, tick_info &T) {
   if(p.size() < 32 || p[0] != 0x81 || p[1] != 1) ret -1;

   $d R = ( $d )rd16(p, 2);
   T.nr = rd32(p, 4);
   T.tick = rd64(p, 8);
   T.nb = rd32(p, 16);
   T.nk = rd32(p, 20);
   T.ns = rd32(p, 24);
   if(R < 0 || R > 16) ret -2;
   T.R = R;

   u$q A = 2ull * R * R + 2ull * R + 1;
   u$q need = 32 + 12ull * T.nb + pad4sz(2ull * T.nb) + pad4sz(T.nr) + 4ull * T.nr
              + 4ull * T.nk + 4ull * T.nk * A + pad4sz(2ull * T.ns);
   if(need != p.size()) ret -3;

   size_t bt = 32;
   size_t bi = bt + 8ull * T.nb;
   size_t bs = bi + 4ull * T.nb;
   size_t st = bs + pad4sz(2ull * T.nb);
   size_t va = st + pad4sz(T.nr);
   size_t si = va + 4ull * T.nr;
   size_t sc = si + 4ull * T.nk;
   size_t ls = sc + 4ull * T.nk * A;

   T.status.resize(T.nr); T.value.resize(T.nr);
   T.brt.resize(T.nb); T.bridx.resize(T.nb); T.brslot.resize(T.nb);
   T.sonidx.resize(T.nk); T.soncol.resize((size_t)T.nk * A);
   T.lost.resize(T.ns);

   for($d k = 0; k < ( $d )T.nr; k ++) {
      T.status[k] = p[st + k];
      T.value[k] = rd32(p, va + 4 * k);
   }
   for($d k = 0; k < ( $d )T.nb; k ++) {
      T.brt[k] = rd64(p, bt + 8 * k);
      T.bridx[k] = rd32(p, bi + 4 * k);
      T.brslot[k] = rd16(p, bs + 2 * k);
   }
   for($d k = 0; k < ( $d )T.nk; k ++) {
      T.sonidx[k] = rd32(p, si + 4 * k);
      for(u$q q = 0; q < A; q ++)
         T.soncol[(size_t)k * A + q] = rd32(p, sc + 4 * (k * A + q));
   }
   for($d k = 0; k < ( $d )T.ns; k ++)
      T.lost[k] = rd16(p, ls + 2 * k);

   ret 0;
}

static const char *rsname(u$b s) {
   switch(s) {
      case 0: ret "OK";
      case 1: ret "COOLING";
      case 2: ret "NO_PAINT_TOKEN";
      case 3: ret "BAD_PAINT_TOKEN";
      case 4: ret "BANNED";
      case 5: ret "CLOSED";
      case 6: ret "IP_LIMIT";
      default: ret "UNKNOWN";
   }
}

static bool has_input(tcSocket &ws) {
   if(ws.heat < ws.buf.size()) ret true;
   if(ws.ssl && SSL_pending(ws.ssl) > 0) ret true;
   pollfd pfd{ws.file, POLLIN, 0};
   ret poll(&pfd, 1, 0) > 0;
}

// 画布线程（public=1）：只维护公共画面，绘画由 getWork 负责
fn getPublic() {
   while(!g_sig && !g_abort) {
      try {
         tcSocket pub;
         std::string S = "/ws?public=1";
         if(since != 0) S = S + "&since=" + std::to_string(since);
         pub.connect(ikapb_host, ikapb_port, S.c_str());
         pub.set_timeouts(180, 10);
         logp("[pub] connected {}\n", S);
         while(!g_sig) {
            if(!has_input(pub)) {
               pollfd pfd{pub.file, POLLIN, 0};
               $d pr = poll(&pfd, 1, 200);     // 200ms 一跳，方便响应 SIGINT
               if(pr < 0 && errno != EINTR) throw std::runtime_error("poll failed");
               continue;
            }
            auto pmsg = pub.recvbin();
            if(!pmsg) break;
            stream org;
            zstddec(*pmsg, org, 16u << 20);
            readptr P = {&org, 0};
            u$b typ = P.readb();
            if(typ == 0x84) {
               procRule(P);
            }
            elif(typ == 0x83) {
               procSnap(P);
            }
            elif(typ == 0x82) {
               if(procPublic(P)) break;
            }
            // 未知类型忽略（前向兼容）
         }
      } catch(const std::exception &e) {
         if(!g_sig && !g_abort)
            logp("[pub] {}\n", e.what());
      }
      if(g_sig || g_abort) break;
      logp("[pub] reconnect\n");
      ::sleep(1);
   }
}

// ===================== PPM 输出 / 输入 =====================

static bool write_ppm(const char *path, const vector<u$d> &pix, u$d W, u$d H) {
   FILE *f = fopen(path, "wb");
   if(!f) { logp("[ppm] cannot write {}\n", path); ret false; }
   fprintf(f, "P6\n%u %u\n255\n", W, H);
   for(size_t i = 0; i < pix.size(); i ++) {
      u$d c = pix[i], a = c >> 24;
      u$b rgb[3] = {
         u$b((c & 255) * a / 255 + (255 - a)),
         u$b(((c >> 8) & 255) * a / 255 + (255 - a)),
         u$b(((c >> 16) & 255) * a / 255 + (255 - a))
      };
      fwrite(rgb, 1, 3, f);
   }
   fclose(f);
   logp("[ppm] wrote {} ({}x{})\n", path, W, H);
   ret true;
}

static bool write_board(const char *path) {
   std::lock_guard<std::mutex> lk(canvas_mu);
   if(cols.empty()) { logp("[ppm] board not initialized\n"); ret false; }
   vector<u$d> pix((size_t)glob.W * glob.H);
   for($d y = 0; y < ( $d )glob.H; y ++)
      for($d x = 0; x < ( $d )glob.W; x ++)
         pix[(size_t)y * glob.W + x] = cols[y][x];
   ret write_ppm(path, pix, glob.W, glob.H);
}

static bool write_sonar(const char *path) {
   std::lock_guard<std::mutex> lk(canvas_mu);
   if(sonar_col.empty()) { logp("[ppm] no sonar data\n"); ret false; }
   ret write_ppm(path, sonar_col, glob.W, glob.H);
}

static u$d hexval(char c) {
   if(c >= '0' && c <= '9') ret u$d(c - '0');
   if(c >= 'a' && c <= 'f') ret u$d(c - 'a' + 10);
   if(c >= 'A' && c <= 'F') ret u$d(c - 'A' + 10);
   ret 99;
}

// "RRGGBB" / "RRGGBBAA" -> u32，内存字节序 R,G,B,A
static bool parse_color(const char *s, u$d &c) {
   size_t n = strlen(s);
   u$d v = 0;
   for(size_t i = 0; i < n; i ++) {
      u$d d = hexval(s[i]);
      if(d > 15) ret false;
      v = (v << 4) | d;
   }
   u$d r, g, b, a = 255;
   if(n == 6) {
      r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
   } elif(n == 8) {
      r = (v >> 24) & 255; g = (v >> 16) & 255; b = (v >> 8) & 255; a = v & 255;
      if(a != 255) ret false;
   } else ret false;
   c = r | (g << 8) | (b << 16) | (a << 24);
   ret true;
}

static bool parse_token(const char *s, array<u$b,16> &out) {
   char h[33];
   $d n = 0;
   for(const char *p = s; *p; p ++) {
      if(*p == '-') continue;
      if(n >= 32) ret false;
      u$d d = hexval(*p);
      if(d > 15) ret false;
      h[n ++] = char(d);
   }
   if(n != 32) ret false;
   for($d i = 0; i < 16; i ++)
      out[i] = u$b((h[2 * i] << 4) | h[2 * i + 1]);
   ret true;
}

// P6 PPM -> RGBA u32 列表（A 固定 255）
static bool ppm_load(const char *path, u$d &W, u$d &H, vector<u$d> &out) {
   FILE *f = fopen(path, "rb");
   if(!f) { logp("[ppm] cannot open {}\n", path); ret false; }

   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   vector<u$b> d;
   if(n <= 0) { fclose(f); ret false; }
   d.resize((size_t)n);
   if(fread(d.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); ret false; }
   fclose(f);

   size_t i = 0;
   auto skip = [&]() {
      while(i < d.size()) {
         if(isspace((unsigned char)d[i])) { i ++; continue; }
         if(d[i] == '#') {
            while(i < d.size() && d[i] != '\n') i ++;
            continue;
         }
         break;
      }
   };
   auto tok = [&](u$d &v) -> bool {
      skip();
      if(i >= d.size()) ret false;
      u$d x = 0;
      bool any = false;
      while(i < d.size() && isdigit((unsigned char)d[i])) {
         x = x * 10 + (u$d)(d[i] - '0');
         i ++; any = true;
      }
      if(!any) ret false;
      v = x;
      ret true;
   };

   skip();
   // 不是 P6 就静默返回（--image 传 png 时会先试 PPM，别刷 "not P6" 噪音）
   if(d.size() < 2 || d[0] != 'P' || d[1] != '6') ret false;
   i = 2;
   u$d mx = 0;
   if(!tok(W) || !tok(H) || !tok(mx) || mx != 255) { logp("[ppm] bad header\n"); ret false; }
   if(i < d.size() && isspace((unsigned char)d[i])) i ++;

   u$q need = (u$q)W * H * 3;
   if(W == 0 || H == 0 || need > (u$q)(d.size() - i)) { logp("[ppm] truncated\n"); ret false; }

   out.resize((size_t)W * H);
   for(size_t p = 0; p < out.size(); p ++) {
      u$d r = d[i], g = d[i + 1], b = d[i + 2];
      i += 3;
      out[p] = r | (g << 8) | (b << 16) | 0xFF000000u;
   }
   ret true;
}

// ===================== 命令行 =====================

static bool g_observe = false;
static bool g_dry = false;
static u$d g_limit = 0;
static const char *g_sonar_out = nullptr;
static const char *g_board_out = nullptr;
static u$d g_ox = 950, g_oy = 8;   // 目标图左上角默认偏移（--offset 覆盖）

// ===================== 绘画调度（全局操作队列 + 实施线程 + 三组 token） =====================
//
// 架构：
//   生产者线程（paint / repair / sonar）只做一件事：把“想画哪一格、什么颜色”的
//   操作（op_t）塞进全局时间优先队列 q_ops（q_mu 保护，按 due、seq 排序）。
//   实施线程 getWork() 独占 websocket 连接，每隔 --exec-ms 唤醒一次：
//     1. 绑定尚未绑定的槽位；
//     2. 从队列里取出所有 due<=now 的操作，给每个操作挑一个同组内“已就绪”的槽位；
//     3. 合批成 BATCH 发出，并立即把该槽位的画笔投影前进（“释放画笔”），
//        使同一 slot 的下一条操作能在冷却结束后继续排队；
//     4. 收 TICK，按顺序把结果映射回操作，用权威 brush 校正投影。
//
// 三组 token：
//   G_PAINT  绘制组：周期性把目标图里还没画好的像素写进去
//   G_REPAIR 修复组：周期性比较声纳与目标，把“相差较大”的像素重新画一遍
//   G_SONAR  声纳组：用较稀疏的 token 重画期望色来换取声纳读数（探测绘制区域）
//
// 目标图：target_image 是画布尺寸的“期望颜色 + 是否已知”表。图片由
//   stb_image（png/jpg/...）或 PPM 解码成 src_image，再叠加 --draw 生成。

struct rawjob { u$d x, y, color; };
static vector<rawjob> g_raw;

struct src_image {
   u$d W = 0, H = 0;
   vector<u$d> pix;
   bool ok = false;
};

struct target_image {
   u$d W = 0, H = 0;
   vector<u$d> pix;      // 画布尺寸，idx = y*W+x
   vector<u$b> known;    // 1 = 目标色已知
   bool built = false;

   u$d at(u$d idx) const { ret idx < pix.size() ? pix[idx] : 0; }
   bool has(u$d idx) const { ret idx < known.size() && known[idx]; }
};

static src_image g_src;
static target_image g_target;
static u$d g_bb_x0 = 0, g_bb_y0 = 0, g_bb_x1 = 0, g_bb_y1 = 0;   // 目标包围盒
static vector<u$d> g_sp_pts;   // 声纳网格点（idx），build_target 时算好，供常驻 repair 判断
static u$d g_repair_follow = 2;   // repair 组常驻声纳网格点的槽位数（0=关闭）

// stb_image 适配：png/jpg/... → src_image（RGBA，颜色统一转成 R|G<<8|B<<16|A<<24，
// 半透明按白底混合并令 A=255，因为协议只接受 A=255 的绘画颜色）。
static bool image_load_stb(const char *path, src_image &out) {
   int w = 0, h = 0, n = 0;
   unsigned char *data = stbi_load(path, &w, &h, &n, 4);
   if(!data) {
      logp("[image] stb 解码失败 {}: {}\n", path,
           stbi_failure_reason() ? stbi_failure_reason() : "unknown");
      ret false;
   }
   if(w <= 0 || h <= 0) { stbi_image_free(data); ret false; }

   out.W = (u$d)w;
   out.H = (u$d)h;
   out.pix.resize((size_t)w * h);
   for(size_t i = 0; i < (size_t)w * h; i ++) {
      u$d r = data[4 * i], g = data[4 * i + 1], b = data[4 * i + 2], a = data[4 * i + 3];
      if(a != 255) {
         r = r * a / 255 + (255 - a);
         g = g * a / 255 + (255 - a);
         b = b * a / 255 + (255 - a);
      }
      out.pix[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
   }
   stbi_image_free(data);
   out.ok = true;
   logp("[image] stb loaded {} ({}x{})\n", path, w, h);
   ret true;
}

enum op_kind { OP_BIND = 0, OP_PAINT = 1 };
enum grp_id { G_PAINT = 0, G_REPAIR = 1, G_SONAR = 2, G_NONE = 3 };
static const size_t NPOS = (size_t)-1;

static const char *grp_name(int g) {
   switch(g) {
      case G_PAINT: ret "paint";
      case G_REPAIR: ret "repair";
      case G_SONAR: ret "sonar";
      default: ret "none";
   }
}

struct op_t {
   op_kind kind = OP_PAINT;
   grp_id grp = G_NONE;
   size_t slot = NPOS;      // 派发时由实施线程填写
   u$d idx = 0;
   u$d color = 0;
   u$q due = 0;             // 服务器时间（us），0 = 立即
   u$q seq = 0;             // FIFO 次序
   u$d prio = 0;            // 越小越优先（repair：0=坏色，1=复检；其他组恒 0）
};

struct op_cmp {
   bool operator()(const op_t &a, const op_t &b) const {
      if(a.due != b.due) ret a.due < b.due;
      ret a.seq < b.seq;
   }
};

static std::mutex q_mu;
static std::multiset<op_t, op_cmp> q_ops;
static std::atomic<u$q> q_seq{1};
static std::atomic<size_t> q_count{0};
static std::atomic<size_t> g_qc[3] = {};   // 各组在队列里的操作数（限流生产用）

static void q_push(op_t o) {
   if(!o.seq) o.seq = q_seq.fetch_add(1);
   std::lock_guard<std::mutex> lk(q_mu);
   q_ops.insert(o);
   q_count.fetch_add(1);
   if((size_t)o.grp < 3) g_qc[o.grp].fetch_add(1);
}

// 取到 dispatch 的 live 缓冲区里的操作仍然计入 q_count/g_qc，直到真正“发送”
// 或“丢弃”时才调 q_consumed() 递减；否则生产者线程会看到队列瞬间变空而继续
// 补货，第 4 步 requeue 又把这些操作塞回去，队列会一路超过 q_queue_cap。
static void q_take_due(u$q now, vector<op_t> &out) {
   std::lock_guard<std::mutex> lk(q_mu);
   while(!q_ops.empty() && q_ops.begin()->due <= now) {
      out.pb(*q_ops.begin());
      q_ops.erase(q_ops.begin());
   }
}

// 操作最终离开调度系统（已放进 wk.pend 待结果，或已丢弃）：释放计数。
static void q_consumed(const op_t &o) {
   q_count.fetch_sub(1);
   if((size_t)o.grp < 3) g_qc[o.grp].fetch_sub(1);
}

// 把已取出但没配到槽位的操作插回队列：它一直没被 q_consumed，计数不动。
static void q_requeue(op_t o) {
   std::lock_guard<std::mutex> lk(q_mu);
   q_ops.insert(o);
}

// 目标完成/入队标记（跨线程共享）
static std::mutex plan_mu;
static vector<u$b> g_enq;    // 该像素已有操作在队列/在途
static vector<u$b> g_enq_s;  // sonar 专用认领表：声纳探测与 paint 互不阻塞
static vector<u$b> g_done;   // 该像素已确认为目标色
static std::atomic<size_t> g_need_total{0};
static std::atomic<size_t> g_done_total{0};

// 重连后按真实队列内容重置计数：发送失败时可能有 op 被 q_consumed 后丢失。
static void q_recount() {
   std::lock_guard<std::mutex> lk(q_mu);
   size_t c[3] = {0, 0, 0};
   for(auto &o : q_ops) if((size_t)o.grp < 3) c[o.grp] ++;
   q_count.store(q_ops.size());
   for(int i = 0; i < 3; i ++) g_qc[i].store(c[i]);
}

static void plan_alloc(size_t n) {
   std::lock_guard<std::mutex> lk(plan_mu);
   g_enq.assign(n, 0);
   g_enq_s.assign(n, 0);
   g_done.assign(n, 0);
}

static bool plan_claim(u$d idx) {
   std::lock_guard<std::mutex> lk(plan_mu);
   if(idx >= g_enq.size() || g_enq[idx]) ret false;
   g_enq[idx] = 1;
   ret true;
}

// repair 只有在像素已被本轮画对之后才有资格“复检”，
// 否则会和 paint 抢未完成像素的 g_enq，导致 target_done 永远差一截。
static bool plan_done(u$d idx) {
   std::lock_guard<std::mutex> lk(plan_mu);
   ret idx < g_done.size() && g_done[idx];
}

// 声纳专用认领表：和 paint 的 g_enq 分开，声纳可以探测任意网格点（包括还没画的），
// 同时不会挡住 paint 认领同一像素——两边画的都是目标色，撞了最多浪费一笔。
static bool plan_claim_s(u$d idx) {
   std::lock_guard<std::mutex> lk(plan_mu);
   if(idx >= g_enq_s.size() || g_enq_s[idx]) ret false;
   g_enq_s[idx] = 1;
   ret true;
}

static void plan_release_s(u$d idx) {
   std::lock_guard<std::mutex> lk(plan_mu);
   if(idx < g_enq_s.size()) g_enq_s[idx] = 0;
}

// 整轮重画开始时，把队列里还没发出的非 paint 操作（修复复检、声纳探测）全撤掉：
// 这张图马上要整张重画，留着它们只会占着共享 g_enq，让 paint 认领不到像素、
// 看起来像在等修复/声纳跑完。声纳/修复生产者下一轮会重新认领。
static size_t q_drop_nonpaint() {
   vector<u$d> drop_p, drop_s;
   {
      std::lock_guard<std::mutex> lk(q_mu);
      for(auto it = q_ops.begin(); it != q_ops.end(); ) {
         if(it->kind == OP_PAINT && it->grp != G_PAINT) {
            if(it->grp == G_SONAR) drop_s.pb(it->idx);
            else drop_p.pb(it->idx);
            if((size_t)it->grp < 3) g_qc[it->grp].fetch_sub(1);
            it = q_ops.erase(it);
            q_count.fetch_sub(1);
         } else ++ it;
      }
   }
   if(!drop_p.empty() || !drop_s.empty()) {
      std::lock_guard<std::mutex> lk(plan_mu);
      for(u$d idx : drop_p)
         if(idx < g_enq.size()) g_enq[idx] = 0;
      for(u$d idx : drop_s)
         if(idx < g_enq_s.size()) g_enq_s[idx] = 0;
   }
   ret drop_p.size() + drop_s.size();
}

// 运行期配置（CLI 在 main 里填写）
static u$d g_exec_ms = 20;          // 实施线程轮询周期
static u$q g_early_us = 0;         // 冷却提前量（µs）：实测提前发只会换来 COOLING，默认 0；--early-us 可调
static u$d g_paint_ms = 250;        // 绘制组补货周期
static u$d g_repair_ms = 500;       // 修复组巡检周期
static u$d g_repair_verify_ms = 60000;  // 声纳证据超过这么久没更新就复检一次（0=关闭；默认 60s）
static u$d g_sonar_ms = 800;        // 声纳组探测周期
static u$d g_repair_thresh = 96;    // RGB 曼哈顿差阈值（“相差较大”）
static u$d g_sonar_stride = 5;      // 声纳采样步长（0 = 自动：2R+1）
static u$d g_round_cap = 256;       // 每个生产者每轮最多入队操作数
static u$d g_queue_cap = 2048;      // 全局队列软上限，避免一次把整图塞进内存
static u$d g_paint_loop_ms = 0;     // >0：整图刷完一遍后等这么久，再整图刷下一遍
static std::atomic<u$d> g_passes{0}; // 已完成的整图轮数（--paint-loop）
static std::atomic<u$q> g_last_tick{0}; // 最近一帧 TICK 的服务器 tick（判断声纳证据是否过期）
static bool g_daemon = false;
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_stop{false};
static std::atomic<u$d> g_gslots[3];
static std::atomic<u$q> g_ok[3];
static std::atomic<u$q> g_painted{0};     // 所有组 OK 的绘画总数（诊断用）
static std::atomic<u$d> g_help_sent{0};    // 跨组帮忙发出的笔数（诊断）
static std::atomic<u$q> g_sent_grp[3] = {};   // 诊断：各组实际发出的笔数
static std::atomic<u$q> g_drop_np{0};         // 诊断：过滤/无 enq 丢弃的非 paint op 数
static std::atomic<u$q> g_live_np{0};         // 诊断：进入 live 的非 paint op 数

static bool has_group(grp_id g) { ret g_gslots[g].load() > 0; }

static void sleep_ms(u$d ms) {
   std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct slot_t {
   array<u$b,16> tok{};
   grp_id group = G_NONE;
   bool want_bind = true;
   bool used = false;
   bool follow = false;   // repair 常驻声纳网格点：只守这些点的修复

   // 权威画笔（TICK 的 brush 列）
   bool a_has = false;
   u$d a_idx = 0;
   u$q a_t = 0;

   // 本地投影：派发时前进，相当于“释放画笔”
   bool p_has = false;
   u$d p_idx = 0;
   u$q p_t = 0;

   u$q not_before = 0;     // COOLING 给出的硬下限
   int inflight = 0;       // 在途记录数
};

static const int kMaxInflight = 2;   // c0 远大于 RTT 时通常实际只有 1 条

struct work_t {
   vector<slot_t> sl;
   deque<op_t> pend;        // 在途记录，按发送顺序，TICK 结果一一对应
   u$d W = 0, H = 0, maxmsg = 0, maxpaints = 0;
   u$q c0 = 0, k = 0;
   bool saw_tick = false;
   bool built = false;
   u$d painted = 0;
};
static work_t wk;

static bool is_sp(u$d idx) {   // 是否声纳网格点
   for(u$d p : g_sp_pts) if(p == idx) ret true;
   ret false;
}

static void refresh_cfg() {
   std::lock_guard<std::mutex> lk(canvas_mu);
   wk.W = glob.W; wk.H = glob.H;
   wk.c0 = glob.cooldown_base;
   wk.k = glob.cooldown_per_px;
   wk.maxmsg = glob.max_message_bytes;
   wk.maxpaints = glob.max_paints_per_message ? glob.max_paints_per_message : 65536;
}

static void refresh_params() {
   std::lock_guard<std::mutex> lk(canvas_mu);
   wk.c0 = glob.cooldown_base;
   wk.k = glob.cooldown_per_px;
   wk.maxmsg = glob.max_message_bytes;
   wk.maxpaints = glob.max_paints_per_message ? glob.max_paints_per_message : 65536;
}

static u$q manh_dist(u$d a, u$d b, u$d W) {
   if(!W) ret 0;
   $q ax = ( $q )(a % W), ay = ( $q )(a / W);
   $q bx = ( $q )(b % W), by = ( $q )(b / W);
   u$q dx = ax > bx ? (u$q)(ax - bx) : (u$q)(bx - ax);
   u$q dy = ay > by ? (u$q)(ay - by) : (u$q)(by - ay);
   ret dx + dy;
}

static u$q slot_ready_us(const slot_t &s, u$d idx, u$d W, u$q c0, u$q k) {
   u$q r = 0;
   if(s.p_has) r = s.p_t + c0 + k * manh_dist(idx, s.p_idx, W);
   if(s.not_before > r) r = s.not_before;
   ret r;
}

static u$d colordiff(u$d a, u$d b) {
   u$d ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
   u$d br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
   u$d dr = ar > br ? ar - br : br - ar;
   u$d dg = ag > bg ? ag - bg : bg - ag;
   u$d db = ab > bb ? ab - bb : bb - ab;
   ret dr + dg + db;
}

// 把 --ppm/--image 的源图 + --draw 叠加成画布尺寸的目标表
static void build_target() {
   u$d W = 0, H = 0;
   {
      std::lock_guard<std::mutex> lk(canvas_mu);
      W = glob.W; H = glob.H;
   }
   if(!W || !H) ret;

   size_t N = (size_t)W * H;
   g_target.W = W; g_target.H = H;
   g_target.pix.assign(N, 0);
   g_target.known.assign(N, 0);

   // 1) 图片（stb/PPM）按 --offset 贴到画布
   if(g_src.ok) {
      for(u$d y = 0; y < g_src.H; y ++)
         for(u$d x = 0; x < g_src.W; x ++) {
            u$d gx = x + g_ox, gy = y + g_oy;
            if(gx >= W || gy >= H) continue;
            size_t p = (size_t)gy * W + gx;
            g_target.pix[p] = g_src.pix[(size_t)y * g_src.W + x];
            g_target.known[p] = 1;
         }
   }
   // 2) --draw 逐点叠加（优先级最高）
   for(auto &r : g_raw) {
      if(r.x >= W || r.y >= H) continue;
      size_t p = (size_t)r.y * W + r.x;
      g_target.pix[p] = r.color;
      g_target.known[p] = 1;
   }
   g_target.built = true;
   plan_alloc(N);

   size_t nk = 0;
   g_bb_x0 = W; g_bb_y0 = H; g_bb_x1 = 0; g_bb_y1 = 0;
   for(u$d y = 0; y < H; y ++)
      for(u$d x = 0; x < W; x ++)
         if(g_target.known[(size_t)y * W + x]) {
            nk ++;
            if(x < g_bb_x0) g_bb_x0 = x;
            if(y < g_bb_y0) g_bb_y0 = y;
            if(x > g_bb_x1) g_bb_x1 = x;
            if(y > g_bb_y1) g_bb_y1 = y;
         }
   g_need_total = nk;
   g_done_total = 0;
   if(!nk) { g_bb_x0 = 0; g_bb_y0 = 0; g_bb_x1 = W - 1; g_bb_y1 = H - 1; }

   logp("[target] {}x{} known={} (src={} draw={}) bbox={},{}..{},{} \n",
        W, H, (u$d)nk, g_src.ok ? "yes" : "no", (u$d)g_raw.size(),
        g_bb_x0, g_bb_y0, g_bb_x1, g_bb_y1);

   // 声纳网格点：与 sonar_worker 的 stride/行优先完全一致，供常驻 repair 槽位判断
   {
      std::lock_guard<std::mutex> lk(canvas_mu);
      u$d stride = g_sonar_stride;
      u$d need = 2 * (u$d)glob.rad + 1;
      if(stride < need) stride = need;
      if(stride < 1) stride = 1;
      g_sp_pts.clear();
      u$d bw = (g_bb_x1 >= g_bb_x0) ? (g_bb_x1 - g_bb_x0) / stride + 1 : 0;
      u$d bh = (g_bb_y1 >= g_bb_y0) ? (g_bb_y1 - g_bb_y0) / stride + 1 : 0;
      for(u$d k = 0; k < bw * bh; k ++) {
         u$d gx = g_bb_x0 + (k % bw) * stride;
         u$d gy = g_bb_y0 + (k / bw) * stride;
         if(gx >= W || gy >= H) continue;
         g_sp_pts.pb(gy * W + gx);
      }
      logp("[target] sonar posts={}\n", (u$d)g_sp_pts.size());
   }
}

static bool target_complete() {
   size_t need = g_need_total.load();
   ret need > 0 && g_done_total.load() >= need;
}

static void send_binds(tcSocket &ws) {
   vector<bt> bs;
   vector<$d> ids;
   for($d i = 0; i < ( $d )wk.sl.size(); i ++)
      if(wk.sl[i].want_bind && !wk.sl[i].used) { bs.pb({i, wk.sl[i].tok}); ids.pb(i); }
   if(bs.empty()) ret;

   stream pl = enc_batch(bs, {});
   stream wire; zstdenc(pl, wire, 1);
   if(wire.size() > wk.maxmsg) {
      logp("[work] bind frame {} > max_message_bytes {}\n", (u$d)wire.size(), wk.maxmsg);
      g_abort = true;
      ret;
   }
   ws.sendbin(wire);
   for($d i : ids) {
      wk.sl[i].want_bind = false;
      op_t o; o.kind = OP_BIND; o.grp = wk.sl[i].group; o.slot = i;
      wk.pend.pb(o);
   }
   logp("[work] bind {} slot(s)\n", (u$d)bs.size());
}

// 取所有到期操作，按“槽位 -> 最近到期操作”配对（距离优先，减少 k·d 冷却），
// 合批发送，并把画笔投影前进（“释放画笔”）。未配到槽位的 20ms 后重试。
static void dispatch_ops(tcSocket &ws) {
   u$q now = srv_now();
   if(!now) now = 1;
   vector<op_t> due;
   q_take_due(now, due);
   if(due.empty()) ret;

   // 1) 过滤：只保留仍有效的绘画操作
   vector<op_t> live;
   live.reserve(due.size());
   for(auto &o : due) {
      if(o.kind != OP_PAINT) continue;
      bool still = false;
      {
         std::lock_guard<std::mutex> lk(plan_mu);
         if(o.grp == G_SONAR) {
            // 声纳用独立认领表：只看声纳侧是否仍持有该点，不受 paint 的 g_enq 影响。
            // 分块归属的槽位若已解绑/失效，这笔 op 永远不会被派发，直接丢弃并释放。
            bool own = (o.slot < wk.sl.size() && wk.sl[o.slot].used &&
                        !wk.sl[o.slot].want_bind);
            if(own && o.idx < g_enq_s.size() && g_enq_s[o.idx]) still = true;
            else {
               if(o.idx < g_enq_s.size()) g_enq_s[o.idx] = 0;
               g_drop_np.fetch_add(1);
            }
         } else if(o.idx < g_enq.size() && g_enq[o.idx]) {
            bool done = (o.idx < g_done.size() && g_done[o.idx]);
            // 绘制组只处理未完成像素；修复组画的就是目标色，任何时候都有效
            bool valid = (o.grp == G_PAINT) ? !done : true;
            if(valid) still = true;
            else { g_enq[o.idx] = 0; if(o.grp != G_PAINT) g_drop_np.fetch_add(1); }
         } else if(o.grp != G_PAINT) g_drop_np.fetch_add(1);
      }
      if(still) { live.pb(o); if(o.grp != G_PAINT) g_live_np.fetch_add(1); }
      else q_consumed(o);   // 过滤掉的操作不会发送，补上计数释放
   }
   if(live.empty()) ret;

   vector<char> used(live.size(), 0);
   vector<pair<size_t, op_t> > chosen;

   // 2) 给每个空闲槽位挑同组里到期的操作：
   //    sonar 组按入队顺序（seq）服务，保证固定扫描不因近点插队而饿死；
   //    paint/repair 组按距离最近挑，减少 k·d 冷却（repair 每个像素只认领一次，
   //    队列不会无限补近点，所以不会被饿死）。
   for(size_t i = 0; i < wk.sl.size(); i ++) {
      slot_t &s = wk.sl[i];
      if(!s.used || s.want_bind || s.inflight >= kMaxInflight) continue;
      size_t best = NPOS;
      u$d bd = 0;
      u$q br = 0;
      u$d bp = 0;   // 当前 best 的优先级（repair：0=坏色，1=复检）
      if(s.group == G_SONAR) {
         // 每个 sonar 槽位只接自己那一块的操作（生产者已按槽位分配 o.slot）；
         // 块内蛇形顺序由生产者的游标保证，槽位之间不互相抢，也就不会跨块跳跃。
         for(size_t j = 0; j < live.size(); j ++) {
            if(used[j] || live[j].grp != s.group || live[j].slot != (u$d)i) continue;
            if(slot_ready_us(s, live[j].idx, wk.W, wk.c0, wk.k) <= now + g_early_us) best = j;
            break;   // 只认自己那一笔：没到冷却就等，不允许后入队的插队
         }
      } else {
         for(size_t j = 0; j < live.size(); j ++) {
            if(used[j] || live[j].grp != s.group) continue;
            if(s.follow && !is_sp(live[j].idx)) continue;   // 常驻槽位只守声纳网格点
            u$q r = slot_ready_us(s, live[j].idx, wk.W, wk.c0, wk.k);
            if(r > now + g_early_us) continue;
            u$d d = s.p_has ? manh_dist(live[j].idx, s.p_idx, wk.W) : 0;
            // 先比优先级（坏色 repair 优先），再比距离，最后比就绪时间
            bool better = false;
            if(best == NPOS) better = true;
            else if(live[j].prio != bp) better = (live[j].prio < bp);
            else better = (d < bd || (d == bd && r < br));
            if(better) { best = j; bd = d; br = r; bp = live[j].prio; }
         }
      }
      // 不再跨组代画：repair/sonar 槽位只发本组的操作，本组没活就空等。
      // 代画会让声纳/修复槽位被 paint 的活占满（它们总能挑到“更近、随时就绪”的
      // paint op），扫描/修复的实际进度被拖慢——2026-11-21 用户实测声纳仍只聚集
      // 在少数网格点上，因此取消该兜底。
      if(best == NPOS) continue;
      used[best] = 1;
      op_t o = live[best];
      if(o.grp != s.group) g_help_sent.fetch_add(1);   // 诊断：跨组帮忙
      g_sent_grp[(int)o.grp].fetch_add(1);
      if(s.follow) {
         static bool flog = getenv("IKAPB_FOLLOW_LOG") != nullptr;
         if(flog) logp("[follow] slot={} idx={} grp={}\n", (u$d)i, o.idx, grp_name(o.grp));
      }
      u$d dd = s.p_has ? ( $d )manh_dist(o.idx, s.p_idx, wk.W) : -1;
      static const bool dlog = getenv("IKAPB_DIAG2") != nullptr;
      if(dlog) logp("[d] slot={} d={} grp={}\n", (u$d)i, dd, grp_name(o.grp));
      s.inflight ++;
      s.p_t = now; s.p_idx = o.idx; s.p_has = true;   // 释放画笔：投影前进
      o.slot = i;
      chosen.pb({i, o});
   }

   // 3) 合批发送（保守的 4096 条/消息，远低于 1MiB 压缩上限）
   size_t chunk = 4096;
   if(wk.maxpaints && chunk > wk.maxpaints) chunk = wk.maxpaints;
   size_t at = 0;
   while(at < chosen.size()) {
      size_t end = at + chunk;
      if(end > chosen.size()) end = chosen.size();
      vector<pt> ps;
      for(size_t i = at; i < end; i ++)
         ps.pb({( $d )chosen[i].first, chosen[i].second.idx, chosen[i].second.color});

      stream pl = enc_batch({}, ps);
      stream wire; zstdenc(pl, wire, 1);
      if(wire.size() > wk.maxmsg) {
         logp("[work] paint frame {} > max_message_bytes {}\n", (u$d)wire.size(), wk.maxmsg);
         g_abort = true; ret;
      }
      ws.sendbin(wire);
      for(size_t i = at; i < end; i ++) {
         wk.pend.pb(chosen[i].second);
         q_consumed(chosen[i].second);   // 已发送，离开队列计数
      }
      at = end;
   }
   static u$d exec_sends = 0;
   if(!chosen.empty() && ((++exec_sends % 20) == 0 || chosen.size() >= 64))
      logp("[exec] sent {} op(s) q={} sends={}\n", (u$d)chosen.size(), q_count.load(), exec_sends);

   // 4) 没配到槽位的操作 20ms 后重试（不能丢，否则 g_enq 永远挂着）
   for(size_t j = 0; j < live.size(); j ++)
      if(!used[j]) { op_t r = live[j]; r.due = now + 20000; q_requeue(r); }
}

static void apply_sonar(const tick_info &T) {
   std::lock_guard<std::mutex> lk(canvas_mu);
   u$d W = glob.W, H = glob.H;
   if(!W || !H || sonar_col.size() != (size_t)W * H) ret;

   u$q A = 2ull * T.R * T.R + 2ull * T.R + 1;
   for($d k = 0; k < ( $d )T.nk; k ++) {
      u$d idx = T.sonidx[k];
      if((u$q)idx >= (u$q)W * H) continue;
      $d cx = ( $d )(idx % W), cy = ( $d )(idx / W);
      size_t q = 0;
      for($d dy = -T.R; dy <= T.R; dy ++) {
         $d w = T.R - abs(dy);
         for($d dx = -w; dx <= w; dx ++) {
            u$d c = T.soncol[(size_t)k * A + q ++];
            $d px = cx + dx, py = cy + dy;
            if(px < 0 || py < 0 || px >= ( $d )W || py >= ( $d )H) continue;
            size_t p = (size_t)py * W + px;
            sonar_col[p] = c;
            sonar_tick[p] = T.tick;
         }
      }
   }
}

static void apply_tick(const tick_info &T) {
   wk.saw_tick = true;
   g_last_tick.store((u$q)T.tick);
   u$q now = srv_now();

   if(( $d )T.nr > ( $d )wk.pend.size())
      logp("[work] {} results but only {} pending (extra ignored)\n", T.nr, (u$d)wk.pend.size());

   for($d k = 0; k < ( $d )T.nr && !wk.pend.empty(); k ++) {
      op_t rec = wk.pend.front();
      wk.pend.pop_front();
      u$b st = T.status[k];
      u$d val = T.value[k];

      if(rec.kind == OP_BIND) {
         if(rec.slot >= wk.sl.size()) continue;
         slot_t &s = wk.sl[rec.slot];
         if(st == 0) {
            s.used = true;
            logp("[work] slot {} bound ({}{})\n", rec.slot, grp_name(s.group), s.follow ? ", follow" : "");
         } else {
            logp("[work] bind slot {} failed: {} value={}\n", rec.slot, rsname(st), val);
            g_abort = true;
         }
         continue;
      }

      if(rec.slot >= wk.sl.size()) continue;
      slot_t &s = wk.sl[rec.slot];
      if(s.inflight > 0) s.inflight --;

      if(st == 0) {
         wk.painted ++;
         g_painted.fetch_add(1);
         g_ok[rec.grp].fetch_add(1);

         bool hit = (rec.idx < g_target.known.size() && g_target.known[rec.idx] &&
                     g_target.pix[rec.idx] == rec.color);
         {
            std::lock_guard<std::mutex> lk(plan_mu);
            if(rec.grp == G_SONAR) {
               if(rec.idx < g_enq_s.size()) g_enq_s[rec.idx] = 0;
            } else if(rec.idx < g_enq.size()) g_enq[rec.idx] = 0;
            if(hit && rec.idx < g_done.size() && !g_done[rec.idx]) {
               g_done[rec.idx] = 1;
               g_done_total.fetch_add(1);
            }
         }
         if(wk.painted <= 5 || wk.painted % 50 == 0) {
            char hex[16];
            snprintf(hex, sizeof(hex), "%08x", rec.color);
            logp("[work] OK {} idx={} color={} hit={} group={} total={}\n",
                 wk.painted, rec.idx, hex, hit ? 1 : 0, grp_name(rec.grp), g_painted.load());
         }
      } elif(st == 1) {
         s.p_t = s.a_t; s.p_idx = s.a_idx; s.p_has = s.a_has;
         u$q nb = now + (val ? val : 1000);
         s.not_before = nb;
         // 冷却被拒 = 这一笔没画上：保留 op，冷却到期后重试同一个像素。
         // 旧实现丢掉 op 并释放 g_enq，生产者会立刻换个（更远的）像素重新认领；
         // 而客户端并不知道画笔真实位置（p_has=false 时 d 按 0 估），于是整条连接
         // 变成「发送 → 冷却 → 丢弃 → 重认领」的空转（实测 ~88 次/秒、OK 仅 3 次/秒）。
         op_t again = rec;
         again.slot = (rec.grp == G_SONAR) ? rec.slot : NPOS;   // 声纳保留分块归属
         again.due = nb + 20000;   // 20ms 余量，避免在边界上又吃一次冷却
         q_push(again);             // g_enq 保持 1，不释放
         {
            static u$d clog = 0;
            if((++clog % 50) == 0 || val >= 300000)
               logp("[work] cooling slot {} {} ms\n", rec.slot, val / 1000);
         }
      } elif(st == 2) {
         s.used = false; s.want_bind = true; s.inflight = 0;
         {
            std::lock_guard<std::mutex> lk(plan_mu);
            if(rec.grp == G_SONAR) {
               if(rec.idx < g_enq_s.size()) g_enq_s[rec.idx] = 0;
            } else if(rec.idx < g_enq.size()) g_enq[rec.idx] = 0;
         }
      } else {
         logp("[work] paint slot {} failed: {} value={}\n", rec.slot, rsname(st), val);
         s.inflight = 0;
         {
            std::lock_guard<std::mutex> lk(plan_mu);
            if(rec.grp == G_SONAR) {
               if(rec.idx < g_enq_s.size()) g_enq_s[rec.idx] = 0;
            } else if(rec.idx < g_enq.size()) g_enq[rec.idx] = 0;
         }
      }
   }

   // 释放槽位（lost.slot）
   for(u$d sid : T.lost) {
      if(sid >= wk.sl.size()) continue;
      slot_t &s = wk.sl[sid];
      s.used = false; s.want_bind = true; s.inflight = 0;
      s.a_has = s.p_has = false; s.not_before = 0;
      logp("[work] slot {} released\n", sid);
   }

   // 画笔是服务器权威值，最后应用
   for($d k = 0; k < ( $d )T.nb; k ++) {
      u$d sid = T.brslot[k];
      if(sid >= wk.sl.size()) continue;
      slot_t &s = wk.sl[sid];
      s.a_t = T.brt[k]; s.a_idx = T.bridx[k]; s.a_has = (T.brt[k] != 0);
      s.p_t = s.a_t; s.p_idx = s.a_idx; s.p_has = s.a_has;
   }

   apply_sonar(T);
}

static bool handle_msg(stream &raw) {
   if(raw.empty()) ret true;
   if(raw[0] == 0x84) {
      readptr P = {&raw, 1};
      procRule(P);
      if(wk.built) refresh_params();
      ret true;
   }
   if(raw[0] == 0x81) {
      tick_info T;
      $d rc = procTick(raw, T);
      if(rc == 0) apply_tick(T);
      else logp("[work] bad TICK rc={} len={}\n", rc, (u$d)raw.size());
      ret true;
   }
   ret false;
}

static bool should_stop() {
   if(g_sig || g_abort || g_stop.load()) ret true;
   if(g_dry && wk.saw_tick) ret true;
   if(g_limit && g_painted.load() >= (u$q)g_limit) ret true;
   if(g_daemon) ret false;
   // --paint-loop 是常驻刷图，不能画完就退
   if(g_paint_loop_ms) ret false;
   // 没有修复/声纳组时，目标画完就收工；有这两组则持续防守（--daemon 也可强制常驻）
   if(!has_group(G_REPAIR) && !has_group(G_SONAR) &&
      target_complete() && q_count.load() == 0 && wk.pend.empty())
      ret true;
   ret false;
}

// 泄漏回收：op 可能在 dispatch_ops 发送抛异常时从队列消失（g_enq 仍为 1），
// 或走任何未覆盖的分支。定期把“已认领、未完成、既不在队列也不在途”的像素释放，
// 否则 paint 永远认领不了它 → 整轮卡在一个点上，sonar 还在扫。
static void unstick_leaks() {
   std::unordered_set<u$d> keep;
   {
      std::lock_guard<std::mutex> lk(q_mu);
      keep.reserve(q_ops.size() * 2 + 8);
      for(auto &o : q_ops)
         if(o.kind == OP_PAINT) keep.insert(o.idx);
   }
   for(auto &o : wk.pend)
      if(o.kind == OP_PAINT) keep.insert(o.idx);
   u$d freed = 0;
   {
      std::lock_guard<std::mutex> lk(plan_mu);
      for(size_t p = 0; p < g_enq.size(); p ++)
         if(g_enq[p] && (p >= g_done.size() || !g_done[p]) && !keep.count((u$d)p)) {
            g_enq[p] = 0;
            freed ++;
         }
      // 声纳认领表也要回收：op 在发送抛异常时同样会消失
      for(size_t p = 0; p < g_enq_s.size(); p ++)
         if(g_enq_s[p] && !keep.count((u$d)p)) {
            g_enq_s[p] = 0;
            freed ++;
         }
   }
   if(freed) logp("[schedule] released {} stuck pixel(s)\n", freed);
}

// 断线：把在途绘画操作放回队列（绑定直接丢弃，重连会重绑）
static void requeue_pending() {
   while(!wk.pend.empty()) {
      op_t o = wk.pend.front();
      wk.pend.pop_front();
      if(o.kind == OP_PAINT) {
         if(o.grp == G_SONAR) { plan_release_s(o.idx); continue; }  // 重连后由生产者重新分块认领
         o.slot = NPOS;
         o.due = 0;
         q_push(o);          // 保留 g_enq 标记
      }
   }
   for(auto &s : wk.sl) s.inflight = 0;
}

static void run_conn(tcSocket &ws) {
   bool got_rules = false;
   while(!got_rules) {
      if(g_sig || g_abort) ret;
      auto m = ws.recvbin();
      if(!m) throw std::runtime_error("closed before RULES");
      stream raw; zstddec(*m, raw, 16u << 20);
      if(raw.empty()) continue;
      if(raw[0] == 0x84) {
         readptr P = {&raw, 1};
         procRule(P);
         got_rules = true;
      }
   }

   refresh_cfg();
   if(!wk.built) {
      if(wk.maxmsg == 0) { g_abort = true; ret; }
      build_target();
      wk.built = true;
      if(g_paint_loop_ms)
         logp("[target] paint-loop on: 每轮扫完后等 {} ms 再刷\n", g_paint_loop_ms);
   }
   if(( $d )wk.sl.size() > ( $d )glob.max_slots) {
      logp("[work] {} tokens > max_slots {}\n", (u$d)wk.sl.size(), glob.max_slots);
      g_abort = true;
      ret;
   }

   // 新连接：槽位状态清空（在途操作已在重连前 requeue）
   for(auto &s : wk.sl) {
      s.used = false; s.want_bind = true; s.inflight = 0;
      s.a_has = s.p_has = false; s.not_before = 0;
   }
   wk.pend.clear();
   q_recount();   // 重连后按真实队列重置计数（发送失败时可能有 op 只减不消失）
   wk.saw_tick = false;
   g_ready = false;

   while(!g_sig && !g_abort) {
      if(should_stop()) ret;

      send_binds(ws);
      dispatch_ops(ws);
      // 每 2 秒回收一次“无法再被发送”的认领标记
      {
         static auto t_gc = std::chrono::steady_clock::now();
         if(std::chrono::steady_clock::now() - t_gc >= std::chrono::seconds(2)) {
            t_gc = std::chrono::steady_clock::now();
            unstick_leaks();
         }
      }

      // 临时诊断：每 5 秒打印一次队列/完成度/各组空闲槽位数
      static const bool stat_on = getenv("IKAPB_STAT") != nullptr;
      static auto t_stat = std::chrono::steady_clock::now();
      if(stat_on && std::chrono::steady_clock::now() - t_stat >= std::chrono::seconds(5)) {
         t_stat = std::chrono::steady_clock::now();
         u$d fr[3] = {0, 0, 0};
         for(auto &s : wk.sl)
            if(s.used && !s.want_bind && s.inflight < kMaxInflight) fr[( $d )s.group] ++;
         // 诊断：最早还能再发一笔的槽位还要等多久（只看 c0，不含 k·d）
         u$q nowd = srv_now();
         u$q minw = 0;
         bool hasw = false;
         for(auto &s : wk.sl)
            if(s.used && !s.want_bind) {
               u$q t = s.p_has ? s.p_t + wk.c0 : 0;
               if(!hasw || t < minw) { minw = t; hasw = true; }
            }
         u$q waitms = (!hasw || minw <= nowd) ? 0 : (minw - nowd) / 1000;
         logp("[stat] q={} done={}/{} painted={} passes={} free[p={} r={} s={}] pend={} help={} minwait={}ms live_np={} drop_np={} sent[p={} r={} s={}] qc[p={} r={} s={}]\n",
              q_count.load(), g_done_total.load(), g_need_total.load(),
              g_painted.load(), g_passes.load(), fr[0], fr[1], fr[2], (u$d)wk.pend.size(), g_help_sent.load(), waitms,
              g_live_np.load(), g_drop_np.load(), g_sent_grp[0].load(), g_sent_grp[1].load(), g_sent_grp[2].load(),
              g_qc[0].load(), g_qc[1].load(), g_qc[2].load());
      }

      stream wire;
      $d rr = ws.try_recvbin(wire);
      if(rr == 2) throw std::runtime_error("connection closed");
      if(rr == 1) {
         stream raw; zstddec(wire, raw, 16u << 20);
         handle_msg(raw);
         if(!g_ready.load()) {
            bool all = !wk.sl.empty();
            for(auto &s : wk.sl) if(!s.used) all = false;
            if(all) {
               g_ready = true;
               logp("[work] all {} slot(s) bound, producers go\n", (u$d)wk.sl.size());
            }
         }
         continue;
      }

      pollfd pfd{ws.file, POLLIN, 0};
      $d to = ( $d )g_exec_ms;
      if(to < 1) to = 1;
      $d pr = poll(&pfd, 1, to);
      if(pr < 0 && errno != EINTR) throw std::runtime_error("poll failed");
   }
}

fn getWork() {
   while(!g_sig && !g_abort && !g_stop.load()) {
      try {
         tcSocket ws;
         logp("[work] connecting...\n");
         ws.connect(ikapb_host, ikapb_port, "/ws?public=0");
         ws.set_timeouts(30, 10);
         logp("[work] connected\n");
         run_conn(ws);
      } catch(const std::exception &e) {
         if(g_sig) break;
         logp("[work] {}\n", e.what());
      }
      if(g_sig || g_abort) break;
      if(should_stop()) break;
      logp("[work] reconnect\n");
      requeue_pending();
      ::sleep(1);
   }
   g_stop = true;
   g_ready = false;
}

// ===================== 生产者线程 =====================

// 绘制组：周期性把目标图里还没画好的像素塞进队列；
// --paint-loop 开启时无视 g_done，整图一轮一轮循环刷（防别人覆盖）
fn paint_worker() {
   u$d cursor = 0;
   while(!g_sig && !g_abort && !g_stop.load()) {
      if(!g_ready.load()) { sleep_ms(50); continue; }
      if(g_limit && g_painted.load() >= (u$q)g_limit) break;

      // --paint-loop：整张图确认完成后，等 N 秒，清空完成标记，再整图刷一遍
      if(target_complete()) {
         if(g_paint_loop_ms > 0) {
            for(u$d t = 0; t < g_paint_loop_ms && !g_sig && !g_stop.load(); t += 100) {
               if(g_limit && g_painted.load() >= (u$q)g_limit) break;
               sleep_ms(100);
            }
            if(g_sig || g_stop.load()) ret;
            // 先撤掉还没发出的修复/声纳操作，paint 立刻能认领整张图
            size_t dropped = q_drop_nonpaint();
            {
               std::lock_guard<std::mutex> lk(plan_mu);
               std::fill(g_done.begin(), g_done.end(), (u$b)0);
            }
            g_done_total.store(0);
            cursor = 0;
            logp("[paint] pass {} complete -> next pass after {} ms (dropped {} queued repair/sonar op(s))\n",
                 g_passes.fetch_add(1) + 1, g_paint_loop_ms, (u$d)dropped);
            continue;
         }
         sleep_ms(g_paint_ms);
         continue;
      }

      u$d W = g_target.W, H = g_target.H;
      size_t N = (size_t)W * H;
      u$d made = 0;
      size_t scanned = 0;
      if(W && H && g_target.known.size() == N && g_target.built) {
         for(size_t n = 0; n < N && made < g_round_cap && q_count.load() + 256 < g_queue_cap; n ++) {
            size_t p = ((size_t)cursor + n) % N;
            scanned = n + 1;
            if(!g_target.known[p]) continue;
            {
               std::lock_guard<std::mutex> lk(plan_mu);
               if(g_done.size() != N) { scanned = 0; break; }
               if(g_done[p]) continue;
            }
            if(!plan_claim((u$d)p)) continue;
            op_t o; o.kind = OP_PAINT; o.grp = G_PAINT; o.idx = (u$d)p;
            o.color = g_target.pix[p]; o.due = 0;
            q_push(o);
            made ++;
         }
         cursor = (u$d)((cursor + scanned) % N);
      }
      static u$d plog = 0;
      if(made && ((++plog % 20) == 0 || made >= 32))
         logp("[paint] queued {} q={}\n", made, q_count.load());
      sleep_ms(g_paint_ms);
   }
}

// 修复组：周期性比较声纳与目标，把相差较大的像素重新画一遍
fn repair_worker() {
   while(!g_sig && !g_abort && !g_stop.load()) {
      if(!g_ready.load()) { sleep_ms(50); continue; }
      if(g_limit && g_painted.load() >= (u$q)g_limit) break;

      u$d W = g_target.W, H = g_target.H;
      size_t N = (size_t)W * H;
      u$d made = 0;
      if(W && H && g_target.built && g_target.known.size() == N) {
         std::lock_guard<std::mutex> lk(canvas_mu);
         if(sonar_col.size() == N && sonar_tick.size() == N) {
            u$q last_tick = g_last_tick.load();
            u$q verify_span = g_repair_verify_ms
                              ? (u$q)glob.tps * (u$q)g_repair_verify_ms / 1000 : 0;
            // 本轮还没画完时禁止“复检”：否则 repair 会把已经画对的像素反复认领，
            // 占满共享队列、挤掉 paint，整图进度反而停住。复检只在画完后做。
            // 复检限流：pending repair op 不超 16 个；plan_done 保证不抢未画像素。
            // 扫描游标：repair 每轮最多认领 16 个，若每轮都从 p=0 起，前几千个
            // 像素会把名额吃光、后面的像素永远轮不到（日志里 repair 的 OK 只出现在
            // idx≈985..10950）。跨轮保留 rcur，像 sonar 一样轮转整张画布。
            static size_t rcur = 0;
            size_t scanned = 0;
            // 先扫完一个窗口、再按优先级入队：坏色（prio=0）优先于复检（prio=1）。
            // 若边扫边入队，16 个名额会被窗口前半段的“复检”占满，后面真正被
            // 改坏的像素反而排不进去，所以分两趟：先收集，再让坏色先 claim。
            vector<u$d> dmg, chk;
            size_t win = g_round_cap ? g_round_cap : 256;
            if(win > N) win = N;
            for(size_t n = 0; n < win; n ++) {
               size_t p = (rcur + n) % N;
               scanned = n + 1;
               if(!g_target.known[p]) continue;
               u$d idx = (u$d)p;
               if(!plan_done(idx)) continue;   // 未画过的像素归 paint，repair 不抢
               // 需要修的理由有两种：
               //   1) 声纳看到这个像素和目标是两种颜色（被改坏了）——最高优先；
               //   2) 声纳很久没看过/很久没更新（可能早被改坏，但视野里看不到）——复检。
               if(!sonar_tick[p]) {
                  if(verify_span != 0) chk.pb(idx);
               } else if(colordiff(sonar_col[p], g_target.pix[p]) > g_repair_thresh) {
                  dmg.pb(idx);
               } else if(verify_span && last_tick > 0 &&
                         (u$q)sonar_tick[p] + verify_span < last_tick) {
                  chk.pb(idx);
               }
            }
            {
               static bool rdbg = getenv("IKAPB_RDBG") != nullptr;
               if(rdbg) logp("[rdbg] dmg={} chk={} qc={} done={}\n",
                             (u$d)dmg.size(), (u$d)chk.size(), (u$d)g_qc[G_REPAIR].load(),
                             (u$d)g_done_total.load());
            }
            for(int pass = 0; pass < 2 && made < g_round_cap; pass ++) {
               const vector<u$d> &src = (pass == 0) ? dmg : chk;
               for(size_t k = 0; k < src.size() && made < g_round_cap; k ++) {
                  if(g_qc[G_REPAIR].load() >= 16) break;   // pending 限流
                  u$d idx = src[k];
                  if(!plan_claim(idx)) continue;
                  op_t o; o.kind = OP_PAINT; o.grp = G_REPAIR; o.idx = idx;
                  o.color = g_target.pix[idx]; o.due = 0; o.prio = (u$d)pass;
                  q_push(o);
                  made ++;
               }
            }
            if(scanned) rcur = (rcur + scanned) % N;
         }
      }
      static u$d rlog = 0;
      if(made && ((++rlog % 10) == 0 || made >= 32))
         logp("[repair] queued {} q={}\n", made, q_count.load());
      sleep_ms(g_repair_ms);
   }
}

// 声纳组：用较稀疏的 token 在目标区域按步长重画期望色，换取该 tick 的声纳读数
fn sonar_worker() {
   while(!g_sig && !g_abort && !g_stop.load()) {
      if(!g_ready.load()) { sleep_ms(50); continue; }
      if(g_limit && g_painted.load() >= (u$q)g_limit) break;

      u$d W = g_target.W, H = g_target.H;
      if(!W || !H || !g_target.built) { sleep_ms(g_sonar_ms); continue; }

      u$d stride = g_sonar_stride;
      {
         std::lock_guard<std::mutex> lk(canvas_mu);
         u$d need = 2 * (u$d)glob.rad + 1;
         if(stride < need) stride = need;
         if(stride < 1) stride = 1;
      }

      u$d bw = (g_bb_x1 >= g_bb_x0) ? (g_bb_x1 - g_bb_x0) / stride + 1 : 0;
      u$d bh = (g_bb_y1 >= g_bb_y0) ? (g_bb_y1 - g_bb_y0) / stride + 1 : 0;
      u$d total = bw * bh;
      u$d made = 0;
      if(total) {
         // 每个 sonar 槽位分一块连续的列条带，块内按行蛇形扫：
         // 相邻两点在网格上只差 1 步（stride 像素），避免 FIFO 让槽位跨行跳
         // （~50 px ⇒ c0+k·d 要 5–10 秒，实测只有 ~1.2 笔/秒）。
         vector<u$d> ssl;
         for(size_t i = 0; i < wk.sl.size(); i ++) {
            slot_t &s = wk.sl[i];
            if(s.group == G_SONAR && s.used && !s.want_bind) ssl.pb((u$d)i);
         }
         u$d sk = (u$d)ssl.size();
         if(sk) {
            u$d sbw = (bw + sk - 1) / sk;   // 每块列数
            static vector<u$d> scur;
            static u$d sig_bw = ~0u, sig_bh = ~0u, sig_sk = ~0u;
            if(sig_bw != bw || sig_bh != bh || sig_sk != sk) {
               scur.assign(sk, 0);
               sig_bw = bw; sig_bh = bh; sig_sk = sk;
            }
            for(u$d t = 0; t < sk; t ++) {
               // 每个槽位留 4 笔 pending：不够时槽位会在冷却间隙空等
               if(made >= g_round_cap || g_qc[G_SONAR].load() >= sk * 4) break;
               u$d x0 = t * sbw;
               if(x0 >= bw) continue;
               u$d w = min(sbw, bw - x0);   // 尾块列数
               u$d per = w * bh;
               if(!per) continue;
               u$d m = scur[t] % per;
               u$d row = m / w, col = m % w;
               if(row & 1) col = w - 1 - col;   // 蛇形：奇数行反向
               u$d k = row * bw + x0 + col;
               u$d gx = g_bb_x0 + (k % bw) * stride;
               u$d gy = g_bb_y0 + (k / bw) * stride;
               scur[t] = (m + 1) % per;
               if(gx >= W || gy >= H) continue;
               size_t p = (size_t)gy * W + gx;
               if(!g_target.known[p]) continue;
               u$d idx = (u$d)p;
               if(!plan_claim_s(idx)) continue;
               op_t o; o.kind = OP_PAINT; o.grp = G_SONAR; o.idx = idx;
               o.color = g_target.pix[p]; o.due = 0;
               o.slot = ssl[t];          // 指定槽位：块内自己扫，避免跨块跳跃
               q_push(o);
               if(getenv("IKAPB_SC_LOG")) logp("[sc] slot={} k={} idx={}\n", ssl[t], k, idx);
               made ++;
            }
         }
      }
      static u$d slog = 0;
      if(made && ((++slog % 10) == 0 || made >= 32))
         logp("[sonar] probe {} q={}\n", made, q_count.load());
      sleep_ms(g_sonar_ms);
   }
}

static vector<array<u$b,16> > g_tok[3];   // 0=paint 1=repair 2=sonar
static const char *g_image_path = nullptr;
static const char *g_tokens_path = nullptr;

// 极简 JSON 读取：只认 { "paint": [...], "repair": [...], "sonar": [...] }，
// 值里每个双引号字符串都按 paint token 解析。测试服务允许明文存 token。
static bool tokens_load_file(const char *path) {
   FILE *f = fopen(path, "rb");
   if(!f) ret false;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if(n <= 0) { fclose(f); ret false; }
   std::string s;
   s.resize((size_t)n);
   if(fread(&s[0], 1, (size_t)n, f) != (size_t)n) { fclose(f); ret false; }
   fclose(f);

   bool any = false;
   auto collect = [&](const char *key, grp_id g) {
      std::string pat = std::string("\"") + key + "\"";
      size_t k = s.find(pat);
      if(k == std::string::npos) ret;
      size_t a = s.find('[', k);
      if(a == std::string::npos) ret;
      size_t b = s.find(']', a);
      if(b == std::string::npos) b = s.size();
      size_t p = a;
      while(p < b) {
         size_t q1 = s.find('"', p);
         if(q1 == std::string::npos || q1 >= b) break;
         size_t q2 = s.find('"', q1 + 1);
         if(q2 == std::string::npos || q2 > b) break;
         std::string tk = s.substr(q1 + 1, q2 - q1 - 1);
         array<u$b,16> tok;
         if(parse_token(tk.c_str(), tok)) {
            g_tok[g].pb(tok);
            any = true;
         } else {
            logp("[tokens] 忽略非法 token（{} 组）: {}\n", grp_name(g), tk);
         }
         p = q2 + 1;
      }
   };
   collect("paint", G_PAINT);
   collect("repair", G_REPAIR);
   collect("sonar", G_SONAR);
   if(any) logp("[tokens] loaded {} (paint={} repair={} sonar={})\n", path,
                (u$d)g_tok[G_PAINT].size(), (u$d)g_tok[G_REPAIR].size(),
                (u$d)g_tok[G_SONAR].size());
   ret any;
}

static bool add_token_group(grp_id g, const char *s) {
   array<u$b,16> t;
   if(!parse_token(s, t)) ret false;
   g_tok[g].pb(t);
   ret true;
}

static bool load_image_into_src(const char *path) {
   u$d w = 0, h = 0;
   vector<u$d> px;
   if(ppm_load(path, w, h, px)) {
      g_src.W = w; g_src.H = h; g_src.pix.swap(px); g_src.ok = true;
      ret true;
   }
   if(image_load_stb(path, g_src)) {
      g_src.ok = g_src.W && g_src.H && g_src.pix.size() == (size_t)g_src.W * g_src.H;
      ret g_src.ok;
   }
   ret false;
}

static void usage(const char *p) {
   print("usage: {} [options] [paint-token]\n", p);
   print("  --token / --token-paint <uuid>   绘制组 token（可重复；也可用 IKAPB_TOKEN）\n");
   print("  --token-repair <uuid>            修复组 token（可重复）\n");
   print("  --token-sonar <uuid>             声纳组 token（可重复，建议用稀疏/小号 token）\n");
   print("  --draw x,y,RRGGBB                目标图上一个像素（可重复）\n");
   print("  --ppm <file.ppm>                 用 P6 图片作目标图\n");
   print("  --image <file>                   图片（png/jpg/... 由 stb_image 解码；PPM 也可）\n");
   print("  --tokens <file.json>             从 JSON 读三组 token，默认自动读 ./tokens.json\n");
   print("  --offset x,y                     目标图左上角偏移，默认 950,8\n");
   print("  --limit N                        所有组累计收到 OK 的绘画数达到 N 后退出\n");
   print("                                   （含修复/声纳探测；日志按组统计）\n");
   print("  --dry-run                        只绑定并打印第一帧 TICK，不绘画\n");
   print("  --daemon                         常驻（有修复/声纳组时默认常驻）\n");
   print("  --exec-ms N                      实施线程轮询周期，默认 20\n");
   print("  --paint-ms N / --repair-ms N / --sonar-ms N   各组补货周期\n");
   print("  --paint-loop N                   整图刷完一遍后等 N 秒，再整图刷一遍（0=关闭）\n");
   print("  --repair-thresh N                声纳与目标 RGB 曼哈顿差阈值，默认 96\n");
   print("  --repair-verify-ms N             声纳证据旧于此毫秒数就复检，默认 60000（0=关闭）\n");
   print("  --repair-follow N                repair 组抽 N 个常驻声纳网格点，默认 2（0=关闭）\n");
   print("  --sonar-stride N                 声纳采样步长，0=自动(2R+1)\n");
   print("  --round-cap N                    每个生产者每轮最多入队操作数，默认 256\n");
   print("  --queue-cap N                    全局队列软上限，默认 2048\n");
   print("  --observe                        同时跑 public=1 观察连接\n");
   print("  --sonar-out <file.ppm>           退出时导出声纳表\n");
   print("  --board-out <file.ppm>           退出时导出公共画面（需 --observe）\n");
}

signed main($d argc, char **argv) {
   for($d i = 1; i < argc; i ++) {
      const char *a = argv[i];
      auto need = [&](const char *opt) -> const char * {
         if(i + 1 >= argc) { print("{} needs an argument\n", opt); exit(2); }
         ret argv[++ i];
      };
      if(!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); ret 0; }
      elif(!strcmp(a, "--token") || !strcmp(a, "--token-paint")) {
         if(!add_token_group(G_PAINT, need(a))) { print("bad token\n"); ret 2; }
      }
      elif(!strcmp(a, "--token-repair")) {
         if(!add_token_group(G_REPAIR, need(a))) { print("bad token\n"); ret 2; }
      }
      elif(!strcmp(a, "--token-sonar")) {
         if(!add_token_group(G_SONAR, need(a))) { print("bad token\n"); ret 2; }
      }
      elif(!strcmp(a, "--draw")) {
         u$d x, y, c;
         char col[32];
         if(sscanf(need(a), "%u,%u,%31s", &x, &y, col) != 3 || !parse_color(col, c)) {
            print("bad --draw (want x,y,RRGGBB)\n"); ret 2;
         }
         g_raw.pb({x, y, c});
      }
      elif(!strcmp(a, "--ppm") || !strcmp(a, "--image")) g_image_path = need(a);
      elif(!strcmp(a, "--offset")) {
         if(sscanf(need(a), "%u,%u", &g_ox, &g_oy) != 2) { print("bad --offset\n"); ret 2; }
      }
      elif(!strcmp(a, "--limit")) g_limit = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--dry-run")) g_dry = true;
      elif(!strcmp(a, "--daemon")) g_daemon = true;
      elif(!strcmp(a, "--tokens")) g_tokens_path = need(a);
      elif(!strcmp(a, "--observe")) g_observe = true;
      elif(!strcmp(a, "--exec-ms")) g_exec_ms = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--early-us")) g_early_us = (u$q)strtoull(need(a), nullptr, 10);
      elif(!strcmp(a, "--paint-ms")) g_paint_ms = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--paint-loop")) {
         g_paint_loop_ms = (u$d)strtoul(need(a), nullptr, 10) * 1000u;   // 0=关闭
      }
      elif(!strcmp(a, "--repair-ms")) g_repair_ms = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--sonar-ms")) g_sonar_ms = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--repair-thresh")) g_repair_thresh = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--repair-verify-ms")) g_repair_verify_ms = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--repair-follow")) g_repair_follow = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--sonar-stride")) g_sonar_stride = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--round-cap")) g_round_cap = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--queue-cap")) g_queue_cap = (u$d)strtoul(need(a), nullptr, 10);
      elif(!strcmp(a, "--sonar-out")) g_sonar_out = need(a);
      elif(!strcmp(a, "--board-out")) g_board_out = need(a);
      elif(a[0] != '-') {
         if(!add_token_group(G_PAINT, a)) { print("bad token: {}\n", a); ret 2; }
      } else {
         print("unknown option {}\n", a);
         usage(argv[0]);
         ret 2;
      }
   }

   // token 文件：默认读取当前目录的 tokens.json（测试服务允许明文）
   const char *tkpath = g_tokens_path;
   if(!tkpath && access("tokens.json", R_OK) == 0) tkpath = "tokens.json";
   if(tkpath) {
      if(!tokens_load_file(tkpath))
         logp("[tokens] 未能从 {} 读到任何 token\n", tkpath);
   }

   const char *envtok = getenv("IKAPB_TOKEN");
   if(envtok && *envtok && g_tok[G_PAINT].empty() && g_tok[G_REPAIR].empty() && g_tok[G_SONAR].empty()) {
      if(!add_token_group(G_PAINT, envtok)) { print("bad IKAPB_TOKEN\n"); ret 2; }
   }

   if(g_image_path && !load_image_into_src(g_image_path)) {
      print("cannot load image: {}\n", g_image_path);
      ret 2;
   }
   if(g_src.ok && (u$q)g_src.W * g_src.H > 4000000ull) {
      print("image too large ({}x{})\n", g_src.W, g_src.H);
      ret 2;
   }

   size_t ntok = g_tok[G_PAINT].size() + g_tok[G_REPAIR].size() + g_tok[G_SONAR].size();
   if(!ntok && !g_observe) g_observe = true;   // 兼容旧的纯公共流用法

   // 没有目标图 -> 只绑定（当成 dry-run），避免空转
   if(ntok && !g_src.ok && g_raw.empty() && !g_dry && !g_daemon) {
      logp("[main] 没有目标图（--ppm/--image/--draw），仅绑定后退出\n");
      g_dry = true;
   }

   // 建槽位：按 paint -> repair -> sonar 顺序，每组的 token 各占一个槽位
   wk.sl.clear();
   for(int g = 0; g < 3; g ++) {
      g_gslots[g] = (u$d)g_tok[g].size();
      for(auto &t : g_tok[g]) {
         slot_t s;
         s.tok = t;
         s.group = (grp_id)g;
         wk.sl.pb(s);
      }
   }
   // repair 组抽前 N 个槽位常驻声纳网格点：只修网格点上的坏色，空闲也不去帮画
   {
      u$d left = g_repair_follow, got = 0;
      for(auto &s : wk.sl)
         if(s.group == G_REPAIR && left > 0) { s.follow = true; left --; got ++; }
      if(got) logp("[work] repair follow slots={} (常驻声纳网格点)\n", got);
   }
   if(!ntok) for(int g = 0; g < 3; g ++) g_gslots[g] = 0;

   struct sigaction sa{};
   sa.sa_handler = on_signal;
   sa.sa_flags = 0;              // 不要 SA_RESTART，让阻塞系统调用及时返回
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, nullptr);
   sigaction(SIGTERM, &sa, nullptr);

   bool has_work = ntok > 0;
   std::thread work, pub, tp, tr, ts;
   if(has_work) {
      work = std::thread(getWork);
      if(has_group(G_PAINT)) tp = std::thread(paint_worker);
      if(has_group(G_REPAIR)) tr = std::thread(repair_worker);
      if(has_group(G_SONAR)) ts = std::thread(sonar_worker);
   }
   if(g_observe) pub = std::thread(getPublic);

   if(has_work) {
      work.join();
      g_stop = true;              // 通知生产者退出
      if(tp.joinable()) tp.join();
      if(tr.joinable()) tr.join();
      if(ts.joinable()) ts.join();
      if(g_observe) { g_sig = 1; pub.join(); }
   } elif(g_observe) {
      pub.join();
   }

   if(g_sonar_out) write_sonar(g_sonar_out);
   if(g_board_out) write_board(g_board_out);

   if(has_work) {
      size_t enq_stuck = 0;
      {
         std::lock_guard<std::mutex> lk(plan_mu);
         for(size_t i = 0; i < g_enq.size(); i ++) if(g_enq[i]) enq_stuck ++;
      }
      u$q qmin = 0;
      {
         std::lock_guard<std::mutex> lk(q_mu);
         if(!q_ops.empty()) qmin = q_ops.begin()->due;
      }
      u$q qn = srv_now();
      logp("[main] queue={} enq_stuck={} qmin_due={}\n", q_count.load(), (u$d)enq_stuck,
           (qmin && qn && qmin > qn) ? (qmin - qn) / 1000 : (u$d)0);
      logp("[main] painted={} passes={} target_done={}/{} ok[paint={} repair={} sonar={}] {}\n",
           g_painted.load(), g_passes.load(), g_done_total.load(), g_need_total.load(),
           g_ok[G_PAINT].load(), g_ok[G_REPAIR].load(), g_ok[G_SONAR].load(),
           g_abort ? "aborted" : (g_dry ? "dry-run" : "done"));
   }
   ret g_abort ? 1 : 0;
}
