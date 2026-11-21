#include "websock.hpp"

static void put16(stream &v, $d x) { v.push_back(u$b(x & 255)); v.push_back(u$b(x >> 8 & 255)); }
static void put32(stream &v, unsigned x) {
   for ($d i = 0; i < 4; ++i) v.push_back(u$b(x >> (8 * i) & 255));
}
static void padto(stream &v) { while (v.size() & 3) v.push_back(0); }

struct bt { $d slot; u$b tok[16]; };
struct pt { $d slot; unsigned idx, color; };

static stream encbatch(const vector<bt> &bs, const vector<pt> &ps) {
   stream v;
   v.push_back(0x01); v.push_back(1); put16(v, 0); put32(v, bs.size()); put32(v, ps.size()); put32(v, 0);
   for (auto &b : bs) put16(v, b.slot);
   padto(v);
   for (auto &b : bs) for ($d i = 0; i < 16; ++i) v.push_back(b.tok[i]);
   for (auto &p : ps) put16(v, p.slot);
   padto(v);
   for (auto &p : ps) put32(v, p.idx);
   for (auto &p : ps) put32(v, p.color);
   ret v;
}
static void show(const stream &v) {
   for (u$b c : v) printf("%02x", c);
   puts("");
}
static bt mkbind($d s, $d i) {
   bt b; b.slot = s;
   for ($d j = 0; j < 16; ++j) b.tok[j] = u$b((i * 7 + j) % 256);
   ret b;
}
___main {
   show(encbatch({}, {}));
   show(encbatch({mkbind(3, 1)}, {{7, 123456, 0xFF0000FFu}}));
   show(encbatch({mkbind(3, 1), mkbind(40000, 2), mkbind(65535, 3)},
                 {{0, 0, 0xFF000000u}, {1, 599999, 0x00FF00FFu}, {2, 123456, 0x0000FFFFu},
                  {3, 42, 0x123456FFu}, {40000, 1, 0xFFFFFFFFu}}));
   show(encbatch({mkbind(0, 9), mkbind(1, 10)}, {{5, 599999, 0x112233FFu}}));
   ret 0;
}
