#!/usr/bin/env python3
"""tests/symbols/dwarf_symbols.py DOSBOX-X: DWARF in a real LE and a real PE image, with no .MAP beside them.

prog.c (llrm-c -m32 -g, OMF) and start.asm (jwasm -Zi) are linked by jwlink with `debug dwarf`, which appends an
ELF block: DWARF 2, lines and the publics (see dwarf/build.sh). SYMLED.EXE runs under the emulator's DOS/32A;
SYMPED.EXE under HX's DPMILD32 (HX_DOS, see README.md), and is skipped without it.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
EXTENDED = 0x100000
BUMP_LINE = 11  # int delta = twice(n);
CALL_LINE = 15  # call run_ in start.asm


def run(dosbox: str, tag: str, files: list, command: str) -> None:
    s = Session(dosbox, FIXTURES, files, command)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "bump_"})
        check(f"{tag}_sym_names_the_function_from_dwarf", sym.get("status") == "ok" and sym.get("source") == "dwarf", sym)
        check(f"{tag}_the_function_is_where_the_cpu_runs_it", int(sym.get("linear", "0"), 16) > EXTENDED, sym)

        lines = s.send({"cmd": "line_list", "file": "prog.c"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check(f"{tag}_line_list_is_in_the_image", listed.get(BUMP_LINE) == int(sym.get("linear", "0"), 16), lines)

        s.send({"cmd": "bp_set", "location": f"start.asm:{CALL_LINE}"})
        s.send({"cmd": "bp_set", "location": "bump_"})
        s.type(["x"])
        check(f"{tag}_bp_fires_on_the_line", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_where_gives_the_source_line", here.get("file") == "start.asm" and here.get("line") == CALL_LINE, here)

        s.events.clear()
        s.send({"cmd": "continue"})
        check(f"{tag}_bp_fires_in_the_function", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_where_names_function_and_line", here.get("symbol") == "bump_" and here.get("file") == "prog.c" and
              here.get("line") == BUMP_LINE and here.get("source") == "dwarf", here)

        counter = s.send({"cmd": "var", "name": "_counter", "len": 2})
        check(f"{tag}_var_reads_the_global", counter.get("bytes") == "0100", counter)
    finally:
        s.close()


run(sys.argv[1], "led", ["SYMLED.EXE"], "SYMLED.EXE")

hx = Path(os.environ.get("HX_DOS", "/nonexistent"))
if (hx / "DPMILD32.EXE").is_file():
    run(sys.argv[1], "ped", [hx / "HDPMI32.EXE", hx / "DPMILD32.EXE", "SYMPED.EXE"], "DPMILD32.EXE SYMPED.EXE")
else:
    skip("ped_*", "HX_DOS does not name a directory with HDPMI32.EXE and DPMILD32.EXE")
finish()
