#include "zstdwrap.hpp"
using stream = vector<u$b>;

static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $d rd32(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8) | ($d(v[o + 2]) << 16) | ($d(v[o + 3]) << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }

// 解一条完整的 /api/board?since= 增量链：载荷里是若干条 PUBLIC 首尾相接。
// 输出（可选）CSV：tick,idx,color，每条变更一行，按消息内顺序。
signed main($d argc, char **argv) {
   if (argc < 2) { fprintf(stderr, "usage: %s <board.zst> [changes.csv] [W] [H]\n", argv[0]); ret 1; }
   $d W = argc > 3 ? atoi(argv[3]) : 1000;
   $d H = argc > 4 ? atoi(argv[4]) : 600;
   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror("fopen"); ret 1; }
   stream in; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) in.insert(in.end(), tmp, tmp + got);
   fclose(f);
   stream raw;
   try { zstddec(in, raw, 16u << 20); } catch (const std::exception &ex) { printf("decompress: %s\n", ex.what()); ret 1; }
   printf("wire=%zu raw=%zu\n", in.size(), raw.size());
   FILE *csv = argc > 2 ? fopen(argv[2], "wb") : nullptr;
   if (csv) fprintf(csv, "tick,idx,w\n");
   size_t pos = 0; $d rec = 0; unsigned long long total = 0;
   vector<$d> d;
   while (pos < raw.size()) {
      $d typ = raw[pos];
      if (typ == 0x82) {
         if (raw.size() - pos < 36) { printf("truncated PUBLIC header at %zu\n", pos); break; }
         $d p = rd16(raw, pos + 2);
         $d n = rd32(raw, pos + 4);
         unsigned long long from = rd64(raw, pos + 8), to = rd64(raw, pos + 16);
         $d m = rd32(raw, pos + 24), e = rd32(raw, pos + 28), order = rd32(raw, pos + 32);
         size_t need = 36 + 8ull * m + pad4(n) + 4ull * e + 4ull * n;
         if (pos + need > raw.size()) { printf("#%d TRUNCATED n=%d need=%zu have=%zu\n", rec, n, need, raw.size() - pos); break; }
         size_t o = pos + 36;
         vector<$d> segn(m); $d sum = 0;
         for ($d j = 0; j < m; ++j) { segn[j] = rd32(raw, o + 4 * m + 4 * j); sum += segn[j]; }
         const u$b *small = &raw[o + 8 * m];
         size_t od = o + 8 * m + pad4(n), oc = od + 4 * e;
         d.assign(n, 0);
         $d big = 0;
         for ($d i = 0; i < n; ++i) {
            $d v = small[i];
            if (v == 255) { if (big >= e) { printf("dsmall/dbig mismatch\n"); ret 1; } v = rd32(raw, od + 4 * big++); }
            d[i] = v;
         }
         printf("#%d p=%u n=%d e=%d m=%d order=%u from=%llu to=%llu segsum=%d need=%zu\n",
                rec, p, n, e, m, order, from, to, sum, need);
         size_t ent = 0; $d shown = 0;
         for ($d j = 0; j < m; ++j) {
            unsigned long long tick = from + rd32(raw, o + 4 * j);
            long long k = 0;
            for ($d q = 0; q < segn[j]; ++q, ++ent) {
               k += d[ent];
               unsigned long long key = (unsigned long long)k;
               $d idx = order == 0 ? $d(key) : $d((key % H) * W + key / H);
               $d col = rd32(raw, oc + 4 * ent);
               ++total;
               if (csv) fprintf(csv, "%llu,%d,%d\n", tick, idx, col);
               else if (shown < 8) { printf("   tick=%llu idx=%d key=%llu w=%u\n", tick, idx, key, col); ++shown; }
            }
         }
         if (ent != size_t(n)) printf("   WARN segsum entries=%zu != n=%d\n", ent, n);
         pos += need; ++rec;
      } else if (typ == 0x83) {
         if (raw.size() - pos < 16) break;
         $d codec = raw[pos + 2], w = rd16(raw, pos + 4), h = rd16(raw, pos + 6);
         size_t need = 16 + size_t(codec ? 12 : 4) * w * h;
         if (pos + need > raw.size()) { printf("SNAPSHOT truncated need=%zu have=%zu\n", need, raw.size() - pos); break; }
         printf("SNAPSHOT w=%d h=%d codec=%d ver=%llu\n", w, h, codec, rd64(raw, pos + 8));
         pos += need; ++rec;
      } else { printf("unknown type 0x%02x at %zu\n", typ, pos); break; }
   }
   if (csv) fclose(csv);
   printf("records=%d entries=%llu consumed=%zu/%zu\n", rec, total, pos, raw.size());
   ret 0;
}
