#!/usr/bin/env python3
"""tests/symbols/ne_symbols.py DOSBOX-X: symbols and debug symbols of a real NE program under HX's DPMI loader.

SYMNE.EXE is prog.c built by llrm-c -m16 -g and linked by jwlink as an NE; its segments land wherever HDPMI16
maps them, in a paged address space. HX is third-party and not in the tree: HX_DOS names the directory holding
HDPMI16.EXE and DPMILD16.EXE (README.md says where to get it); the test is skipped without it.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
EXTENDED = 0x100000  # SYMNE's code is mapped above 1 MB
BUMP_LINE = 11  # int delta = twice(n);
RETURN_LINE = 13  # return counter;


def run(dosbox: str, hx: Path) -> None:
    loader = [hx / "HDPMI16.EXE", hx / "DPMILD16.EXE"]
    s = Session(dosbox, FIXTURES, loader + ["SYMNE.EXE", "SYMNE.MAP"], "DPMILD16.EXE SYMNE.EXE")
    try:
        s.send({"cmd": "continue"})
        check("ne_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "bump"})
        check("ne_sym_names_the_function", sym.get("status") == "ok" and sym.get("segment") == 1, sym)
        check("ne_the_function_is_where_the_cpu_runs_it", int(sym.get("linear", "0"), 16) > EXTENDED, sym)

        bp = s.send({"cmd": "bp_set", "location": "bump"})
        check("ne_bp_set_on_the_function", bp.get("status") == "ok" and bp.get("linear") == sym.get("linear"), bp)
        later = s.send({"cmd": "bp_set", "location": f"prog.c:{RETURN_LINE}"})
        check("ne_bp_set_on_a_line", later.get("status") == "ok", later)
        s.type(["x"])
        check("ne_bp_fires_when_the_function_is_called", s.wait_stopped(30))

        here = s.send({"cmd": "where"})
        check("ne_where_names_the_function_at_eip", here.get("symbol") == "bump" and here.get("delta") == 0, here)
        check("ne_where_gives_the_source_line", here.get("file") == "prog.c" and here.get("line") == BUMP_LINE, here)

        lines = s.send({"cmd": "line_list", "file": "prog.c"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check("ne_line_list_has_the_body_line", listed.get(RETURN_LINE, 0) > EXTENDED, lines)

        s.events.clear()
        s.send({"cmd": "continue"})
        check("ne_bp_fires_on_the_line", s.wait_stopped(30))
        values = {v["name"]: v.get("value") for v in s.send({"cmd": "locals"}).get("locals", [])}
        check("ne_locals_read_the_frame", values.get("n") == 5 and values.get("delta") == 10, values)
        counter = s.send({"cmd": "var", "name": "counter"})
        check("ne_var_reads_the_global", counter.get("value") == 11, counter)
    finally:
        s.close()


hx = os.environ.get("HX_DOS", "")
if hx and (Path(hx) / "DPMILD16.EXE").is_file():
    run(sys.argv[1], Path(hx))
else:
    skip("ne_*", "HX_DOS does not name a directory with HDPMI16.EXE and DPMILD16.EXE")
finish()
