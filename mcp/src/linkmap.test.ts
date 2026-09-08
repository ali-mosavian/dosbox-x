import assert from "node:assert/strict";
import test from "node:test";

import {
  describeMapAddress,
  parseLinkMap,
  resolveLinkMapSymbol,
} from "./linkmap.js";

const SAMPLE_MAP = `
 Start  Stop   Length Name                   Class
 00000H 0024BH 0024CH D32TEST_CODE           BC_CODE
 00250H 00EA1H 00C52H D32WRAP                CODE
 00EA2H 04461H 035C0H CODE                   CODE
 04470H 0499FH 00530H _TEXT                  CODE
 0517CH 0669FH 01524H _DATA                  DATA

 Origin   Group
 050E:0   DGROUP
 04A7:0   FMGROUP

  Address         Publics by Value

 0447:043C       _main
 0000:0250       _wrap_start

Program entry point at 0447:043C
`;

test("parseLinkMap parses segments, groups, publics, and entry point", () => {
  const map = parseLinkMap(SAMPLE_MAP, "sample.map");

  assert.equal(map.segments.length, 5);
  assert.equal(map.segments[1].name, "D32WRAP");
  assert.equal(map.segments[1].start, 0x250);
  assert.equal(map.groups[0].name, "DGROUP");
  assert.equal(map.groups[0].mapOffset, 0x50e0);
  assert.equal(map.publics.get("_main")?.mapOffset, 0x48ac);
  assert.equal(map.entryPoint?.mapOffset, 0x48ac);
});

test("resolveLinkMapSymbol resolves segment plus offset specs", () => {
  const map = parseLinkMap(SAMPLE_MAP, "sample.map");
  const resolved = resolveLinkMapSymbol(map, "D32WRAP+0x250", { loadSeg: 0x1234 });

  assert.equal(resolved.linear, 0x12340 + 0x250 + 0x250);
  assert.equal(resolved.mapOffset, 0x4a0);
});

test("resolveLinkMapSymbol resolves segment colon specs and publics", () => {
  const map = parseLinkMap(SAMPLE_MAP, "sample.map");
  const loadInfo = { loadLinear: 0x20000 };

  assert.equal(resolveLinkMapSymbol(map, "D32WRAP:0", loadInfo).linear, 0x20250);
  assert.equal(resolveLinkMapSymbol(map, "_main", loadInfo).linear, 0x248ac);
  assert.equal(resolveLinkMapSymbol(map, "entry", loadInfo).linear, 0x248ac);
});

test("describeMapAddress returns the nearest map symbol context", () => {
  const map = parseLinkMap(SAMPLE_MAP, "sample.map");

  assert.equal(describeMapAddress(map, { loadLinear: 0x20000 }, 0x20250), "_wrap_start");
  assert.equal(describeMapAddress(map, { loadLinear: 0x20000 }, 0x204a0), "D32WRAP+0x00000250");
});
