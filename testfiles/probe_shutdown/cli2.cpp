#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{}; tv.tv_usec = 200000;              // 200ms 读超时
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (sockaddr*)&a, sizeof a)) { perror("connect"); return 1; }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx); SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { ERR_print_errors_fp(stderr); return 2; }
    puts("client: handshake done, peer is silent");
    char buf[16];
    int r = SSL_read(ssl, buf, sizeof buf);
    int e = SSL_get_error(ssl, r);
    printf("SSL_read -> %d, SSL_get_error -> %d (%s)\n", r, e,
           e == SSL_ERROR_WANT_READ ? "WANT_READ" :
           e == SSL_ERROR_SYSCALL ? "SYSCALL" : "other");
    SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return 0;
}
