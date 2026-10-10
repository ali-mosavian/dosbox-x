#!/usr/bin/env python3
"""tests/symbols/le_symbols.py DOSBOX-X: symbols and debug symbols of a real LE program under DOS/32A.

SYMPROBE.BAS is built by llrm-qb -m32 and linked by jwlink into an LE image, plain (SYMPROBE) and with -g and
`debug codeview` (SYMPCV). DOS/32A is the emulator's own Z:\\BIN copy. Before this change every question below
answered "unknown", or named the .map's addresses as if the stub's load segment held the program.
"""

import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
FUNCTION = "BUMP"
FUNCTION_LINE = 15  # SUB Bump (line 13) has code from its first statement
BODY_LINE = 16  # counter = counter + delta
EXTENDED = 0x100000


def run(dosbox: str, program: str, debug_info: bool) -> None:
    tag = program.split(".")[0].lower()
    s = Session(dosbox, FIXTURES, [program, program.replace(".EXE", ".MAP")], program)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": FUNCTION})
        check(f"{tag}_sym_names_the_function_at_its_relocated_address", sym.get("status") == "ok" and sym.get("segment") == 1, sym)
        check(f"{tag}_the_function_is_in_extended_memory", int(sym.get("linear", "0"), 16) > EXTENDED, sym)

        images = s.send({"cmd": "images"}).get("images", [])
        mine = [i for i in images if i["program"].upper().endswith(program)]
        placed = mine[0]["placed"] if mine else []
        check(f"{tag}_images_lists_the_program_placed_in_extended_memory",
              bool(mine) and mine[0]["complete"] and placed and int(placed[0]["linear"], 16) > EXTENDED, images)

        bp = s.send({"cmd": "bp_set", "location": FUNCTION})
        check(f"{tag}_bp_set_on_the_function", bp.get("status") == "ok" and bp.get("linear") == sym.get("linear"), bp)
        s.type(["x"])
        check(f"{tag}_bp_fires_when_the_function_is_called", s.wait_stopped(30))

        here = s.send({"cmd": "where"})
        check(f"{tag}_where_names_the_function_at_eip", here.get("symbol") == FUNCTION and here.get("delta") == 0, here)
        if not debug_info:
            return

        check(f"{tag}_where_gives_the_source_line", here.get("file") == "SYMPROBE.BAS" and here.get("line") == FUNCTION_LINE, here)
        lines = s.send({"cmd": "line_list", "file": "SYMPROBE.BAS"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check(f"{tag}_line_list_has_the_body_line_in_extended_memory", listed.get(BODY_LINE, 0) > EXTENDED, lines)

        body = s.send({"cmd": "resolve", "spec": f"SYMPROBE.BAS:{BODY_LINE}"})
        check(f"{tag}_resolve_a_line_inside_the_function", body.get("linear") == f"0x{listed.get(BODY_LINE, 0):08X}", body)

        locals_ = s.send({"cmd": "locals"})
        names = [v.get("name") for v in locals_.get("locals", [])]
        if names or not LOCALS_PENDING:
            check(f"{tag}_locals_name_delta_and_n", "delta" in [n.lower() for n in names] and "n" in [n.lower() for n in names], locals_)
        else:
            skip(f"{tag}_locals_name_delta_and_n", "the image carries no 32-bit proc/local records to read")
    finally:
        s.close()


# jwlink keeps llrm's publics and lines in the NB05 block but QB's per-module symbols are BC dialect; standard
# CV4 is on its way (debug-info). The reader's 32-bit procs and locals are unit-tested on synthetic records.
LOCALS_PENDING = True

run(sys.argv[1], "SYMPROBE.EXE", False)
run(sys.argv[1], "SYMPCV.EXE", True)
finish()
