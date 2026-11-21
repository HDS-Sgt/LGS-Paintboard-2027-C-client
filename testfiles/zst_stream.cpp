#include "zstdwrap.hpp"
using stream = vector<u$b>;
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
signed main($d argc, char **argv) {
   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror("fopen"); ret 1; }
   stream in; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) in.insert(in.end(), tmp, tmp + got);
   fclose(f);
   printf("file=%zu bytes\n", in.size());
   ZSTD_DStream *ds = ZSTD_createDStream();
   ZSTD_initDStream(ds);
   ZSTD_inBuffer ib{in.data(), in.size(), 0};
   vector<u$b> obuf(1u << 20);
   stream out;
   while (ib.pos < ib.size) {
      ZSTD_outBuffer ob{obuf.data(), obuf.size(), 0};
      size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
      if (ZSTD_isError(rc)) { printf("stream error: %s\n", ZSTD_getErrorName(rc)); ret 2; }
      out.insert(out.end(), obuf.begin(), obuf.begin() + ob.pos);
      if (rc == 0) { printf("frame complete\n"); break; }
   }
   ZSTD_freeDStream(ds);
   printf("decoded=%zu bytes\n", out.size());
   if (out.size() < 16) ret 3;
   printf("head: %02x %02x %02x %02x | W=%d H=%d ver=%llu\n", out[0], out[1], out[2], out[3], rd16(out, 4), rd16(out, 6), rd64(out, 8));
   $d W = rd16(out, 4), H = rd16(out, 6);
   size_t need = out[2] == 1 ? 16 + 12ull * W * H : 16 + 4ull * W * H;
   printf("codec=%u need=%zu have=%zu (%s)\n", (unsigned)out[2], need, out.size(), out.size() == need ? "complete" : "partial");
   // 颜色抽样：颜色应为 A=255 或 0/1/2 特殊值
   $d hist[4] = {0, 0, 0, 0}, n = $d(min<size_t>(out.size() / 4, 200000));
   for ($d i = 0; i < n; ++i) {
      $q c = $q(out[16 + 4 * i]) | ($q(out[17 + 4 * i]) << 8) | ($q(out[18 + 4 * i]) << 16) | ($q(out[19 + 4 * i]) << 24);
      $d a = (c >> 24) & 255;
      hist[a == 255 ? 0 : (c <= 2 ? 1 + $d(c) : 2)]++;
   }
   printf("color sample n=%d A=255:%d c<=2:%d other:%d\n", n, hist[0], hist[1], hist[2]);
   ret 0;
}
