#!/usr/bin/env python3
"""tests/symbols/profile_symbols.py DOSBOX-X: the profile of a real run, in every executable format.

profile/prof.c: run() calls outer() once, outer() calls inner(100) three times, and inner() adds in a loop of 100
trips on line 8. So a correct profile has calls inner=3, outer=1, run=1; the loop line's count is a multiple of the
300 trips; a caller's inclusive count is its own plus its callees' inclusive counts; and the functions' counts add up
to the program's total. Run as an LE (DOS/32A), a 16-bit MZ (DOS), and, with HX_DOS, a PE and a 16-bit NE.
"""

import os
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish, skip  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
LOOP_LINE = 8  # sum += i;
TRIPS = 300


def short(name: str) -> str:
    """Watcom's `inner_` and the 16-bit `_inner@3` are the function inner."""
    return re.sub(r"^_|_$|@\d+$", "", name)


def run(dosbox: str, tag: str, files: list, command: str) -> None:
    s = Session(dosbox, FIXTURES, files, command)
    try:
        s.send({"cmd": "continue"})
        check(f"{tag}_program_starts", s.wait_for("ready", 60), s.screen())
        s.send({"cmd": "sym", "name": "inner_" if tag in ("led", "ped") else "_inner@3"})  # place the image
        s.send({"cmd": "profile_start"})
        s.type(["x"])
        check(f"{tag}_program_runs", s.wait_for("done", 60), s.screen())

        report = s.send({"cmd": "profile"})
        check(f"{tag}_profile_answers", report.get("status") == "ok", report)
        by = {short(f["name"]): f for f in report.get("functions", [])}
        check(f"{tag}_functions_are_named", {"inner", "outer", "run"} <= set(by), list(by))
        if not {"inner", "outer", "run"} <= set(by):
            return

        check(f"{tag}_call_counts_are_the_programs", (by["inner"]["calls"], by["outer"]["calls"], by["run"]["calls"]) == (3, 1, 1),
              {k: by[k]["calls"] for k in ("inner", "outer", "run")})
        check(f"{tag}_a_leaf_is_inclusive_of_itself", by["inner"]["inclusive"] == by["inner"]["self"], by["inner"])
        check(f"{tag}_a_caller_includes_its_callees", by["outer"]["inclusive"] == by["outer"]["self"] + by["inner"]["inclusive"] and
              by["run"]["inclusive"] == by["run"]["self"] + by["outer"]["inclusive"], by)
        check(f"{tag}_memory_is_inclusive_too", by["outer"]["inclusive_memory"] == by["outer"]["self_memory"] + by["inner"]["inclusive_memory"], by)

        loop = [l for l in report["lines"] if l["file"].lower().endswith("prof.c") and l["line"] == LOOP_LINE]
        check(f"{tag}_the_loop_line_ran_its_trips", len(loop) == 1 and loop[0]["self"] > 0 and loop[0]["self"] % TRIPS == 0, loop)
        in_inner = sum(l["self"] for l in report["lines"] if short(l["function"]) == "inner")
        check(f"{tag}_the_lines_of_a_function_add_up_to_it", in_inner == by["inner"]["self"], (in_inner, by["inner"]))

        check(f"{tag}_functions_add_up_to_the_total", report["attributed_instructions"] == report["instructions"] > 0, report)
        check(f"{tag}_memory_adds_up_too", report["attributed_memory"] == report["memory"] > 0, report)
    finally:
        s.close()


run(sys.argv[1], "led", ["SYMPRL.EXE"], "SYMPRL.EXE")
run(sys.argv[1], "mzd", ["SYMPRM.EXE"], "SYMPRM.EXE")

hx = Path(os.environ.get("HX_DOS", "/nonexistent"))
if (hx / "DPMILD32.EXE").is_file() and (hx / "DPMILD16.EXE").is_file():
    run(sys.argv[1], "ped", [hx / "HDPMI32.EXE", hx / "DPMILD32.EXE", "SYMPRP.EXE"], "DPMILD32.EXE SYMPRP.EXE")
    run(sys.argv[1], "ned", [hx / "HDPMI16.EXE", hx / "DPMILD16.EXE", "SYMPRN.EXE"], "DPMILD16.EXE SYMPRN.EXE")
else:
    skip("ped_* ned_*", "HX_DOS does not name a directory with the HX loaders")
finish()
