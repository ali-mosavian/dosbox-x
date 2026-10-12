#!/usr/bin/env python3
"""tests/symbols/near_pointers.py DOSBOX-X: a near pointer is followed to what it points at, through DS.

BASIC passes a parameter by reference: `Add (a, b)` gets two near pointers, and `locals` said `A% 22, B% 24` (the addresses of
the arguments 3 and 4), a far pointer's follow (`pointer`) being all the reader had. The value is under the pointer.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

s = Session(sys.argv[1], Path(__file__).parent / "fixtures", ["SYMQBFN.EXE"], "SYMQBFN.EXE")
try:
    s.send({"cmd": "continue"})
    time.sleep(2)
    s.send({"cmd": "bp_set", "location": "fn.bas:14"})  # `sum = a + b`, past the prologue
    s.type(["x"])
    check("near_the_function_is_reached", s.wait_stopped(30))
    found = {v["name"]: v for v in s.send({"cmd": "locals"}).get("locals", [])}
    a, b = found.get("A%", {}).get("pointer", {}), found.get("B%", {}).get("pointer", {})
    check("near_a_by_reference_parameter_reads_through_its_pointer", a.get("value") == 3 and b.get("value") == 4, found)
    check("near_and_says_how_it_was_followed", a.get("reach") == "near" and a.get("via") == "DS" and a.get("length") == 2, a)
    check("near_a_value_that_is_no_pointer_has_none", "pointer" not in found.get("SUM%", {}) and found.get("SUM%", {}).get("value") == 7, found.get("SUM%"))
finally:
    s.close()
finish()
