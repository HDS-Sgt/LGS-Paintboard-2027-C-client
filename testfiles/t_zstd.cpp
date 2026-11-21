#include <zstd.h>
#include <cstdio>
int main() {
   printf("zstd version=%u\n", ZSTD_versionNumber());
   return 0;
}
