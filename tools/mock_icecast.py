#!/usr/bin/env python3
# Mock icecast server: validates the SOURCE handshake + Basic auth, replies
# 200, captures the pushed MP3 to push.mp3, logs byte-rate. One client, ~35s.
import socket, base64, time, sys

EXPECT_AUTH = base64.b64encode(b"source:hackme").decode()
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("", 8001))
srv.listen(1)
print("mock icecast on :8001, expecting Basic", EXPECT_AUTH, flush=True)

conn, addr = srv.accept()
print("connect from", addr, flush=True)
buf = b""
while b"\r\n\r\n" not in buf:
    d = conn.recv(2048)
    if not d: sys.exit("EOF during headers")
    buf += d
head, rest = buf.split(b"\r\n\r\n", 1)
print("---- handshake ----", flush=True)
print(head.decode(errors="replace"), flush=True)
hs = head.decode(errors="replace")
ok = hs.startswith("SOURCE /live HTTP/1.0") and ("Authorization: Basic " + EXPECT_AUTH) in hs \
     and "Content-Type: audio/mpeg" in hs
print("handshake valid:", ok, flush=True)
if not ok:
    conn.send(b"HTTP/1.0 401 Unauthorized\r\n\r\n")
    sys.exit(1)
conn.send(b"HTTP/1.0 200 OK\r\n\r\n")

out = open("push.mp3", "wb")
out.write(rest)
total = len(rest)
t0 = time.time()
last = t0
conn.settimeout(10)
try:
    while time.time() - t0 < 35:
        d = conn.recv(4096)
        if not d:
            print("source disconnected", flush=True)
            break
        out.write(d)
        total += len(d)
        if time.time() - last >= 5:
            last = time.time()
            print(f"{total} bytes @ {total/(time.time()-t0):.0f} B/s", flush=True)
except socket.timeout:
    print("recv timeout", flush=True)
out.close()
print(f"FINAL: {total} bytes in {time.time()-t0:.1f}s = {total/(time.time()-t0):.0f} B/s (96kbps = 12000)", flush=True)
