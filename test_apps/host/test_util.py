"""Unit tests of the helpers in src/cft_util.c (util_test.c)."""

import subprocess

from conftest import BUILD


def test_json_strings():
    p = subprocess.run([str(BUILD / "util_test")], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0 and "Sanitizer" not in p.stderr, p.stdout + p.stderr
