#include "websock.hpp"

___main {
   try {
      tcSocket s;
      s.connect("pb.ika.zip", "443", "/ws?public=0");
      puts("connected");

      s.sendping();
      puts("ping sent");

      auto m = s.recvbin();
      if(!m) {
         puts("closed by server");
         ret 0;
      }
      printf("got %zu bytes, first=%02x\n", m->size(), (*m)[0]);
   } catch(const std::exception &e) {
      fprintf(stderr, "error: %s\n", e.what());
      ret 1;
   }
   ret 0;
}
