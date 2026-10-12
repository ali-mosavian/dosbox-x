#!/usr/bin/env python3
"""tests/symbols/dwarf_static.py DOSBOX-X: a variable at an address inside a function is a symbol, as one at the unit's top.

llrm-qb writes a BASIC module's array (the variable of its descriptor, `struct ARRAY`) inside the module's function, with
DW_OP_addr; the reader kept only variables of the frame and dropped it: `var GORILLAX%` was an unknown symbol while WIND% and
GRAVITY# (at the unit's top) read. A C function's `static` is the same DIE.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

s = Session(sys.argv[1], Path(__file__).parent / "fixtures", ["SYMQBDW.EXE"], "SYMQBDW.EXE")
try:
    s.send({"cmd": "continue"})
    time.sleep(2)
    top = s.send({"cmd": "var", "name": "GRAVITY#"})
    check("dwarf_a_variable_at_the_units_top_has_its_size", top.get("length") == 8 and top.get("size_known") is True, top)
    array = s.send({"cmd": "var", "name": "GORILLAX%"})
    check("dwarf_an_array_inside_its_modules_function_is_a_symbol", array.get("status") == "ok", array)
    check("dwarf_and_is_its_descriptor", (array.get("length"), array.get("size_known"), array.get("type")) == (18, True, "struct ARRAY"), array)
finally:
    s.close()
finish()
