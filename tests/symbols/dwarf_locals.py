#!/usr/bin/env python3
"""tests/symbols/dwarf_locals.py DOSBOX-X: a BASIC function's locals read from DWARF in a running 16-bit image.

SYMQBFD.EXE is qb/fn.bas with jwlink's `debug dwarf` tail (an EXE whose image the loader read to the page's end crashed at
0100:0000 until the tail was left out of the image). `Add (a, b)` has two by-reference parameters (near pointers) and a local.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

s = Session(sys.argv[1], Path(__file__).parent / "fixtures", ["SYMQBFD.EXE"], "SYMQBFD.EXE")
try:
    s.send({"cmd": "continue"})
    time.sleep(2)
    s.send({"cmd": "bp_set", "location": "fn.bas:14"})
    s.type(["x"])
    check("dwarf_locals_the_function_is_reached", s.wait_stopped(30))
    found = {v["name"]: v for v in s.send({"cmd": "locals"}).get("locals", [])}
    check("dwarf_locals_a_local_has_its_value", found.get("SUM%", {}).get("value") == 7, found)
    a, b = found.get("A%", {}).get("pointer", {}), found.get("B%", {}).get("pointer", {})
    check("dwarf_locals_by_reference_parameters_read_through_their_pointers", (a.get("value"), b.get("value")) == (3, 4), found)
finally:
    s.close()
finish()
