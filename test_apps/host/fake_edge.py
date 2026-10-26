"""Fake Cloudflare tunnel edge for host tests.

Speaks the edge side of cloudflared's HTTP/2 transport over plain TCP: it is
the HTTP/2 client, opens the control stream, answers the registration RPC
(pycapnp, i.e. the reference Cap'n Proto implementation) and sends requests
the way the edge does. Tests drive it from the pytest thread; the asyncio
loop runs in a background thread.
"""

import asyncio
import base64
import json
import socket
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import capnp
import h2.config
import h2.connection
import h2.events
import hyperframe.frame

SCHEMAS = Path(__file__).parent / "schemas"
tunnelrpc = capnp.load(str(SCHEMAS / "tunnelrpc.capnp"), imports=[str(SCHEMAS)])

UPGRADE = "cf-cloudflared-proxy-connection-upgrade"


def b64raw_decode(s):
    return base64.b64decode(s + "=" * (-len(s) % 4))


@dataclass
class Response:
    status: int = 0
    headers: dict = field(default_factory=dict)
    body: bytes = b""
    reset: int | None = None

    @property
    def user_headers(self):
        """Headers serialized in cf-cloudflared-response-headers, lower-cased names"""
        out = {}
        for pair in filter(None, self.headers.get("cf-cloudflared-response-headers", "").split(";")):
            name, value = pair.split(":")
            out[b64raw_decode(name).decode().lower()] = b64raw_decode(value).decode()
        return out


@dataclass
class Registration:
    account_tag: str
    tunnel_id: bytes
    conn_index: int
    client_id: bytes
    features: list
    version: str
    arch: str
    num_previous_attempts: int


class _Stream:
    def __init__(self):
        self.resp = Response()
        self.headers_received = asyncio.Event()
        self.done = asyncio.Event()
        self.chunks = asyncio.Queue()


class EdgeConn:
    """One connection from the connector"""

    def __init__(self, edge, reader, writer):
        self.edge = edge
        self.reader = reader
        self.writer = writer
        self.h2 = h2.connection.H2Connection(h2.config.H2Configuration(client_side=True, header_encoding="utf-8"))
        self.streams = {}
        self.pending = {}  # stream id -> [unsent data, end_stream]
        self.registration = None
        self.registered = asyncio.Event()
        self.unregistered = asyncio.Event()
        self.closed = asyncio.Event()
        self.silent = False
        self.control_sock = None
        self.opened_at = time.monotonic()
        self.closed_at = None

    def flush(self):
        data = self.h2.data_to_send()
        if data and not self.writer.is_closing():
            self.writer.write(data)

    async def run(self):
        self.h2.initiate_connection()
        self.flush()
        await self._start_control_stream()
        try:
            while True:
                data = await self.reader.read(65536)
                if not data:
                    break
                if self.silent:
                    continue
                for ev in self.h2.receive_data(data):
                    self._on_event(ev)
                self.flush()
        except (ConnectionError, h2.exceptions.ProtocolError):
            pass
        finally:
            self.closed_at = time.monotonic()
            self.closed.set()
            for s in self.streams.values():
                s.done.set()
            if getattr(self, "pump_task", None):
                self.pump_task.cancel()
            if getattr(self, "rpc_writer", None):
                self.rpc_writer.close()  # also closes the socket
            elif self.control_sock:
                self.control_sock.close()
            self.writer.close()

    async def _start_control_stream(self):
        """Stream 1 carries the registration RPC; bridge it to pycapnp through a socketpair."""
        ours, theirs = socket.socketpair()
        self.control_sock = ours
        sid = self.h2.get_next_available_stream_id()
        self.control_id = sid
        self.h2.send_headers(sid, [(":method", "GET"), (":scheme", "https"),
                                   (":authority", "control-stream.cloudflared.com"), (":path", "/"),
                                   (UPGRADE, "control-stream")])
        self.flush()
        self.streams[sid] = _Stream()
        stream = await capnp.AsyncIoStream.create_connection(sock=theirs)
        self.rpc_server = capnp.TwoPartyServer(stream, bootstrap=_RegistrationServer(self))
        rpc_reader, self.rpc_writer = await asyncio.open_connection(sock=ours)

        async def pump():
            while True:
                data = await rpc_reader.read(65536)
                if not data:
                    return
                self.h2.send_data(sid, data)
                self.flush()

        self.pump_task = asyncio.ensure_future(pump())

    def _on_event(self, ev):
        if isinstance(ev, h2.events.ResponseReceived):
            s = self.streams.get(ev.stream_id)
            if s:
                s.resp.headers = dict(ev.headers)
                s.resp.status = int(s.resp.headers[":status"])
                s.headers_received.set()
        elif isinstance(ev, h2.events.DataReceived):
            self.h2.acknowledge_received_data(ev.flow_controlled_length, ev.stream_id)
            if ev.stream_id == self.control_id:
                self.rpc_writer.write(ev.data)
            elif ev.stream_id in self.streams:
                s = self.streams[ev.stream_id]
                s.resp.body += ev.data
                s.chunks.put_nowait(ev.data)
        elif isinstance(ev, h2.events.StreamEnded):
            if ev.stream_id in self.streams:
                self.streams[ev.stream_id].done.set()
        elif isinstance(ev, h2.events.StreamReset):
            if ev.stream_id in self.streams:
                self.streams[ev.stream_id].resp.reset = ev.error_code
                self.streams[ev.stream_id].headers_received.set()
                self.streams[ev.stream_id].done.set()
        elif isinstance(ev, h2.events.WindowUpdated):
            self._send_pending()
        elif isinstance(ev, h2.events.ConnectionTerminated):
            self.writer.close()

    # --- actions (run on the loop) ---

    def open_stream(self, method, path, headers=(), body=None, authority="test.example.com", end_stream=True):
        sid = self.h2.get_next_available_stream_id()
        hdrs = [(":method", method), (":scheme", "https"), (":authority", authority), (":path", path)]
        hdrs += list(headers)
        self.streams[sid] = _Stream()
        self.h2.send_headers(sid, hdrs, end_stream=end_stream and not body)
        if body:
            self.send_body(sid, body, end_stream)
        self.flush()
        return sid

    def send_body(self, sid, data, end_stream=False):
        """Sends what flow control allows now; the rest follows as the window opens."""
        self.pending.setdefault(sid, [b"", False])
        self.pending[sid][0] += data
        self.pending[sid][1] |= end_stream
        self._send_pending()

    def _send_pending(self):
        for sid in list(self.pending):
            data, end = self.pending[sid]
            ended = False
            while data:
                n = min(len(data), self.h2.max_outbound_frame_size, self.h2.local_flow_control_window(sid))
                if n <= 0:
                    break
                ended = end and n == len(data)
                self.h2.send_data(sid, data[:n], end_stream=ended)
                data = data[n:]
            if data:
                self.pending[sid][0] = data
                continue
            if end and not ended:
                self.h2.end_stream(sid)
            del self.pending[sid]
        self.flush()

    async def request(self, method, path, headers=(), body=None, authority="test.example.com"):
        sid = self.open_stream(method, path, headers, body, authority)
        await self.streams[sid].done.wait()
        return self.streams.pop(sid).resp

    def drain(self):
        """GOAWAY as the edge sends it: last_stream_id is one of its own (odd) stream IDs.
        Written directly: h2 would refuse further frames after its own GOAWAY, but the
        edge keeps serving the connection (the unregister RPC comes after it)."""
        self.flush()
        frame = hyperframe.frame.GoAwayFrame(0, last_stream_id=max(self.streams) if self.streams else 1)
        self.writer.write(frame.serialize())

    def abort(self):
        self.writer.transport.abort()


