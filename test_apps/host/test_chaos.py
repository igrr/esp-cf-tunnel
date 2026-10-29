"""Randomized stress against the fake edge: many concurrent operations of
every kind, clients that disconnect mid-transfer, and connections that are
lost, drained or stop answering. The connector runs under the sanitizers
(see conftest.py), so memory errors and leaks fail the test.

Runs for $CFT_CHAOS_SECONDS seconds (default 30).
"""

import asyncio
import hashlib
import json
import os
import random
import re
import time
from collections import Counter
from pathlib import Path

import h2.connection

from fake_edge import WsReader, ws_frame, ws_open
from test_proxy import ORIGIN_HOST, origins  # noqa: F401 (fixture)

DURATION = float(os.environ.get("CFT_CHAOS_SECONDS", "30"))
CONCURRENCY = 24


def pattern(n, pos=0):
    return bytes(ord("a") + (pos + i) % 26 for i in range(n))


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


class Interrupted(Exception):
    """The connection closed during the operation; not a failure"""


class Shed(Exception):
    """503 from the connector: over a limit (streams, origin connections, memory)"""


def shed_check(r):
    if r.status == 503 and not r.body and r.reset is None:
        raise Shed()


class Chaos:
    def __init__(self, edge, rng):
        self.edge = edge
        self.rng = rng
        self.counts = Counter()
        self.failures = []

    def live(self):
        return [c for c in self.edge.conns
                if c.registered.is_set() and not c.closed.is_set() and not c.unregistered.is_set()
                and not c.silent and not getattr(c, "draining", False)]

    async def done(self, conn, sid, timeout=30):
        s = conn.streams[sid]
        await asyncio.wait_for(s.done.wait(), timeout)
        conn.streams.pop(sid, None)
        if conn.closed.is_set():
            raise Interrupted()
        shed_check(s.resp)
        return s.resp

    async def headers(self, conn, sid, timeout=15):
        await asyncio.wait_for(conn.streams[sid].headers_received.wait(), timeout)
        if conn.closed.is_set():
            raise Interrupted()
        shed_check(conn.streams[sid].resp)
        return conn.streams[sid].resp

    async def cancel_after(self, conn, sid, nbytes, timeout=15):
        """Waits for nbytes of response, then resets the stream simulating a client disconnect"""
        s = conn.streams[sid]
        got = 0
        while got < nbytes and not s.done.is_set():
            got += len(await asyncio.wait_for(s.chunks.get(), timeout))
        conn.reset(sid)
        conn.streams.pop(sid, None)

    # --- operations; each asserts on what it gets back ---

    async def op_hello(self, conn):
        r = await self.done(conn, conn.open_stream("GET", "/hello"))
        assert r.status == 200 and b"Hello from" in r.body, r

    async def op_big(self, conn):
        n = self.rng.choice([0, 1, 4095, 4096, 50000, 300000, 1500000])
        sid = conn.open_stream("GET", f"/big?size={n}")
        try:
            r = await self.done(conn, sid)
        except asyncio.TimeoutError:
            s = conn.streams[sid]
            raise AssertionError(f"stream {sid} size {n}: status {s.resp.status} got {len(s.resp.body)} "
                                 f"conn window {conn.h2.inbound_flow_control_window} "
                                 f"conn {conn.edge.conns.index(conn)}")
        assert r.status == 200 and r.body == pattern(n), (r.status, len(r.body), n)

    async def op_big_cancel(self, conn):
        sid = conn.open_stream("GET", "/big?size=5000000")
        await self.cancel_after(conn, sid, self.rng.randint(0, 200000))

    async def op_echo(self, conn):
        sid = conn.open_stream("POST", "/echo", end_stream=False)
        sent = b""
        for _ in range(self.rng.randint(0, 5)):
            chunk = os.urandom(self.rng.randint(1, 30000))
            conn.send_body(sid, chunk)
            sent += chunk
            await asyncio.sleep(self.rng.random() * 0.05)
        if self.rng.random() < 0.2:
            conn.reset(sid)
            conn.streams.pop(sid, None)
            return
        conn.send_body(sid, b"", end_stream=True)
        r = await self.done(conn, sid)
        assert r.status == 200 and r.body == sent, (r.status, len(r.body), len(sent))

    async def op_sink(self, conn):
        body = os.urandom(self.rng.randint(0, 400000))
        hdrs = [("content-length", str(len(body)))] if self.rng.random() < 0.5 else []
        r = await self.done(conn, conn.open_stream("POST", "/sink", hdrs, body))
        assert r.body == b"%d %016x\n" % (len(body), fnv(body)), (r.status, r.body)

    async def op_upload_limit(self, conn):
        r = await self.done(conn, conn.open_stream("POST", "/upload", body=b"u" * 20000))
        assert r.status == 413, r

    async def op_later(self, conn):
        ms = self.rng.randint(0, 1500)
        sid = conn.open_stream("GET", f"/later?ms={ms}")
        if self.rng.random() < 0.4:
            # The visitor leaves before the answer: on_close races the ticker
            await asyncio.sleep(self.rng.random() * ms / 1000)
            conn.reset(sid)
            conn.streams.pop(sid, None)
            return
        r = await self.done(conn, sid)
        assert r.status == 200 and r.body == b"later\n", r

    async def op_slow(self, conn):
        sid = conn.open_stream("GET", "/slow?size=30000&rate=3000")
        if self.rng.random() < 0.4:
            await self.cancel_after(conn, sid, self.rng.randint(0, 20000))
            return
        r = await self.done(conn, sid)
        assert r.body == pattern(30000), len(r.body)

    async def op_reset(self, conn):
        r = await self.done(conn, conn.open_stream("GET", f"/reset?after={self.rng.randint(0, 50000)}"))
        assert r.reset is not None, r

    async def op_pause(self, conn):
        n = self.rng.randint(1, 300000)
        r = await self.done(conn, conn.open_stream("POST", "/pause", body=b"p" * n))
        assert r.body == b"received %d bytes\n" % n, r

    async def op_ws(self, conn):
        sid, _ = ws_open(conn)
        await self.headers(conn, sid)
        rd = WsReader(conn.streams[sid])
        for _ in range(self.rng.randint(0, 8)):
            msg = os.urandom(self.rng.randint(0, 15000))
            conn.send_body(sid, ws_frame(2, msg))
            op, got = await rd.frame()
            assert (op, got) == (2, msg), (op, len(got), len(msg))
        end = self.rng.choice(["close-msg", "close-frame", "reset"])
        if end == "reset":
            conn.reset(sid)
            conn.streams.pop(sid, None)
            return
        conn.send_body(sid, ws_frame(1, b"close") if end == "close-msg" else ws_frame(8, (1000).to_bytes(2, "big")))
        op, _ = await rd.frame()
        assert op == 8
        conn.reset(sid)  # the edge drops the stream after the close handshake
        conn.streams.pop(sid, None)

    async def op_ws_push(self, conn):
        sid, _ = ws_open(conn, f"/ws/push?n={self.rng.randint(0, 10)}&ms=20")
        await self.headers(conn, sid)
        rd = WsReader(conn.streams[sid])
        while True:
            op, payload = await rd.frame()
            if op == 8:
                break
            assert op == 1 and payload.startswith(b"push "), payload
        conn.reset(sid)
        conn.streams.pop(sid, None)

    async def op_lan(self, conn):
        kind = self.rng.choice(["big", "post", "drop", "cancel", "dead"])
        if kind == "big":
            n = self.rng.choice([0, 1000, 200000, 1000000])
            r = await self.done(conn, conn.open_stream("GET", f"/lan/big?size={n}"))
            assert r.status == 200 and r.body == pattern(n), (r.status, len(r.body))
        elif kind == "post":
            body = os.urandom(self.rng.randint(0, 300000))
            r = await self.done(conn, conn.open_stream("POST", "/lan/info", body=body))
            assert json.loads(r.body)["body_sha"] == hashlib.sha256(body).hexdigest()
        elif kind == "drop":
            r = await self.done(conn, conn.open_stream("GET", "/lan/drop"))
            assert r.reset is not None or r.status == 502, r
        elif kind == "cancel":
            await self.cancel_after(conn, conn.open_stream("GET", "/lan/big?size=3000000"),
                                    self.rng.randint(0, 100000))
        else:
            r = await self.done(conn, conn.open_stream("GET", "/landead/x"))
            assert r.status == 502, r

    async def op_tcp(self, conn):
        sid, _ = ws_open(conn, "/tcp")
        r = await self.headers(conn, sid)
        assert r.status == 200, r
        rd = WsReader(conn.streams[sid])
        sent = os.urandom(self.rng.randint(1, 100000))
        conn.send_body(sid, ws_frame(2, sent))
        if self.rng.random() < 0.3:
            await asyncio.sleep(self.rng.random() * 0.1)
            conn.reset(sid)
            conn.streams.pop(sid, None)
            return
        got = b""
        while len(got) < len(sent):
            op, payload = await rd.frame()
            assert op == 2, op
            got += payload
        assert got == sent
        conn.reset(sid)
        conn.streams.pop(sid, None)

    # --- connection-level chaos ---

    async def chaos_abort(self, conn):
        conn.abort()

    async def chaos_drain(self, conn):
        conn.draining = True
        conn.drain()

    async def chaos_silent(self, conn):
        conn.silent = True

    async def chaos_config(self, conn):
        self.version = getattr(self, "version", 1) + 1
        body = json.dumps({"version": self.version, "config": self.config}).encode()
        sid = conn.open_stream("PUT", "/", [("cf-cloudflared-proxy-connection-upgrade", "update-configuration")],
                               body)
        r = await self.done(conn, sid)
        assert r.status == 200, r

    OPS = [
        ("op_hello", 10), ("op_big", 8), ("op_big_cancel", 6), ("op_echo", 8), ("op_sink", 6),
        ("op_upload_limit", 2), ("op_later", 8), ("op_slow", 4), ("op_reset", 3), ("op_pause", 2),
        ("op_ws", 8), ("op_ws_push", 3), ("op_lan", 8), ("op_tcp", 5), ("chaos_config", 1),
        ("chaos_abort", 0.3), ("chaos_drain", 0.3), ("chaos_silent", 0.15),
    ]

    async def one(self):
        conns = self.live()
        if not conns:
            await asyncio.sleep(0.1)
            return
        conn = self.rng.choice(conns)
        names, weights = zip(*self.OPS)
        name = self.rng.choices(names, weights)[0]
        if name.startswith("chaos_") and name != "chaos_config" and len(conns) < 2:
            name = "op_hello"  # keep at least one connection serving
        try:
            await asyncio.wait_for(getattr(self, name)(conn), 60)
            self.counts[name] += 1
        except Interrupted:
            self.counts["interrupted"] += 1
        except Shed:
            self.counts["shed"] += 1
        except Exception as e:  # noqa: BLE001
            closed_by_connector = conn.h2.state_machine.state == h2.connection.ConnectionState.CLOSED
            if conn.closed.is_set() or conn.silent or getattr(conn, "draining", False) or closed_by_connector:
                self.counts["interrupted"] += 1
            else:
                self.counts["failed"] += 1
                self.failures.append(f"{name}: {type(e).__name__}: {e!r}"[:500])

    async def run(self, duration):
        end = time.monotonic() + duration
        sem = asyncio.Semaphore(CONCURRENCY)
        tasks = set()

        async def worker():
            async with sem:
                await self.one()

        while time.monotonic() < end:
            await sem.acquire()
            sem.release()
            t = asyncio.ensure_future(worker())
            tasks.add(t)
            t.add_done_callback(tasks.discard)
            await asyncio.sleep(0)
        await asyncio.gather(*tasks)


