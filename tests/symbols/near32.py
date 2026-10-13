#!/usr/bin/env python3
"""tests/symbols/near32.py DOSBOX-X: a flat 32-bit pointer is a near one, four bytes through DS.

SYMQBM3.EXE is qb/fn.bas by llrm-qb -m32 -g and jwlink `debug codeview` (an LE under DOS/32A). `Add (a, b)` gets two 32-bit
pointers (CodeView's near32, kind 10); the reader took every pointer of four bytes for a 16:16 far one, so `A%` read
`0017:156C` and a value of 24661 where the argument 3 is.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

s = Session(sys.argv[1], Path(__file__).parent / "fixtures", ["SYMQBM3.EXE"], "SYMQBM3.EXE")
try:
    s.send({"cmd": "continue"})
    time.sleep(4)
    s.send({"cmd": "bp_set", "location": "fn.bas:14"})
    s.type(["x"])
    check("near32_the_function_is_reached", s.wait_stopped(30))
    found = {v["name"]: v for v in s.send({"cmd": "locals"}).get("locals", [])}
    a, b = found.get("A%", {}).get("pointer", {}), found.get("B%", {}).get("pointer", {})
    check("near32_a_local_has_its_value", found.get("SUM%", {}).get("value") == 7, found.get("SUM%"))
    check("near32_the_pointer_is_near_through_ds", a.get("reach") == "near" and a.get("via") == "DS", a)
    check("near32_a_by_reference_parameter_reads_through_its_pointer", (a.get("value"), b.get("value")) == (3, 4), found)
finally:
    s.close()
finish()
