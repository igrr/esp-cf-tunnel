"""Origin services on another LAN host for the stress test (Python 3.8+, no
dependencies). The device proxies /lan/... here (the prefix is ignored) and
/tcp to the TCP port.

HTTP (port 18921):
  GET  /big?size=N          N bytes of 'a'..'z' repeating, with Content-Length
  GET  /chunked?size=N      the same, chunked
  GET  /close?size=N        the same, ended by closing the connection
  GET  /slow?size=N&rate=R  the same, R bytes every 50 ms
  GET  /drop                promises 100000 bytes, sends 3000 and closes
  GET  /stall?ms=N          answers after N ms
  POST /info                the body's length and SHA-256 as JSON (any framing)
TCP (port 18922): the first byte selects the service:
  b"E"                      echo
  b"D" + 8-byte size        sends size bytes of the pattern, then closes

usage: origin.py BIND_ADDRESS
"""

import hashlib
import json
import socketserver
import sys
import threading
import time

PATTERN = bytes(ord("a") + i % 26 for i in range(26 * 2600))  # 67600 bytes, a multiple of 26


def pattern_chunks(size):
    left = size
    while left:
        n = min(left, len(PATTERN))
        yield PATTERN[:n]
        left -= n


class Http(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode()
        if not line:
            return
        method, target, _ = line.split()
        hdr = {}
        while True:
            h = self.rfile.readline()
            if h in (b"\r\n", b"\n", b""):
                break
            k, v = h.decode().split(":", 1)
            hdr[k.strip().lower()] = v.strip()
        path, _, query = target.partition("?")
        if path.startswith("/lan/"):
            path = path[4:]
        q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        size = int(q.get("size", "100000"))
        body = self.read_body(hdr)
        w = self.wfile.write
        if path == "/big":
            w(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % size)
            for c in pattern_chunks(size):
                w(c)
        elif path == "/chunked":
            w(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
            for c in pattern_chunks(size):
                w(b"%x\r\n%s\r\n" % (len(c), c))
            w(b"0\r\n\r\n")
        elif path == "/close":
            w(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
            for c in pattern_chunks(size):
                w(c)
        elif path == "/slow":
            rate = int(q.get("rate", "5000"))
            w(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % size)
            data = b"".join(pattern_chunks(size))
            for i in range(0, size, rate):
                w(data[i:i + rate])
                self.wfile.flush()
                time.sleep(0.05)
        elif path == "/drop":
            w(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + b"d" * 3000)
        elif path == "/stall":
            time.sleep(int(q.get("ms", "1000")) / 1000)
            w(b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nstalled\n")
        else:
            info = json.dumps({"len": len(body), "sha256": hashlib.sha256(body).hexdigest(),
                               "method": method}).encode()
            w(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s"
              % (len(info), info))

    def read_body(self, hdr):
        if "content-length" in hdr:
            return self.rfile.read(int(hdr["content-length"]))
        if hdr.get("transfer-encoding", "").endswith("chunked"):
            parts = []
            while True:
                n = int(self.rfile.readline().split(b";")[0], 16)
                if n == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return b"".join(parts)
                parts.append(self.rfile.read(n))
                self.rfile.readline()
        return b""


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError
        buf += d
    return buf


class Tcp(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        op = recv_exact(s, 1)
        if op == b"E":
            while True:
                d = s.recv(65536)
                if not d:
                    return
                s.sendall(d)
        elif op == b"D":
            for c in pattern_chunks(int.from_bytes(recv_exact(s, 8), "big")):
                s.sendall(c)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # visitors that go away reset the proxy's connections


if __name__ == "__main__":
    ip = sys.argv[1]
    tcp = Server((ip, 18922), Tcp)
    threading.Thread(target=tcp.serve_forever, daemon=True).start()
    print("serving on", ip, flush=True)
    Server((ip, 18921), Http).serve_forever()
