#!/usr/bin/env python3
"""tests/symbols/qb_vars.py DOSBOX-X: a BASIC program's module variables through `var`, with their sizes.

vars.bas (qb/README.md) has Wind (INTEGER), gravity# (DOUBLE) and GorillaX (an array, through its descriptor).
`var GRAVITY#` said length 2 with size_known false: the BC dialect's type index of a scalar was never read, so a double
was two bytes. A CodeView 4 image gives all three their types (the array its 18-byte descriptor); the BC dialect gives a scalar
its size and says nothing of an array's, which it has no type for here.
"""

import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


def run(dosbox: str, image: str, tag: str, array: tuple) -> None:
    s = Session(dosbox, FIXTURES, [image], image)
    try:
        s.send({"cmd": "continue"})
        time.sleep(2)
        wind = s.send({"cmd": "var", "name": "WIND%"})
        check(f"{tag}_an_integer_is_two_bytes_of_a_known_size", wind.get("length") == 2 and wind.get("size_known") is True, wind)
        gravity = s.send({"cmd": "var", "name": "GRAVITY#"})
        check(f"{tag}_a_double_is_eight_bytes_of_a_known_size", gravity.get("length") == 8 and gravity.get("size_known") is True, gravity)
        check(f"{tag}_a_double_reads_as_one", abs(gravity.get("value", 0) - 9.8) < 1e-9, gravity)
        gx = s.send({"cmd": "var", "name": "GORILLAX%"})
        check(f"{tag}_an_array_is_said_as_far_as_the_image_says_it", (gx.get("length"), gx.get("size_known")) == array, gx)
    finally:
        s.close()


run(sys.argv[1], "SYMQBBC.EXE", "bc", (2, False))
run(sys.argv[1], "SYMQBCV.EXE", "cv4", (18, True))
run(sys.argv[1], "SYMQBJW.EXE", "jwlink", (18, True))
finish()
