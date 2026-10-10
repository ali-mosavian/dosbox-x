#!/usr/bin/env python3
"""tests/symbols/virtual_time.py DOSBOX-X: virtual time, guest time that follows what the guest runs.

VTBUSY.COM reads the BIOS tick, runs 3 million loop iterations, reads it again and leaves the difference in video
memory. At a fixed cycle count per emulated millisecond with the host throttle off the difference is the same on every
run; before, at cycles=max, it followed how busy the host was. VTWAIT.COM waits two seconds of DOS time by asking
for it again and again: with idle skipping the waiting costs fewer instructions.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


def busy(dosbox: str, cycles: int) -> tuple[int, float]:
    """The ticks VTBUSY counted, and the host seconds it took, with the clock virtual from the first instruction."""
    s = Session(dosbox, FIXTURES, ["VTBUSY.COM"], "VTBUSY.COM",
                cpu=f"virtual time=true\nvirtual time rate={cycles}\nvirtual idle skip=false")
    try:
        if s.send({"cmd": "virtual_time_status"}).get("cycles_per_ms") != cycles:
            return -1, 0.0
        started = time.monotonic()
        s.send({"cmd": "continue"})
        for _ in range(600):
            if s.send({"cmd": "mem_read", "addr": 0xB8F02, "len": 1})["data"] == "55":
                break
            time.sleep(0.05)
        ticks = int(s.send({"cmd": "mem_read", "addr": 0xB8F00, "len": 1})["data"], 16)
        return ticks, time.monotonic() - started
    finally:
        s.close()


def waiting(dosbox: str, skip: bool) -> dict:
    s = Session(dosbox, FIXTURES, ["VTWAIT.COM"], "VTWAIT.COM",
                cpu=f"virtual time=true\nvirtual time rate=25000\nvirtual idle skip={'true' if skip else 'false'}")
    try:
        s.send({"cmd": "continue"})
        for _ in range(600):
            if s.send({"cmd": "mem_read", "addr": 0xB8F10, "len": 1})["data"] == "44":
                break
            time.sleep(0.1)
        return s.send({"cmd": "virtual_time_status"})
    finally:
        s.close()


def run(dosbox: str) -> None:
    results = [busy(dosbox, 2000) for _ in range(3)]
    ticks = [t for t, _ in results]
    check("vt_the_command_exists", ticks[0] != -1, ticks)
    check("vt_the_same_ticks_pass_on_every_run", len(set(ticks)) == 1 and ticks[0] > 0, ticks)
    # 3 million iterations of two instructions at 2000 per millisecond: 3000 ms, 55 ticks of 54.9 ms
    check("vt_the_ticks_are_the_cycles_over_the_rate", 54 <= ticks[0] <= 57, ticks)

    s = Session(dosbox, FIXTURES, ["VTWAIT.COM"], "VTWAIT.COM", cpu="virtual time=true")
    try:
        starts = [s.send({"cmd": "virtual_time_status"}).get("emulated_us")]
    finally:
        s.close()
    s = Session(dosbox, FIXTURES, ["VTWAIT.COM"], "VTWAIT.COM", cpu="virtual time=true")
    try:
        starts.append(s.send({"cmd": "virtual_time_status"}).get("emulated_us"))
    finally:
        s.close()
    check("vt_the_guest_waits_for_a_client_at_time_zero", starts[0] == starts[1] and abs(starts[0]) < 5000, starts)

    fast = busy(dosbox, 20000)
    check("vt_a_faster_guest_cpu_counts_fewer_ticks_for_the_same_work", 0 < fast[0] < ticks[0] // 5, (fast, ticks))

    plain = waiting(dosbox, False)
    skipping = waiting(dosbox, True)
    check("vt_without_skipping_nothing_is_skipped", plain.get("skipped_ms") == 0, plain)
    check("vt_a_wait_on_the_clock_is_skipped", skipping.get("skipped_ms", 0) > 100, skipping)
    check("vt_and_costs_fewer_clock_reads", skipping.get("clock_reads", 1 << 30) < plain.get("clock_reads", 0), (plain, skipping))


run(sys.argv[1])
finish()
