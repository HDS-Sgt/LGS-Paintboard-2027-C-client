// 离线自检：BATCH 编码、TICK 解析、声纳菱形顺序、颜色字节序。
// 不联网；直接 include ../main.cpp，走真实实现。
//
// 编译（仓库根目录）：
//   g++ -std=c++20 -O2 -Wall -Wextra -I. -I.deps/zstd-dev/usr/include \
//       testfiles/tick_selftest.cpp -o testfiles/tick_selftest \
//       -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1
#define main prog_unused
#include "../main.cpp"
#undef main

#include <cassert>

static void p16(stream &v, u$d x) { v.pb(u$b(x & 255)); v.pb(u$b((x >> 8) & 255)); }
static void p32(stream &v, u$d x) { for($d i = 0; i < 4; i++) v.pb(u$b((x >> (8*i)) & 255)); }
static void p64(stream &v, u$q x) { for($d i = 0; i < 8; i++) v.pb(u$b((x >> (8*i)) & 255)); }

int main() {
   // ---- 1. enc_batch：按协议布局逐字节核对 ----
   bt b; b.slot = 3;
   for($d j = 0; j < 16; j++) b.tok[j] = u$b((1 * 7 + j) % 256);
   stream got = enc_batch({b}, {{7, 123456u, 0xFF0000FFu}});

   stream exp;
   exp.pb(1); exp.pb(1); p16(exp, 0); p32(exp, 1); p32(exp, 1); p32(exp, 0);
   p16(exp, 3); p16(exp, 0);
   for($d j = 0; j < 16; j++) exp.pb(b.tok[j]);
   p16(exp, 7); p16(exp, 0);
   p32(exp, 123456); p32(exp, 0xFF0000FFu);
   assert(got == exp);
   printf("enc_batch OK (%zu bytes)\n", got.size());

   // ---- 2. procTick：合成一条 TICK 并检查每个列 ----
   $d R = 2, nr = 2, nb = 1, nk = 1, ns = 1;
   u$q A = 2ull*R*R + 2ull*R + 1;
   stream p;
   p.pb(0x81); p.pb(1); p16(p, R); p32(p, nr); p64(p, 987654321ull);
   p32(p, nb); p32(p, nk); p32(p, ns); p32(p, 0);
   p64(p, 111222333ull); p32(p, 4242); p16(p, 3); p16(p, 0);   // brush.t / idx / slot + pad
   p.pb(0); p.pb(1); p16(p, 0);                                // status + pad
   p32(p, 0); p32(p, 500000);                                  // value
   p32(p, 4242);                                               // sonar.idx
   for(u$d q = 0; q < A; q++) p32(p, 0x11223300u + q);         // sonar.color
   p16(p, 3); p16(p, 0);                                       // lost.slot + pad

   tick_info T;
   assert(procTick(p, T) == 0);
   assert(T.R == R && T.nr == 2 && T.nb == 1 && T.nk == 1 && T.ns == 1);
   assert(T.tick == 987654321ull);
   assert(T.status[0] == 0 && T.status[1] == 1);
   assert(T.value[0] == 0 && T.value[1] == 500000);
   assert(T.brt[0] == 111222333ull && T.bridx[0] == 4242 && T.brslot[0] == 3);
   assert(T.sonidx[0] == 4242 && T.soncol[12] == 0x11223300u + 12);
   assert(T.lost[0] == 3);

   stream bad = p; bad.pp();      // 截断必须报错而不是越界读
   tick_info T2;
   assert(procTick(bad, T2) != 0);
   printf("procTick OK, malformed rejected\n");

   // ---- 3. 声纳菱形顺序（R=2 共 13 格） ----
   $d seq[13][2] = {{0,-2},{-1,-1},{0,-1},{1,-1},{-2,0},{-1,0},{0,0},{1,0},{2,0},{-1,1},{0,1},{1,1},{0,2}};
   $d q = 0;
   for($d dy = -R; dy <= R; dy++)
      for($d dx = -(R - abs(dy)); dx <= R - abs(dy); dx++, q++)
         assert(seq[q][0] == dx && seq[q][1] == dy);
   printf("sonar order OK (%d cells)\n", q);

   // ---- 4. 颜色文本 -> u32，内存字节序必须是 R,G,B,A ----
   u$d c = 0;
   assert(parse_color("A2A8BE", c) && c == 0xFFBEA8A2u);
   assert(parse_color("FF0000", c) && c == 0xFF0000FFu);   // R 在最低字节
   assert(parse_color("00FF00", c) && c == 0xFF00FF00u);
   assert(parse_color("0000FF", c) && c == 0xFFFF0000u);
   assert(parse_color("A2A8BEFF", c) && c == 0xFFBEA8A2u);
   assert(!parse_color("A2A8BE00", c));                    // alpha 必须 255
   printf("parse_color OK\n");

   // ---- 5. ppm_load：RGB 字节 -> u32 的 R,G,B,A 布局 ----
   const char *pp = "/tmp/ikapb_selftest.ppm";
   FILE *f = fopen(pp, "wb");
   assert(f);
   fprintf(f, "P6\n2 1\n255\n");
   unsigned char raw[6] = {0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC};
   assert(fwrite(raw, 1, 6, f) == 6);
   fclose(f);

   u$d pw = 0, ph = 0;
   vector<u$d> pix;
   assert(ppm_load(pp, pw, ph, pix));
   assert(pw == 2 && ph == 1 && pix.size() == 2);
   assert(pix[0] == 0xFF332211u && pix[1] == 0xFFCCBBAAu);
   remove(pp);
   printf("ppm_load OK\n");
   return 0;
}
