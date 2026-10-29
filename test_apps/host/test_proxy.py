"""Proxying to other hosts by the tunnel's ingress rules (--ingress).

The origins listen on 127.0.0.2, simulating other LAN hosts
(127.0.0.1 and localhost count as the device itself).
"""

import asyncio
import base64
import hashlib
import json
import socket
import socketserver
import threading
import time

import pytest

from fake_edge import WsReader, ws_frame, ws_open

ORIGIN_HOST = "127.0.0.2"


class _OriginHandler(socketserver.StreamRequestHandler):
    """A small HTTP/1.1 server with the response framings the proxy must handle"""

    def handle(self):
        line = self.rfile.readline().decode()
        if not line:
            return
        method, target, _ = line.split()
        headers = []
        while True:
            h = self.rfile.readline()
            if h in (b"\r\n", b"\n", b""):
                break
            name, value = h.decode().split(":", 1)
            headers.append((name.strip(), value.strip()))
        hdr = {n.lower(): v for n, v in headers}
        path, _, query = target.partition("?")
        if path.startswith("/lan/"):  # the test handlers' proxy prefix
            path = path[4:]
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        self.server.requests.append({"method": method, "target": target, "headers": headers})

        if path == "/ws":
            accept = base64.b64encode(hashlib.sha1(
                (hdr["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            while data := self.request.recv(65536):  # raw echo: frames come back as they were sent
                self.wfile.write(data)
            return
        if path == "/stall":
            self.server.release.wait(10)
        body = self._read_body(hdr)
        if path == "/chunked":
            out = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
            for part in (b"hello ", b"chunked ", b"x" * 5000, b" world"):
                out += b"%x;ext=1\r\n%s\r\n" % (len(part), part)
            self.wfile.write(out + b"0\r\nX-Trailer: t\r\n\r\n")
        elif path == "/close":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                             + b"until close " * 1000)
        elif path == "/continue":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok")
        elif path == "/big":
            size = int(params.get("size", "100000"))
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % size)
            for i in range(0, size, 65536):
                self.wfile.write(bytes(ord("a") + (i + j) % 26 for j in range(min(65536, size - i))))
        elif path == "/drop":
            # Promises more than it sends, then closes
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + b"d" * 3000)
        elif path == "/missing":
            self.wfile.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\nX-Origin: yes\r\n\r\nnope")
        else:
            info = {"method": method, "target": target, "headers": headers, "body_len": len(body),
                    "body_sha": hashlib.sha256(body).hexdigest()}
            payload = json.dumps(info).encode()
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Origin: yes\r\n"
                             b"Content-Length: %d\r\n\r\n%s" % (len(payload), payload))

    def _read_body(self, hdr):
        if "content-length" in hdr:
            return self.rfile.read(int(hdr["content-length"]))
        if hdr.get("transfer-encoding") == "chunked":
            body = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0], 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return body
                body += self.rfile.read(size)
                self.rfile.readline()
        return b""


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, handler, rcvbuf=None):
        self.rcvbuf = rcvbuf
        self.requests = []
        self.release = threading.Event()
        super().__init__((ORIGIN_HOST, 0), handler)
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def handle_error(self, request, client_address):
        pass  # disconnected clients reset the proxy's connection; not interesting

    def server_bind(self):
        if self.rcvbuf:
            # Accepted sockets inherit it: the origin takes data slowly
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf)
        super().server_bind()


class _EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(65536):
            self.request.sendall(data)


@pytest.fixture
def origins():
    http = _Server(_OriginHandler)
    slow = _Server(_OriginHandler, rcvbuf=4096)
    echo = _Server(_EchoHandler)
    with socket.socket() as s:  # an unbound port
        s.bind((ORIGIN_HOST, 0))
        closed_port = s.getsockname()[1]
    yield {"http": http, "slow": slow, "echo": echo, "closed_port": closed_port}
    for srv in (http, slow, echo):
        srv.release.set()
        srv.shutdown()
        srv.server_close()


def ingress(o):
    http = f"http://{ORIGIN_HOST}:{o['http'].port}"
    return {"ingress": [
        {"hostname": "app.example.com", "service": http},
        {"hostname": "hdr.example.com", "service": http, "originRequest": {"httpHostHeader": "internal.lan"}},
        {"hostname": "slow.example.com", "service": f"http://{ORIGIN_HOST}:{o['slow'].port}"},
        {"hostname": "ws.example.com", "service": f"ws://{ORIGIN_HOST}:{o['http'].port}"},
        {"hostname": "ssh.example.com", "service": f"ssh://{ORIGIN_HOST}:{o['echo'].port}"},
        {"hostname": "*.wild.example.com", "path": "^/api/", "service": http},
        {"hostname": "down.example.com", "service": f"http://{ORIGIN_HOST}:{o['closed_port']}"},
        {"hostname": "test.example.com", "service": "http://localhost:8080"},
        {"service": "http_status:418"},
    ], "warp-routing": {"enabled": False}}


