import assert from "node:assert/strict";
import test from "node:test";

import { j } from "./report.js";

test("j marks an unawaited Promise instead of writing an empty object", () => {
  const rendered = j({ sourceLine: Promise.resolve("TDSPROBE.C:30") });

  assert.match(rendered, /<unawaited Promise>/);
  assert.doesNotMatch(rendered, /"sourceLine": \{\}/);
});

test("j pretty-prints ordinary values unchanged", () => {
  assert.equal(j({ a: 1 }), '{\n  "a": 1\n}');
});
