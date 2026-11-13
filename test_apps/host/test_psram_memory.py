"""Unit tests of the data and nghttp2 allocation policies."""

import subprocess

import pytest

from conftest import BUILD


@pytest.mark.parametrize("binary", ["psram_memory_test", "psram_memory_stats_test",
                                   "psram_prefer_memory_test", "psram_prefer_memory_stats_test",
                                   "alloc_memory_test", "alloc_internal_memory_test"])
def test_data_allocator(binary):
    p = subprocess.run([str(BUILD / binary)], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0 and "Sanitizer" not in p.stderr, p.stdout + p.stderr
