import { readFileSync } from "fs";

/**
 * Watcom ("WAT") debug info, as wlink appends it to a DOS EXE.
 *
 * Found from the END of the file, not from the MZ image end: the master header
 * is the last 14 bytes, and other Watcom trailers (FOX/resource) may be stacked
 * on top of it, each skipped by its own size. The MZ header's declared length
 * deliberately excludes the block.
 *
 * Layouts from OpenWatcom's `bld/watcom/h/wdbginfo.h` and `bld/dip/watcom/c/
 * watldsym.c`/`watgbl.c`. UNVERIFIED against a real Watcom binary -- there is
 * no OpenWatcom toolchain in this tree to produce one -- so this reads the
 * format as documented and reports what it finds rather than being proven
 * against known addresses the way the CodeView reader is.
 */

export const WAT_DBG_SIGNATURE = 0x8386;
/** Other Watcom trailers that can sit after the debug block; skipped by size. */
const STACKED_SIGNATURES = new Set([0x8300, 0x8301, 0x8302]);

export const GBL_KIND = { STATIC: 0x01, DATA: 0x02, CODE: 0x04 } as const;

export interface WatSymbol {
  name: string;
  segment: number;
  offset: number;
  moduleIndex: number;
  kind: number;
}

export interface WatModule {
  index: number;
  name: string;
}

export interface WatInfo {
  version: string;
  base: number;
  symbols: WatSymbol[];
  modules: WatModule[];
  warnings: string[];
}

interface MasterHeader {
  exeMajor: number;
  exeMinor: number;
  objMajor: number;
  objMinor: number;
  langSize: number;
  segmentSize: number;
  debugSize: number;
  /** File offset of this header. */
  at: number;
}

function readMaster(data: Buffer, at: number): { signature: number; header: MasterHeader } | undefined {
  if (at < 0 || at + 14 > data.length) return undefined;
  return {
    signature: data.readUInt16LE(at),
    header: {
      exeMajor: data[at + 2],
      exeMinor: data[at + 3],
      objMajor: data[at + 4],
      objMinor: data[at + 5],
      langSize: data.readUInt16LE(at + 6),
      segmentSize: data.readUInt16LE(at + 8),
      debugSize: data.readUInt32LE(at + 10),
      at,
    },
  };
}

function findMaster(data: Buffer): MasterHeader | undefined {
  let end = data.length - 14;
  for (let guard = 0; guard < 16; guard++) {
    const found = readMaster(data, end);
    if (found === undefined) return undefined;
    if (found.signature === WAT_DBG_SIGNATURE) return found.header;
    if (!STACKED_SIGNATURES.has(found.signature)) return undefined;
    if (found.header.debugSize > end || found.header.debugSize === 0) return undefined;
    end -= found.header.debugSize;
  }
  return undefined;
}

function pstr(data: Buffer, at: number, length: number): string {
  return data.toString("latin1", at, Math.min(at + length, data.length));
}

/**
 * A section's modules, plus where each one started.
 *
 * A V2 symbol names its module by BYTE OFFSET from the section's module area,
 * not by index, so the offsets have to be kept to resolve one.
 */
function readModules(data: Buffer, from: number, to: number, firstIndex: number): {
  modules: WatModule[];
  byOffset: Map<number, number>;
} {
  const modules: WatModule[] = [];
  const byOffset = new Map<number, number>();
  let at = from;
  while (at + 21 <= to) {
    const nameLength = data[at + 20];
    byOffset.set(at - from, firstIndex + modules.length);
    modules.push({ index: firstIndex + modules.length, name: pstr(data, at + 21, nameLength) });
    at += 21 + nameLength;
  }
  return { modules, byOffset };
}

/** V2 has no kind byte; the name length sits where V3 puts the kind. */
function readGlobals(
  data: Buffer,
  from: number,
  to: number,
  v2: boolean,
  firstIndex: number,
  byOffset: Map<number, number>,
): WatSymbol[] {
  const symbols: WatSymbol[] = [];
  let at = from;
  while (at + (v2 ? 9 : 10) <= to) {
    const nameLength = v2 ? data[at + 8] : data[at + 9];
    const nameAt = at + (v2 ? 9 : 10);
    if (nameAt + nameLength > to) break;
    const mod = data.readUInt16LE(at + 6);
    symbols.push({
      name: pstr(data, nameAt, nameLength),
      offset: data.readUInt32LE(at),
      segment: data.readUInt16LE(at + 4),
      // V3's mod is a per-section index, so it needs the section's base added.
      moduleIndex: v2 ? (byOffset.get(mod) ?? -1) : firstIndex + mod,
      kind: v2 ? 0 : data[at + 8],
    });
    at = nameAt + nameLength;
  }
  return symbols;
}

export function parseWatcom(data: Buffer): WatInfo | undefined {
  const master = findMaster(data);
  if (master === undefined) return undefined;

  const warnings: string[] = [];
  const v2 = master.exeMajor === 2;
  if (master.exeMajor !== 2 && master.exeMajor !== 3) {
    warnings.push(`unsupported Watcom exe_major_ver ${master.exeMajor}`);
    return { version: `WAT ${master.exeMajor}.${master.exeMinor}`, base: master.at, symbols: [], modules: [], warnings };
  }

  const base = master.at + 14 - master.debugSize;
  if (base < 0) {
    warnings.push(`debug_size ${master.debugSize} runs past the start of the file`);
    return { version: `WAT ${master.exeMajor}.${master.exeMinor}`, base: master.at, symbols: [], modules: [], warnings };
  }

  const symbols: WatSymbol[] = [];
  const modules: WatModule[] = [];
  // Section count is not stored: walk section_size forward until the master.
  let at = base + master.langSize + master.segmentSize;
  while (at + 18 <= master.at) {
    const modOffset = data.readUInt32LE(at);
    const gblOffset = data.readUInt32LE(at + 4);
    const addrOffset = data.readUInt32LE(at + 8);
    const sectionSize = data.readUInt32LE(at + 12);
    // Sections end where the master header begins.
    if (sectionSize < 18 || at + sectionSize > master.at) break;
    if (!(modOffset <= gblOffset && gblOffset <= addrOffset && addrOffset < sectionSize)) break;
    // mod_offset == gbl_offset marks an empty overlay placeholder.
    if (modOffset !== gblOffset) {
      const read = readModules(data, at + modOffset, at + gblOffset, modules.length);
      symbols.push(...readGlobals(data, at + gblOffset, at + addrOffset, v2, modules.length, read.byOffset));
      modules.push(...read.modules);
    }
    at += sectionSize;
  }

  return {
    version: `WAT ${master.exeMajor}.${master.exeMinor}`,
    base,
    symbols,
    modules,
    warnings,
  };
}

export function readWatcom(file: string): WatInfo | undefined {
  return parseWatcom(readFileSync(file));
}
