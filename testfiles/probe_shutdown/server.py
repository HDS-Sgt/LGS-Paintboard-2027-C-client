import socket, ssl, time, sys
port = int(sys.argv[1])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('cert.pem', 'key.pem')
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(1)
c, _ = s.accept()
t = ctx.wrap_socket(c, server_side=True)   # 完成 TLS 握手
print('server: handshake done, now silent 15s', flush=True)
time.sleep(15)                              # 不读不写不回应 close_notify
print('server: exiting', flush=True)
