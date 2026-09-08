import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { readFileSync } from "node:fs";

import { findCvBase, parseCodeView, parseSymbolRun, readCodeView } from "./codeview.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");
const PROBE = join(FIXTURES, "cvprobe.exe");
const QRENDER = join(FIXTURES, "qrender-cv.exe");

test("readCodeView finds the appended block through the EOF trailer", () => {
  const info = readCodeView(PROBE);
  assert.ok(info);
  assert.equal(info.signature, "NB08");
  assert.equal(info.base, 19560);
  assert.equal(info.directory.length, 61);
  assert.deepEqual(info.warnings, []);
});

test("readCodeView reads modules, publics, segments and line tables", () => {
  const info = readCodeView(PROBE);
  assert.ok(info);
  assert.equal(info.modules[0].name, "cvprobe.obj");
  assert.equal(info.modules.length, 51);
  assert.equal(info.segments.length, 72);

  const publics = info.symbols.filter((symbol) => symbol.kind === "public");
  assert.equal(publics.length, 720);

  const table = info.lines.find((entry) => entry.file === "cvprobe.bas");
  assert.ok(table);
  assert.equal(table.segment, 1);
  assert.deepEqual(table.lines[0], { offset: 48, line: 13 });
});

test("sstAlignSym's CV_OMF_SIG is skipped, so per-module symbols survive", () => {
  // Reading the 4-byte signature as a record length made the whole subsection
  // parse as nothing: 0 procs and 0 module data out of 340 real bytes.
  const info = readCodeView(PROBE);
  assert.ok(info);
  const procs = info.symbols.filter((symbol) => symbol.kind === "proc");
  assert.deepEqual(procs.map((proc) => proc.name).sort(), ["b$sd", "pr_add", "pr_fill", "pr_show"]);

  const add = procs.find((proc) => proc.name === "pr_add");
  assert.equal(add?.segment, 1);
  assert.equal(add?.offset, 180);
  assert.equal(add?.size, 34);
  assert.equal(add?.module, "cvprobe.obj");
});

test("parseSymbolRun keeps a run that has no leading signature", () => {
  const info = readCodeView(PROBE);
  assert.ok(info);
  const body = Buffer.alloc(16);
  body.writeUInt16LE(10, 0);
  body.writeUInt16LE(0x0103, 2); // S_PUB16
  body.writeUInt16LE(0x1234, 4);
  body.writeUInt16LE(7, 6);
  body.writeUInt16LE(0, 8);
  body.writeUInt8(2, 10);
  body.write("ab", 11, "latin1");

  const parsed = parseSymbolRun(body, 1);
  assert.equal(parsed.length, 1);
  assert.deepEqual(
    { name: parsed[0].name, segment: parsed[0].segment, offset: parsed[0].offset },
    { name: "ab", segment: 7, offset: 0x1234 },
  );
});

test("the MZ image end finds the block when the trailer is unusable", () => {
  const data = readFileSync(QRENDER);
  data.writeUInt32LE(0xdeadbeef, data.length - 4);
  assert.deepEqual(findCvBase(data), { base: 484868, signature: "NB08" });
});

test("a directory header near EOF warns instead of throwing", () => {
  // The bounds check read 4 bytes and the header is 8: a truncated or
  // mis-stamped lfoDirectory threw RangeError, which the caller turned into
  // "carries no debug info" for the whole file.
  const data = readFileSync(PROBE);
  const base = 19560;
  data.writeUInt32LE(data.length - base - 5, base + 4);
  const info = parseCodeView(data);
  assert.ok(info);
  assert.deepEqual(info.directory, []);
  assert.match(info.warnings[0], /past end of file/);
});

test("parseSymbolRun reads the 32-bit public form", () => {
  const body = Buffer.alloc(20);
  body.writeUInt16LE(14, 0);
  body.writeUInt16LE(0x0203, 2); // S_PUB32
  body.writeUInt32LE(0x12345678, 4);
  body.writeUInt16LE(9, 8);
  body.writeUInt16LE(0, 10);
  body.writeUInt8(3, 12);
  body.write("wid", 13, "latin1");

  const parsed = parseSymbolRun(body, 1);
  assert.equal(parsed.length, 1);
  assert.deepEqual(
    { name: parsed[0].name, segment: parsed[0].segment, offset: parsed[0].offset, kind: parsed[0].kind },
    { name: "wid", segment: 9, offset: 0x12345678, kind: "public" },
  );
});

test("readCodeView scales to a real medium-model program", () => {
  const info = readCodeView(QRENDER);
  assert.ok(info);
  assert.equal(info.signature, "NB08");
  assert.equal(info.modules.length, 304);
  assert.equal(info.segments.length, 282);
  assert.equal(info.symbols.filter((symbol) => symbol.kind === "public").length, 2104);
  assert.equal(info.symbols.filter((symbol) => symbol.kind === "proc").length, 245);
  assert.equal(info.symbols.filter((symbol) => symbol.kind === "label").length, 6);
  assert.deepEqual(info.warnings, []);
});
