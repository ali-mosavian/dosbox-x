#!/usr/bin/env python3
"""tests/symbols/dwarf16_symbols.py DOSBOX-X: DWARF in a real 16-bit MZ and a real 16-bit NE, with no .MAP beside them.

prog.c (llrm-c -m16 -g, OMF) and a start file (jwasm -Zi) are linked by jwlink with `debug dwarf` (dwarf16/build.sh).
An address in a segmented image is an offset in a segment: for an MZ, read through the load segment the way CV and
TD are, with DW_AT_segment a frame paragraph; for an NE, through the segment's selector. SYMMZD.EXE runs under the
emulator's DOS; SYMNED.EXE under HX's DPMILD16 (HX_DOS, see README.md), and is skipped without it.
"""

import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
BUMP_LINE = 11  # int delta = twice(n);
RETURN_LINE = 13  # return counter;


def run(dosbox: str, tag: str, files: list, command: str) -> None:
    s = Session(dosbox, FIXTURES, files, command)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())

        sym = s.send({"cmd": "sym", "name": "_bump@3"})
        check(f"{tag}_sym_names_the_function_from_dwarf", sym.get("status") == "ok" and sym.get("source") == "dwarf", sym)

        lines = s.send({"cmd": "line_list", "file": "prog.c"})
        listed = {l["line"]: int(l["linear"], 16) for l in lines.get("lines", [])}
        check(f"{tag}_line_list_starts_the_function_at_its_symbol", listed.get(BUMP_LINE) == int(sym.get("linear", "0"), 16), lines)

        s.send({"cmd": "bp_set", "location": "_bump@3"})
        s.type(["x"])
        check(f"{tag}_bp_fires_in_the_function", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_where_names_function_and_line", here.get("symbol") == "_bump@3" and here.get("file") == "prog.c" and
              here.get("line") == BUMP_LINE and here.get("source") == "dwarf", here)

        counter = s.send({"cmd": "var", "name": "_counter", "len": 2})
        check(f"{tag}_var_reads_the_global_through_its_segment", counter.get("bytes") == "0100", counter)
    finally:
        s.close()


def omf(dosbox: str) -> None:
    """locals.c with llrm's own 16-bit DWARF (-gdwarf-2, Open Watcom's convention): address size 2, DW_AT_segment
    on each symbol, DW_OP_addr of 2 bytes, registers numbered ax 27 ... Before, `counter` was left out (the
    location reader took DW_OP_addr for 4 bytes)."""
    tag = "omf"
    s = Session(dosbox, FIXTURES, ["SYMLOC16.EXE"], "SYMLOC16.EXE")
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())
        counter = s.send({"cmd": "var", "name": "counter", "len": 2})
        check(f"{tag}_a_global_with_a_16_bit_address_reads_through_its_segment", counter.get("bytes") == "0700", counter)
        sym = s.send({"cmd": "sym", "name": "add"})
        check(f"{tag}_sym_names_the_function", sym.get("status") == "ok" and sym.get("source") == "dwarf", sym)
        s.send({"cmd": "bp_set", "location": "locals.c:11"})
        s.type(["x"])
        check(f"{tag}_bp_fires_on_the_line", s.wait_stopped(30))
        here = s.send({"cmd": "where"})
        check(f"{tag}_where_names_function_and_line", here.get("symbol") == "add" and here.get("file") == "locals.c" and
              here.get("line") == 11, here)
        found = {v["name"]: v for v in s.send({"cmd": "locals"}).get("locals", [])}
        check(f"{tag}_locals_are_named", {"first", "second", "sum"} <= set(found), list(found))
        # The frame base is the call frame address, and only .debug_frame says where that is at each instruction: without
        # llrm's -m16 frame rows these were read from a guess and gave other words of the stack.
        values = {n: found.get(n, {}).get("value") for n in ("first", "second", "sum")}
        check(f"{tag}_parameters_and_locals_read_through_the_frame_rows", values == {"first": 3, "second": 4, "sum": 7}, found)
    finally:
        s.close()


def far_pointers(dosbox: str) -> None:
    """farptr.c: `char __far *where` and `char __huge *wide` point at `buffer`. llrm writes them as pointers with
    DW_AT_address_class 2 and 3; before, the reader dropped the class, so `locals` gave a bare 32-bit number and nothing
    said it was a segment and an offset, or where it led."""
    tag = "far"
    s = Session(dosbox, FIXTURES, ["SYMFAR16.EXE"], "SYMFAR16.EXE")
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())
        buffer = s.send({"cmd": "sym", "name": "buffer"})
        s.send({"cmd": "bp_set", "location": "farptr.c:7"})
        s.type(["x"])
        check(f"{tag}_bp_fires", s.wait_stopped(30))
        ds = int(s.send({"cmd": "regs"}).get("DS", 0))
        found = {v["name"]: v for v in s.send({"cmd": "locals"}).get("locals", [])}
        for name, reach in (("where", "far"), ("wide", "huge")):
            pointer = found.get(name, {}).get("pointer", {})
            check(f"{tag}_{name}_is_a_{reach}_pointer", pointer.get("reach") == reach, found.get(name))
            check(f"{tag}_{name}_is_a_segment_and_an_offset", pointer.get("segment") == ds and
                  pointer.get("offset") == int(buffer.get("offset", "-1"), 16), (pointer, ds, buffer))
            check(f"{tag}_{name}_is_followed_to_what_it_points_at", pointer.get("bytes") == "05" and pointer.get("value") == 5 and
                  pointer.get("target_type") == "char", pointer)
    finally:
        s.close()


run(sys.argv[1], "mzd", ["SYMMZD.EXE"], "SYMMZD.EXE")
omf(sys.argv[1])
far_pointers(sys.argv[1])

hx = Path(os.environ.get("HX_DOS", "/nonexistent"))
if (hx / "DPMILD16.EXE").is_file():
    run(sys.argv[1], "ned", [hx / "HDPMI16.EXE", hx / "DPMILD16.EXE", "SYMNED.EXE"], "DPMILD16.EXE SYMNED.EXE")
else:
    skip("ned_*", "HX_DOS does not name a directory with HDPMI16.EXE and DPMILD16.EXE")
finish()
