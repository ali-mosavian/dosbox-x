import { readFileSync } from "fs";

import { mzImage } from "./mzexe.js";

/**
 * Borland TLink symbolic debug info as it rides in a 16-bit DOS MZ EXE.
 *
 * This is TDINFO, magic 0x52FB -- not the 32-bit "TDS" (`FB09`/`FB0A`) that
 * C++ Builder writes, which is a different format under a similar name. There
 * is no trailer to search back from: the block starts exactly where the MZ
 * header says the load image ends, and the name pool sits at the very end of
 * the file rather than with the tables it serves.
 *
 * Layouts from ramikg/tdinfo-parser's `tdinfo_structs.py`, the only complete
 * open-source description of the 16-bit form. Its source-file and line-number
 * records are skipped as padding there, so their fields are unknown and this
 * reader carries no line numbers -- see `lineRecordCount`, which reports how
 * many are present but unread.
 */

export const TDINFO_MAGIC = 0x52fb;
/** 32-bit Borland TDS, recognised so it can be reported rather than misread. */
export const TDS32_SIGNATURES = ["FB09", "FB0A"] as const;

export type TdSymbolClass =
  | "static"
  | "absolute"
  | "auto"
  | "pascal-var"
  | "register"
  | "constant"
  | "typedef"
  | "struct-union-enum";

const SYMBOL_CLASSES: TdSymbolClass[] = [
  "static",
  "absolute",
  "auto",
  "pascal-var",
  "register",
  "constant",
  "typedef",
  "struct-union-enum",
];

export interface TdSymbol {
  name: string;
  segment: number;
  offset: number;
  symbolClass: TdSymbolClass;
  type: number;
}

export interface TdModule {
  index: number;
  name: string;
}

export interface TdSegment {
  module: number;
  codeSegment: number;
  codeOffset: number;
  codeLength: number;
}

export interface TdInfo {
  version: string;
  base: number;
  symbols: TdSymbol[];
  modules: TdModule[];
  segments: TdSegment[];
  /** Line records present in the file but not decoded; their layout is unknown. */
  lineRecordCount: number;
  warnings: string[];
}

interface TdHeader {
  namesPoolSize: number;
  namesCount: number;
  typesCount: number;
  membersCount: number;
  symbolsCount: number;
  globalsCount: number;
  modulesCount: number;
  localsCount: number;
  scopesCount: number;
  lineNumbersCount: number;
  sourceFilesCount: number;
  segmentsCount: number;
  correlationsCount: number;
  extensionSize: number;
  minor: number;
  major: number;
}

function readHeader(data: Buffer, at: number): TdHeader | undefined {
  if (at + 48 > data.length || data.readUInt16LE(at) !== TDINFO_MAGIC) return undefined;
  return {
    minor: data[at + 2],
    major: data[at + 3],
    namesPoolSize: data.readUInt32LE(at + 4),
    namesCount: data.readUInt16LE(at + 8),
    typesCount: data.readUInt16LE(at + 10),
    membersCount: data.readUInt16LE(at + 12),
    symbolsCount: data.readUInt16LE(at + 14),
    globalsCount: data.readUInt16LE(at + 16),
    modulesCount: data.readUInt16LE(at + 18),
    localsCount: data.readUInt16LE(at + 20),
    scopesCount: data.readUInt16LE(at + 22),
    lineNumbersCount: data.readUInt16LE(at + 24),
    sourceFilesCount: data.readUInt16LE(at + 26),
    segmentsCount: data.readUInt16LE(at + 28),
    correlationsCount: data.readUInt16LE(at + 30),
    extensionSize: data.readUInt16LE(at + 46),
  };
}

/** Name-pool entries are NUL-terminated and indexed from 1. */
function readNamePool(data: Buffer, header: TdHeader): string[] {
  const start = data.length - header.namesPoolSize;
  if (start < 0) return [];
  const names: string[] = [];
  let at = start;
  while (at < data.length && names.length < header.namesCount) {
    let end = at;
    while (end < data.length && data[end] !== 0) end++;
    names.push(data.toString("latin1", at, end));
    at = end + 1;
  }
  return names;
}

/**
 * Where the TDINFO block starts: the MZ image end, or offset 0.
 *
 * TDSTRIP writes the block out byte-identical to what it removed, so a
 * standalone .TDS is the same bytes with no MZ header in front -- measured on
 * tdsprobe.tds, which matches tdsprobe.exe's appended 2545 bytes exactly.
 */
export function findTdInfoBase(data: Buffer): number | undefined {
  if (data.length >= 48 && data.readUInt16LE(0) === TDINFO_MAGIC) return 0;
  const image = mzImage(data);
  if (image === undefined) return undefined;
  const at = image.appendedOffset;
  if (at + 48 > data.length || data.readUInt16LE(at) !== TDINFO_MAGIC) return undefined;
  return at;
}

export function parseBorland(data: Buffer): TdInfo | undefined {
  const base = findTdInfoBase(data);
  if (base === undefined) return undefined;
  const header = readHeader(data, base);
  if (header === undefined) return undefined;

  const warnings: string[] = [];
  const names = readNamePool(data, header);
  if (names.length !== header.namesCount) {
    warnings.push(`name pool holds ${names.length} names, header claims ${header.namesCount}`);
  }
  const nameAt = (index: number): string => (index >= 1 && index <= names.length ? names[index - 1] : "");

  // The tables are a flat run in a fixed order with no directory: every one
  // must be stepped over at its own record size to reach the next.
  let at = base + 48 + header.extensionSize;

  const symbols: TdSymbol[] = [];
  for (let i = 0; i < header.symbolsCount; i++, at += 9) {
    if (at + 9 > data.length) break;
    const name = nameAt(data.readUInt16LE(at));
    if (name === "") continue;
    const symbolClass = SYMBOL_CLASSES[data[at + 8] & 0x07];
    symbols.push({
      name,
      type: data.readUInt16LE(at + 2),
      // An AUTO symbol's offset is a signed BP-relative displacement, not an
      // address; read unsigned it reports 65488 where -48 belongs.
      offset: symbolClass === "auto" ? data.readInt16LE(at + 4) : data.readUInt16LE(at + 4),
      segment: data.readUInt16LE(at + 6),
      symbolClass,
    });
  }
  at = base + 48 + header.extensionSize + header.symbolsCount * 9;

  const modules: TdModule[] = [];
  for (let i = 0; i < header.modulesCount; i++) {
    const off = at + i * 16;
    if (off + 16 > data.length) break;
    modules.push({ index: i + 1, name: nameAt(data.readUInt16LE(off)) });
  }
  at += header.modulesCount * 16;

  at += header.sourceFilesCount * 6;
  at += header.lineNumbersCount * 4;
  at += header.scopesCount * 12;

  const segments: TdSegment[] = [];
  for (let i = 0; i < header.segmentsCount; i++) {
    const off = at + i * 16;
    if (off + 16 > data.length) break;
    segments.push({
      module: data.readUInt16LE(off),
      codeSegment: data.readUInt16LE(off + 2),
      codeOffset: data.readUInt16LE(off + 4),
      codeLength: data.readUInt16LE(off + 6),
    });
  }

  return {
    version: `TDINFO ${header.major}.${header.minor}`,
    base,
    symbols,
    modules,
    segments,
    lineRecordCount: header.lineNumbersCount,
    warnings,
  };
}

export function readBorland(file: string): TdInfo | undefined {
  return parseBorland(readFileSync(file));
}
