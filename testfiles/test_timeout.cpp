// 超时路径测试：把 example.cpp 整个包含进来（只把它的 main 改名），
// 连本机一个「握手后完全静默」的 TLS 服务，验证：
//   1. 连接阶段不会永久挂住；
//   2. 15s 后 read_more() 抛 "SSL_read timeout" 而不是无限阻塞。
//
// 编译（在 paintboard/ 下）：
//   g++ -std=c++20 -O2 -Wall -Wextra test_timeout.cpp -o test_timeout -lssl -lcrypto -pthread
// 运行：
//   SSL_CERT_FILE=.probe_shutdown/cert.pem ./test_timeout <port>
#define main example_main
#include "example.cpp"
#undef main

int main(int argc, char **argv) {
    const string port = (argc > 1 ? argv[1] : "24446");

    auto t0 = chrono::steady_clock::now();

    try {
        WebSocket ws;
        ws.connect("localhost", port, "/");
        cerr << "connected (unexpected)\n";
        auto msg = ws.recv_binary();
        cerr << "recv returned " << (msg ? msg->size() : 0) << " bytes\n";
    }
    catch (const exception &e) {
        auto ms = chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now() - t0
        ).count();
        cerr << "elapsed " << ms << " ms, caught: " << e.what() << '\n';
        return 0;
    }

    return 1;
}