@pytest.fixture
def tunnel(edge, connector, origins):
    """A connector with --ingress and the rules above applied; returns the edge connection"""
    def start(*args):
        connector("--ingress", *args)
        [conn] = edge.wait_registered()
        resp, reply = edge.push_config(conn, 1, ingress(origins))
        assert reply == {"lastAppliedVersion": 1, "err": None}
        return conn
    return start


def info(resp):
    assert resp.status == 200, resp
    return json.loads(resp.body)


def test_proxy_get(edge, tunnel, origins):
    conn = tunnel()
    resp = edge.request(conn, "GET", "/a/b?x=1&y=2", [("x-test", "abc"), ("cf-connecting-ip", "1.2.3.4")],
                        authority="app.example.com")
    i = info(resp)
    assert i["method"] == "GET" and i["target"] == "/a/b?x=1&y=2"
    h = {n.lower(): v for n, v in i["headers"]}
    assert h["host"] == "app.example.com"
    assert h["x-test"] == "abc" and h["cf-connecting-ip"] == "1.2.3.4"
    assert h["connection"] == "close"
    assert "content-length" not in h and "transfer-encoding" not in h
    assert resp.user_headers["x-origin"] == "yes"
    assert resp.headers["cf-cloudflared-response-meta"] == '{"src":"origin"}'


def test_proxy_host_header(edge, tunnel):
    conn = tunnel()
    h = {n.lower(): v for n, v in info(edge.request(conn, "GET", "/", authority="hdr.example.com"))["headers"]}
    assert h["host"] == "internal.lan" and h["x-forwarded-host"] == "hdr.example.com"


def test_proxy_routing(edge, tunnel):
    conn = tunnel()
    # Rule for localhost: the device's own handler, which sees the service
    resp = edge.request(conn, "GET", "/", authority="test.example.com")
    assert b"Hello from" in resp.body and b"service: http://localhost:8080" in resp.body
    # Wildcard host with a path regex
    assert info(edge.request(conn, "GET", "/api/v1", authority="x.wild.example.com"))["target"] == "/api/v1"
    assert edge.request(conn, "GET", "/other", authority="x.wild.example.com").status == 418
    # Catch-all http_status
    assert edge.request(conn, "GET", "/", authority="unknown.example.com").status == 418


def test_rules_ignored_without_ingress_option(edge, connector, origins):
    connector()
    [conn] = edge.wait_registered()
    edge.push_config(conn, 1, ingress(origins))
    assert b"Hello from" in edge.request(conn, "GET", "/", authority="app.example.com").body


def test_proxy_bodies(edge, tunnel):
    conn = tunnel()
    body = bytes(range(256)) * 2000  # 512000 bytes
    # With Content-Length
    i = info(edge.request(conn, "POST", "/", [("content-length", str(len(body)))], body,
                          authority="app.example.com"))
    assert i["body_len"] == len(body) and i["body_sha"] == hashlib.sha256(body).hexdigest()
    assert ["Content-Length", str(len(body))] in i["headers"]
    # Without: sent chunked
    i = info(edge.request(conn, "PUT", "/", body=body[:70000], authority="app.example.com"))
    assert i["body_sha"] == hashlib.sha256(body[:70000]).hexdigest()
    assert ["Transfer-Encoding", "chunked"] in i["headers"]
    # No body at all
    i = info(edge.request(conn, "DELETE", "/", authority="app.example.com"))
    assert i["body_len"] == 0 and not any(n in ("Transfer-Encoding", "Content-Length") for n, _ in i["headers"])


def test_proxy_response_framings(edge, tunnel):
    conn = tunnel()
    resp = edge.request(conn, "GET", "/chunked", authority="app.example.com")
    assert resp.status == 200 and resp.body == b"hello chunked " + b"x" * 5000 + b" world"
    assert "transfer-encoding" not in resp.user_headers
    resp = edge.request(conn, "GET", "/close", authority="app.example.com")
    assert resp.status == 200 and resp.body == b"until close " * 1000
    resp = edge.request(conn, "POST", "/continue", body=b"x", authority="app.example.com")
    assert resp.status == 201 and resp.body == b"ok"
    resp = edge.request(conn, "GET", "/missing", authority="app.example.com")
    assert resp.status == 404 and resp.body == b"nope" and resp.user_headers["x-origin"] == "yes"
    resp = edge.request(conn, "HEAD", "/big?size=1000", authority="app.example.com")
    assert resp.status == 200 and resp.body == b"" and resp.headers["content-length"] == "1000"
    resp = edge.request(conn, "GET", "/big?size=2000000", authority="app.example.com", timeout=60)
    assert resp.status == 200 and len(resp.body) == 2000000
    assert resp.body[:52] == b"abcdefghijklmnopqrstuvwxyz" * 2


def test_proxy_origin_down(edge, tunnel):
    conn = tunnel()
    resp = edge.request(conn, "GET", "/", authority="down.example.com")
    assert resp.status == 502
    assert resp.headers["cf-cloudflared-response-meta"] == '{"src":"cloudflared"}'


