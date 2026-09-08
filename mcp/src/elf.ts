import { readFileSync } from "fs";

import { hex } from "./symbols.js";
import type { UnifiedSymbol } from "./symbolIndex.js";

const ELF_MAGIC = "\x7fELF";
const ELFCLASS32 = 1;
const ELFDATA2LSB = 1;
const ET_REL = 1;
const ET_EXEC = 2;
const ET_DYN = 3;
const SHT_SYMTAB = 2;
const SHT_STRTAB = 3;
const STT_FUNC = 2;
const STT_OBJECT = 1;
const STT_NOTYPE = 0;

function u16(data: Buffer, off: number): number {
  return data.readUInt16LE(off);
}

function u32(data: Buffer, off: number): number {
  return data.readUInt32LE(off);
}

function cstr(data: Buffer, off: number): string {
  let end = off;
  while (end < data.length && data[end] !== 0) end++;
  return data.toString("utf8", off, end);
}

function symbolTypeName(type: number): string {
  if (type === STT_FUNC) return "func";
  if (type === STT_OBJECT) return "object";
  return "notype";
}

export function parseElf32Symbols(file: string, base: number): UnifiedSymbol[] {
  const data = readFileSync(file);
  if (data.length < 52 || data.toString("latin1", 0, 4) !== ELF_MAGIC) {
    throw new Error(`${file}: not an ELF file`);
  }
  if (data[4] !== ELFCLASS32 || data[5] !== ELFDATA2LSB) {
    throw new Error(`${file}: only ELF32 little-endian is supported`);
  }

  const e_type = u16(data, 16);
  if (e_type !== ET_REL && e_type !== ET_EXEC && e_type !== ET_DYN) {
    throw new Error(`${file}: unsupported ELF type ${e_type}`);
  }

  const e_shoff = u32(data, 32);
  const e_shentsize = u16(data, 46);
  const e_shnum = u16(data, 48);
  const e_shstrndx = u16(data, 50);
  if (e_shoff + e_shnum * e_shentsize > data.length) {
    throw new Error(`${file}: corrupt ELF section header table`);
  }

  const sections: Array<{ type: number; offset: number; size: number; link: number }> = [];
  for (let index = 0; index < e_shnum; index++) {
    const off = e_shoff + index * e_shentsize;
    sections.push({
      type: u32(data, off + 4),
      offset: u32(data, off + 16),
      size: u32(data, off + 20),
      link: u32(data, off + 24),
    });
  }

  const shstr = sections[e_shstrndx];
  if (shstr === undefined) {
    throw new Error(`${file}: missing section name string table`);
  }

  let symtabIndex = -1;
  for (let index = 0; index < sections.length; index++) {
    if (sections[index].type === SHT_SYMTAB) {
      symtabIndex = index;
      break;
    }
  }
  if (symtabIndex < 0) {
    throw new Error(`${file}: no SHT_SYMTAB section`);
  }

  const symtab = sections[symtabIndex];
  const strtab = sections[symtab.link];
  if (strtab === undefined || strtab.type !== SHT_STRTAB) {
    throw new Error(`${file}: symtab has no linked strtab`);
  }

  const symbols: UnifiedSymbol[] = [];
  const entrySize = 16;
  const count = Math.floor(symtab.size / entrySize);
  for (let index = 0; index < count; index++) {
    const off = symtab.offset + index * entrySize;
    if (off + entrySize > data.length) break;

    const st_name = u32(data, off);
    const st_value = u32(data, off + 4);
    const st_size = u32(data, off + 8);
    const st_info = data[off + 12];
    const st_type = st_info & 0x0f;
    if (st_type !== STT_FUNC && st_type !== STT_OBJECT && st_type !== STT_NOTYPE) continue;
    if (st_value === 0 && st_size === 0) continue;

    const name = cstr(data, strtab.offset + st_name);
    if (name.length === 0) continue;

    const linear = (base + st_value) >>> 0;
    symbols.push({
      name,
      linear,
      offset: st_value,
      source: "elf",
      space: "elf",
      size: st_size > 0 ? st_size : undefined,
      explanation: `${name} from ${file} @ base ${hex(base)} + ${hex(st_value)} (${symbolTypeName(st_type)})`,
    });
  }

  return symbols;
}
