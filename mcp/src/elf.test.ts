import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { parseElf32Symbols } from "./elf.js";
import { SymbolIndex } from "./symbolIndex.js";

function writeElf32(path: string, symbols: Array<{ name: string; value: number; size: number; type: number }>): void {
  const strtab = Buffer.from("\0main\0data_obj\0", "ascii");
  const symtab = Buffer.alloc(Math.max(16, symbols.length * 16));
  for (let index = 0; index < symbols.length; index++) {
    const symbol = symbols[index];
    const nameOffset = symbol.name === "main" ? 1 : 6;
    symtab.writeUInt32LE(nameOffset, index * 16);
    symtab.writeUInt32LE(symbol.value, index * 16 + 4);
    symtab.writeUInt32LE(symbol.size, index * 16 + 8);
    symtab[index * 16 + 12] = symbol.type;
  }

  const shstr = Buffer.from("\0.symtab\0.strtab\0.shstrtab\0", "ascii");
  const symtabOff = 52 + 4 * 40;
  const strtabOff = symtabOff + symtab.length;
  const shstrOff = strtabOff + strtab.length;
  const headers = Buffer.alloc(4 * 40);

  const writeSection = (
    index: number,
    nameOff: number,
    type: number,
    offset: number,
    size: number,
    link = 0,
  ) => {
    const off = index * 40;
    headers.writeUInt32LE(nameOff, off);
    headers.writeUInt32LE(type, off + 4);
    headers.writeUInt32LE(offset, off + 16);
    headers.writeUInt32LE(size, off + 20);
    headers.writeUInt32LE(link, off + 24);
  };

  writeSection(0, 0, 0, 0, 0);
  writeSection(1, 1, 2, symtabOff, symtab.length, 2);
  writeSection(2, 8, 3, strtabOff, strtab.length);
  writeSection(3, 16, 3, shstrOff, shstr.length);

  const elf = Buffer.concat([
    Buffer.alloc(52),
    headers,
    symtab,
    strtab,
    shstr,
  ]);

  elf.write("\x7fELF", 0, "latin1");
  elf[4] = 1;
  elf[5] = 1;
  elf.writeUInt16LE(2, 16);
  elf.writeUInt16LE(0x386, 18);
  elf.writeUInt32LE(52, 32);
  elf.writeUInt16LE(40, 46);
  elf.writeUInt16LE(4, 48);
  elf.writeUInt16LE(3, 50);
  writeFileSync(path, elf);
}

test("parseElf32Symbols reads FUNC and OBJECT symbols at base + st_value", () => {
  const dir = mkdtempSync(join(tmpdir(), "elf-test-"));
  const file = join(dir, "blob.elf");
  writeElf32(file, [
    { name: "main", value: 0x120, size: 0x10, type: 2 },
    { name: "data_obj", value: 0x200, size: 4, type: 1 },
  ]);

  const symbols = parseElf32Symbols(file, 0x100000);
  assert.equal(symbols.length, 2);
  assert.equal(symbols[0].name, "main");
  assert.equal(symbols[0].linear, 0x100120);
  assert.equal(symbols[0].source, "elf");

  const index = new SymbolIndex();
  index.addMany(symbols);
  assert.equal(index.describe(0x100125), "main+0x00000005");
});