def test_proxy_stalled_origin(edge, tunnel, origins):
    """The upload waits for an origin that does not read yet (paused body), then completes"""
    conn = tunnel()
    body = b"z" * 3_000_000

    async def upload():
        sid = conn.open_stream("POST", "/stall", [("content-length", str(len(body)))], body,
                               "slow.example.com", end_stream=True)
        await asyncio.sleep(1)
        origins["slow"].release.set()
        await asyncio.wait_for(conn.streams[sid].done.wait(), 30)
        return conn.streams.pop(sid).resp

    assert info(edge.call(upload(), timeout=40))["body_len"] == len(body)


def test_proxy_origin_limit(edge, tunnel, origins):
    conn = tunnel("--max-origins", "1")

    async def two():
        first = conn.open_stream("GET", "/stall", authority="slow.example.com")
        await asyncio.sleep(0.5)
        second = await conn.request("GET", "/", authority="app.example.com")
        origins["slow"].release.set()
        await asyncio.wait_for(conn.streams[first].done.wait(), 10)
        return conn.streams.pop(first).resp.status, second.status

    assert edge.call(two()) == (200, 503)
    # The slot is free again
    assert edge.request(conn, "GET", "/", authority="app.example.com").status == 200


def test_proxy_websocket(edge, tunnel, origins):
    conn = tunnel()

    async def session():
        sid, key = ws_open(conn, "/ws", "ws.example.com")
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        frames = ws_frame(1, b"hello") + ws_frame(2, bytes(20000))
        conn.send_body(sid, frames)
        data = b""
        while len(data) < len(frames):
            data += await asyncio.wait_for(s.chunks.get(), 5)
        return s.resp, key, data, frames

    resp, key, data, frames = edge.call(session())
    assert resp.status == 200
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    assert resp.user_headers["sec-websocket-accept"] == accept
    assert resp.user_headers["upgrade"] == "websocket"
    assert data == frames  # passed through unchanged, both ways
    h = {n.lower(): v for n, v in origins["http"].requests[-1]["headers"]}
    assert h["upgrade"] == "websocket" and h["connection"] == "Upgrade" and h["sec-websocket-key"] == key


def test_tcp_over_websocket(edge, tunnel):
    """ssh:// and friends, as `cloudflared access` uses them: binary messages carry the bytes"""
    conn = tunnel()

    async def session():
        sid, key = ws_open(conn, "/", "ssh.example.com")
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        sent = b"SSH-2.0-test\r\n" + bytes(range(256)) * 400  # over the 16 KiB WebSocket message limit
        conn.send_body(sid, ws_frame(2, sent[:20]) + ws_frame(2, sent[20:]))
        got = b""
        while len(got) < len(sent):
            op, payload = await r.frame()
            assert op == 2
            got += payload
        conn.send_body(sid, ws_frame(8, (1000).to_bytes(2, "big")))
        op, _ = await r.frame()
        return s.resp, key, sent, got, op

    resp, key, sent, got, op = edge.call(session())
    assert resp.status == 200
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    assert resp.user_headers["sec-websocket-accept"] == accept
    assert got == sent
    assert op == 8


def test_tcp_service_needs_websocket(edge, tunnel):
    conn = tunnel()
    assert edge.request(conn, "GET", "/", authority="ssh.example.com").status == 400


def test_config_update_replaces_rules(edge, tunnel, origins):
    conn = tunnel()
    assert edge.request(conn, "GET", "/", authority="app.example.com").status == 200
    edge.push_config(conn, 2, {"ingress": [{"service": "http_status:404"}]})
    assert edge.request(conn, "GET", "/", authority="app.example.com").status == 404
    # Older versions are ignored
    edge.push_config(conn, 1, ingress(origins))
    assert edge.request(conn, "GET", "/", authority="app.example.com").status == 404


@pytest.mark.parametrize("size", ["4096", "131072"])
def test_proxy_buffer_size(edge, tunnel, size):
    conn = tunnel("--proxy-buffer", size, "--stream-window", "262144")
    resp = edge.request(conn, "GET", "/big?size=1000000", authority="app.example.com", timeout=60)
    assert resp.status == 200 and resp.body == bytes(ord("a") + i % 26 for i in range(1000000))
    body = bytes(range(256)) * 2000
    i = info(edge.request(conn, "POST", "/", [("content-length", str(len(body)))], body,
                          authority="app.example.com"))
    assert i["body_sha"] == hashlib.sha256(body).hexdigest()


def test_tcp_over_websocket_visitor_ends(edge, tunnel):
    """When the visitor's side ends (END_STREAM), the origin sees EOF and the stream ends:
    an echo origin would otherwise keep the stream and origin connection open indefinitely"""
    conn = tunnel("--max-origins", "1")

    async def session():
        sid, _ = ws_open(conn, "/", "ssh.example.com")
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        conn.send_body(sid, ws_frame(2, b"bye"))
        op, payload = await r.frame()
        conn.send_body(sid, b"", end_stream=True)
        await asyncio.wait_for(s.done.wait(), 5)
        return payload

    assert edge.call(session()) == b"bye"
    # The origin connection was released: the limit of 1 allows another
    resp = edge.request(conn, "GET", "/", authority="app.example.com")
    assert resp.status == 200