# Why the connector may close a connection during the chaos: drains,
# unresponsive connections, and registrations the busy fake edge answers late
EXPECTED_CLOSE = {"unregistered", "edge stopped responding", "registration timed out"}


def test_chaos(edge, connector, origins):  # noqa: F811
    run_chaos(edge, connector, origins, DURATION, None)


def test_chaos_out_of_memory(edge, connector, origins):  # noqa: F811
    """The same, with allocations failing at random (fault.c) for the first
    20 s: no crash, leak or stuck stream, and normal service afterwards"""
    run_chaos(edge, connector, origins, 35, {"CFT_FAIL_RATE": "0.005", "CFT_FAIL_SECONDS": "20"})


def run_chaos(edge, connector, origins, duration, env):  # noqa: F811
    o = origins
    proc = connector(env=env, *["--ingress", "--connections", "2", "--max-streams", "64", "--max-origins", "8",
              "--posters", "3", "--stream-window", "131072",
              "--lan-http", f"http://{ORIGIN_HOST}:{o['http'].port}",
              "--lan-tcp", f"tcp://{ORIGIN_HOST}:{o['echo'].port}",
              "--lan-dead", f"http://{ORIGIN_HOST}:{o['closed_port']}"])
    edge.wait_registered(1 if env else 2)
    seed = int(os.environ.get("CFT_CHAOS_SEED", str(random.randrange(1 << 30))))
    print("seed", seed)
    chaos = Chaos(edge, random.Random(seed))
    chaos.config = {"ingress": [{"hostname": "test.example.com", "service": "http://localhost:8080"},
                                {"service": "http_status:404"}]}
    edge.call(chaos.run(duration), timeout=duration + 120)
    print(dict(chaos.counts))
    if env:
        # Failures are expected while allocations fail; now everything must work.
        # Connections that failed repeatedly may be in their (up to 60 s) backoff.
        time.sleep(3)
        edge.wait_registered(1, timeout=75)
        conn = chaos.live()[0]
        assert edge.request(conn, "GET", "/hello").status == 200
        body = os.urandom(300000)
        r = edge.request(conn, "POST", "/sink", body=body)
        assert r.body == b"%d %016x\n" % (len(body), fnv(body))
    else:
        assert not chaos.failures, "\n".join(chaos.failures[:20])

    # After traffic stops, all streams must be closed
    time.sleep(5)  # silent and draining connections close
    edge.wait_registered(1)
    conn = chaos.live()[0]
    stats = json.loads(edge.request(conn, "GET", "/stats").body)
    print(stats)
    assert stats["streams_open"] == 1, stats  # this request
    assert stats["posts_out_of_order"] == 0, stats
    assert stats["posts_run"] > 0

    reasons = Counter(re.findall(r"closing connection: (.*)", Path(proc.log_path).read_text(errors="replace")))
    print("connections closed:", dict(reasons))
    if not env:
        assert set(reasons) <= EXPECTED_CLOSE, reasons
