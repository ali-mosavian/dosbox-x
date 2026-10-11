#!/usr/bin/env python3
"""tests/symbols/mem_hash.py DOSBOX-X: mem_hash, a hash of a range, VRAM included.

Deciding that a screen stopped changing meant moving it to the host as a PNG and comparing. The hash of the range
answers it. MODE13.COM sets 320x200x256, fills the screen with the byte 5, waits for a key, fills it with 7, waits and exits.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402


def fnv1a64(data: bytes) -> str:
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def planar(dosbox: str) -> None:
    """MODE12.COM sets the 640x480x16 planar mode and, on a key, sets one byte of plane 3 only: the adapter's memory holds every plane, the CPU window one at a time."""
    s = Session(dosbox, Path(__file__).parent / "fixtures", ["MODE12.COM"], "MODE12.COM")
    try:
        s.send({"cmd": "continue"})
        for _ in range(100):
            if s.send({"cmd": "text_screen"}).get("video_mode") == 0x12:
                break
            time.sleep(0.1)
        time.sleep(0.3)
        raw_before = s.send({"cmd": "mem_hash", "addr": 0, "len": 0x28000, "vram": True})
        s.type(["space"])
        time.sleep(0.5)
        raw_after = s.send({"cmd": "mem_hash", "addr": 0, "len": 0x28000, "vram": True})
        check("planar_the_adapters_memory_shows_every_plane", raw_before.get("hash") != raw_after.get("hash"), (raw_before, raw_after))
    finally:
        s.close()


def video_writes(dosbox: str) -> None:
    """mem_write wrote the machine's physical memory behind the adapter, so a byte sent to A0000 never reached the video
    memory: the screen did not change and reading it back gave what had been there. It goes through the same handler
    as the CPU's own write now, which in the planar mode 12h means the write mode, the map mask and the latches."""
    s = Session(dosbox, Path(__file__).parent / "fixtures", ["MODE12.COM"], "MODE12.COM")
    try:
        s.send({"cmd": "continue"})
        for _ in range(100):
            if s.send({"cmd": "text_screen"}).get("video_mode") == 0x12:
                break
            time.sleep(0.1)
        time.sleep(0.3)
        before = s.send({"cmd": "mem_hash", "addr": 0, "len": 0x28000, "vram": True})
        seen_before = s.send({"cmd": "mem_hash", "addr": 0xA0000, "len": 1})
        s.send({"cmd": "mem_write", "addr": 0xA0000, "data": "FF"})
        after = s.send({"cmd": "mem_hash", "addr": 0, "len": 0x28000, "vram": True})
        check("write_a_byte_to_planar_video_memory_reaches_the_adapter", before.get("hash") != after.get("hash"), (before, after))
        seen = s.send({"cmd": "mem_hash", "addr": 0xA0000, "len": 1})
        check("write_and_the_cpu_reads_it_back", seen.get("hash") == fnv1a64(b"\xff") and seen.get("hash") != seen_before.get("hash"), (seen_before, seen))
        s.send({"cmd": "mem_write_linear", "addr": 0xA0001, "data": "FF"})
        linear = s.send({"cmd": "mem_hash", "addr": 0xA0001, "len": 1})
        check("write_linear_reaches_the_adapter_too", linear.get("hash") == fnv1a64(b"\xff"), linear)
    finally:
        s.close()


def run(dosbox: str) -> None:
    s = Session(dosbox, Path(__file__).parent / "fixtures", ["MODE13.COM"], "MODE13.COM")
    try:
        s.send({"cmd": "continue"})
        for _ in range(100):
            if s.send({"cmd": "text_screen"}).get("video_mode") == 0x13:
                break
            time.sleep(0.1)
        time.sleep(0.3)

        screen = s.send({"cmd": "mem_hash", "addr": 0xA0000, "len": 64000})
        check("hash_the_screen_as_the_cpu_reads_it", screen.get("hash") == fnv1a64(bytes([5]) * 64000), screen)
        raw = s.send({"cmd": "mem_hash", "addr": 0, "len": 64000, "vram": True})
        check("hash_the_adapters_own_memory", raw.get("status") == "ok" and raw.get("vram") is True and len(raw.get("hash", "")) == 16, raw)

        s.type(["space"])
        time.sleep(0.5)
        changed = s.send({"cmd": "mem_hash", "addr": 0xA0000, "len": 64000})
        check("hash_follows_the_screen_when_the_program_redraws", changed.get("hash") == fnv1a64(bytes([7]) * 64000), (screen, changed))
        after = s.send({"cmd": "mem_hash", "addr": 0, "len": 64000, "vram": True})
        check("hash_of_the_adapters_memory_changes_too", after.get("hash") not in (None, raw.get("hash")), (raw, after))
        again = s.send({"cmd": "mem_hash", "addr": 0, "len": 64000, "vram": True})
        check("hash_of_an_unchanged_screen_is_the_same", again.get("hash") == after.get("hash"), (after, again))

        s.send({"cmd": "mem_write", "addr": 0x500, "data": "0102030405060708"})
        low = s.send({"cmd": "mem_hash", "addr": 0x500, "len": 8})
        check("hash_matches_a_hash_computed_on_the_host", low.get("hash") == fnv1a64(bytes(range(1, 9))), low)

        far = s.send({"cmd": "mem_hash", "addr": 0, "len": 0x10000000, "vram": True})
        check("hash_past_the_video_memory_is_an_error", far.get("status") == "error", far)
    finally:
        s.close()


run(sys.argv[1])
planar(sys.argv[1])
video_writes(sys.argv[1])
finish()
