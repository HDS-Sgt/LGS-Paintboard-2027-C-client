#include "zstdwrap.hpp"
#include "websock.hpp"

static void put16(stream &v, $d x) { v.push_back(u$b(x & 255)); v.push_back(u$b(x >> 8 & 255)); }
static void put32(stream &v, unsigned x) { for ($d i = 0; i < 4; ++i) v.push_back(u$b(x >> (8 * i) & 255)); }
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $q rd32(const stream &v, size_t o) { ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }

static stream bind_batch($d slot, u$b fill) {
   stream v;
   v.push_back(0x01); v.push_back(1); put16(v, 0); put32(v, 1); put32(v, 0); put32(v, 0);
   put16(v, slot); put16(v, 0);
   for ($d i = 0; i < 16; ++i) v.push_back(fill);
   ret v;
}

static void dump_tick(const stream &p) {
   $d R = rd16(p, 2), nr = $d(rd32(p, 4)), nb = $d(rd32(p, 16)), nk = $d(rd32(p, 20)), ns = $d(rd32(p, 24));
   size_t bt = 32, bi = bt + 8 * nb, bs = bi + 4 * nb, st = bs + pad4(2 * nb);
   size_t va = st + pad4(nr), si = va + 4 * nr, sc = si + 4 * nk;
   size_t need = sc + 4 * nk * (2 * R * R + 2 * R + 1) + pad4(2 * ns);
   printf("TICK tick=%llu R=%d nr=%d nb=%d nk=%d ns=%d len=%zu expected=%zu %s\n",
          rd64(p, 8), R, nr, nb, nk, ns, p.size(), need, p.size() == need ? "OK" : "MISMATCH");
   for ($d k = 0; k < nr; ++k)
      printf("  result[%d] status=%u value=%u\n", k, (unsigned)p[st + k], (unsigned)rd32(p, va + 4 * k));
   for ($d k = 0; k < nb; ++k)
      printf("  brush[%d] slot=%u t=%llu idx=%u\n", k, (unsigned)rd16(p, bs + 2 * k), rd64(p, bt + 8 * k), (unsigned)rd32(p, bi + 4 * k));
}

static $d run_one(tcSocket &s, const char *tag, $d slot, u$b fill) {
   stream w; zstdenc(bind_batch(slot, fill), w);
   s.sendbin(w);
   printf("%s: sent %zu-byte frame\n", tag, w.size());
   auto f = s.recvframe();
   printf("%s: frame fin=%d op=%u dat=%zu\n", tag, (int)f.fin, (unsigned)f.op, f.dat.size());
   if (f.op == 0x8) {
      if (f.dat.size() >= 2) printf("  CLOSE code=%u reason=%s\n", (unsigned)rd16(f.dat, 0), _Str(f.dat.begin() + 2, f.dat.end()).c_str());
      ret 1;
   }
   if (f.op == 0x2) { stream p; zstddec(f.dat, p); dump_tick(p); }
   ret 0;
}

___main {
   tcSocket s;
   s.connect("pb.ika.zip", "443", "/ws?public=0");
   auto m = s.recvbin(); stream raw; zstddec(*m, raw);
   printf("RULES len=%zu type=%02x\n", raw.size(), raw[0]);
   if (run_one(s, "unbind-slot5", 5, 0x00)) ret 0;
   if (run_one(s, "bad-token", 6, 0xAA)) ret 0;
   ret 0;
}
