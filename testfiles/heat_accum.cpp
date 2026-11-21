// 把一批 /api/board?since= 的 .zst 响应体离线累积成逐像素活动热力图。
// 用法: heat_accum <W> <H> <out.ppm> <file.zst>...
#include "zstdwrap.hpp"
using stream = vector<u$b>;

static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $d rd32(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8) | ($d(v[o + 2]) << 16) | ($d(v[o + 3]) << 24); }
static $d pad4($d n) { ret (n + 3) & ~3; }

signed main($d argc, char **argv) {
   if (argc < 4) { fprintf(stderr, "usage: %s <W> <H> <out.ppm> <file.zst>...\n", argv[0]); ret 1; }
   $d W = atoi(argv[1]), H = atoi(argv[2]);
   vector<unsigned> cnt((size_t)W * H, 0);
   unsigned long long total = 0;
   $d files = 0, pubs = 0, snaps = 0;
   vector<$d> d;
   for ($d a = 4; a < argc; ++a) {
      FILE *f = fopen(argv[a], "rb");
      if (!f) continue;
      stream in; u$b tmp[65536]; size_t got;
      while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) in.insert(in.end(), tmp, tmp + got);
      fclose(f);
      stream raw;
      try { zstddec(in, raw, 16u << 20); } catch (const std::exception &) { continue; }
      ++files;
      size_t pos = 0;
      while (pos < raw.size()) {
         $d typ = raw[pos];
         if (typ == 0x82) {
            if (raw.size() - pos < 36) break;
            $d n = rd32(raw, pos + 4);
            $d m = rd32(raw, pos + 24), e = rd32(raw, pos + 28), order = rd32(raw, pos + 32);
            size_t need = 36 + 8ull * m + pad4(n) + 4ull * e + 4ull * n;
            if (pos + need > raw.size()) break;
            size_t o = pos + 36;
            vector<$d> segn(m); for ($d j = 0; j < m; ++j) segn[j] = rd32(raw, o + 4 * m + 4 * j);
            const u$b *small = &raw[o + 8 * m];
            size_t od = o + 8 * m + pad4(n);
            d.assign(n, 0);
            $d big = 0;
            for ($d i = 0; i < n; ++i) {
               $d v = small[i];
               if (v == 255) { if (big >= e) { big = -1; break; } v = rd32(raw, od + 4 * big++); }
               d[i] = v;
            }
            if (big < 0) { pos += need; continue; }
            size_t ent = 0; $d k = 0;
            for ($d j = 0; j < m; ++j) {
               long long key = 0;
               for ($d q = 0; q < segn[j]; ++q, ++ent) {
                  key += d[ent];
                  $d idx = order == 0 ? $d(key) : $d((key % H) * W + key / H);
                  if (idx >= 0 && idx < W * H) ++cnt[(size_t)idx];
                  ++total;
               }
            }
            pos += need; ++pubs;
         } else if (typ == 0x83) {
            if (raw.size() - pos < 16) break;
            $d codec = raw[pos + 2], w = rd16(raw, pos + 4), h = rd16(raw, pos + 6);
            pos += 16 + size_t(codec ? 12 : 4) * w * h; ++snaps;
         } else break;
      }
   }
   unsigned mx = 0, touched = 0;
   for (unsigned c : cnt) { if (c) ++touched; if (c > mx) mx = c; }
   printf("files=%d publics=%d snapshots=%d entries=%llu touched=%u max=%u\n", files, pubs, snaps, total, touched, mx);
   double den = log1p((double)mx), inv = den > 0 ? 1.0 / den : 0.0;
   FILE *out = fopen(argv[3], "wb");
   if (!out) { perror("fopen out"); ret 1; }
   fprintf(out, "P6\n%d %d\n255\n", W, H);
   for (size_t i = 0; i < cnt.size(); ++i) {
      double t = cnt[i] ? log1p((double)cnt[i]) * inv : 0.0;
      u$b rgb[3];
      rgb[0] = (u$b)(255.0 * pow(t, 0.45));
      rgb[1] = (u$b)(255.0 * pow(t, 1.6));
      rgb[2] = (u$b)(255.0 * pow(t, 4.0));
      fwrite(rgb, 1, 3, out);
   }
   fclose(out);
   ret 0;
}
