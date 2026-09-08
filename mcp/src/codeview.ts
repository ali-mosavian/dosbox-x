import { readFileSync } from "fs";

import { mzImage } from "./mzexe.js";

/**
 * Microsoft CodeView debug info as CVPACK/LINK appends it to a linked MZ EXE.
 *
 * Not the same shape as the pre-link CodeView BC leaves in an OMF .OBJ, where
 * every record's length and kind are one byte each and there is no directory:
 * here records are [length:u16][kind:u16][data] and the whole program's symbols
 * hang off one subsection directory, addressed by logical segment index.
 */

export const CV_SIGNATURES = ["NB00", "NB01", "NB02", "NB03", "NB04", "NB05", "NB06", "NB07", "NB08", "NB09", "NB10", "NB11"] as const;
export type CvSignature = (typeof CV_SIGNATURES)[number];

// CV4 (NB05/NB08/NB09) and CV5 (NB11) subsection kinds.
export const sst = {
  Module: 0x120,
  Types: 0x121,
  Public: 0x122,
  PublicSym: 0x123,
  Symbols: 0x124,
  AlignSym: 0x125,
  SrcLnSeg: 0x126,
  SrcModule: 0x127,
  Libraries: 0x128,
  GlobalSym: 0x129,
  GlobalPub: 0x12a,
  GlobalTypes: 0x12b,
  MPC: 0x12c,
  SegMap: 0x12d,
  SegName: 0x12e,
  PreComp: 0x12f,
  PreCompMap: 0x130,
  OffsetMap16: 0x131,
  OffsetMap32: 0x132,
  FileIndex: 0x133,
  StaticSym: 0x134,
} as const;

// Symbol record kinds. The 32-bit forms are here because a mixed-model image
// can carry them; every other kind is stepped over by its own length.
export const S = {
  LDATA16: 0x0101,
  GDATA16: 0x0102,
  PUB16: 0x0103,
  LPROC16: 0x0104,
  GPROC16: 0x0105,
  LABEL16: 0x0109,
  LDATA32: 0x0201,
  GDATA32: 0x0202,
  PUB32: 0x0203,
} as const;

export type CvSymbolKind = "public" | "global-data" | "local-data" | "proc" | "label";

export interface CvSymbol {
  name: string;
  segment: number;
  offset: number;
  kind: CvSymbolKind;
  /** Present on proc records; the code length CV writes down. */
  size?: number;
  module?: string;
  moduleIndex?: number;
  type?: number;
}

export interface CvSegInfo {
  segment: number;
  offset: number;
  length: number;
}

export interface CvModule {
  index: number;
  name: string;
  style: string;
  segments: CvSegInfo[];
}

export interface CvSegMapEntry {
  index: number;
  flags: number;
  ovl: number;
  group: number;
  frame: number;
  segNameIndex: number;
  classNameIndex: number;
  offset: number;
  length: number;
}

export interface CvLine {
  line: number;
  offset: number;
}

export interface CvLineTable {
  moduleIndex: number;
  module?: string;
  file: string;
  segment: number;
  lines: CvLine[];
}

export interface CvDirEntry {
  subsection: number;
  moduleIndex: number;
  offset: number;
  length: number;
}

export interface CvInfo {
  signature: CvSignature;
  /** File offset of the signature that starts the debug block. */
  base: number;
  directory: CvDirEntry[];
  modules: CvModule[];
  symbols: CvSymbol[];
  segments: CvSegMapEntry[];
  lines: CvLineTable[];
  warnings: string[];
}

function pstr(data: Buffer, at: number): { text: string; next: number } {
  if (at >= data.length) return { text: "", next: at };
  const len = data[at];
  const end = Math.min(at + 1 + len, data.length);
  return { text: data.toString("latin1", at + 1, end), next: at + 1 + len };
}

function readSignature(data: Buffer, at: number): CvSignature | undefined {
  if (at < 0 || at + 8 > data.length) return undefined;
  const text = data.toString("latin1", at, at + 4);
  return (CV_SIGNATURES as readonly string[]).includes(text) ? (text as CvSignature) : undefined;
}

