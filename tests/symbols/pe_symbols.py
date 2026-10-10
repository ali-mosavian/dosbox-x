#!/usr/bin/env python3
"""tests/symbols/pe_symbols.py DOSBOX-X: symbols and debug symbols of a real PE program under HX's DPMI loader.

SYMPE.EXE is prog.c built by llrm-c -m32 and start.asm by jwasm -Zi, linked by jwlink as a PE; its sections land
wherever HDPMI32 maps them, in a paged address space. HX is third-party and not in the tree: HX_DOS names the
directory holding HDPMI32.EXE and DPMILD32.EXE (README.md says where to get it); the test is skipped without it.

jwlink carries start.asm's lines and every public into the image, but not the CodeView of llrm's COFF object:
prog.c has no lines and no locals here, so those are asserted on start.asm and skipped for prog.c.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
EXTENDED = 0x100000  # the image base is 4 MB
CALL_LINE = 14  # call run_ in start.asm


def run(dosbox: str, hx: Path) -> None:
    loader = [hx / "HDPMI32.EXE", hx / "DPMILD32.EXE"]
    s = Session(dosbox, FIXTURES, loader + ["SYMPE.EXE", "SYMPE.MAP"], "DPMILD32.EXE SYMPE.EXE")
    try:
        s.send({"cmd": "continue"})
        check("pe_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "bump_"})
        check("pe_sym_names_the_function", sym.get("status") == "ok" and sym.get("segment") == 1, sym)
        check("pe_the_function_is_where_the_cpu_runs_it", int(sym.get("linear", "0"), 16) > EXTENDED, sym)

        bp = s.send({"cmd": "bp_set", "location": "bump_"})
        check("pe_bp_set_on_the_function", bp.get("status") == "ok" and bp.get("linear") == sym.get("linear"), bp)
        call = s.send({"cmd": "bp_set", "location": f"start.asm:{CALL_LINE}"})
        check("pe_bp_set_on_a_line", call.get("status") == "ok", call)

        lines = s.send({"cmd": "line_list", "file": "start.asm"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check("pe_line_list_is_in_the_image", listed.get(CALL_LINE, 0) > EXTENDED, lines)

        s.type(["x"])
        check("pe_bp_fires_on_the_line", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check("pe_where_gives_the_source_line", here.get("file") == "start.asm" and here.get("line") == CALL_LINE, here)

        s.events.clear()
        s.send({"cmd": "continue"})
        check("pe_bp_fires_when_the_function_is_called", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check("pe_where_names_the_function_at_eip", here.get("symbol") == "bump_" and here.get("delta") == 0, here)

        counter = s.send({"cmd": "var", "name": "_counter", "len": 2})
        check("pe_var_reads_the_global", counter.get("bytes") == "0100", counter)
        skip("pe_locals", "jwlink drops the CodeView of llrm's COFF objects, so prog.c has no locals in the image")
    finally:
        s.close()


hx = os.environ.get("HX_DOS", "")
if hx and (Path(hx) / "DPMILD32.EXE").is_file():
    run(sys.argv[1], Path(hx))
else:
    skip("pe_*", "HX_DOS does not name a directory with HDPMI32.EXE and DPMILD32.EXE")
finish()
