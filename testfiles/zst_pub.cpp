#include "zstdwrap.hpp"
using stream = vector<u$b>;
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $q rd32(const stream &v, size_t o) { ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }
signed main($d argc, char **argv) {
   FILE *f = fopen(argv[1], "rb"); if (!f) { perror("fopen"); ret 1; }
   stream in; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) in.insert(in.end(), tmp, tmp + got);
   fclose(f);
   ZSTD_DStream *ds = ZSTD_createDStream(); ZSTD_initDStream(ds);
   ZSTD_inBuffer ib{in.data(), in.size(), 0};
   vector<u$b> obuf(1u << 20); stream out;
   while (ib.pos < ib.size) {
      ZSTD_outBuffer ob{obuf.data(), obuf.size(), 0};
      size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
      if (ZSTD_isError(rc)) { printf("stream error: %s\n", ZSTD_getErrorName(rc)); break; }
      out.insert(out.end(), obuf.begin(), obuf.begin() + ob.pos);
      if (rc == 0) break;
   }
   ZSTD_freeDStream(ds);
   printf("compressed=%zu decoded=%zu\n", in.size(), out.size());
   size_t pos = 0; $d cnt = 0;
   while (pos + 36 <= out.size() && out[pos] == 0x82) {
      $d n = $d(rd32(out, pos + 4)), m = $d(rd32(out, pos + 24)), e = $d(rd32(out, pos + 28));
      size_t need = 36 + 8ull * m + pad4(n) + 4ull * e + 4ull * n;
      printf("#%d p=%u n=%d from=%llu to=%llu m=%d e=%d order=%u need=%zu %s\n", cnt, rd16(out, pos + 2), n,
             rd64(out, pos + 8), rd64(out, pos + 16), m, e, (unsigned)rd32(out, pos + 32), need,
             pos + need <= out.size() ? "OK" : "TRUNCATED");
      if (pos + need > out.size()) break;
      pos += need; ++cnt;
   }
   printf("records=%d consumed=%zu/%zu\n", cnt, pos, out.size());
   ret 0;
}
