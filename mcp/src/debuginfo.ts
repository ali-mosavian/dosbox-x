import { existsSync, readFileSync } from "fs";

import { parseBorland, type TdInfo } from "./borland.js";
import { parseCodeView, type CvInfo } from "./codeview.js";
import { hex } from "./symbols.js";
import type { UnifiedSymbol } from "./symbolIndex.js";
import { parseWatcom } from "./watcom.js";

/**
 * One entry point for every debug format that can ride along with a 16-bit
 * DOS EXE. Detection is by signature; the toolchain that produced the file is
 * never used to guess.
 */

export type DebugFormatId = "codeview" | "tdinfo" | "watcom";

export interface DebugModule {
  name: string;
  index: number;
}

export interface DebugLine {
  module?: string;
  file: string;
  line: number;
  /** Load-relative byte offset, the same space a LINK .MAP uses. */
  imageOffset: number;
  /**
   * One past the last byte this line covers.
   *
   * Without it "nearest entry at or before the address" reaches across
   * segments: at CVPROBE's entry in the runtime it answered
   * `..\rt\strdsp1.c:40`, a line from another module entirely.
   */
  endOffset: number;
}

export interface DebugInfo {
  file: string;
  format: DebugFormatId;
  /** The format's own version marker, e.g. "NB08" or "TDINFO 3.16". */
  version: string;
  modules: DebugModule[];
  symbols: UnifiedSymbol[];
  lines: DebugLine[];
  warnings: string[];
}

/**
 * Logical segment index -> load-relative BYTE offset, the space a .MAP uses.
 *
 * A CodeView symbol names a LINK segment index, not an address, and sstSegMap
 * is the only table that says where each one landed. `frame` alone is not the
 * answer: segments sharing a group share a frame and are told apart by
 * `offset`, the byte position within it. frame*16 alone puts 345 of 715
 * publics in the wrong place on cvprobe.exe; frame*16 + offset puts all 715
 * exactly where QRENDER-style .MAP publics say they are.
 */
export function segmentBases(info: CvInfo): Map<number, number> {
  return new Map(info.segments.map((segment) => [segment.index, ((segment.frame << 4) + segment.offset) >>> 0]));
}

function codeViewSymbols(file: string, info: CvInfo, loadLinear: number): {
  symbols: UnifiedSymbol[];
  warnings: string[];
} {
  const bases = segmentBases(info);
  const warnings: string[] = [];
  const missing = new Set<number>();
  const symbols: UnifiedSymbol[] = [];

  for (const symbol of info.symbols) {
    const base = bases.get(symbol.segment);
    if (base === undefined) {
      missing.add(symbol.segment);
      continue;
    }
    const imageOffset = (base + symbol.offset) >>> 0;
    symbols.push({
      name: symbol.name,
      linear: (loadLinear + imageOffset) >>> 0,
      offset: symbol.offset,
      source: "codeview",
      space: `seg${symbol.segment}`,
      section: `seg${symbol.segment}`,
      size: symbol.size,
      module: symbol.module,
      explanation:
        `${symbol.name} = loadLinear ${hex(loadLinear)} + segment ${symbol.segment} ` +
        `base ${hex(base)} + ${hex(symbol.offset)} (${file})`,
    });
  }

  if (missing.size > 0) {
    warnings.push(`no sstSegMap entry for segment(s) ${[...missing].join(", ")}`);
  }
  return { symbols, warnings };
}

/** The source line covering a load-relative offset, if any line covers it. */
export function sourceLineAt(info: DebugInfo, imageOffset: number): DebugLine | undefined {
  let best: DebugLine | undefined;
  for (const entry of info.lines) {
    if (imageOffset < entry.imageOffset || imageOffset >= entry.endOffset) continue;
    if (best === undefined || entry.imageOffset > best.imageOffset) best = entry;
  }
  return best;
}