/**
 * Where the debug block starts, or undefined if the file carries none.
 *
 * The trailer's u32 is a distance measured backwards from the END of the
 * file, not an absolute offset -- so a byte appended after the trailer would
 * break it. The MZ image end is a second candidate for that case, and it is
 * only approximately where the block starts: qrender-cv.exe's lands exactly
 * there, cvprobe.exe's two bytes later, so it is a fallback and not the
 * primary.
 */
export function findCvBase(data: Buffer): { base: number; signature: CvSignature } | undefined {
  const candidates: number[] = [];

  if (data.length >= 8) {
    const trailerSignature = data.toString("latin1", data.length - 8, data.length - 4);
    if ((CV_SIGNATURES as readonly string[]).includes(trailerSignature)) {
      candidates.push(data.length - data.readUInt32LE(data.length - 4));
    }
  }

  const image = mzImage(data);
  if (image !== undefined && image.appendedOffset < data.length) {
    candidates.push(image.appendedOffset);
  }

  for (const base of candidates) {
    const signature = readSignature(data, base);
    if (signature !== undefined) return { base, signature };
  }
  return undefined;
}

function readDirectory(data: Buffer, base: number, isCv3: boolean, warnings: string[]): CvDirEntry[] {
  const lfoDirectory = data.readUInt32LE(base + 4);
  const at = base + lfoDirectory;
  // The CV4 header is 8 bytes before cDir has been read; CV3's count is 2.
  if (at + (isCv3 ? 2 : 8) > data.length) {
    warnings.push(`directory offset ${lfoDirectory} past end of file`);
    return [];
  }

  // CV3 has no directory header: a bare u16 count followed by 12-byte entries
  // whose length field is 16-bit.
  if (isCv3) {
    const count = data.readUInt16LE(at);
    const entries: CvDirEntry[] = [];
    for (let i = 0; i < count; i++) {
      const off = at + 2 + i * 12;
      if (off + 12 > data.length) break;
      entries.push({
        subsection: data.readUInt16LE(off),
        moduleIndex: data.readUInt16LE(off + 2),
        offset: data.readUInt32LE(off + 4),
        length: data.readUInt16LE(off + 8),
      });
    }
    return entries;
  }

  const cbDirHeader = data.readUInt16LE(at);
  const cbDirEntry = data.readUInt16LE(at + 2);
  const cDir = data.readUInt32LE(at + 4);
  if (cbDirEntry < 12 || cbDirHeader < 8) {
    warnings.push(`implausible directory header cbDirHeader=${cbDirHeader} cbDirEntry=${cbDirEntry}`);
    return [];
  }

  const entries: CvDirEntry[] = [];
  for (let i = 0; i < cDir; i++) {
    const off = at + cbDirHeader + i * cbDirEntry;
    if (off + cbDirEntry > data.length) break;
    entries.push({
      subsection: data.readUInt16LE(off),
      moduleIndex: data.readUInt16LE(off + 2),
      offset: data.readUInt32LE(off + 4),
      length: data.readUInt32LE(off + 8),
    });
  }
  if (entries.length !== cDir) {
    warnings.push(`directory claims ${cDir} entries, ${entries.length} fit in the file`);
  }
  return entries;
}

function parseModule(body: Buffer, index: number): CvModule | undefined {
  if (body.length < 8) return undefined;
  const cSeg = body.readUInt16LE(4);
  const style = body.toString("latin1", 6, 8);
  const segments: CvSegInfo[] = [];
  let at = 8;
  for (let i = 0; i < cSeg; i++) {
    if (at + 12 > body.length) break;
    segments.push({
      segment: body.readUInt16LE(at),
      offset: body.readUInt32LE(at + 4),
      length: body.readUInt32LE(at + 8),
    });
    at += 12;
  }
  return { index, name: pstr(body, at).text, style, segments };
}

