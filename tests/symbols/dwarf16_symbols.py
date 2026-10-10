#!/usr/bin/env python3
"""tests/symbols/dwarf16_symbols.py DOSBOX-X: DWARF in a real 16-bit MZ and a real 16-bit NE, with no .MAP beside them.

prog.c (llrm-c -m16 -g, OMF) and a start file (jwasm -Zi) are linked by jwlink with `debug dwarf` (dwarf16/build.sh).
An address in a segmented image is an offset in a segment: for an MZ, read through the load segment the way CV and
TD are, with DW_AT_segment a frame paragraph; for an NE, through the segment's selector. SYMMZD.EXE runs under the
emulator's DOS; SYMNED.EXE under HX's DPMILD16 (HX_DOS, see README.md), and is skipped without it.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
BUMP_LINE = 11  # int delta = twice(n);
RETURN_LINE = 13  # return counter;


def run(dosbox: str, tag: str, files: list, command: str) -> None:
    s = Session(dosbox, FIXTURES, files, command)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "_bump@3"})
        check(f"{tag}_sym_names_the_function_from_dwarf", sym.get("status") == "ok" and sym.get("source") == "dwarf", sym)

        lines = s.send({"cmd": "line_list", "file": "prog.c"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check(f"{tag}_line_list_starts_the_function_at_its_symbol", listed.get(BUMP_LINE) == int(sym.get("linear", "0"), 16), lines)

        s.send({"cmd": "bp_set", "location": "_bump@3"})
        s.type(["x"])
        check(f"{tag}_bp_fires_in_the_function", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_where_names_function_and_line", here.get("symbol") == "_bump@3" and here.get("file") == "prog.c" and
              here.get("line") == BUMP_LINE and here.get("source") == "dwarf", here)

        counter = s.send({"cmd": "var", "name": "_counter", "len": 2})
        check(f"{tag}_var_reads_the_global_through_its_segment", counter.get("bytes") == "0100", counter)
    finally:
        s.close()


run(sys.argv[1], "mzd", ["SYMMZD.EXE"], "SYMMZD.EXE")

hx = Path(os.environ.get("HX_DOS", "/nonexistent"))
if (hx / "DPMILD16.EXE").is_file():
    run(sys.argv[1], "ned", [hx / "HDPMI16.EXE", hx / "DPMILD16.EXE", "SYMNED.EXE"], "DPMILD16.EXE SYMNED.EXE")
else:
    skip("ned_*", "HX_DOS does not name a directory with HDPMI16.EXE and DPMILD16.EXE")
finish()
