#include "zstdwrap.hpp"
using stream = vector<u$b>;
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
using stream = vector<u$b>;
static $q rd32(const stream &v, size_t o) { ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }
signed main($d argc, char **argv) {
   if (argc < 2) { printf("usage: %s file.zst\n", argv[0]); ret 1; }
   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror("fopen"); ret 1; }
   stream in; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) in.insert(in.end(), tmp, tmp + got);
   fclose(f);
   printf("compressed=%zu\n", in.size());
   stream out;
   try { zstddec(in, out, 64u << 20); }
   catch (const std::exception &e) { printf("zstddec failed: %s\n", e.what()); ret 2; }
   printf("decompressed=%zu type=%02x version=%u\n", out.size(), out.empty() ? 0 : out[0], out.size() > 1 ? out[1] : 0);
   if (out[0] == 0x82) {
      $d n = $d(rd32(out, 4)), m = $d(rd32(out, 24)), e = $d(rd32(out, 28));
      size_t need = 36 + 8ull * m + pad4(n) + 4ull * e + 4ull * n;
      printf("PUBLIC p=%u n=%d from=%llu to=%llu m=%d e=%d order=%u\n", rd16(out, 2), n, rd64(out, 8), rd64(out, 16), m, e, (unsigned)rd32(out, 32));
      printf("expected=%zu %s\n", need, need == out.size() ? "OK" : "MISMATCH");
   } else if (out[0] == 0x83) {
      $d W = rd16(out, 4), H = rd16(out, 6);
      size_t need = out[2] == 1 ? 16 + 12ull * W * H : 16 + 4ull * W * H;
      printf("SNAPSHOT codec=%u W=%d H=%d ver=%llu\n", (unsigned)out[2], W, H, rd64(out, 8));
      printf("expected=%zu %s\n", need, need == out.size() ? "OK" : "MISMATCH");
   } else if (out[0] == 0x84) {
      printf("RULES len=%zu %s (expect 96)\n", out.size(), out.size() == 96 ? "OK" : "MISMATCH");
   }
   ret 0;
}
