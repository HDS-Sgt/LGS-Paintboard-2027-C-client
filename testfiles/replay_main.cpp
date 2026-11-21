#define main prog_unused
#include "../main.cpp"
#undef main

static stream slurp(const char *path) {
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   stream s; u$b tmp[65536]; size_t got;
   while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) s.insert(s.end(), tmp, tmp + got);
   fclose(f);
   ret s;
}

signed main($d argc, char **argv) {
   if (argc < 4) { fprintf(stderr, "usage: %s <snap.zst> <delta.zst> <out.ppm>\n", argv[0]); ret 1; }

   stream rules(96, 0);
   rules[0] = 0x84; rules[1] = 1;
   rules[4] = 1000 & 255; rules[5] = 1000 >> 8;
   rules[6] = 600 & 255;  rules[7] = 600 >> 8;
   rules[8] = 2; rules[12] = 128;
   readptr R = {&rules, 1};
   procRule(R);

   stream sr = slurp(argv[1]), sraw;
   zstddec(sr, sraw, 16u << 20);
   readptr S = {&sraw, 0};
   if (S.readb() != 0x83) { puts("not a snapshot"); ret 2; }
   procSnap(S);

   stream dr = slurp(argv[2]), draw;
   zstddec(dr, draw, 16u << 20);
   readptr D = {&draw, 0};
   while (!D.end()) {
      u$b typ = D.readb();
      if (typ == 0x82) {
         if (procPublic(D)) { puts("from mismatch"); ret 3; }
      } elif (typ == 0x83) {
         procSnap(D);
      } else {
         printf("unknown type 0x%02x at %d\n", typ, D.ptr - 1);
         ret 4;
      }
   }

   FILE *ppm = fopen(argv[3], "wb");
   if (!ppm) { perror(argv[3]); ret 5; }
   fprintf(ppm, "P6\n%d %d\n255\n", glob.W, glob.H);
   rep(i, 0, glob.H - 1) rep(j, 0, glob.W - 1) {
      u$d c = cols[i][j], a = c >> 24;
      u$d r = (c & 255) * a / 255 + (255 - a);
      u$d g = ((c >> 8) & 255) * a / 255 + (255 - a);
      u$d b = ((c >> 16) & 255) * a / 255 + (255 - a);
      u$b rgb[3] = {(u$b)r, (u$b)g, (u$b)b};
      fwrite(rgb, 1, 3, ppm);
   }
   fclose(ppm);
   printf("wrote %s (%dx%d) since=%lld\n", argv[3], glob.W, glob.H, (long long)since);
   ret 0;
}
