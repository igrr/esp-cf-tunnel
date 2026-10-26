"""Host tests: build directory from $CFT_HOST_BUILD (default: ./build).

Run: pytest test_apps/host (needs pycapnp for the RPC test)
"""

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

HERE = Path(__file__).parent
BUILD = Path(os.environ.get("CFT_HOST_BUILD", HERE / "build"))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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
