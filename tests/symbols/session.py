"""A DOSBox-X under the debug socket, running a guest program: the harness the symbol tests share."""

import json
import os
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path


class Session:
    def __init__(self, dosbox: str, fixtures: Path, files: list, command: str):
        """files: names under `fixtures`, or paths to copy in by their own name."""
        self.work = tempfile.TemporaryDirectory()
        work = Path(self.work.name)
        for name in files:
            source = name if isinstance(name, Path) else fixtures / name
            (work / source.name).write_bytes(source.read_bytes())
        (work / "t.conf").write_text(
            "[sdl]\nautolock=false\n[dosbox]\nmemsize=32\nstartbanner=false\nquit warning=false\n"
            f"[cpu]\ncore=normal\n[autoexec]\nmount c {work}\nc:\n{command}\n"
        )
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        env = {**os.environ, "SDL_VIDEODRIVER": "dummy", "DOSBOX_DEBUG_PORT": str(port)}
        self.process = subprocess.Popen(
            [dosbox, "-nolog", "-conf", str(work / "t.conf")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        for _ in range(150):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.2)
        self.lines = self.sock.makefile("r")
        self.events: list[dict] = []

    def read(self) -> dict:
        while True:
            message = json.loads(self.lines.readline())
            if "status" in message:
                return message
            self.events.append(message)

    def send(self, command: dict) -> dict:
        self.sock.sendall((json.dumps(command) + "\n").encode())
        return self.read()

    def wait_stopped(self, seconds: float) -> bool:
        """Until the program hits a breakpoint: the emulator reports it as a "stopped" event."""
        deadline = time.monotonic() + seconds
        while not any(e.get("event") == "stopped" for e in self.events):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                return False
            message = json.loads(self.lines.readline())
            if "status" not in message:
                self.events.append(message)
        return True

    def wait_event(self, name: str, seconds: float) -> dict | None:
        """The first event of that name not yet taken, or None after `seconds`."""
        deadline = time.monotonic() + seconds
        while True:
            for i, event in enumerate(self.events):
                if event.get("event") == name:
                    return self.events.pop(i)
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                return None
            message = json.loads(self.lines.readline())
            if "status" not in message:
                self.events.append(message)

    def screen(self) -> str:
        return self.send({"cmd": "text_screen"}).get("text") or ""

    def wait_for(self, text: str, seconds: float) -> bool:
        deadline = time.monotonic() + seconds
        while text not in self.screen():
            if time.monotonic() > deadline:
                return False
            time.sleep(0.25)
        return True

    def type(self, keys: list[str]) -> None:
        for key in keys:
            self.send({"cmd": "key", "key": key})

    def close(self) -> None:
        self.process.kill()
        self.work.cleanup()
