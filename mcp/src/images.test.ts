import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { d32ModulesFromImages, findD32ByName, watchedImages } from "./images.js";

function d32Module(): Buffer {
  const bytes = Buffer.alloc(220);
  bytes.writeUInt32LE(0x58323344, 0);
  bytes.writeUInt32LE(180, 8);
  bytes.writeUInt32LE(72, 20);
  bytes.writeUInt32LE(16, 24);
  bytes.writeUInt32LE(112, 52);
  bytes.writeUInt32LE(16, 56);
  bytes.writeUInt32LE(128, 60);
  bytes.writeUInt32LE(4, 64);
  bytes.writeUInt32LE(4, 68);
  return bytes;
}

test("watchedImages reads the emulator's images answer", () => {
  const images = watchedImages({
    status: "ok",
    images: [{ program: "C:\\MODS\\ADD.D32", objects: 3, complete: true, placed: [{ object: 1, linear: "0x00200070" }] }],
  });
  assert.deepEqual(images, [{
    program: "C:\\MODS\\ADD.D32",
    objects: 3,
    complete: true,
    placed: [{ object: 1, linear: 0x200070 }],
  }]);
});

test("an emulator without the images command falls back to the memory scan", () => {
  assert.equal(watchedImages({ status: "error", msg: "Unknown command" }), undefined);
});

test("any other failure of images is an error, not a reason to scan", () => {
  assert.throws(() => watchedImages({ status: "error", msg: "Timeout" }), /images failed/);
  assert.throws(() => watchedImages({ status: "ok" }), /images failed/);
});

test("d32ModulesFromImages puts the module base where the code object, less its offset in the module, is", () => {
  const dir = mkdtempSync(join(tmpdir(), "images-"));
  writeFileSync(join(dir, "add.d32"), d32Module());

  const modules = d32ModulesFromImages([
    { program: "C:\\MODS\\ADD.D32", objects: 3, complete: true, placed: [{ object: 1, linear: 0x200070 }] },
    { program: "C:\\GAME.EXE", objects: 1, complete: true, placed: [{ object: 1, linear: 0x300000 }] },
    { program: "C:\\MODS\\NOPE.D32", objects: 3, complete: false, placed: [] },
  ], [dir]);

  assert.equal(modules.length, 1);
  assert.equal(modules[0].probe.base, 0x200000);
  assert.equal(modules[0].probe.formatId, "d32x");
  assert.equal(modules[0].file, join(dir, "add.d32"));
});

test("findD32ByName matches the guest's file name without regard to case or directory", () => {
  const dir = mkdtempSync(join(tmpdir(), "images-"));
  writeFileSync(join(dir, "Mixed.D32"), d32Module());
  assert.equal(findD32ByName("c:/x/MIXED.d32", [dir]), join(dir, "Mixed.D32"));
  assert.equal(findD32ByName("c:/x/OTHER.d32", [dir]), undefined);
});
