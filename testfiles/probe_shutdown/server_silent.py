# 测试用 TLS 服务：完成握手后「完全不说话」，也不回应 close_notify。
# 用来验证客户端的读写超时是否生效（原 server.py 只静默 15s，和客户端
# 的 15s 读超时撞车，这里延长到 60s）。
import socket, ssl, sys

port = int(sys.argv[1])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('cert.pem', 'key.pem')

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(1)

c, _ = s.accept()
t = ctx.wrap_socket(c, server_side=True)   # 完成 TLS 握手
print('server: handshake done, silent for 60s', flush=True)

try:
    t.recv(4096)                            # 只读一次：把 Upgrade 请求吃掉
    print('server: got request, staying silent', flush=True)
except Exception as e:
    print('server: recv error:', e, flush=True)

import time
time.sleep(60)
print('server: exiting', flush=True)
