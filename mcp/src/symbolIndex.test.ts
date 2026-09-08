import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  SymbolIndex,
  loadSidecarSymbols,
} from "./symbolIndex.js";

test("SymbolIndex resolves exact and nearest symbol plus offset", () => {
  const index = new SymbolIndex();
  index.add({
    name: "d32x_thunk16",
    linear: 0x123450,
    offset: 0x50,
    source: "map",
    space: "D32WRAP",
    size: 0x40,
    explanation: "test",
  });

  assert.equal(index.resolve("d32x_thunk16")?.linear, 0x123450);
  assert.equal(index.describe(0x123453), "d32x_thunk16+0x00000003");
  assert.equal(index.describe(0x123490), undefined);
});

test("loadSidecarSymbols resolves segment-relative EXE16 labels", () => {
  const dir = mkdtempSync(join(tmpdir(), "exe16-symbols-"));
  const file = join(dir, "D32TEST.EXE.sym.json");
  writeFileSync(file, JSON.stringify({
    symbols: [
      {
        name: "d32x_prepatch",
        segmentName: "D32WRAP",
        offset: 0xbd5,
        source: "wrapper",
      },
    ],
  }));

  const segmentStarts = new Map<string, number>([["D32WRAP", 0x250]]);
  const symbols = loadSidecarSymbols(file, 0x20000, segmentStarts);

  assert.equal(symbols.length, 1);
  assert.equal(symbols[0].name, "d32x_prepatch");
  assert.equal(symbols[0].linear, 0x20000 + 0x250 + 0xbd5);
  assert.equal(symbols[0].source, "exe16-sidecar");
});

test("SymbolIndex resolves module-qualified names case-insensitively", () => {
  const index = new SymbolIndex();
  index.add({
    name: "loader_body",
    linear: 0x810000,
    offset: 0x25d,
    source: "d32-debug",
    space: "code",
    module: "libc.d32",
    explanation: "test",
  });

  assert.equal(index.resolve("libc!loader_body")?.linear, 0x810000);
  assert.equal(index.resolve("LIBC.D32!loader_body")?.linear, 0x810000);
  assert.equal(index.resolve("missing!loader_body"), undefined);
});
