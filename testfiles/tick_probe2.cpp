#include "zstdwrap.hpp"
#include "websock.hpp"

static void put16(stream &v, $d x) { v.push_back(u$b(x & 255)); v.push_back(u$b(x >> 8 & 255)); }
static void put32(stream &v, unsigned x) { for ($d i = 0; i < 4; ++i) v.push_back(u$b(x >> (8 * i) & 255)); }

static stream mkbatch($d nb, $d np) {
   stream v;
   v.push_back(0x01); v.push_back(1); put16(v, 0); put32(v, nb); put32(v, np); put32(v, 0);
   ret v;
}
static void hex(const stream &v) { for (u$b c : v) printf("%02x", c); puts(""); }

___main {
   tcSocket s;
   s.connect("pb.ika.zip", "443", "/ws?public=0");
   puts("connected");
   auto m = s.recvbin();
   printf("first recvbin has_value=%d\n", (int)m.has_value());
   stream raw; zstddec(*m, raw);
   printf("RULES len=%zu type=%02x\n", raw.size(), raw[0]);

   stream b = mkbatch(0, 0);           // 空 BATCH
   stream w; zstdenc(b, w);
   printf("empty batch payload="); hex(b);
   printf("empty batch wire="); hex(w);
   s.sendbin(w);
   puts("sent empty batch");

   for ($d i = 0; i < 3; ++i) {
      auto r = s.recvbin();
      printf("recvbin #%d has_value=%d\n", i, (int)r.has_value());
      if (!r) break;
      stream p; zstddec(*r, p);
      printf("  payload len=%zu type=%02x\n", p.size(), p[0]);
   }
   ret 0;
}
