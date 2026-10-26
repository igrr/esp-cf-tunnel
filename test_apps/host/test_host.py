"""Host tests. Build directory from $CFT_HOST_BUILD (default: ./build).

Run: pytest test_apps/host (needs pycapnp and h2)
"""

import asyncio
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from fake_edge import FakeEdge

HERE = Path(__file__).parent
BUILD = Path(os.environ.get("CFT_HOST_BUILD", HERE / "build"))

ACCOUNT = "0123456789abcdef0123456789abcdef"
TUNNEL_ID = "e9b6087f-d155-4cca-bb1b-07a77bfc4306"
SECRET = bytes(range(32))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# --- registration RPC against the reference Cap'n Proto implementation ---

@pytest.fixture(scope="module")
def mock_server():
    port = free_port()
    proc = subprocess.Popen(
        [sys.executable, str(HERE / "mock_registration_server.py"), str(HERE / "schemas"), str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            break
        except OSError:
            time.sleep(0.2)
    else:
        proc.kill()
        pytest.fail("mock server did not start: " + proc.stdout.read())
    yield port
    proc.kill()
    proc.wait()


@pytest.mark.parametrize("args", [[], ["--fail"]], ids=["register", "refused"])
def test_rpc(mock_server, args):
    r = subprocess.run([str(BUILD / "rpc_test"), str(mock_server), *args], capture_output=True, text=True,
                       timeout=20)
    assert r.returncode == 0, r.stdout + r.stderr


# --- connector against the fake edge ---

@pytest.fixture
def edge():
    e = FakeEdge()
    yield e
    e.stop()


@pytest.fixture
def token_file(tmp_path):
    token = {"a": ACCOUNT, "t": TUNNEL_ID, "s": base64.b64encode(SECRET).decode()}
    path = tmp_path / "tunnel.token"
    path.write_text(base64.b64encode(json.dumps(token).encode()).decode())
    return path


@pytest.fixture
def connector(edge, token_file, tmp_path):
    procs = []

    def start(*args):
        log = open(tmp_path / f"connector{len(procs)}.log", "w")
        p = subprocess.Popen([str(BUILD / "cft_host"), "--token-file", str(token_file), "--plain",
                              "--edge", f"127.0.0.1:{edge.port}", "-v", *args],
                             stdout=log, stderr=subprocess.STDOUT)
        p.log_path = log.name
        procs.append(p)
        return p

    yield start
    for p in procs:
        if p.poll() is None:
            # A clean exit runs LeakSanitizer
            p.send_signal(signal.SIGINT)
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        log = Path(p.log_path).read_text()
        assert "Sanitizer" not in log and "runtime error" not in log, log


def test_register_and_request(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    reg = conn.registration
    assert reg.account_tag == ACCOUNT
    assert reg.tunnel_id.hex() == TUNNEL_ID.replace("-", "")
    assert reg.conn_index == 0
    assert "serialized_headers" in reg.features
    resp = edge.request(conn, "GET", "/hello?x=1", [("x-test", "abc")])
    assert resp.status == 200
    assert b"GET /hello?x=1" in resp.body and b"x-test: abc" in resp.body
    assert resp.user_headers["content-type"].startswith("text/plain")
    assert resp.headers["cf-cloudflared-response-meta"] == '{"src":"origin"}'


def test_config_push(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    resp, reply = edge.push_config(conn, 5, {"ingress": []})
    assert resp.status == 200 and reply == {"lastAppliedVersion": 5, "err": None}
    _, reply = edge.push_config(conn, 3, {"ingress": []})
    assert reply["lastAppliedVersion"] == 5


def test_two_connections(edge, connector):
    connector("--connections", "2")
    conns = edge.wait_registered(2)
    assert sorted(c.registration.conn_index for c in conns) == [0, 1]
    assert conns[0].registration.client_id == conns[1].registration.client_id


def test_drain_opens_replacement_first(edge, connector):
    connector()
    [old] = edge.wait_registered()
    edge.call_soon(old.drain)
    # The replacement registers under another index while the old one unregisters
    edge.wait(old.unregistered)
    edge.wait(old.closed)
    [new] = edge.wait_registered()
    assert new.registration.conn_index == 1
    # The replacement was opened before the old connection went away
    assert new.opened_at < old.closed_at
    assert edge.request(new, "GET", "/").status == 200


def test_reconnect_after_connection_loss(edge, connector):
    connector()
    [first] = edge.wait_registered()
    edge.call_soon(first.abort)
    edge.wait(first.closed)
    [second] = edge.wait_registered()
    assert second is not first
    assert second.registration.conn_index == 0
    assert second.registration.client_id == first.registration.client_id


def test_unresponsive_edge_detected(edge, connector):
    connector()
    [first] = edge.wait_registered()
    first.silent = True  # stop answering, including PINGs
    edge.wait(first.closed, timeout=10)
    [second] = edge.wait_registered()
    assert second is not first


def test_graceful_stop(edge, connector):
    p = connector("--connections", "2")
    conns = edge.wait_registered(2)
    p.send_signal(signal.SIGINT)
    assert p.wait(10) == 0
    for c in conns:
        assert c.unregistered.is_set()


def test_read_body(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    resp = edge.request(conn, "POST", "/upload", body=b"x" * 10000)
    assert resp.status == 200 and resp.body == b"received 10000 bytes\n"
    assert edge.request(conn, "POST", "/upload", body=b"x" * 20000).status == 413
    # Refused from the header alone, before the body is sent

    async def early():
        sid = conn.open_stream("POST", "/upload", [("content-length", "100000")], end_stream=False)
        await asyncio.wait_for(conn.streams[sid].headers_received.wait(), 5)
        return conn.streams[sid].resp.status

    assert edge.call(early()) == 413
    resp = edge.request(conn, "GET", "/upload")
    assert resp.body == b"received 0 bytes\n"


def test_concurrency_limit(edge, connector):
    connector("--max-streams", "2")
    [conn] = edge.wait_registered()

    async def burst():
        # Requests stay in flight until their bodies end
        sids = [conn.open_stream("POST", f"/{i}", end_stream=False) for i in range(5)]
        await asyncio.sleep(0.5)
        for sid in sids:
            conn.send_body(sid, b"", end_stream=True)
        for sid in sids:
            await conn.streams[sid].done.wait()
        return [conn.streams.pop(sid).resp.status for sid in sids]

    statuses = edge.call(burst())
    assert statuses.count(200) == 2 and statuses.count(503) == 3, statuses


def test_streamed_response(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    resp = edge.request(conn, "GET", "/big?size=300000")
    assert resp.status == 200
    assert resp.headers["content-length"] == "300000"
    assert resp.body == bytes(ord("a") + i % 26 for i in range(300000))


def test_streamed_request_and_response(edge, connector):
    connector()
    [conn] = edge.wait_registered()

    async def exchange():
        sid = conn.open_stream("POST", "/echo", end_stream=False)
        s = conn.streams[sid]
        got = []
        for i in range(3):
            chunk = bytes([65 + i]) * 1000
            conn.send_body(sid, chunk)
            # Each chunk comes back before the next one is sent
            data = b""
            while len(data) < len(chunk):
                data += await asyncio.wait_for(s.chunks.get(), 5)
            got.append(data)
        conn.send_body(sid, b"", end_stream=True)
        await asyncio.wait_for(s.done.wait(), 5)
        return s.resp.status, got

    status, got = edge.call(exchange())
    assert status == 200
    assert got == [b"A" * 1000, b"B" * 1000, b"C" * 1000]


def test_post_from_another_thread(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    t = time.monotonic()
    resp = edge.request(conn, "GET", "/later")
    assert resp.body == b"later\n" and time.monotonic() - t >= 0.2


def test_streams_closed_with_connection(edge, connector):
    """Open streams get on_close when the connection goes; LeakSanitizer checks the rest."""
    connector()
    [conn] = edge.wait_registered()
    edge.call_soon(conn.open_stream, "GET", "/later")
    edge.call_soon(conn.open_stream, "POST", "/echo", (), None, "x", False)
    time.sleep(0.1)
    edge.call_soon(conn.abort)
    edge.wait_registered()


# --- WebSocket ---

def ws_frame(opcode, payload, fin=True, mask=b"\x01\x02\x03\x04"):
    """Masked client frame"""
    hdr = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n < 65536:
        hdr += bytes([0x80 | 126]) + n.to_bytes(2, "big")
    else:
        hdr += bytes([0x80 | 127]) + n.to_bytes(8, "big")
    return hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class WsReader:
    """Parses unmasked server frames from a stream's data chunks"""

    def __init__(self, stream):
        self.stream = stream
        self.buf = b""

    async def frame(self):
        while True:
            if len(self.buf) >= 2:
                n = self.buf[1] & 0x7f
                pos = 2
                if n == 126:
                    n, pos = int.from_bytes(self.buf[2:4], "big"), 4
                elif n == 127:
                    n, pos = int.from_bytes(self.buf[2:10], "big"), 10
                if len(self.buf) >= pos + n:
                    op, payload = self.buf[0] & 0x0f, self.buf[pos:pos + n]
                    self.buf = self.buf[pos + n:]
                    return op, payload
            self.buf += await asyncio.wait_for(self.stream.chunks.get(), 5)


def ws_open(conn, path="/ws"):
    key = base64.b64encode(os.urandom(16)).decode()
    sid = conn.open_stream("GET", path, [("cf-cloudflared-proxy-connection-upgrade", "websocket"),
                                         ("sec-websocket-key", key), ("sec-websocket-version", "13")],
                           end_stream=False)
    return sid, key


def test_websocket_echo(edge, connector):
    import hashlib
    connector()
    [conn] = edge.wait_registered()

    async def session():
        sid, key = ws_open(conn)
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        out = []
        conn.send_body(sid, ws_frame(1, b"hello"))
        out.append(await r.frame())
        big = os.urandom(12000)  # under the 16 KiB default limit, in two fragments
        conn.send_body(sid, ws_frame(2, big[:5000], fin=False) + ws_frame(0, big[5000:]))
        out.append(await r.frame())
        conn.send_body(sid, ws_frame(9, b"ping!"))
        out.append(await r.frame())
        conn.send_body(sid, ws_frame(1, b"close"))
        out.append(await r.frame())
        return s.resp, out, big, key

    resp, frames, big, key = edge.call(session())
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    assert resp.status == 200
    assert resp.user_headers["sec-websocket-accept"] == accept
    assert resp.user_headers["upgrade"] == "websocket"
    assert frames[0] == (1, b"hello")
    assert frames[1] == (2, big)
    assert frames[2] == (10, b"ping!")
    assert frames[3] == (8, (1000).to_bytes(2, "big") + b"bye")


def test_websocket_client_close(edge, connector):
    connector()
    [conn] = edge.wait_registered()

    async def session():
        sid, _ = ws_open(conn)
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        conn.send_body(sid, ws_frame(8, (1001).to_bytes(2, "big")))
        return await r.frame()

    assert edge.call(session()) == (8, (1001).to_bytes(2, "big"))


def test_websocket_errors(edge, connector):
    connector()
    [conn] = edge.wait_registered()

    async def session():
        sid, _ = ws_open(conn)
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        # Unmasked frame from the client
        conn.send_body(sid, b"\x81\x02hi")
        return await r.frame()

    op, payload = edge.call(session())
    assert op == 8 and int.from_bytes(payload[:2], "big") == 1002


def test_websocket_message_limit(edge, connector):
    connector()
    [conn] = edge.wait_registered()

    async def session():
        sid, _ = ws_open(conn)
        s = conn.streams[sid]
        await asyncio.wait_for(s.headers_received.wait(), 5)
        r = WsReader(s)
        conn.send_body(sid, ws_frame(2, b"x" * 20000))
        return await r.frame()

    op, payload = edge.call(session())
    assert op == 8 and int.from_bytes(payload[:2], "big") == 1009
