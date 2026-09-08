import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  d32HeaderFingerprint,
  findMatchingD32File,
  parseD32HeaderBytes,
} from "./formats.js";

function buildHeaderBytes(): Uint8Array {
  const bytes = new Uint8Array(72);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0x58323344, true);
  view.setUint32(8, 180, true);
  view.setUint32(56, 16, true);
  view.setUint32(64, 4, true);
  view.setUint32(68, 4, true);
  view.setUint16(32, 1, true);
  view.setUint16(34, 0, true);
  view.setUint32(36, 1, true);
  return bytes;
}

test("parseD32HeaderBytes accepts live D32X header bytes", () => {
  const header = parseD32HeaderBytes(buildHeaderBytes());
  assert.ok(header !== null);
  assert.equal(header?.codeSize, 16);
  assert.equal(d32HeaderFingerprint(header!), "180:16:4:4:1:0:1");
});

test("findMatchingD32File matches on-disk module by header fingerprint", () => {
  const dir = mkdtempSync(join(tmpdir(), "formats-test-"));
  const file = join(dir, "rt32.d32");
  const disk = Buffer.alloc(72);
  buildHeaderBytes().forEach((byte, index) => { disk[index] = byte; });
  writeFileSync(file, disk);

  const matched = findMatchingD32File(buildHeaderBytes(), [dir]);
  assert.equal(matched, file);
});
