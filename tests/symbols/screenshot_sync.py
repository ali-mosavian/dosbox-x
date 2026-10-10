#!/usr/bin/env python3
"""tests/symbols/screenshot_sync.py DOSBOX-X: a screenshot's reply comes after its PNG is written.

The frame is drawn on the next render, so the reply used to come first and a client read a file that was not
there yet (or half written): it polled the file's size. Now the reply says the file is complete.
"""

import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
from harness import check, finish  # noqa: E402
from session import Session  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n"
IEND = b"IEND\xaeB`\x82"


def run(dosbox: str) -> None:
    s = Session(dosbox, Path(__file__).parent / "fixtures", [], "ECHO ready")
    out = Path(tempfile.mkdtemp())
    try:
        s.send({"cmd": "continue"})
        check("shot_the_machine_runs", s.wait_for("ready", 60), s.screen())

        whole = 0
        for n in range(5):
            path = out / f"shot{n}.png"
            reply = s.send({"cmd": "screenshot", "path": str(path)})
            data = path.read_bytes() if path.exists() else b""
            if reply.get("status") == "ok" and data.startswith(PNG) and data.endswith(IEND) and reply.get("size") == len(data):
                whole += 1
        check("shot_the_file_is_whole_when_the_reply_comes", whole == 5, f"{whole} of 5")

        s.send({"cmd": "break"})
        path = out / "stopped.png"
        reply = s.send({"cmd": "screenshot", "path": str(path)})
        data = path.read_bytes() if path.exists() else b""
        check("shot_a_stopped_guest_is_drawn_too", reply.get("status") == "ok" and data.startswith(PNG) and data.endswith(IEND), reply)
    finally:
        s.close()


run(sys.argv[1])
finish()
