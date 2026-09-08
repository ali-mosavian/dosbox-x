import { existsSync, readdirSync, readFileSync } from "fs";
import { basename, extname, resolve } from "path";

import {
  parseD32File,
  sectionRuntimeAddress,
  type D32Header,
  type D32Module,
} from "./d32x.js";
import type { UnifiedSymbol } from "./symbolIndex.js";

const D32X_MAGIC = 0x58323344;

export interface ModuleProbeResult {
  name: string;
  base: number;
  size: number;
  formatId: string;
  fingerprint: string;
}

export interface ModuleScanner {
  id: string;
  magic: Uint8Array;
  headerProbe(bytes: Uint8Array, addr: number): ModuleProbeResult | null;
  ingest(file: string, base: number): UnifiedSymbol[];
}

function u16(data: Uint8Array, off: number): number {
  return data[off] | (data[off + 1] << 8);
}

function u32(data: Uint8Array, off: number): number {
  return (
    data[off] |
    (data[off + 1] << 8) |
    (data[off + 2] << 16) |
    (data[off + 3] << 24)
  ) >>> 0;
}

export function d32HeaderFingerprint(header: D32Header): string {
  return [
    header.fileSize,
    header.codeSize,
    header.dataSize,
    header.bssSize,
    header.exportsCount,
    header.importsCount,
    header.relocsCount,
  ].join(":");
}

export function parseD32HeaderBytes(data: Uint8Array): D32Header | null {
  if (data.length < 72 || u32(data, 0) !== D32X_MAGIC) return null;
  return {
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
}

export function d32RuntimeSize(header: D32Header): number {
  return (header.fileSize + header.bssSize) >>> 0;
}

function d32SymbolsFromModule(module: D32Module, base: number): UnifiedSymbol[] {
  const symbols: UnifiedSymbol[] = [];
  for (const symbol of module.debugSymbols) {
    symbols.push({
      name: symbol.name,
      linear: sectionRuntimeAddress(module, base, symbol.section, symbol.value),
      offset: symbol.value,
      source: "d32-debug",
      space: symbol.section === 0 ? "code" : symbol.section === 1 ? "data" : "bss",
      section: symbol.section === 0 ? "code" : symbol.section === 1 ? "data" : "bss",
      size: symbol.size,
      module: module.name,
      explanation: `${module.name}:${symbol.name} = module base 0x${base.toString(16)} + section+0x${symbol.value.toString(16)}`,
    });
  }
  for (const exp of module.exports) {
    symbols.push({
      name: exp.name,
      linear: sectionRuntimeAddress(module, base, exp.section, exp.value),
      offset: exp.value,
      source: "d32-debug",
      space: exp.section === 0 ? "code" : exp.section === 1 ? "data" : "bss",
      section: exp.section === 0 ? "code" : exp.section === 1 ? "data" : "bss",
      module: module.name,
      explanation: `${module.name}:${exp.name} export`,
    });
  }
  return symbols;
}

export const d32xScanner: ModuleScanner = {
  id: "d32x",
  magic: new Uint8Array([0x44, 0x33, 0x32, 0x58]),
  headerProbe(bytes: Uint8Array, addr: number): ModuleProbeResult | null {
    const header = parseD32HeaderBytes(bytes);
    if (header === null) return null;
    return {
      name: `d32x@${addr.toString(16)}`,
      base: addr >>> 0,
      size: d32RuntimeSize(header),
      formatId: "d32x",
      fingerprint: d32HeaderFingerprint(header),
    };
  },
  ingest(file: string, base: number): UnifiedSymbol[] {
    return d32SymbolsFromModule(parseD32File(file), base);
  },
};

export const moduleScanners: ModuleScanner[] = [d32xScanner];

export function scannerMagicPattern(scanner: ModuleScanner): string {
  return Array.from(scanner.magic, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function headerMatchesFile(header: D32Header, fileBytes: Buffer): boolean {
  if (fileBytes.length < 72 || fileBytes.readUInt32LE(0) !== D32X_MAGIC) return false;
  const fileHeader = parseD32HeaderBytes(fileBytes);
  if (fileHeader === null) return false;
  return d32HeaderFingerprint(fileHeader) === d32HeaderFingerprint(header);
}

export function findMatchingD32File(headerBytes: Uint8Array, searchPaths: string[]): string | undefined {
  const header = parseD32HeaderBytes(headerBytes);
  if (header === null) return undefined;

  for (const searchPath of searchPaths) {
    const resolved = resolve(searchPath);
    if (!existsSync(resolved)) continue;

    const candidates: string[] = [];
    try {
      const stat = readdirSync(resolved);
      for (const entry of stat) {
        const lower = entry.toLowerCase();
        if (extname(lower) === ".d32") {
          candidates.push(resolve(resolved, entry));
        }
      }
    } catch {
      if (resolved.toLowerCase().endsWith(".d32")) {
        candidates.push(resolved);
      }
    }

    for (const candidate of candidates) {
      try {
        const fileBytes = readFileSync(candidate);
        if (headerMatchesFile(header, fileBytes)) return candidate;
      } catch {
        // skip unreadable candidates
      }
    }
  }

  return undefined;
}

export function resolveProbeName(
  probe: ModuleProbeResult,
  headerBytes: Uint8Array,
  searchPaths: string[],
): { name: string; file?: string } {
  if (probe.formatId !== "d32x") return { name: probe.name };
  const matched = findMatchingD32File(headerBytes, searchPaths);
  if (matched === undefined) return { name: probe.name };
  return { name: basename(matched), file: matched };
}
