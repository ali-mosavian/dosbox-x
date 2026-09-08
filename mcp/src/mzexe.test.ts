import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { mzFingerprint, mzImage, parseMzHeader } from "./mzexe.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");

test("mzImage puts the appended block exactly where LINK wrote the CV signature", () => {
  const data = readFileSync(join(FIXTURES, "qrender-cv.exe"));
  const image = mzImage(data);
  assert.ok(image);
  assert.equal(image.appendedOffset, 484868);
  assert.equal(data.toString("latin1", image.appendedOffset, image.appendedOffset + 4), "NB08");
});

test("parseMzHeader rejects a file that is not an MZ image", () => {
  assert.equal(parseMzHeader(Buffer.from("not an exe at all, but long enough")), undefined);
});

test("mzFingerprint separates two programs of the same name", () => {
  const probe = parseMzHeader(readFileSync(join(FIXTURES, "cvprobe.exe")));
  const qrender = parseMzHeader(readFileSync(join(FIXTURES, "qrender-cv.exe")));
  assert.ok(probe && qrender);
  assert.notEqual(mzFingerprint(probe), mzFingerprint(qrender));
});
