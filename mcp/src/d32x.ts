import { readFileSync } from "fs";
import { basename } from "path";

export const SECTION_CODE = 0;
export const SECTION_DATA = 1;
export const SECTION_BSS = 2;

const D32X_MAGIC = 0x58323344;
const D32S_MAGIC = 0x53323344;
const D32T_MAGIC = 0x54323344;

export interface D32Header {
  fileSize: number;
  loaderOffset: number;
  loaderSize: number;
  strtabOffset: number;
  strtabSize: number;
  moduleImportsOffset: number;
  exportsCount: number;
  importsCount: number;
  relocsCount: number;
  exportsOffset: number;
  importsOffset: number;
  relocsOffset: number;
  codeOffset: number;
  codeSize: number;
  dataOffset: number;
  dataSize: number;
  bssSize: number;
}

export interface D32Export {
  name: string;
  value: number;
  section: number;
  flags: number;
}

export interface D32Reloc {
  patchOff: number;
  targetOff: number;
  kind: number;
  patchSec: number;
  targetSec: number;
}

export interface D32DebugSymbol {
  name: string;
  value: number;
  size: number;
  section: number;
  bind: number;
  kind: number;
}

export interface D32Module {
  path: string;
  name: string;
  header: D32Header;
  exports: D32Export[];
  relocs: D32Reloc[];
  debugSymbols: D32DebugSymbol[];
}

function u16(data: Buffer, off: number): number {
  return data.readUInt16LE(off);
}

function u32(data: Buffer, off: number): number {
  return data.readUInt32LE(off);
}

function cstr(data: Buffer, base: number, rel: number): string {
  const start = base + rel;
  let end = start;
  while (end < data.length && data[end] !== 0) end++;
  return data.toString("utf8", start, end);
}

export function sectionName(section: number): "code" | "data" | "bss" | "unknown" {
  if (section === SECTION_CODE) return "code";
  if (section === SECTION_DATA) return "data";
  if (section === SECTION_BSS) return "bss";
  return "unknown";
}

export function parseD32File(path: string): D32Module {
  const data = readFileSync(path);
  if (data.length < 72 || u32(data, 0) !== D32X_MAGIC) {
    throw new Error(`${path}: not a D32X module`);
  }

  const header: D32Header = {
    fileSize: u32(data, 8),
    loaderOffset: u32(data, 12),
    loaderSize: u32(data, 16),
    strtabOffset: u32(data, 20),
    strtabSize: u32(data, 24),
    moduleImportsOffset: u32(data, 28),
    exportsCount: u16(data, 32),
    importsCount: u16(data, 34),
    relocsCount: u32(data, 36),
    exportsOffset: u32(data, 40),
    importsOffset: u32(data, 44),
    relocsOffset: u32(data, 48),
    codeOffset: u32(data, 52),
    codeSize: u32(data, 56),
    dataOffset: u32(data, 60),
    dataSize: u32(data, 64),
    bssSize: u32(data, 68),
  };

  const exports: D32Export[] = [];
  for (let index = 0; index < header.exportsCount; index++) {
    const off = header.exportsOffset + index * 12;
    exports.push({
      name: cstr(data, header.strtabOffset, u32(data, off)),
      value: u32(data, off + 4),
      section: data[off + 8],
      flags: data[off + 9],
    });
  }

  const relocs: D32Reloc[] = [];
  for (let index = 0; index < header.relocsCount; index++) {
    const off = header.relocsOffset + index * 12;
    relocs.push({
      patchOff: u32(data, off),
      targetOff: u32(data, off + 4),
      kind: data[off + 8],
      patchSec: data[off + 9],
      targetSec: data[off + 10],
    });
  }

  const debugSymbols = readDebugSymbols(data);
  return { path, name: basename(path), header, exports, relocs, debugSymbols };
}

function readDebugSymbols(data: Buffer): D32DebugSymbol[] {
  if (data.length < 12) return [];
  const trailer = data.length - 12;
  if (u32(data, trailer) !== D32T_MAGIC) return [];

  const debugOffset = u32(data, trailer + 4);
  const debugSize = u32(data, trailer + 8);
  if (debugOffset + debugSize > data.length || debugSize < 20) return [];

  const debug = data.subarray(debugOffset, debugOffset + debugSize);
  if (u32(debug, 0) !== D32S_MAGIC) return [];
  const count = u16(debug, 6);
  const symOffset = u32(debug, 8);
  const strOffset = u32(debug, 12);
  const strSize = u32(debug, 16);
  if (symOffset + count * 16 > debug.length || strOffset + strSize > debug.length) return [];

  const symbols: D32DebugSymbol[] = [];
  for (let index = 0; index < count; index++) {
    const off = symOffset + index * 16;
    symbols.push({
      name: cstr(debug, strOffset, u32(debug, off)),
      value: u32(debug, off + 4),
      size: u32(debug, off + 8),
      section: debug[off + 12],
      bind: debug[off + 13],
      kind: u16(debug, off + 14),
    });
  }
  return symbols;
}

export function sectionRuntimeBase(module: D32Module, moduleBase: number, section: number): number {
  switch (section) {
    case SECTION_CODE:
      return (moduleBase + module.header.codeOffset) >>> 0;
    case SECTION_DATA:
      return (moduleBase + module.header.dataOffset) >>> 0;
    case SECTION_BSS:
      return (moduleBase + module.header.fileSize) >>> 0;
    default:
      throw new Error(`unknown D32 section ${section}`);
  }
}

export function sectionRuntimeAddress(module: D32Module, moduleBase: number, section: number, offset: number): number {
  return (sectionRuntimeBase(module, moduleBase, section) + offset) >>> 0;
}
