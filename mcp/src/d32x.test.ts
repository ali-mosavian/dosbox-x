import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  parseD32File,
  sectionRuntimeAddress,
} from "./d32x.js";

function cstr(bytes: Buffer, off: number, text: string): void {
  bytes.write(text, off, "ascii");
  bytes[off + text.length] = 0;
}

function buildFixture(): Buffer {
  const bytes = Buffer.alloc(220);
  bytes.writeUInt32LE(0x58323344, 0);
  bytes.writeUInt32LE(1, 4);
  bytes.writeUInt32LE(180, 8);
  bytes.writeUInt32LE(0, 12);
  bytes.writeUInt32LE(0, 16);
  bytes.writeUInt32LE(72, 20);
  bytes.writeUInt32LE(16, 24);
  bytes.writeUInt32LE(0, 28);
  bytes.writeUInt16LE(1, 32);
  bytes.writeUInt16LE(0, 34);
  bytes.writeUInt32LE(1, 36);
  bytes.writeUInt32LE(88, 40);
  bytes.writeUInt32LE(100, 44);
  bytes.writeUInt32LE(100, 48);
  bytes.writeUInt32LE(112, 52);
  bytes.writeUInt32LE(16, 56);
  bytes.writeUInt32LE(128, 60);
  bytes.writeUInt32LE(4, 64);
  bytes.writeUInt32LE(4, 68);
  cstr(bytes, 72, "add");
  cstr(bytes, 76, "helper");

  bytes.writeUInt32LE(0, 88);
  bytes.writeUInt32LE(4, 92);
  bytes[96] = 0;

  bytes.writeUInt32LE(8, 100);
  bytes.writeUInt32LE(4, 104);
  bytes[108] = 2;
  bytes[109] = 0;
  bytes[110] = 0;

  bytes.writeUInt32LE(0x11111111, 120);

  const debugOff = 132;
  bytes.writeUInt32LE(0x53323344, debugOff);
  bytes.writeUInt16LE(1, debugOff + 4);
  bytes.writeUInt16LE(1, debugOff + 6);
  bytes.writeUInt32LE(20, debugOff + 8);
  bytes.writeUInt32LE(36, debugOff + 12);
  bytes.writeUInt32LE(8, debugOff + 16);
  bytes.writeUInt32LE(0, debugOff + 20);
  bytes.writeUInt32LE(4, debugOff + 24);
  bytes.writeUInt32LE(7, debugOff + 28);
  bytes[debugOff + 32] = 0;
  bytes[debugOff + 33] = 1;
  bytes.writeUInt16LE(2, debugOff + 34);
  cstr(bytes, debugOff + 36, "helper");

  bytes.writeUInt32LE(0x54323344, 180);
  bytes.writeUInt32LE(debugOff, 184);
  bytes.writeUInt32LE(44, 188);
  return bytes.subarray(0, 192);
}

test("parseD32File reads exports, relocs, and D32S debug symbols", () => {
  const dir = mkdtempSync(join(tmpdir(), "d32x-test-"));
  const file = join(dir, "mod.d32");
  writeFileSync(file, buildFixture());

  const module = parseD32File(file);

  assert.equal(module.exports[0].name, "add");
  assert.equal(module.relocs[0].kind, 2);
  assert.equal(module.debugSymbols[0].name, "helper");
  assert.equal(sectionRuntimeAddress(module, 0x400000, 0, 4), 0x400000 + 112 + 4);
});