function parseSegMap(body: Buffer): CvSegMapEntry[] {
  if (body.length < 4) return [];
  // OpenWatcom's DIP allocates cSegLog descriptors, but VBDOS LINK writes
  // cSeg of them and cSeg is the larger of the two on both fixtures (72 vs
  // 70, 282 vs 280). Taking the smaller drops the last descriptors and any
  // symbol in them resolves nowhere.
  const count = Math.max(body.readUInt16LE(0), body.readUInt16LE(2));
  const entries: CvSegMapEntry[] = [];
  for (let i = 0; i < count; i++) {
    const at = 4 + i * 20;
    if (at + 20 > body.length) break;
    entries.push({
      index: i + 1,
      flags: body.readUInt16LE(at),
      ovl: body.readUInt16LE(at + 2),
      group: body.readUInt16LE(at + 4),
      frame: body.readUInt16LE(at + 6),
      segNameIndex: body.readUInt16LE(at + 8),
      classNameIndex: body.readUInt16LE(at + 10),
      offset: body.readUInt32LE(at + 12),
      length: body.readUInt32LE(at + 16),
    });
  }
  return entries;
}

function parseSymbol(body: Buffer, at: number, kindCode: number, moduleIndex: number): CvSymbol | undefined {
  switch (kindCode) {
    case S.PUB16:
    case S.LDATA16:
    case S.GDATA16: {
      if (at + 6 > body.length) return undefined;
      const name = pstr(body, at + 6).text;
      return {
        name,
        offset: body.readUInt16LE(at),
        segment: body.readUInt16LE(at + 2),
        type: body.readUInt16LE(at + 4),
        kind: kindCode === S.PUB16 ? "public" : kindCode === S.GDATA16 ? "global-data" : "local-data",
        moduleIndex,
      };
    }
    case S.PUB32:
    case S.LDATA32:
    case S.GDATA32: {
      if (at + 8 > body.length) return undefined;
      return {
        name: pstr(body, at + 8).text,
        offset: body.readUInt32LE(at),
        segment: body.readUInt16LE(at + 4),
        type: body.readUInt16LE(at + 6),
        kind: kindCode === S.PUB32 ? "public" : kindCode === S.GDATA32 ? "global-data" : "local-data",
        moduleIndex,
      };
    }
    case S.LPROC16:
    case S.GPROC16: {
      // pParent/pEnd/pNext u32 each, then procLength, debugStart, debugEnd,
      // offset, segment, procType, flags u8, name.
      if (at + 25 > body.length) return undefined;
      return {
        name: pstr(body, at + 25).text,
        size: body.readUInt16LE(at + 12),
        offset: body.readUInt16LE(at + 18),
        segment: body.readUInt16LE(at + 20),
        type: body.readUInt16LE(at + 22),
        kind: "proc",
        moduleIndex,
      };
    }
    case S.LABEL16: {
      if (at + 5 > body.length) return undefined;
      return {
        name: pstr(body, at + 5).text,
        offset: body.readUInt16LE(at),
        segment: body.readUInt16LE(at + 2),
        kind: "label",
        moduleIndex,
      };
    }
    default:
      return undefined;
  }
}

/**
 * Walk a run of [length:u16][kind:u16][data] records.
 *
 * sstSymbols/sstAlignSym/sstStaticSym open with a 4-byte version signature
 * (1 for CV4) that is not a record. It is recognised rather than assumed per
 * subsection: a record length below 2 cannot be one, since the length always
 * covers at least the kind. Reading past it costs the whole subsection --
 * cvprobe.exe's two sstAlignSym blocks yielded 0 symbols before this.
 */
export function parseSymbolRun(body: Buffer, moduleIndex: number, from = 0, to = body.length): CvSymbol[] {
  const out: CvSymbol[] = [];
  let at = from;
  if (at + 4 <= to && body.readUInt16LE(at) < 2) at += 4;
  while (at + 4 <= to) {
    const length = body.readUInt16LE(at);
    if (length < 2) break;
    const kindCode = body.readUInt16LE(at + 2);
    const symbol = parseSymbol(body, at + 4, kindCode, moduleIndex);
    if (symbol !== undefined && symbol.name.length > 0) out.push(symbol);
    at += 2 + length;
  }
  return out;
}

/**
 * sstGlobalPub / sstGlobalSym: a 16-byte hash header, then the symbol area.
 *
 * cbSymbol bounds the symbols; what follows is the name and address hash
 * tables, which are not records. Both fixtures happen to yield the same
 * symbols without the bound -- the extra records decode to kind 0 and are
 * dropped -- so this is right by the format, not by measurement.
 */
