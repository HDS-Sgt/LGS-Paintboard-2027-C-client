#include "zstdwrap.hpp"
#include "websock.hpp"

static void put16(stream &v, $d x) { v.push_back(u$b(x & 255)); v.push_back(u$b(x >> 8 & 255)); }
static void put32(stream &v, unsigned x) { for ($d i = 0; i < 4; ++i) v.push_back(u$b(x >> (8 * i) & 255)); }
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $q rd32(const stream &v, size_t o) { ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) {
   unsigned long long r = 0;
   for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i);
   ret r;
}

// 一条 BATCH：只绑定一个槽位；token 16 个 0 = 解绑，全 0xAA = 无效 token。
static stream mkbatch($d slot, u$b fill) {
   stream v;
   v.push_back(0x01); v.push_back(1); put16(v, 0); put32(v, 1); put32(v, 0); put32(v, 0);
   put16(v, slot); put16(v, 0);
   for ($d i = 0; i < 16; ++i) v.push_back(fill);
   ret v;
}

static $d pad4($d n) { ret (n + 3) & ~3; }

static void dump_tick(const stream &p) {
   if (p.size() < 32 || p[0] != 0x81) { printf("not TICK: size=%zu first=%02x\n", p.size(), p[0]); ret; }
   $d R = rd16(p, 2), nr = $d(rd32(p, 4)), nb = $d(rd32(p, 16)), nk = $d(rd32(p, 20)), ns = $d(rd32(p, 24));
   $d A = 2 * R * R + 2 * R + 1;
   size_t need = 32 + 12 * nb + pad4(2 * nb) + pad4(nr) + 4 * nr + 4 * nk + 4 * nk * A + pad4(2 * ns);
   printf("TICK R=%d tick=%llu nr=%d nb=%d nk=%d ns=%d size=%zu expected=%zu %s\n",
          R, rd64(p, 8), nr, nb, nk, ns, p.size(), need, need == p.size() ? "OK" : "MISMATCH");
   size_t off = 32 + 8 * nb + 4 * nb;
   for ($d k = 0; k < nr; ++k) printf("  result[%d] status=%u value=%u\n", k, (unsigned)p[off + k], (unsigned)rd32(p, off + pad4(nr) + 4 * k));
   for ($d k = 0; k < nb; ++k)
      printf("  brush[%d] slot=%u idx=%u t=%llu\n", k, (unsigned)rd16(p, 32 + 12 * nb + pad4(2 * nb) * 0 + 0), 0, 0);
}

// 解析一遍并把列位置打印出来（用于核对偏移，不做完整状态机）
static void dump_tick2(const stream &p) {
   if (p.size() < 32 || p[0] != 0x81) { printf("not TICK\n"); ret; }
   $d R = rd16(p, 2), nr = $d(rd32(p, 4)), nb = $d(rd32(p, 16)), nk = $d(rd32(p, 20)), ns = $d(rd32(p, 24));
   size_t bt = 32, bi = bt + 8 * nb, bs = bi + 4 * nb, st = bs + pad4(2 * nb);
   size_t va = st + pad4(nr), si = va + 4 * nr, sc = si + 4 * nk, ls = sc + 4 * nk * (2 * R * R + 2 * R + 1);
   printf("TICK tick=%llu R=%d nr=%d nb=%d nk=%d ns=%d len=%zu\n", rd64(p, 8), R, nr, nb, nk, ns, p.size());
   for ($d k = 0; k < nr; ++k) printf("  result[%d] status=%u value=%u\n", k, (unsigned)p[st + k], (unsigned)rd32(p, va + 4 * k));
   for ($d k = 0; k < nb; ++k) printf("  brush[%d] slot=%u t=%llu idx=%u\n", k, (unsigned)rd16(p, bs + 2 * k), rd64(p, bt + 8 * k), (unsigned)rd32(p, bi + 4 * k));
   printf("  cols: brush_t=%zu brush_idx=%zu brush_slot=%zu status=%zu value=%zu sonar_idx=%zu sonar_color=%zu lost=%zu\n",
          bt, bi, bs, st, va, si, sc, ls);
}

___main {
   tcSocket s;
   s.connect("pb.ika.zip", "443", "/ws?public=0");
   stream raw, wire;
   auto m = s.recvbin(); zstddec(*m, raw);
   printf("first message type=0x%02x len=%zu\n", raw[0], raw.size());

   zstdenc(mkbatch(5, 0x00), wire);          // 解绑空槽位 → 期望 OK
   s.sendbin(wire);
   m = s.recvbin(); zstddec(*m, raw); dump_tick2(raw);

   zstdenc(mkbatch(6, 0xAA), wire);          // 无效 token → 期望 BAD_PAINT_TOKEN
   s.sendbin(wire);
   m = s.recvbin(); zstddec(*m, raw); dump_tick2(raw);
   ret 0;
}
