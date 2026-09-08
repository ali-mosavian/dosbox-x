import assert from "node:assert/strict";
import test from "node:test";

import { symbolFromSocket, symbolsFromSocketList } from "./socketSymbols.js";

/** Copied from a live {"cmd":"sym_list"} reply, cvprobe.exe loaded at 0823. */
const LIVE_REPLY = {
  status: "ok",
  total: 725,
  matched: 3,
  lines: 23,
  symbols: [
    {
      name: "pr_add", linear: "0x000082E4", segment: 1, offset: "0x000000B4",
      source: "codeview", size: 34, module: "cvprobe.obj", program: "cvprobe.exe",
    },
    {
      name: "R_DRAW_WORLD", linear: "0x0002275E", segment: 4103, offset: "0x000003AE",
      source: "map", program: "QRENDER.EXE",
    },
    { linear: "0x00001000", source: "codeview" },
  ],
};

test("a CodeView entry keeps its size and module through the hex fields", () => {
  const symbol = symbolFromSocket(LIVE_REPLY.symbols[0]);
  assert.ok(symbol);
  assert.equal(symbol.name, "pr_add");
  assert.equal(symbol.linear, 0x82e4);
  assert.equal(symbol.offset, 0xb4);
  assert.equal(symbol.source, "codeview");
  assert.equal(symbol.size, 34);
  assert.equal(symbol.module, "cvprobe.obj");
  assert.equal(symbol.space, "seg1");
});

test("a map entry carries no size, and undefined must not become 0", () => {
  // size 0 makes SymbolIndex.nearest treat the symbol as covering nothing, so
  // an address inside it would resolve to whatever lies further below.
  const symbol = symbolFromSocket(LIVE_REPLY.symbols[1]);
  assert.ok(symbol);
  assert.equal(symbol.source, "map");
  assert.equal(symbol.size, undefined);
  assert.equal(symbol.linear, 0x2275e);
});

test("an entry with no name or no address is dropped, not indexed at 0", () => {
  assert.equal(symbolFromSocket(LIVE_REPLY.symbols[2]), undefined);
  assert.equal(symbolFromSocket({ name: "x" }), undefined);
  assert.equal(symbolFromSocket(undefined), undefined);
});

test("symbolsFromSocketList reads the reply and skips what it cannot use", () => {
  assert.deepEqual(symbolsFromSocketList(LIVE_REPLY).map((symbol) => symbol.name), ["pr_add", "R_DRAW_WORLD"]);
  assert.deepEqual(symbolsFromSocketList({ status: "error" }), []);
});