function codeViewLines(info: CvInfo): DebugLine[] {
  const bases = segmentBases(info);
  // A module's own SegInfo is what bounds its last line; CV writes no end
  // offset for it.
  const contributions = new Map<string, number>();
  for (const module of info.modules) {
    for (const segment of module.segments) {
      contributions.set(`${module.index}:${segment.segment}`, segment.offset + segment.length);
    }
  }

  const out: DebugLine[] = [];
  for (const table of info.lines) {
    const base = bases.get(table.segment);
    if (base === undefined) continue;
    const sorted = [...table.lines].sort((a, b) => a.offset - b.offset);
    const last = contributions.get(`${table.moduleIndex}:${table.segment}`);
    for (const [index, line] of sorted.entries()) {
      const next = sorted[index + 1]?.offset ?? last ?? line.offset + 1;
      out.push({
        module: table.module,
        file: table.file,
        line: line.line,
        imageOffset: (base + line.offset) >>> 0,
        endOffset: (base + Math.max(next, line.offset + 1)) >>> 0,
      });
    }
  }
  return out;
}

function borlandSymbols(file: string, info: TdInfo, loadLinear: number): UnifiedSymbol[] {
  // A TDINFO `segment` is already a load-relative paragraph, unlike CodeView's
  // logical index, so there is no segment map to go through. A symbol record
  // carries no module index either -- modules are reached the other way, from
  // the segment table -- so these are not module-qualified.
  return info.symbols
    .filter((symbol) => symbol.symbolClass === "static" || symbol.symbolClass === "absolute")
    .map((symbol) => ({
      name: symbol.name,
      linear: (loadLinear + (symbol.segment << 4) + symbol.offset) >>> 0,
      offset: symbol.offset,
      source: "tdinfo" as const,
      space: `seg${symbol.segment}`,
      section: `seg${symbol.segment}`,
      explanation:
        `${symbol.name} = loadLinear ${hex(loadLinear)} + ${hex(symbol.segment)}:${hex(symbol.offset)} (${file})`,
    }));
}

/** TDSTRIP moves the block into a .TDS beside the stripped EXE. */
function parseBorlandSidecar(file: string): TdInfo | undefined {
  for (const candidate of [file.replace(/\.[^.\\/]*$/, ".tds"), file.replace(/\.[^.\\/]*$/, ".TDS")]) {
    if (candidate === file || !existsSync(candidate)) continue;
    const found = parseBorland(readFileSync(candidate));
    if (found !== undefined) return found;
  }
  return undefined;
}

export function parseDebugInfo(file: string, loadLinear: number): DebugInfo | undefined {
  const data = readFileSync(file);

  const td = parseBorland(data) ?? parseBorlandSidecar(file);
  if (td !== undefined) {
    return {
      file,
      format: "tdinfo",
      version: td.version,
      modules: td.modules,
      symbols: borlandSymbols(file, td, loadLinear),
      lines: [],
      warnings: td.lineRecordCount === 0
        ? td.warnings
        : [...td.warnings, `${td.lineRecordCount} TDINFO line records present but their layout is unknown`],
    };
  }

  const wat = parseWatcom(data);
  if (wat !== undefined) {
    const moduleNames = new Map(wat.modules.map((module) => [module.index, module.name]));
    return {
      file,
      format: "watcom",
      version: wat.version,
      modules: wat.modules,
      symbols: wat.symbols.map((symbol) => ({
        name: symbol.name,
        linear: (loadLinear + (symbol.segment << 4) + symbol.offset) >>> 0,
        offset: symbol.offset,
        source: "watcom" as const,
        space: `seg${symbol.segment}`,
        section: `seg${symbol.segment}`,
        module: moduleNames.get(symbol.moduleIndex),
        explanation:
          `${symbol.name} = loadLinear ${hex(loadLinear)} + ${hex(symbol.segment)}:${hex(symbol.offset)} (${file})`,
      })),
      lines: [],
      warnings: wat.warnings,
    };
  }

  const cv = parseCodeView(data);
  if (cv !== undefined) {
    const { symbols, warnings } = codeViewSymbols(file, cv, loadLinear);
    return {
      file,
      format: "codeview",
      version: cv.signature,
      modules: cv.modules.map((module) => ({ name: module.name, index: module.index })),
      symbols,
      lines: codeViewLines(cv),
      warnings: [...cv.warnings, ...warnings],
    };
  }

  return undefined;
}
