"""Stress test load generator: runs a random mix of operations against a
device (test_apps/device) through the real Cloudflare edge for a given time,
verifies every response, and watches the device's /stats.

    uv run --with 'httpx[http2]' --with websockets load.py \\
        --url https://HOST --duration 10800 --out results/

Operations: bursts of concurrent requests, long downloads, downloads and
uploads the visitor abandons, slow readers, streamed uploads, WebSockets
(echo, server push, idle long-lived, abrupt drops), delayed answers from
another thread, stream resets by the device, proxied HTTP and TCP to
origin.py on another LAN host (including origins that fail), and Ethernet
outages on the device (/chaos/netdown).

Failures during an outage, and for a grace period after it, are counted
separately. Every --checkpoint seconds the load pauses; once quiet, the
device's heap and allocation count are recorded, so leaks show up as a
trend across checkpoints.
"""

import argparse
import asyncio
import hashlib
import json
import os
import random
import socket
import subprocess
import sys
import time
import traceback
from collections import Counter

import httpx
import websockets

PATTERN = bytes(ord("a") + i % 26 for i in range(26 * 4000))


def check_pattern(data, pos):
    """True if data matches the pattern at offset pos"""
    off = pos % 26
    i = 0
    while i < len(data):
        n = min(len(data) - i, len(PATTERN) - 26)
        if data[i:i + n] != PATTERN[off:off + n]:
            return False
        i += n
        off = (off + n) % 26
    return True


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


class Fail(Exception):
    """Wrong response: always a failure"""


class _Dropped(Exception):
    """The visitor dropped the WebSocket on purpose"""


class Shed(Exception):
    """The device refused the request for lack of memory or streams (503)"""


class EdgeError(Exception):
    """The edge answered instead of the device (502, 530, ...)"""


