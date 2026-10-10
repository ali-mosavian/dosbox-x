"""What the symbol tests share: the pass/fail ledger."""

import sys

failed = False


def check(name: str, ok: bool, detail: object = "") -> None:
    global failed
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"  {detail}"))
    failed |= not ok


def skip(name: str, reason: str) -> None:
    print(f"SKIP {name}  {reason}")


def finish() -> None:
    sys.exit(failed)