class _RegistrationServer(tunnelrpc.RegistrationServer.Server):
    def __init__(self, conn):
        self.conn = conn

    async def registerConnection(self, auth, tunnelId, connIndex, options, _context, **kwargs):
        c = options.client
        self.conn.registration = Registration(
            account_tag=auth.accountTag, tunnel_id=bytes(tunnelId), conn_index=connIndex,
            client_id=bytes(c.clientId), features=list(c.features), version=c.version, arch=c.arch,
            num_previous_attempts=options.numPreviousAttempts)
        details = _context.results.init("result").result.init("connectionDetails")
        details.uuid = bytes(range(16))
        details.locationName = "fake01"
        details.tunnelIsRemotelyManaged = True
        self.conn.registered.set()

    async def unregisterConnection(self, _context, **kwargs):
        # The real edge sends GOAWAY before answering
        self.conn.drain()
        self.conn.unregistered.set()

    async def updateLocalConfiguration(self, config, _context, **kwargs):
        pass


class FakeEdge:
    def __init__(self):
        self.conns = []
        self._new_conn = None
        self.loop = None
        ready = threading.Event()
        self.thread = threading.Thread(target=self._thread, args=(ready,), daemon=True)
        self.thread.start()
        ready.wait()

    def _thread(self, ready):
        async def main():
            self.loop = asyncio.get_running_loop()
            self._new_conn = asyncio.Condition()
            self.server = await asyncio.start_server(self._on_client, "127.0.0.1", 0)
            self.port = self.server.sockets[0].getsockname()[1]
            self._stop = asyncio.Event()
            ready.set()
            await self._stop.wait()
            self.server.close()

        asyncio.run(capnp.run(main()))

    async def _on_client(self, reader, writer):
        conn = EdgeConn(self, reader, writer)
        self.conns.append(conn)
        async with self._new_conn:
            self._new_conn.notify_all()
        await conn.run()

    def call(self, coro, timeout=30):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def call_soon(self, fn, *args):
        async def wrapper():
            return fn(*args)
        return self.call(wrapper())

    def stop(self):
        self.loop.call_soon_threadsafe(self._stop.set)
        self.thread.join(5)

    # --- helpers for tests ---

    def wait_registered(self, count=1, timeout=15):
        """Waits until `count` connections are registered; returns them."""
        async def wait():
            while True:
                live = [c for c in self.conns if c.registered.is_set() and not c.closed.is_set()]
                if len(live) >= count:
                    return live
                async with self._new_conn:
                    try:
                        await asyncio.wait_for(self._new_conn.wait(), 0.1)
                    except asyncio.TimeoutError:
                        pass
        return self.call(asyncio.wait_for(wait(), timeout), timeout + 1)

    def wait(self, event, timeout=15):
        return self.call(asyncio.wait_for(event.wait(), timeout), timeout + 1)

    def request(self, conn, method, path, headers=(), body=None, timeout=15):
        return self.call(asyncio.wait_for(conn.request(method, path, headers, body), timeout), timeout + 1)

    def push_config(self, conn, version, config):
        body = json.dumps({"version": version, "config": config}).encode()
        resp = self.request(conn, "PUT", "/", [(UPGRADE, "update-configuration")], body)
        return resp, json.loads(resp.body)
