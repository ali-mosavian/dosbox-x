#!/usr/bin/env python3
"""tests/symbols/pe_symbols.py DOSBOX-X: symbols and debug symbols of a real PE program under HX's DPMI loader.

SYMPE.EXE is prog.c built by llrm-c -m32 -g (OMF) and linked by jwlink as a PE; its sections land wherever HDPMI32
maps them, in a paged address space. HX is third-party and not in the tree: HX_DOS names the directory holding
HDPMI32.EXE and DPMILD32.EXE (README.md says where to get it); the test is skipped without it.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
EXTENDED = 0x100000  # the image base is 4 MB
BUMP_LINE = 11  # int delta = twice(n);
RETURN_LINE = 13  # return counter;


def run(dosbox: str, hx: Path) -> None:
    loader = [hx / "HDPMI32.EXE", hx / "DPMILD32.EXE"]
    s = Session(dosbox, FIXTURES, loader + ["SYMPE.EXE", "SYMPE.MAP"], "DPMILD32.EXE SYMPE.EXE")
    try:
        s.send({"cmd": "continue"})
        check("pe_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "bump"})
        check("pe_sym_names_the_function", sym.get("status") == "ok" and sym.get("segment") == 1, sym)
        check("pe_the_function_is_where_the_cpu_runs_it", int(sym.get("linear", "0"), 16) > EXTENDED, sym)

        bp = s.send({"cmd": "bp_set", "location": "bump"})
        check("pe_bp_set_on_the_function", bp.get("status") == "ok" and bp.get("linear") == sym.get("linear"), bp)
        later = s.send({"cmd": "bp_set", "location": f"prog.c:{RETURN_LINE}"})
        check("pe_bp_set_on_a_line", later.get("status") == "ok", later)
        s.type(["x"])
        check("pe_bp_fires_when_the_function_is_called", s.wait_stopped(30))

        here = s.send({"cmd": "where"})
        check("pe_where_names_the_function_at_eip", here.get("symbol") == "bump" and here.get("delta") == 0, here)
        check("pe_where_gives_the_source_line", here.get("file") == "prog.c" and here.get("line") == BUMP_LINE, here)

        lines = s.send({"cmd": "line_list", "file": "prog.c"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check("pe_line_list_has_the_body_line", listed.get(RETURN_LINE, 0) > EXTENDED, lines)

        s.events.clear()
        s.send({"cmd": "continue"})
        check("pe_bp_fires_on_the_line", s.wait_stopped(30))
        found = s.send({"cmd": "locals"}).get("locals", [])
        values = {v["name"]: v.get("value") for v in found}
        if found:
            check("pe_locals_read_the_frame", values.get("n") == 5 and values.get("delta") == 10, values)
        else:
            # -m32 functions without EBP keep locals ESP-relative; llrm does not emit S_REGREL32 yet (llrm #1343)
            skip("pe_locals_read_the_frame", "the image has no local records for prog.c yet")
        counter = s.send({"cmd": "var", "name": "counter"})
        check("pe_var_reads_the_global", counter.get("value") == 11, counter)
    finally:
        s.close()


hx = os.environ.get("HX_DOS", "")
if hx and (Path(hx) / "DPMILD32.EXE").is_file():
    run(sys.argv[1], Path(hx))
else:
    skip("pe_*", "HX_DOS does not name a directory with HDPMI32.EXE and DPMILD32.EXE")
finish()
