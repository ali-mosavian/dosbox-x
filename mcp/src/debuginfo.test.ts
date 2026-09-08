import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { readCodeView } from "./codeview.js";
import { parseDebugInfo, segmentBases, sourceLineAt } from "./debuginfo.js";
import { loadLinkMapFile } from "./linkmap.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");

interface Agreement {
  agree: number;
  disagree: number;
  absent: number;
}

/**
 * Every CodeView public that the .MAP also names, checked against it.
 *
 * The two files come from the same LINK invocation, so an address that
 * differs is this parser being wrong -- there is nothing else it could be.
 */
function agreementWithMap(exe: string, map: string): Agreement {
  const info = readCodeView(join(FIXTURES, exe));
  assert.ok(info);
  const linkMap = loadLinkMapFile(join(FIXTURES, map));
  const bases = segmentBases(info);

  const counts: Agreement = { agree: 0, disagree: 0, absent: 0 };
  for (const symbol of info.symbols) {
    if (symbol.kind !== "public") continue;
    const publicSymbol = linkMap.publics.get(symbol.name) ?? linkMap.publics.get(symbol.name.toUpperCase());
    if (publicSymbol === undefined) {
      counts.absent++;
      continue;
    }
    const base = bases.get(symbol.segment);
    if (base !== undefined && base + symbol.offset === publicSymbol.mapOffset) counts.agree++;
    else counts.disagree++;
  }
  return counts;
}

test("CodeView publics land where cvprobe.map puts them", () => {
  const counts = agreementWithMap("cvprobe.exe", "cvprobe.map");
  assert.equal(counts.disagree, 0);
  assert.equal(counts.agree, 715);
});

test("CodeView publics land where QRENDER.MAP puts them", () => {
  // segmap.frame alone -- the obvious reading, and wrong -- put 345 of 715
  // cvprobe publics at the wrong address. Segments sharing a group share a
  // frame and are told apart by segmap.offset.
  const counts = agreementWithMap("qrender-cv.exe", "qrender-cv.map");
  assert.equal(counts.disagree, 0);
  assert.equal(counts.agree, 2086);
});

test("sourceLineAt answers only inside a line's own range", () => {
  // "nearest entry at or before the address" put CVPROBE's entry — which is in
  // the runtime, a module with no line info — at `..\rt\strdsp1.c:40`.
  const info = parseDebugInfo(join(FIXTURES, "cvprobe.exe"), 0);
  assert.ok(info);
  assert.equal(sourceLineAt(info, 0x2dbc), undefined);

  const first = info.lines.find((entry) => entry.file === "cvprobe.bas");
  assert.ok(first);
  assert.equal(sourceLineAt(info, first.imageOffset)?.line, first.line);
  assert.equal(sourceLineAt(info, first.endOffset - 1)?.line, first.line);
  assert.notEqual(sourceLineAt(info, first.endOffset)?.line, first.line);
});

test("parseDebugInfo applies the load base and reports modules and lines", () => {
  const info = parseDebugInfo(join(FIXTURES, "qrender-cv.exe"), 0x12340);
  assert.ok(info);
  assert.equal(info.format, "codeview");
  assert.equal(info.version, "NB08");
  assert.equal(info.modules.length, 304);
  assert.ok(info.modules.some((module) => module.name === "d_poly.obj"));

  const symbol = info.symbols.find((entry) => entry.name.toUpperCase() === "R_DRAW_WORLD");
  assert.ok(symbol);
  assert.equal(symbol.source, "codeview");

  const map = loadLinkMapFile(join(FIXTURES, "qrender-cv.map"));
  const publicSymbol = map.publics.get(symbol.name) ?? map.publics.get(symbol.name.toUpperCase());
  assert.ok(publicSymbol);
  assert.equal(symbol.linear, 0x12340 + publicSymbol.mapOffset);

  const line = info.lines.find((entry) => entry.file.toLowerCase().endsWith(".bas"));
  assert.ok(line);
  assert.ok(line.line > 0);

  // Segment 0 is CodeView's absolutes bucket and is never in sstSegMap, so
  // this warning is expected. Pinning it means a NEW warning gets noticed
  // instead of joining a list nobody reads.
  assert.deepEqual(info.warnings, ["no sstSegMap entry for segment(s) 0"]);
});
