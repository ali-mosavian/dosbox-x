import { readFileSync } from "fs";

export type AddressSpace = "flat" | "com";

export interface SymModule {
  name: string;
  org: number;
}

export interface SymEntry {
  name: string;
  offset: number;
  tags: string[];
  attrs: Record<string, string>;
}

export interface CopiedReloc {
  kind: "copied";
  name: string;
  origin: string;
  baseSymbol?: string;
  baseLiteral?: number;
  baseRegister?: string;
  baseSpace: AddressSpace;
}

export interface SymFile {
  module: SymModule;
  symbols: Map<string, SymEntry>;
  relocs: CopiedReloc[];
}

export interface ResolveCallbacks {
  loadSegment?: number;
  registers?: Record<string, number>;
  readU32?: (linear: number) => Promise<number>;
}

export interface ResolvedSymbol {
  name: string;
  requested: string;
  space: string;
  linear: number;
  offset: number;
  explanation: string;
  symbol: SymEntry;
}

function parseNumber(text: string): number {
  const trimmed = text.trim();
  const value = Number.parseInt(trimmed, trimmed.toLowerCase().startsWith("0x") ? 16 : 10);
  if (!Number.isFinite(value)) {
    throw new Error(`Invalid number: ${text}`);
  }
  return value >>> 0;
}

function parseAttrs(tokens: string[]): { tags: string[]; attrs: Record<string, string> } {
  const tags: string[] = [];
  const attrs: Record<string, string> = {};

  for (const token of tokens) {
    const eq = token.indexOf("=");
    if (eq === -1) {
      tags.push(token);
      continue;
    }
    attrs[token.slice(0, eq)] = token.slice(eq + 1);
  }

  return { tags, attrs };
}

export function loadSymFile(path: string): SymFile {
  return parseSymFile(readFileSync(path, "utf8"), path);
}

