import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { parseWatcom } from "./watcom.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");

/**
 * A hand-built block, not a fixture: there is no OpenWatcom toolchain in this
 * tree to link a real one with. It pins this reader's own arithmetic — the
 * table order and the V2/V3 `mod` difference — and claims nothing about what
 * wlink actually writes.
 */
function watcomBlock(v2: boolean): Buffer {
  const symbolName = "_main";
  const moduleRecord = (name: string): Buffer => {
    const buf = Buffer.alloc(21 + name.length);
    buf.writeUInt8(name.length, 20);
    buf.write(name, 21, "latin1");
    return buf;
  };
  const first = moduleRecord("startup.c");
  const module = Buffer.concat([first, moduleRecord("hello.c")]);

  const symbol = Buffer.alloc((v2 ? 9 : 10) + symbolName.length);
  symbol.writeUInt32LE(0x1234, 0);
  symbol.writeUInt16LE(0x0abc, 4);
  // The second module: V2 names it by byte offset into the module area, V3 by
  // index. Two modules is what tells the two readings apart -- with one, offset
  // and index are both 0 and any reading passes.
  symbol.writeUInt16LE(v2 ? first.length : 1, 6);
  if (v2) symbol.writeUInt8(symbolName.length, 8);
  else {
    symbol.writeUInt8(0x04, 8);
    symbol.writeUInt8(symbolName.length, 9);
  }
  symbol.write(symbolName, v2 ? 9 : 10, "latin1");

  const section = Buffer.concat([Buffer.alloc(18), module, symbol, Buffer.alloc(4)]);
  section.writeUInt32LE(18, 0); // mod_offset
  section.writeUInt32LE(18 + module.length, 4); // gbl_offset
  section.writeUInt32LE(18 + module.length + symbol.length, 8); // addr_offset
  section.writeUInt32LE(section.length, 12);

  const master = Buffer.alloc(14);
  master.writeUInt16LE(0x8386, 0);
  master.writeUInt8(v2 ? 2 : 3, 2);
  master.writeUInt8(1, 4);
  master.writeUInt16LE(2, 6); // lang_size
  master.writeUInt16LE(2, 8); // segment_size
  master.writeUInt32LE(4 + section.length + 14, 10);

  return Buffer.concat([Buffer.from("C\0"), Buffer.alloc(2), section, master]);
}

for (const v2 of [false, true]) {
  test(`Watcom V${v2 ? 2 : 3} globals resolve their module`, () => {
    const info = parseWatcom(watcomBlock(v2));
    assert.ok(info);
    assert.equal(info.version, `WAT ${v2 ? 2 : 3}.0`);
    assert.deepEqual(info.modules, [{ index: 0, name: "startup.c" }, { index: 1, name: "hello.c" }]);
    assert.deepEqual(info.symbols, [{
      name: "_main",
      offset: 0x1234,
      segment: 0x0abc,
      moduleIndex: 1,
      kind: v2 ? 0 : 0x04,
    }]);
  });
}

test("a CodeView EXE is not mistaken for Watcom debug info", () => {
  // Watcom's master header is the last 14 bytes; CodeView's trailer is the
  // last 8, so both readers look at overlapping bytes and only the signature
  // separates them.
  assert.equal(parseWatcom(readFileSync(join(FIXTURES, "qrender-cv.exe"))), undefined);
  assert.equal(parseWatcom(readFileSync(join(FIXTURES, "cvprobe.exe"))), undefined);
});
