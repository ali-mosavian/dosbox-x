import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { findTdInfoBase, parseBorland, readBorland } from "./borland.js";
import { parseDebugInfo } from "./debuginfo.js";
import { loadLinkMapFile } from "./linkmap.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");
const PROBE = join(FIXTURES, "tdsprobe.exe");

test("readBorland finds TDINFO at the MZ image end", () => {
  const info = readBorland(PROBE);
  assert.ok(info);
  assert.equal(info.base, 8512);
  assert.equal(info.version, "TDINFO 3.16");
  assert.deepEqual(info.modules, [{ index: 1, name: "TDSPROBE" }]);
  assert.deepEqual(info.warnings, []);
});

test("TDINFO symbols land where tdsprobe.map puts them", () => {
  // The EXE and the MAP come from the same TLINK run, so a differing address
  // can only be this parser.
  const info = readBorland(PROBE);
  assert.ok(info);
  const map = loadLinkMapFile(join(FIXTURES, "tdsprobe.map"));

  let agree = 0;
  let disagree = 0;
  for (const symbol of info.symbols) {
    const publicSymbol = map.publics.get(symbol.name);
    if (publicSymbol === undefined) continue;
    if ((symbol.segment << 4) + symbol.offset === publicSymbol.mapOffset) agree++;
    else disagree++;
  }
  assert.equal(disagree, 0);
  assert.equal(agree, 46);
});

test("an AUTO symbol's BP displacement is signed", () => {
  // Read unsigned, tdsprobe's local `t` comes back as 65534 instead of -2.
  const info = readBorland(PROBE);
  assert.ok(info);
  const local = info.symbols.find((symbol) => symbol.symbolClass === "auto" && symbol.name === "t");
  assert.equal(local?.offset, -2);
});

test("a standalone .TDS parses to the same block TDSTRIP removed", () => {
  const embedded = readBorland(PROBE);
  const sidecar = readBorland(join(FIXTURES, "tdsprobe.tds"));
  assert.ok(embedded && sidecar);
  assert.equal(sidecar.base, 0);
  assert.deepEqual(sidecar.symbols, embedded.symbols);
});

test("parseDebugInfo reads a stripped EXE through its .TDS sidecar", () => {
  const info = parseDebugInfo(join(FIXTURES, "tdsprobe-stripped.exe"), 0x1000);
  assert.ok(info);
  assert.equal(info.format, "tdinfo");
  const symbol = info.symbols.find((entry) => entry.name === "_main");
  assert.equal(symbol?.linear, 0x1000 + (0x1a3 << 4) + 0x44);
});

test("a CodeView EXE is not mistaken for Borland TDINFO", () => {
  // Both formats put their block at the end of the MZ image, so detection has
  // to be by magic and nothing else.
  const data = readFileSync(join(FIXTURES, "qrender-cv.exe"));
  assert.equal(findTdInfoBase(data), undefined);
  assert.equal(parseBorland(data), undefined);
});