function parseGlobalSymbols(body: Buffer, moduleIndex: number): CvSymbol[] {
  if (body.length < 16) return [];
  const cbSymbol = body.readUInt32LE(4);
  const end = Math.min(16 + cbSymbol, body.length);
  return parseSymbolRun(body, moduleIndex, 16, end);
}

function parseSrcModule(body: Buffer, moduleIndex: number): CvLineTable[] {
  if (body.length < 4) return [];
  const cFile = body.readUInt16LE(0);
  const tables: CvLineTable[] = [];

  for (let f = 0; f < cFile; f++) {
    const ptrAt = 4 + f * 4;
    if (ptrAt + 4 > body.length) break;
    const fileAt = body.readUInt32LE(ptrAt);
    if (fileAt + 4 > body.length) continue;

    const fileSegs = body.readUInt16LE(fileAt);
    const nameAt = fileAt + 4 + fileSegs * 4 + fileSegs * 8;
    const file = nameAt < body.length ? pstr(body, nameAt).text : "";

    for (let s = 0; s < fileSegs; s++) {
      const lnPtrAt = fileAt + 4 + s * 4;
      if (lnPtrAt + 4 > body.length) break;
      const lnAt = body.readUInt32LE(lnPtrAt);
      if (lnAt + 4 > body.length) continue;
      const segment = body.readUInt16LE(lnAt);
      const cPair = body.readUInt16LE(lnAt + 2);
      const offsetsAt = lnAt + 4;
      const linesAt = offsetsAt + cPair * 4;
      if (linesAt + cPair * 2 > body.length) continue;
      const lines: CvLine[] = [];
      for (let p = 0; p < cPair; p++) {
        lines.push({
          offset: body.readUInt32LE(offsetsAt + p * 4),
          line: body.readUInt16LE(linesAt + p * 2),
        });
      }
      tables.push({ moduleIndex, file, segment, lines });
    }
  }
  return tables;
}

export function parseCodeView(data: Buffer): CvInfo | undefined {
  const found = findCvBase(data);
  if (found === undefined) return undefined;

  const warnings: string[] = [];
  const { base, signature } = found;
  const isCv3 = signature === "NB00" || signature === "NB01" || signature === "NB02";
  const directory = readDirectory(data, base, isCv3, warnings);
  if (isCv3) {
    // The CV3 subsection numbers are known; their record layouts are not, and
    // no CV3 binary was available to measure against. Say so rather than
    // reporting whatever the CV4 readers make of the bytes.
    warnings.push(`${signature} is a pre-CV4 layout; only the subsection directory is read`);
    return { signature, base, directory, modules: [], symbols: [], segments: [], lines: [], warnings };
  }

  const modules: CvModule[] = [];
  const symbols: CvSymbol[] = [];
  let segments: CvSegMapEntry[] = [];
  const lines: CvLineTable[] = [];

  for (const entry of directory) {
    const at = base + entry.offset;
    if (at + entry.length > data.length) {
      warnings.push(`subsection ${entry.subsection.toString(16)} runs past end of file`);
      continue;
    }
    const body = data.subarray(at, at + entry.length);

    switch (entry.subsection) {
      case sst.Module: {
        const module = parseModule(body, entry.moduleIndex);
        if (module !== undefined) modules.push(module);
        break;
      }
      case sst.PublicSym:
      case sst.Symbols:
      case sst.AlignSym:
      case sst.StaticSym:
        symbols.push(...parseSymbolRun(body, entry.moduleIndex));
        break;
      case sst.GlobalPub:
      case sst.GlobalSym:
        symbols.push(...parseGlobalSymbols(body, entry.moduleIndex));
        break;
      case sst.SegMap:
        segments = parseSegMap(body);
        break;
      case sst.SrcModule:
        lines.push(...parseSrcModule(body, entry.moduleIndex));
        break;
      default:
        break;
    }
  }

  const moduleNames = new Map(modules.map((module) => [module.index, module.name]));
  for (const symbol of symbols) {
    if (symbol.moduleIndex !== undefined) symbol.module = moduleNames.get(symbol.moduleIndex);
  }
  for (const table of lines) table.module = moduleNames.get(table.moduleIndex);

  return { signature, base, directory, modules, symbols, segments, lines, warnings };
}

export function readCodeView(file: string): CvInfo | undefined {
  return parseCodeView(readFileSync(file));
}
