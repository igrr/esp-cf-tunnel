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
            p.kill()
            p.wait()
        log = Path(p.log_path).read_text()
        assert "ERROR: AddressSanitizer" not in log and "runtime error" not in log, log


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
    assert int(resp.user_headers["content-length"]) == len(resp.body)
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


def test_body_limit(edge, connector):
    connector()
    [conn] = edge.wait_registered()
    assert edge.request(conn, "POST", "/", body=b"x" * 10000).status == 200
    assert edge.request(conn, "POST", "/", body=b"x" * 20000).status == 413


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