export function parseSymFile(text: string, sourceName = "<memory>"): SymFile {
  let moduleInfo: SymModule | undefined;
  const symbols = new Map<string, SymEntry>();
  const relocs: CopiedReloc[] = [];

  for (const [index, rawLine] of text.split(/\r?\n/).entries()) {
    const lineNumber = index + 1;
    const line = rawLine.replace(/#.*/, "").trim();
    if (line.length === 0) continue;

    const tokens = line.split(/\s+/);
    const op = tokens[0];

    if (op === "module") {
      if (tokens.length < 2) {
        throw new Error(`${sourceName}:${lineNumber}: module requires a name`);
      }
      const { attrs } = parseAttrs(tokens.slice(2));
      moduleInfo = {
        name: tokens[1],
        org: attrs["org"] === undefined ? 0x100 : parseNumber(attrs["org"]),
      };
      continue;
    }

    if (op === "sym") {
      if (tokens.length < 3) {
        throw new Error(`${sourceName}:${lineNumber}: sym requires name and offset`);
      }
      const { tags, attrs } = parseAttrs(tokens.slice(3));
      const sym: SymEntry = {
        name: tokens[1],
        offset: parseNumber(tokens[2]),
        tags,
        attrs,
      };
      symbols.set(sym.name, sym);
      continue;
    }

    if (op === "reloc") {
      if (tokens[1] !== "copied") {
        throw new Error(`${sourceName}:${lineNumber}: unsupported reloc kind ${tokens[1] ?? ""}`);
      }
      const { attrs } = parseAttrs(tokens.slice(2));
      const name = attrs["name"];
      const origin = attrs["origin"];
      if (name === undefined || origin === undefined) {
        throw new Error(`${sourceName}:${lineNumber}: copied reloc requires name and origin`);
      }
      const reloc: CopiedReloc = {
        kind: "copied",
        name,
        origin,
        baseSymbol: attrs["base_symbol"],
        baseLiteral: attrs["base_literal"] === undefined ? undefined : parseNumber(attrs["base_literal"]),
        baseRegister: attrs["base_register"],
        baseSpace: attrs["base_space"] === "com" ? "com" : "flat",
      };
      const baseKinds = [reloc.baseSymbol, reloc.baseLiteral, reloc.baseRegister]
        .filter((value) => value !== undefined).length;
      if (baseKinds !== 1) {
        throw new Error(
          `${sourceName}:${lineNumber}: copied reloc needs exactly one base_symbol, base_literal, or base_register`,
        );
      }
      relocs.push(reloc);
      continue;
    }

    throw new Error(`${sourceName}:${lineNumber}: unknown directive ${op}`);
  }

  if (moduleInfo === undefined) {
    throw new Error(`${sourceName}: missing module directive`);
  }

  return { module: moduleInfo, symbols, relocs };
}

export function parseSymbolRequest(requested: string): { space?: string; name: string } {
  const colon = requested.indexOf(":");
  if (colon === -1) return { name: requested };
  return {
    space: requested.slice(0, colon),
    name: requested.slice(colon + 1),
  };
}

export function comLinear(moduleInfo: SymModule, loadSegment: number, offset: number): number {
  void moduleInfo;
  return ((loadSegment << 4) + offset) >>> 0;
}

function symbolAddress(moduleInfo: SymModule, loadSegment: number | undefined, symbol: SymEntry, space: AddressSpace): number {
  if (space === "flat") return symbol.offset >>> 0;
  if (loadSegment === undefined) {
    throw new Error("Resolving COM-image symbols requires a loadSegment");
  }
  return comLinear(moduleInfo, loadSegment, symbol.offset);
}

function getSymbol(file: SymFile, name: string): SymEntry {
  const symbol = file.symbols.get(name);
  if (symbol === undefined) throw new Error(`Unknown symbol: ${name}`);
  return symbol;
}

function getReloc(file: SymFile, space: string | undefined, symbol: SymEntry): CopiedReloc | undefined {
  if (space !== undefined && space !== "com") {
    const reloc = file.relocs.find((entry) => entry.name === space);
    if (reloc === undefined) throw new Error(`Unknown relocation space: ${space}`);
    return reloc;
  }

  const copiedFrom = symbol.attrs["copied_from"];
  if (copiedFrom !== undefined) {
    return file.relocs.find((entry) => entry.origin === copiedFrom);
  }

  return undefined;
}

async function resolveRelocBase(
  file: SymFile,
  reloc: CopiedReloc,
  callbacks: ResolveCallbacks,
): Promise<{ base: number; explanation: string }> {
  if (reloc.baseLiteral !== undefined) {
    return {
      base: reloc.baseLiteral,
      explanation: `base_literal ${hex(reloc.baseLiteral)}`,
    };
  }

  if (reloc.baseRegister !== undefined) {
    const regName = reloc.baseRegister.toUpperCase();
    const value = callbacks.registers?.[regName];
    if (value === undefined) {
      throw new Error(`Resolving base_register=${regName} requires captured registers`);
    }
    return {
      base: value >>> 0,
      explanation: `${regName} ${hex(value)}`,
    };
  }

  if (reloc.baseSymbol !== undefined) {
    if (callbacks.readU32 === undefined) {
      throw new Error(`Resolving base_symbol=${reloc.baseSymbol} requires readU32`);
    }
    const baseSym = getSymbol(file, reloc.baseSymbol);
    const baseAddr = symbolAddress(file.module, callbacks.loadSegment, baseSym, reloc.baseSpace);
    const value = await callbacks.readU32(baseAddr);
    return {
      base: value >>> 0,
      explanation: `[${reloc.baseSymbol} ${hex(baseAddr)}] ${hex(value)}`,
    };
  }

  throw new Error(`Relocation ${reloc.name} has no base`);
}

export async function resolveSymbol(
  file: SymFile,
  requested: string,
  callbacks: ResolveCallbacks = {},
): Promise<ResolvedSymbol> {
  const parsed = parseSymbolRequest(requested);
  const symbol = getSymbol(file, parsed.name);
  const reloc = getReloc(file, parsed.space, symbol);

  if (reloc === undefined || parsed.space === "com") {
    const linear = symbolAddress(file.module, callbacks.loadSegment, symbol, "com");
    return {
      name: symbol.name,
      requested,
      space: "com",
      linear,
      offset: symbol.offset,
      symbol,
      explanation:
        `${symbol.name} = loadSegment ${hex(callbacks.loadSegment ?? 0)} * 16 + ` +
        `${hex(symbol.offset)} = ${hex(linear)}`,
    };
  }

  const origin = getSymbol(file, reloc.origin);
  const base = await resolveRelocBase(file, reloc, callbacks);
  const delta = (symbol.offset - origin.offset) >>> 0;
  const linear = (base.base + delta) >>> 0;

  return {
    name: symbol.name,
    requested,
    space: reloc.name,
    linear,
    offset: symbol.offset,
    symbol,
    explanation:
      `${reloc.name}:${symbol.name} = ${base.explanation} + ` +
      `(${hex(symbol.offset)} - ${hex(origin.offset)}) = ${hex(linear)}`,
  };
}

export function hex(value: number): string {
  return `0x${(value >>> 0).toString(16).toUpperCase().padStart(8, "0")}`;
}
