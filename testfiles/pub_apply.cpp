#include "zstdwrap.hpp"
using stream = vector<u$b>;

static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static unsigned rd32u(const stream &v, size_t o) { ret (unsigned)v[o] | ((unsigned)v[o + 1] << 8) | ((unsigned)v[o + 2] << 16) | ((unsigned)v[o + 3] << 24); }
static unsigned long long rd64(const stream &v, size_t o) { unsigned long long r = 0; for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i); ret r; }
static $d pad4($d n) { ret (n + 3) & ~3; }

static stream slurp(const char *path) {
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   stream s; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) s.insert(s.end(), tmp, tmp + got);
   fclose(f);
   ret s;
}

// 把 SNAPSHOT(codec 0/1) 装进 board/l1/l2；返回消耗的字节数。
static size_t load_snap(const stream &raw, size_t pos, $d &W, $d &H, vector<unsigned> &cur, vector<unsigned> &l1, vector<unsigned> &l2) {
   $d codec = raw[pos + 2];
   W = rd16(raw, pos + 4); H = rd16(raw, pos + 6);
   size_t wh = size_t(W) * H;
   size_t need = 16 + size_t(codec ? 12 : 4) * wh;
   printf("SNAPSHOT w=%d h=%d codec=%d ver=%llu\n", W, H, codec, rd64(raw, pos + 8));
   cur.assign(wh, 0); l1.assign(wh, 0); l2.assign(wh, 0);
   for (size_t i = 0; i < wh; ++i) cur[i] = rd32u(raw, pos + 16 + 4 * i);
   if (codec == 1) {
      for (size_t i = 0; i < wh; ++i) l1[i] = rd32u(raw, pos + 16 + 4 * (wh + i));
      for (size_t i = 0; i < wh; ++i) l2[i] = rd32u(raw, pos + 16 + 4 * (2 * wh + i));
   }
   ret pos + need;
}

signed main($d argc, char **argv) {
   if (argc < 4) { fprintf(stderr, "usage: %s <snapshot.zst> <delta.zst|-> <out.ppm>\n", argv[0]); ret 1; }
   stream sraw, draw;
   zstddec(slurp(argv[1]), sraw, 16u << 20);
   if (strcmp(argv[2], "-") != 0) {
      stream dz = slurp(argv[2]);
      if (!dz.empty()) zstddec(dz, draw, 16u << 20);   // 304 Not Modified 时是空 body
   }
   $d W = 0, H = 0;
   vector<unsigned> cur, l1, l2;
   size_t pos = load_snap(sraw, 0, W, H, cur, l1, l2);
   if (pos != sraw.size()) printf("WARN snapshot trailing %zu\n", sraw.size() - pos);
   size_t wh = size_t(W) * H;
   size_t p = 0; $d rec = 0;
   unsigned long long ents = 0, changed = 0, repaint = 0, c0 = 0, c1 = 0, c2 = 0;
   while (p < draw.size()) {
      if (draw[p] != 0x82) { printf("unknown type 0x%02x at %zu\n", draw[p], p); break; }
      if (draw.size() - p < 36) { printf("truncated PUBLIC header\n"); break; }
      $d n = rd32u(draw, p + 4);
      unsigned long long from = rd64(draw, p + 8), to = rd64(draw, p + 16);
      $d m = rd32u(draw, p + 24), e = rd32u(draw, p + 28), order = rd32u(draw, p + 32);
      $d pz = rd16(draw, p + 2);
      size_t need = 36 + 8ull * m + pad4(n) + 4ull * e + 4ull * n;
      if (p + need > draw.size()) { printf("TRUNCATED PUBLIC\n"); break; }
      size_t o = p + 36;
      const u$b *small = &draw[o + 8 * m];
      size_t od = o + 8 * m + pad4(n), oc = od + 4 * e;
      vector<$d> d(n);
      $d big = 0;
      for ($d i = 0; i < n; ++i) {
         $d v = small[i];
         if (v == 255) v = $d(rd32u(draw, od + 4 * big++));
         d[i] = v;
      }
      size_t ent = 0;
      unsigned long long seg_changed = 0;
      for ($d j = 0; j < m; ++j) {
         $d sn = rd32u(draw, o + 4 * m + 4 * j);
         long long k = 0;
         for ($d q = 0; q < sn; ++q, ++ent) {
            k += d[ent];
            unsigned long long key = (unsigned long long)k;
            size_t idx = order == 0 ? size_t(key) : size_t((key % H) * W + key / H);
            unsigned w = rd32u(draw, oc + 4 * ent);
            if (idx >= wh) { printf("BAD idx %zu\n", idx); ret 1; }
            unsigned old = cur[idx], nw;
            if (w == 0) { nw = l1[idx]; ++c0; }
            else if (w == 1) { nw = l2[idx]; ++c1; }
            else if (w == 2) { nw = old; ++c2; }
            else nw = w;
            if (nw == old) { ++repaint; continue; }
            unsigned h;
            if (l1[idx] != old && l1[idx] != nw) h = l1[idx];
            else if (l2[idx] != old && l2[idx] != nw) h = l2[idx];
            else h = nw;
            l2[idx] = h; l1[idx] = old; cur[idx] = nw;
            ++changed; ++seg_changed;
         }
      }
      printf("#%d p=%d n=%d m=%d e=%d order=%d from=%llu to=%llu entries=%zu changed=%llu\n",
             rec++, pz, n, m, e, order, from, to, ent, seg_changed);
      ents += ent; p += need;
   }
   printf("total entries=%llu changed=%llu repaint=%llu recipe0=%llu recipe1=%llu recipe2=%llu consumed=%zu/%zu\n",
          ents, changed, repaint, c0, c1, c2, p, draw.size());
   FILE *ppm = fopen(argv[3], "wb");
   if (!ppm) { perror("ppm"); ret 1; }
   fprintf(ppm, "P6\n%d %d\n255\n", W, H);
   for (size_t i = 0; i < wh; ++i) {
      unsigned c = cur[i], a = c >> 24;
      unsigned r = (c & 255) * a / 255 + (255 - a), g = ((c >> 8) & 255) * a / 255 + (255 - a), b = ((c >> 16) & 255) * a / 255 + (255 - a);
      u$b rgb[3] = {(u$b)r, (u$b)g, (u$b)b};
      fwrite(rgb, 1, 3, ppm);
   }
   fclose(ppm);
   printf("wrote %s (%dx%d)\n", argv[3], W, H);
   ret 0;
}
