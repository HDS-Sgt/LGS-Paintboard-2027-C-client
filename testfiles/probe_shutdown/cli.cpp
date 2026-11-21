#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <chrono>
int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (sockaddr*)&a, sizeof a)) { perror("connect"); return 1; }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx); SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { ERR_print_errors_fp(stderr); return 2; }
    puts("client: handshake done");
    auto t0 = std::chrono::steady_clock::now();
    int r = SSL_shutdown(ssl);            // 只调用一次
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("client: one SSL_shutdown -> %d, elapsed %.1f ms\n", r, ms);
    SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return 0;
}
