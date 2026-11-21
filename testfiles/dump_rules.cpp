#include "zstdwrap.hpp"
#include "websock.hpp"
___main {
   tcSocket s; s.connect("pb.ika.zip", "443", "/ws?public=0");
   auto m = s.recvbin(); stream raw; zstddec(*m, raw);
   printf("payload=%zu\n", raw.size());
   for (size_t i = 0; i < raw.size(); ++i) printf("%02x%c", raw[i], (i % 16 == 15) ? '\n' : ' ');
   printf("\n");
   ret 0;
}
