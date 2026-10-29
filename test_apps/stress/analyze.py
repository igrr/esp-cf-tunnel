"""Correlates failures in events.log (from load.py) with device events in
serial.log (memory exhaustion, lost edge connections, simulated outages, and
reboots) and prints any unexplained failures.

    python3 analyze.py OUTDIR
"""

import json
import re
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

WINDOW = 15  # seconds around a device event that it may explain a failure

DEVICE_EVENTS = [
    ("out of memory", re.compile(r"out of memory|no mem|Failed to allocate|low on memory")),
    ("edge connection lost", re.compile(r"connection to the edge lost|closing connection")),
    ("outage", re.compile(r"chaos: Ethernet")),
    ("reboot", re.compile(r"rst:0x|Rebooting")),
    ("panic", re.compile(r"Guru Meditation|abort\(\)|assert failed|CORRUPT|Stack smashing|stack overflow")),
]


def wall(ts):
    return datetime.strptime(ts, "%Y-%m-%d %H:%M:%S").timestamp()


def main(out):
    out = Path(out)
    events = (out / "events.log").read_text(errors="replace").splitlines()
    serial = re.sub(r"\x1b\[[0-9;]*m", "", (out / "serial.log").read_text(errors="replace")).splitlines()

    # Map device uptime to wall time with the stats samples
    offsets = []
    for line in (out / "stats.jsonl").read_text().splitlines():
        s = json.loads(line)
        if "uptime_s" in s:
            offsets.append(s["t"] - s["uptime_s"])
    if not offsets:
        sys.exit("no stats with uptime")
    boot = min(offsets)  # stats arrive after the device measured its uptime

    dev = []
    for line in serial:
        m = re.match(r"[IWE] \((\d+)\)", line)
        if not m:
            continue
        for kind, rx in DEVICE_EVENTS:
            if rx.search(line):
                dev.append((boot + int(m.group(1)) / 1000, kind, line.strip()))
    kinds = Counter(k for _, k, _ in dev)
    print("device events:", dict(kinds))
    for _, k, line in dev:
        if k in ("reboot", "panic"):
            print("  ", line)

    fails = [(wall(e[:19]), e[20:]) for e in events if " FAIL " in e]
    explained = Counter()
    unexplained = []
    for t, what in fails:
        near = sorted({k for dt, k, _ in dev if abs(dt - t) <= WINDOW})
        if near:
            explained[", ".join(near)] += 1
        else:
            unexplained.append((t, what))
    print(f"failures: {len(fails)}, explained by device events: {sum(explained.values())}")
    for k, n in explained.most_common():
        print(f"  {n:5}  near {k}")
    print(f"unexplained: {len(unexplained)}")
    for t, what in unexplained:
        print("  ", datetime.fromtimestamp(t).strftime("%H:%M:%S"), what[:250])

    report = json.loads((out / "report.json").read_text()) if (out / "report.json").exists() else {}
    if report:
        print("counts:", report["counts"])
        print("edge errors:", report["edge_codes"])
        print("checkpoints:")
        for cp in report["checkpoints"]:
            print("  ", cp)


if __name__ == "__main__":
    main(sys.argv[1])
