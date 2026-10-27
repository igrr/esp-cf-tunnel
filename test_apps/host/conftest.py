"""Fixtures for the host tests: the fake edge and the connector (cft_host)."""

import base64
import json
import os
import signal
import socket
import subprocess
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

    def start(*args, env=None):
        log = open(tmp_path / f"connector{len(procs)}.log", "w")
        p = subprocess.Popen([str(BUILD / "cft_host"), "--token-file", str(token_file), "--plain",
                              "--edge", f"127.0.0.1:{edge.port}", "-v", *args],
                             stdout=log, stderr=subprocess.STDOUT, env={**os.environ, **(env or {})})
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
