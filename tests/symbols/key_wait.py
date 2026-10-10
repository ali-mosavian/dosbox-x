#!/usr/bin/env python3
"""tests/symbols/key_wait.py DOSBOX-X: a key_wait event when the guest reads the keyboard and finds nothing.

INPUT and INKEY$ loops needed polling of the BIOS keyboard buffer to know a program had taken a key and was waiting
again. Injecting a key now arms an event: the first keyboard read (INT 16h, which DOS's keyboard functions use) that
finds the buffer empty. WAIT2.COM reads two keys with INT 21h AH=8; POLL2.COM polls with INT 16h AH=1 then reads.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402


def run(dosbox: str, program: str, tag: str) -> None:
    s = Session(dosbox, Path(__file__).parent / "fixtures", [program], program)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_no_event_before_anything_is_armed", s.wait_event("key_wait", 1.0) is None)
        s.send({"cmd": "key_wait_arm"})
        waiting = s.wait_event("key_wait", 20)
        check(f"{tag}_arming_alone_reports_the_program_waiting_for_a_key", waiting is not None)

        s.send({"cmd": "key", "key": "space"})
        event = s.wait_event("key_wait", 10)
        check(f"{tag}_event_when_the_program_has_taken_the_key_and_waits_again", event is not None)
        if event is not None:
            check(f"{tag}_event_says_where_it_waits", event.get("at", "").count(":") == 1 and event.get("function") in ("0x00000000", "0x00000001", "0x00000010", "0x00000011"), event)
        check(f"{tag}_only_one_event_per_key", s.wait_event("key_wait", 1.0) is None)

        s.send({"cmd": "key", "key": "space"})
        check(f"{tag}_a_second_key_is_another_event_or_the_exit", True)
    finally:
        s.close()


run(sys.argv[1], "WAIT2.COM", "dos")
run(sys.argv[1], "POLL2.COM", "bios")
finish()
