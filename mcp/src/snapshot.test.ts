import assert from "node:assert/strict";
import test from "node:test";

import { coalesceMemoryDiff } from "./snapshot.js";

test("coalesceMemoryDiff merges adjacent dirty bytes and caps range output", () => {
  const oldBytes = Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
  const newBytes = Uint8Array.from([0, 1, 0xAA, 0xBB, 4, 5, 0xCC, 7, 8, 9]);
  const diff = coalesceMemoryDiff(oldBytes, newBytes, 0x1000, 2, 4);

  assert.equal(diff.ranges.length, 2);
  assert.equal(diff.ranges[0].start, 0x1002);
  assert.equal(diff.ranges[0].end, 0x1004);
  assert.equal(diff.ranges[0].oldHex, "0203");
  assert.equal(diff.ranges[0].newHex, "AABB");
  assert.equal(diff.ranges[1].start, 0x1006);
  assert.equal(diff.totalDirtyBytes, 3);
  assert.equal(diff.truncated, false);
});

test("coalesceMemoryDiff reports truncation when dirty ranges exceed cap", () => {
  const oldBytes = Uint8Array.from([0, 9, 0, 9, 0, 9, 0, 9, 0, 9, 0, 9, 0, 9, 0, 9]);
  const newBytes = Uint8Array.from([1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9]);
  const diff = coalesceMemoryDiff(oldBytes, newBytes, 0, 2, 16);

  assert.equal(diff.ranges.length, 2);
  assert.equal(diff.dirtyRangeCount, 8);
  assert.equal(diff.truncated, true);
});
