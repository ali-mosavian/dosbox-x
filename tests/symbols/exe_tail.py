#!/usr/bin/env python3
"""tests/symbols/exe_tail.py DOSBOX-X: what follows an EXE's image in the file is not loaded with it.

The loader read `pages * 512` bytes, the whole of the last page, where DOS reads the last page's bytes
(`extrabytes`): a linker's debug information appended after the image (jwlink's DWARF and CodeView) landed in the memory
after the program, the part it expects as it was: a BASIC program linked with `debug dwarf` crashed at 0100:0000 while the same image
without the tail, or with a tail of zeros, ran. SYMTAIL.EXE (exe/tail.asm) prints the byte after its image.
"""

import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


def printed(dosbox: str, tail: bytes) -> str:
    with tempfile.TemporaryDirectory() as where:
        (Path(where) / "T.EXE").write_bytes((FIXTURES / "SYMTAIL.EXE").read_bytes() + tail)
        s = Session(dosbox, Path(where), ["T.EXE"], "T.EXE")
        try:
            s.send({"cmd": "continue"})
            time.sleep(1.5)
            return s.screen().rsplit("T.EXE", 1)[-1].strip()
        finally:
            s.close()


clean = printed(sys.argv[1], b"")
with_tail = printed(sys.argv[1], b"\x5a" * 300)
check("tail_the_byte_after_the_image_is_read", len(clean) == 2 and all(c in "0123456789ABCDEF" for c in clean), clean)
check("tail_is_the_same_with_bytes_after_the_image_in_the_file", with_tail == clean, (clean, with_tail))
finish()
