#include <chrono>
#include "zstdwrap.hpp"
#include "websock.hpp"

static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $q rd32(const stream &v, size_t o) { ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }

___main {
   setvbuf(stdout, nullptr, _IONBF, 0);
   auto t0 = std::chrono::steady_clock::now();
   auto el = [&] { ret std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

   tcSocket s;
   fprintf(stderr, "[%.1fs] connecting\n", el());
   s.connect("pb.ika.zip", "443", "/ws?public=1");
   fprintf(stderr, "[%.1fs] connected\n", el());

   stream raw;
   auto m = s.recvbin(); zstddec(*m, raw);
   $d W = rd16(raw, 4), H = rd16(raw, 6);
   printf("[%.1fs] RULES type=%02x %dx%d\n", el(), raw[0], W, H);

   fprintf(stderr, "[%.1fs] waiting SNAPSHOT\n", el());
   m = s.recvbin();
   printf("[%.1fs] snapshot wire=%zu\n", el(), m->size());
   zstddec(*m, raw, 64u << 20);
   $d codec = raw[2];
   size_t expect = codec == 1 ? 16 + 12ull * W * H : 16 + 4ull * W * H;
   printf("[%.1fs] SNAPSHOT type=%02x codec=%u W=%u H=%u ver=%llu len=%zu expected=%zu %s\n",
          el(), raw[0], (unsigned)raw[2], rd16(raw, 4), rd16(raw, 6), rd64(raw, 8), raw.size(), expect,
          raw.size() == expect ? "OK" : "MISMATCH");

   for ($d k = 0; k < 2; ++k) {
      fprintf(stderr, "[%.1fs] waiting PUBLIC %d\n", el(), k);
      m = s.recvbin();
      stream p; zstddec(*m, p, 64u << 20);
      $d n = $d(rd32(p, 4)), segs = $d(rd32(p, 24)), e = $d(rd32(p, 28));
      size_t need = 36 + 8ull * segs + pad4(n) + 4ull * e + 4ull * n;
      printf("[%.1fs] PUBLIC type=%02x p=%u n=%d from=%llu to=%llu m=%d e=%d order=%u len=%zu expected=%zu %s\n",
             el(), p[0], rd16(p, 2), n, rd64(p, 8), rd64(p, 16), segs, e, (unsigned)rd32(p, 32),
             p.size(), need, p.size() == need ? "OK" : "MISMATCH");
   }
   ret 0;
}
