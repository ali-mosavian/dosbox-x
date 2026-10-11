import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import { DosboxSession } from "./dosbox.js";
import { EventLog } from "./eventLog.js";

/** An emulator that sends `before` lines ahead of the reply to every command. */
function fakeEmulator(before: object[]): Promise<{ port: number; close: () => void }> {
  return new Promise((resolve) => {
    const server = net.createServer((socket) => {
      socket.setEncoding("utf8");
      let buffered = "";
      socket.on("data", (data: string) => {
        buffered += data;
        for (let nl = buffered.indexOf("\n"); nl >= 0; nl = buffered.indexOf("\n")) {
          const cmd = JSON.parse(buffered.slice(0, nl)) as { id: number };
          buffered = buffered.slice(nl + 1);
          for (const line of before) socket.write(JSON.stringify(line) + "\n");
          socket.write(JSON.stringify({ status: "ok", id: cmd.id, answered: true }) + "\n");
        }
      });
    });
    server.listen(0, "127.0.0.1", () => {
      resolve({ port: (server.address() as net.AddressInfo).port, close: () => server.close() });
    });
  });
}

test("a key_wait event that arrives while a command is pending is not that command's reply", async () => {
  const emulator = await fakeEmulator([{ event: "key_wait", function: "0x08", at: "0F00:00000010" }]);
  const session = new DosboxSession(emulator.port);
  try {
    const reply = await session.sendCommand({ cmd: "regs" }, 3000);
    assert.equal(reply["answered"], true, JSON.stringify(reply));
    assert.deepEqual(session.events.list("key_wait").map((e) => e["function"]), ["0x08"]);
  } finally {
    session.disconnect();
    emulator.close();
  }
});

test("an image_loaded event that stopped the guest wakes a wait for a stop", async () => {
  const emulator = await fakeEmulator([{ event: "image_loaded", stopped: true, program: "GAME.EXE" }]);
  const session = new DosboxSession(emulator.port);
  try {
    await session.sendCommand({ cmd: "regs" }, 3000);
    const stop = await session.waitForStop(1000);
    assert.equal(stop["event"], "image_loaded");
  } finally {
    session.disconnect();
    emulator.close();
  }
});

test("wait returns an event that came after the mark, and nothing at the timeout", async () => {
  const log = new EventLog();
  log.push({ event: "key_wait", n: 1 });
  const mark = log.mark();
  assert.equal(await log.wait("key_wait", mark, 20), undefined);
  setTimeout(() => log.push({ event: "key_wait", n: 2 }), 10);
  assert.equal((await log.wait("key_wait", mark, 1000))?.["n"], 2);
  assert.equal((await log.wait("key_wait", 0, 20))?.["n"], 1);
});
