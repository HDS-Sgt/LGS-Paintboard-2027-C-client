#include "zstdwrap.hpp"
#include "websock.hpp"

static u$b rd8(const stream &v, size_t o) { ret v[o]; }
static $d rd16(const stream &v, size_t o) { ret $d(v[o]) | ($d(v[o + 1]) << 8); }
static $q rd32(const stream &v, size_t o) {
   ret $q(v[o]) | ($q(v[o + 1]) << 8) | ($q(v[o + 2]) << 16) | ($q(v[o + 3]) << 24);
}
static unsigned long long rd64(const stream &v, size_t o) {
   unsigned long long r = 0;
   for ($d i = 0; i < 8; ++i) r |= (unsigned long long)v[o + i] << (8 * i);
   ret r;
}

// 只做一件事：把 recvbin() 拿到的第一条二进制消息按协议解压，并解释 RULES 的 96 字节。
___main {
   tcSocket s;
   s.connect("pb.ika.zip", "443", "/ws?public=0");
   puts("connected");
   auto m = s.recvbin();
   if (!m) {
      puts("closed");
      ret 0;
   }
   stream raw;
   zstddec(*m, raw);
   printf("wire=%zu bytes, payload=%zu bytes, type=0x%02x, version=%u\n",
          m->size(), raw.size(), rd8(raw, 0), rd8(raw, 1));
   if (rd8(raw, 0) != 0x84 || raw.size() != 96) {
      puts("not a RULES message");
      ret 1;
   }
   printf("W=%u H=%u R=%u tps=%u res2=%u res10=%u\n",
          ($d)rd16(raw, 4), ($d)rd16(raw, 6), ($d)rd16(raw, 8), ($d)rd32(raw, 12),
          ($d)rd16(raw, 2), ($d)rd16(raw, 10));
   printf("cooldown_base_us=%llu (%.3f s)\n", rd64(raw, 16), rd64(raw, 16) / 1e6);
   printf("cooldown_per_px_us=%llu (%.3f s/px)\n", rd64(raw, 24), rd64(raw, 24) / 1e6);
   printf("public_delay_ms=%llu (%.1f s) public_period_ms=%llu\n",
          rd64(raw, 32), rd64(raw, 32) / 1e3, rd64(raw, 40));
   printf("event_start_unix_ms=%llu event_end_unix_ms=%llu\n", rd64(raw, 48), rd64(raw, 56));
   printf("max_slots=%u max_paints_per_message=%u msg_rate=%u msg_burst=%u\n",
          ($d)rd32(raw, 64), ($d)rd32(raw, 68), ($d)rd32(raw, 72), ($d)rd32(raw, 76));
   printf("max_message_bytes=%u max_payload_bytes=%u server_time_us=%llu\n",
          ($d)rd32(raw, 80), ($d)rd32(raw, 84), rd64(raw, 88));
   ret 0;
}