class Load:
    def __init__(self, args):
        self.args = args
        self.url = args.url.rstrip("/")
        self.host = self.url.split("://", 1)[1]
        self.rng = random.Random(args.seed)
        self.counts = Counter()
        self.edge_codes = Counter()
        self.edge_samples = []  # (time, status, cf-ray, body) of the first edge errors
        self.failures = []
        self.chaos_until = 0.0
        self.paused = False
        self.inflight = 0
        self.stats_log = open(os.path.join(args.out, "stats.jsonl"), "a")
        self.event_log = open(os.path.join(args.out, "events.log"), "a")
        self.last_uptime = None
        self.reboots = 0
        self.checkpoints = []

    def log(self, msg):
        line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
        print(line, flush=True)
        self.event_log.write(line + "\n")
        self.event_log.flush()

    def in_chaos(self):
        return time.monotonic() < self.chaos_until

    # --- helpers ---

    def client(self):
        return self.rng.choice(self.clients)

    def fresh(self):
        """A client of its own, for operations that abandon a request midway:
        httpx's HTTP/2 connection may not survive a cancelled request, and
        its other requests should not fail with it"""
        return httpx.AsyncClient(timeout=httpx.Timeout(120, connect=30))

    async def get(self, path, **kw):
        r = await self.client().get(self.url + path, **kw)
        self.check_status(r)
        return r

    def check_status(self, r, expect=(200,)):
        if r.status_code in expect:
            return
        self.classify_error(r.status_code, r.content, r.headers)
        raise Fail(f"status {r.status_code}: {r.content[:200]!r}")

    def classify_error(self, status, content, headers):
        """Raises Shed or EdgeError for those kinds of error responses"""
        if status == 503 and not content:
            raise Shed()
        # Errors from the edge itself come with its error page or "error code: N"
        body = content.lower()
        if status >= 500 and (b"error code" in body or b"cloudflare" in body):
            if len(self.edge_samples) < 40:
                self.edge_samples.append((time.strftime("%H:%M:%S"), status, headers.get("cf-ray"),
                                          content[:300].decode(errors="replace")))
            raise EdgeError(f"{status}")
        raise Fail(f"status {r.status_code}: {r.content[:200]!r}")

    async def stream_get(self, path, limit=None, rate=None):
        """Downloads, checking the pattern; stops after limit bytes (visitor leaves); returns bytes read"""
        pos = 0
        async with self.client().stream("GET", self.url + path) as r:
            if r.status_code != 200:
                await r.aread()
                self.check_status(r)
            t0 = time.monotonic()
            async for chunk in r.aiter_bytes():
                if not check_pattern(chunk, pos):
                    raise Fail(f"{path}: wrong data at {pos}")
                pos += len(chunk)
                if limit is not None and pos >= limit:
                    return pos
                if rate:
                    delay = pos / rate - (time.monotonic() - t0)
                    if delay > 0:
                        await asyncio.sleep(delay)
        return pos

    # --- operations ---

    async def op_hello(self):
        r = await self.get("/hello")
        if b"Hello from" not in r.content:
            raise Fail("hello")

    async def op_burst(self):
        n = self.rng.randint(5, 30)
        rs = await asyncio.gather(*(self.client().get(self.url + f"/burst{i}") for i in range(n)))
        for r in rs:
            self.check_status(r, (200, 503))  # 503: over the device's concurrency limit

    async def op_big(self):
        n = self.rng.choice([0, 1, 4096, 100000, 1000000, 5000000, 20000000])
        got = await self.stream_get(f"/big?size={n}")
        if got != n:
            raise Fail(f"/big: {got} of {n}")

    async def op_big_cancel(self):
        await self.stream_get("/big?size=50000000", limit=self.rng.randint(0, 3000000))

    async def op_slow_reader(self):
        n = 3000000
        got = await self.stream_get(f"/big?size={n}", rate=self.rng.choice([50000, 200000]))
        if got != n:
            raise Fail(f"slow reader: {got} of {n}")

    async def op_sink(self):
        body, h = self.rng.choice(self.bodies)
        if self.rng.random() < 0.5:
            content = body
        else:
            async def gen():  # no Content-Length: chunked
                for i in range(0, len(body), 65536):
                    yield body[i:i + 65536]
            content = gen()
        r = await self.client().post(self.url + "/sink", content=content)
        self.check_status(r)
        if r.content != b"%d %016x\n" % (len(body), h):
            raise Fail(f"/sink: {r.content!r} for {len(body)}")

    async def op_upload_cancel(self):
        body, _ = self.rng.choice(self.bodies)
        stop = self.rng.randint(0, len(body))

        async def gen():
            for i in range(0, stop, 65536):
                yield body[i:min(i + 65536, stop)]
            raise ConnectionAbortedError("visitor left")  # aborts the request mid-body
        async with self.fresh() as c:
            try:
                await c.post(self.url + "/sink", content=gen())
            except (ConnectionAbortedError, httpx.HTTPError):
                pass

    async def op_upload_limit(self):
        r = await self.client().post(self.url + "/upload", content=b"u" * 20000)
        self.check_status(r, (413,))

    async def op_echo(self):
        body, _ = self.rng.choice(self.bodies[:3])
        r = await self.client().post(self.url + "/echo", content=body)
        self.check_status(r)
        if r.content != body:
            raise Fail(f"/echo: {len(r.content)} bytes back of {len(body)}")

    async def op_later(self):
        ms = self.rng.randint(0, 5000)
        if self.rng.random() < 0.4:
            async with self.fresh() as c:
                try:
                    await asyncio.wait_for(c.get(self.url + f"/later?ms={ms}"), self.rng.random() * ms / 1000)
                except asyncio.TimeoutError:
                    pass
            return
        r = await self.get(f"/later?ms={ms}")
        if r.content != b"later\n":
            raise Fail("later")

    async def op_slow(self):
        n = self.rng.choice([10000, 100000])
        limit = self.rng.randint(0, n) if self.rng.random() < 0.4 else None
        got = await self.stream_get(f"/slow?size={n}&rate=1000", limit=limit)
        if limit is None and got != n:
            raise Fail(f"/slow: {got} of {n}")

    async def op_reset(self):
        n = self.rng.randint(0, 200000)
        try:
            got = await self.stream_get(f"/reset?after={n}")
        except (httpx.HTTPError, Fail):
            return  # expected: the stream is cut short
        if got > n:
            raise Fail(f"/reset: {got} > {n}")

    async def op_pause(self):
        n = self.rng.randint(1, 1000000)
        r = await self.client().post(self.url + "/pause", content=b"p" * n)
        self.check_status(r)
        if r.content != b"received %d bytes\n" % n:
            raise Fail(f"/pause: {r.content!r}")

    def ws_url(self, path):
        return "wss://" + self.host + path

    async def op_ws(self):
        try:
            await self._ws_session()
        except _Dropped:
            pass

    async def _ws_session(self):
        async with websockets.connect(self.ws_url("/ws"), max_size=None, open_timeout=30) as ws:
            for _ in range(self.rng.randint(1, 20)):
                msg = os.urandom(self.rng.randint(0, 15000))
                await ws.send(msg)
                try:
                    got = await asyncio.wait_for(ws.recv(), 30)
                except asyncio.TimeoutError:
                    self.log(f"ws stall: sent {len(msg)} bytes {msg[:4].hex()} at "
                             f"{time.strftime('%H:%M:%S', time.localtime(time.time() - 30))}, no echo")
                    raise
                if got != msg:
                    raise Fail("ws echo mismatch")
            end = self.rng.choice(["close", "message", "drop"])
            if end == "message":
                await ws.send("close")
                try:
                    await asyncio.wait_for(ws.recv(), 30)
                except websockets.ConnectionClosed:
                    pass
            elif end == "drop":
                ws.transport.abort()
                raise _Dropped()

    async def op_ws_push(self):
        n = self.rng.randint(0, 50)
        got = 0
        async with websockets.connect(self.ws_url(f"/ws/push?n={n}&ms=50"), open_timeout=30) as ws:
            try:
                async for msg in ws:
                    if msg != f"push {got}":
                        raise Fail(f"push: {msg!r} as #{got}")
                    got += 1
            except websockets.ConnectionClosedOK:
                pass
        if got != n:
            raise Fail(f"push: {got} of {n}")

    async def op_ws_idle(self):
        """A WebSocket that stays open for minutes, with an occasional message"""
        async with websockets.connect(self.ws_url("/ws"), open_timeout=30, ping_interval=20) as ws:
            end = time.monotonic() + self.rng.randint(60, 600)
            while time.monotonic() < end and not self.paused:
                await asyncio.sleep(self.rng.randint(5, 30))
                await ws.send("still here")
                if await asyncio.wait_for(ws.recv(), 30) != "still here":
                    raise Fail("idle ws echo")

    async def op_lan(self):
        kind = self.rng.choice(["big", "chunked", "close", "slow", "post", "drop", "stall", "cancel", "dead"])
        if kind in ("big", "chunked", "close"):
            n = self.rng.choice([0, 1000, 500000, 5000000])
            got = await self.stream_get(f"/lan/{kind}?size={n}")
            if got != n:
                raise Fail(f"/lan/{kind}: {got} of {n}")
        elif kind == "slow":
            got = await self.stream_get("/lan/slow?size=200000&rate=5000")
            if got != 200000:
                raise Fail(f"/lan/slow: {got}")
        elif kind == "post":
            body, _ = self.rng.choice(self.bodies)
            r = await self.client().post(self.url + "/lan/info", content=body)
            self.check_status(r)
            info = json.loads(r.content)
            if info["len"] != len(body) or info["sha256"] != hashlib.sha256(body).hexdigest():
                raise Fail(f"/lan/info: {info}")
        elif kind == "drop":
            try:
                await self.stream_get("/lan/drop")
            except (httpx.HTTPError, Fail, EdgeError):
                return
            raise Fail("/lan/drop completed")
        elif kind == "stall":
            ms = self.rng.randint(0, 8000)
            async with self.fresh() as c:
                try:
                    r = await asyncio.wait_for(c.get(self.url + f"/lan/stall?ms={ms}"), self.rng.choice([2, 30]))
                except asyncio.TimeoutError:
                    return
            self.check_status(r)
        elif kind == "cancel":
            await self.stream_get("/lan/big?size=20000000", limit=self.rng.randint(0, 2000000))
        else:
            r = await self.client().get(self.url + "/landead/x")
            if r.status_code != 502:
                self.classify_error(r.status_code, r.content, r.headers)  # shed or edge errors
                raise Fail(f"/landead: {r.status_code}")

    async def op_tcp(self):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.args.access_port)
        try:
            if self.rng.random() < 0.5:
                data = os.urandom(self.rng.randint(1, 2000000))
                writer.write(b"E")
                stop = len(data) if self.rng.random() < 0.8 else self.rng.randint(0, len(data))
                got = bytearray()

                async def send():
                    for i in range(0, stop, 65536):
                        writer.write(data[i:min(i + 65536, stop)])
                        await writer.drain()

                sender = asyncio.ensure_future(send())
                while len(got) < stop:
                    chunk = await asyncio.wait_for(reader.read(65536), 60)
                    if not chunk:
                        raise Fail(f"tcp echo closed at {len(got)} of {stop}")
                    got += chunk
                await sender
                if bytes(got) != data[:stop]:
                    raise Fail("tcp echo mismatch")
            else:
                n = self.rng.choice([1000, 1000000, 5000000])
                writer.write(b"D" + n.to_bytes(8, "big"))
                pos = 0
                limit = n if self.rng.random() < 0.8 else self.rng.randint(0, n)
                while pos < limit:
                    chunk = await asyncio.wait_for(reader.read(65536), 60)
                    if not chunk:
                        raise Fail(f"tcp download closed at {pos} of {n}")
                    if not check_pattern(chunk, pos):
                        raise Fail(f"tcp download: wrong data at {pos}")
                    pos += len(chunk)
        finally:
            writer.transport.abort()

    OPS = [
        ("op_hello", 6), ("op_burst", 3), ("op_big", 6), ("op_big_cancel", 4), ("op_slow_reader", 1),
        ("op_sink", 5), ("op_upload_cancel", 3), ("op_upload_limit", 1), ("op_echo", 3),
        ("op_later", 5), ("op_slow", 2), ("op_reset", 2), ("op_pause", 1), ("op_ws", 5),
        ("op_ws_push", 2), ("op_ws_idle", 0.3), ("op_lan", 8), ("op_tcp", 4),
    ]

    async def one(self):
        names, weights = zip(*self.OPS)
        name = self.rng.choices(names, weights)[0]
        started_in_chaos = self.in_chaos()
        self.inflight += 1
        try:
            await asyncio.wait_for(getattr(self, name)(), 900)
            self.counts[name] += 1
        except Exception as e:  # noqa: BLE001
            during = started_in_chaos or self.in_chaos()
            if isinstance(e, websockets.InvalidStatus):
                try:
                    self.classify_error(e.response.status_code, e.response.body or b"", e.response.headers)
                except (Shed, EdgeError) as ce:
                    e = ce
            if isinstance(e, Shed):
                self.counts["shed"] += 1
            elif name == "op_tcp" and isinstance(e, ConnectionResetError):
                # cloudflared access resets the local connection when the WebSocket is
                # refused (e.g. over the device's origin limit); not visible here
                self.counts["tcp_reset"] += 1
            elif isinstance(e, EdgeError):
                self.edge_codes[str(e)] += 1
                self.counts["edge_error_chaos" if during else "edge_error"] += 1
                if not during:
                    self.log(f"edge error {e} in {name}")
            elif during:
                self.counts["failed_chaos"] += 1
            else:
                self.counts["failed"] += 1
                detail = "".join(traceback.format_exception_only(type(e), e)).strip()
                self.failures.append(f"{time.strftime('%H:%M:%S')} {name}: {detail}"[:400])
                self.log(f"FAIL {name}: {detail[:300]}")
        finally:
            self.inflight -= 1

    # --- background tasks ---

    async def stats_loop(self):
        while True:
            await asyncio.sleep(30)
            s = await self.fetch_stats()
            if s:
                self.check_stats(s)

    async def fetch_stats(self):
        try:
            r = await self.clients[0].get(self.url + "/stats", timeout=30)
            s = json.loads(r.content)
        except Exception as e:  # noqa: BLE001
            self.log(f"stats unavailable: {type(e).__name__}")
            return None
        s["t"] = time.time()
        s["inflight"] = self.inflight
        self.stats_log.write(json.dumps(s) + "\n")
        self.stats_log.flush()
        return s

    def check_stats(self, s):
        if self.last_uptime is not None and s["uptime_s"] < self.last_uptime:
            self.reboots += 1
            self.failures.append(f"{time.strftime('%H:%M:%S')} device rebooted (uptime {s['uptime_s']})")
            self.log("FAIL device rebooted")
        self.last_uptime = s["uptime_s"]
        if not s.get("heap_ok", True):
            self.failures.append(f"{time.strftime('%H:%M:%S')} heap corrupted")
            self.log("FAIL heap integrity check failed")
        if s.get("posts_out_of_order"):
            self.failures.append("posts ran out of order")

    async def chaos_loop(self):
        if not self.args.chaos_interval:
            return
        while True:
            await asyncio.sleep(self.rng.uniform(0.5, 1.5) * self.args.chaos_interval)
            if self.paused:
                continue
            ms = self.rng.randint(3000, 45000)
            try:
                r = await self.clients[0].get(self.url + f"/chaos/netdown?ms={ms}", timeout=30)
                ok = r.status_code == 200
            except httpx.HTTPError:
                ok = True  # the answer may be lost as the link goes down; assume the outage started
            if ok:
                self.chaos_until = time.monotonic() + ms / 1000 + self.args.chaos_grace
                self.counts["netdowns"] += 1
                self.log(f"chaos: Ethernet down for {ms} ms")

    async def checkpoint(self):
        self.paused = True
        self.log("checkpoint: waiting for in-flight operations")
        t = time.monotonic()
        while self.inflight and time.monotonic() - t < 300:
            await asyncio.sleep(1)
        await asyncio.sleep(20)
        s = await self.fetch_stats()
        if s:
            cp = {k: s.get(k) for k in ("uptime_s", "heap_free", "alloc_blocks", "largest_block",
                                        "heap_min_free", "streams_open", "streams_total")}
            cp["left_inflight"] = self.inflight
            self.checkpoints.append(cp)
            self.log(f"checkpoint: {cp}")
        self.paused = False

    def summary(self):
        return {"counts": dict(self.counts), "edge_codes": dict(self.edge_codes), "edge_samples": self.edge_samples,
                "failures": self.failures, "reboots": self.reboots, "checkpoints": self.checkpoints}

    async def run(self):
        self.bodies = []
        for n in (0, 1, 1000, 65536, 300000, 1000000, 4000000):
            b = os.urandom(n)
            self.bodies.append((b, fnv(b)))
        limits = httpx.Limits(max_connections=20)
        timeout = httpx.Timeout(120, connect=30)
        # HTTP/1.1 towards Cloudflare: the edge multiplexes into the tunnel anyway, and
        # httpx's HTTP/2 client does not cope well with abandoned requests
        self.clients = [httpx.AsyncClient(limits=limits, timeout=timeout) for _ in range(4)]
        access = None
        if self.args.access_port:
            access = subprocess.Popen(["cloudflared", "access", "tcp", "--hostname", f"{self.host}/tcp",
                                       "--url", f"127.0.0.1:{self.args.access_port}"],
                                      stdout=subprocess.DEVNULL, stderr=open(os.path.join(self.args.out,
                                                                                         "access.log"), "a"))
        else:
            self.OPS = [o for o in self.OPS if o[0] != "op_tcp"]
        await asyncio.sleep(3)
        first = await self.fetch_stats()
        if first:
            self.check_stats(first)
            self.log(f"start: {first}")
        bg = [asyncio.ensure_future(self.stats_loop()), asyncio.ensure_future(self.chaos_loop())]
        end = time.monotonic() + self.args.duration
        next_cp = time.monotonic() + self.args.checkpoint
        next_summary = time.monotonic() + 300
        sem = asyncio.Semaphore(self.args.concurrency)
        tasks = set()

        async def worker():
            async with sem:
                await self.one()

        while time.monotonic() < end:
            if time.monotonic() >= next_cp:
                await self.checkpoint()
                next_cp = time.monotonic() + self.args.checkpoint
            if time.monotonic() >= next_summary:
                self.log(f"summary: {json.dumps(dict(self.counts))} edge {dict(self.edge_codes)}")
                next_summary = time.monotonic() + 300
            await sem.acquire()
            sem.release()
            if self.paused:
                continue
            t = asyncio.ensure_future(worker())
            tasks.add(t)
            t.add_done_callback(tasks.discard)
            await asyncio.sleep(self.rng.random() * 0.05)
        self.log("duration reached; finishing in-flight operations")
        await asyncio.gather(*tasks, return_exceptions=True)
        await self.checkpoint()
        for t in bg:
            t.cancel()
        if access:
            access.terminate()
        for c in self.clients:
            await c.aclose()
        report = self.summary()
        with open(os.path.join(self.args.out, "report.json"), "w") as f:
            json.dump(report, f, indent=1)
        self.log(f"done: {json.dumps(report['counts'])}")
        return report


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", required=True)
    p.add_argument("--duration", type=float, default=3600)
    p.add_argument("--concurrency", type=int, default=16)
    p.add_argument("--checkpoint", type=float, default=1200, help="seconds between quiet checkpoints")
    p.add_argument("--chaos-interval", type=float, default=900, help="mean seconds between outages; 0: none")
    p.add_argument("--chaos-grace", type=float, default=90, help="seconds after an outage to excuse failures")
    p.add_argument("--access-port", type=int, default=18913, help="local port for cloudflared access tcp; 0: off")
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--out", default="stress-out")
    args = p.parse_args()
    os.makedirs(args.out, exist_ok=True)
    report = asyncio.run(Load(args).run())
    sys.exit(1 if report["failures"] else 0)


if __name__ == "__main__":
    main()
