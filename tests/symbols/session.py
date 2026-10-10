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
    def __init__(self, dosbox: str, fixtures: Path, files: list, command: str, cpu: str = ""):
        """files: names under `fixtures`, or paths to copy in by their own name."""
        self.work = tempfile.TemporaryDirectory()
        work = Path(self.work.name)
        for name in files:
            source = name if isinstance(name, Path) else fixtures / name
            (work / source.name).write_bytes(source.read_bytes())
        (work / "t.conf").write_text(
            "[sdl]\nautolock=false\n[dosbox]\nmemsize=32\nstartbanner=false\nquit warning=false\n"
            f"[cpu]\ncore=normal\n{cpu}\n[autoexec]\nmount c {work}\nc:\n{command}\n"
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
        self.pending = b""
        self.events: list[dict] = []

    def readline(self, timeout: float | None = None) -> str | None:
        """One line from the socket, waiting up to `timeout` seconds (forever when None); None on timeout. A line
        that came in with the previous one is already here, which select on the socket would not say."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = None if deadline is None else deadline - time.monotonic()
            if left is not None and (left <= 0 or not select.select([self.sock], [], [], left)[0]):
                return None
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the emulator closed the socket")
            self.pending += chunk
        line, _, self.pending = self.pending.partition(b"\n")
        return line.decode()

    def read(self) -> dict:
        while True:
            message = json.loads(self.readline())
            if "status" in message:
                return message
            self.events.append(message)

    def send(self, command: dict) -> dict:
        self.sock.sendall((json.dumps(command) + "\n").encode())
        return self.read()

    def wait_stopped(self, seconds: float) -> bool:
        """Until the program hits a breakpoint: the emulator reports it as a "stopped" event."""
        return self.wait_event("stopped", seconds, keep=True) is not None

    def wait_event(self, name: str, seconds: float, keep: bool = False) -> dict | None:
        """The first event of that name not yet taken (left in place when `keep`), or None after `seconds`."""
        deadline = time.monotonic() + seconds
        while True:
            for i, event in enumerate(self.events):
                if event.get("event") == name:
                    return event if keep else self.events.pop(i)
            line = self.readline(max(0.0, deadline - time.monotonic()))
            if line is None:
                return None
            message = json.loads(line)
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
