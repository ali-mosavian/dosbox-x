import assert from "node:assert/strict";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";

import { candidateFiles, locateProgram, parseGuestPath, parseMounts } from "./exeLocator.js";
import { parseMzHeader } from "./mzexe.js";

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures");

const CONF = `
[sdl]
autolock=true

[autoexec]
# a comment
mount C "/host/dos c"
MOUNT -t dir D /host/build
c:
QRENDER.EXE dm3ish.bsp
`;

test("parseMounts reads MOUNT lines out of [autoexec] only", () => {
  assert.deepEqual(parseMounts(CONF), [
    { drive: "C", hostPath: "/host/dos c" },
    { drive: "D", hostPath: "/host/build" },
  ]);
});

test("parseGuestPath splits a DOS path into drive and components", () => {
  assert.deepEqual(parseGuestPath("C:\\BUILD\\QRENDER.EXE"), {
    drive: "C",
    parts: ["BUILD", "QRENDER.EXE"],
    file: "QRENDER.EXE",
  });
  assert.deepEqual(parseGuestPath("QRENDER.EXE"), {
    drive: undefined,
    parts: ["QRENDER.EXE"],
    file: "QRENDER.EXE",
  });
});

/** Two build directories holding the same basename, which is the real case. */
function twoBuilds(): { root: string; wanted: string } {
  const root = mkdtempSync(join(tmpdir(), "exe-locator-"));
  mkdirSync(join(root, "a"));
  mkdirSync(join(root, "b"));
  copyFileSync(join(FIXTURES, "cvprobe.exe"), join(root, "a", "PROG.EXE"));
  copyFileSync(join(FIXTURES, "qrender-cv.exe"), join(root, "b", "PROG.EXE"));
  return { root, wanted: join(root, "b", "PROG.EXE") };
}

test("locateProgram picks the build whose MZ header the guest actually loaded", () => {
  const { root, wanted } = twoBuilds();
  const mounts = [{ drive: "C", hostPath: join(root, "a") }, { drive: "C", hostPath: join(root, "b") }];
  const header = parseMzHeader(readFileSync(wanted));
  assert.ok(header);

  const located = locateProgram("C:\\PROG.EXE", mounts, [], {
    mzExtraBytes: header.extraBytes,
    mzPages: header.pages,
    relocationCount: header.relocationCount,
    headerParagraphs: header.headerParagraphs,
    minAlloc: header.minAlloc,
    maxAlloc: header.maxAlloc,
    initSS: header.initSS,
    initSP: header.initSP,
    initIP: header.initIP,
    initCS: header.initCS,
    relocationTableOffset: header.relocationTableOffset,
  });

  assert.equal(located?.file, wanted);
  assert.equal(located?.matchedBy, "fingerprint");
  assert.equal(located?.candidates, 2);
});

test("locateProgram falls back to the first candidate when loadInfo carries no header", () => {
  const { root } = twoBuilds();
  const mounts = [{ drive: "C", hostPath: join(root, "a") }];
  const located = locateProgram("C:\\PROG.EXE", mounts, []);
  assert.equal(located?.file, join(root, "a", "PROG.EXE"));
  assert.equal(located?.matchedBy, "name");
});

test("candidateFiles searches supplied directories when no mount maps the drive", () => {
  const { root, wanted } = twoBuilds();
  const found = candidateFiles("C:\\PROG.EXE", [], [join(root, "b")]);
  assert.deepEqual(found, [wanted]);
});
