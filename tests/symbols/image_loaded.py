#!/usr/bin/env python3
"""tests/symbols/image_loaded.py DOSBOX-X: the guest stops where a loaded image starts, and a symbol breakpoint is set.

A protected-mode loader is guest code: nothing says when it is done placing an image. bp_on_image_load makes the
emulator stop the guest at the entry of the image, with an image_loaded event naming where each object went, and
bp_on_symbol_load sets a breakpoint at a function the moment its image is placed, before the program runs on.
An LE under DOS/32A; a PE and a 16-bit NE under HX (HX_DOS).
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


def run(dosbox: str, tag: str, files: list, command: str, function: str) -> None:
    s = Session(dosbox, FIXTURES, files, command)
    try:
        check(f"{tag}_arms_before_the_program_runs", s.send({"cmd": "bp_on_image_load"}).get("status") == "ok")
        check(f"{tag}_arms_a_symbol_breakpoint_before_its_symbols_exist",
              s.send({"cmd": "bp_on_symbol_load", "location": function}).get("status") == "ok")
        s.send({"cmd": "continue"})

        event = s.wait_event("image_loaded", 60)
        check(f"{tag}_image_loaded_event_arrives", event is not None)
        if event is None:
            return
        check(f"{tag}_it_says_the_guest_is_stopped_at_the_entry", event.get("stopped") is True and "entry" in event, event)
        check(f"{tag}_it_names_where_the_objects_went", len(event.get("objects", [])) >= 1, event)
        check(f"{tag}_it_names_the_breakpoint_it_set", [b.get("location") for b in event.get("breakpoints", [])] == [function], event)

        where = s.send({"cmd": "where"})
        check(f"{tag}_the_guest_is_at_the_entry", where.get("linear") == event.get("entry"), (where, event.get("entry")))
        sym = s.send({"cmd": "sym", "name": function})
        check(f"{tag}_symbols_are_placed_by_then", sym.get("status") == "ok", sym)

        s.send({"cmd": "continue"})
        check(f"{tag}_program_runs_on", s.wait_for("ready", 60), s.screen())
        s.type(["x"])
        check(f"{tag}_the_symbol_breakpoint_stops_it", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_at_the_function", here.get("symbol") == function and here.get("delta") == 0, here)
    finally:
        s.close()


run(sys.argv[1], "led", ["SYMLED.EXE"], "SYMLED.EXE", "bump_")

hx = Path(os.environ.get("HX_DOS", "/nonexistent"))
if (hx / "DPMILD32.EXE").is_file() and (hx / "DPMILD16.EXE").is_file():
    run(sys.argv[1], "ped", [hx / "HDPMI32.EXE", hx / "DPMILD32.EXE", "SYMPED.EXE"], "DPMILD32.EXE SYMPED.EXE", "bump_")
    run(sys.argv[1], "ned", [hx / "HDPMI16.EXE", hx / "DPMILD16.EXE", "SYMNED.EXE"], "DPMILD16.EXE SYMNED.EXE", "_bump@3")
else:
    skip("ped_* ned_*", "HX_DOS does not name a directory with the HX loaders")
finish()
