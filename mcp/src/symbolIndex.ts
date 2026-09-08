import { readFileSync } from "fs";
import { basename } from "path";

import { hex } from "./symbols.js";

export type UnifiedSymbolSource =
  | "map"
  | "sym"
  | "d32-debug"
  | "exe16-sidecar"
  | "elf"
  | "codeview"
  | "tdinfo"
  | "watcom";

export interface UnifiedSymbol {
  name: string;
  linear: number;
  offset: number;
  source: UnifiedSymbolSource;
  space: string;
  section?: string;
  size?: number;
  module?: string;
  explanation: string;
}

export interface SidecarSymbol {
  name: string;
  segmentName?: string;
  linear?: number;
  offset?: number;
  source?: string;
}

export interface SidecarFile {
  symbols?: SidecarSymbol[];
}

export interface QualifiedSymbolName {
  module?: string;
  symbol: string;
}

export function parseQualifiedSymbolName(name: string): QualifiedSymbolName {
  const bang = name.indexOf("!");
  if (bang < 0) return { symbol: name };
  return {
    module: name.slice(0, bang),
    symbol: name.slice(bang + 1),
  };
}

function moduleNameMatches(candidate: string | undefined, requested: string): boolean {
  if (candidate === undefined) return false;
  const req = requested.toUpperCase();
  const cand = candidate.toUpperCase();
  if (cand === req) return true;
  return basename(candidate).toUpperCase().replace(/\.D32$/i, "") === req.replace(/\.D32$/i, "");
}

export class SymbolIndex {
  private symbols: UnifiedSymbol[] = [];

  clear(): void {
    this.symbols = [];
  }

  add(symbol: UnifiedSymbol): void {
    this.symbols.push({ ...symbol, linear: symbol.linear >>> 0, offset: symbol.offset >>> 0 });
  }

  addMany(symbols: UnifiedSymbol[]): void {
    for (const symbol of symbols) this.add(symbol);
  }

  all(): UnifiedSymbol[] {
    return [...this.symbols].sort((a, b) => a.linear - b.linear || a.name.localeCompare(b.name));
  }

  resolve(name: string): UnifiedSymbol | undefined {
    const { module, symbol } = parseQualifiedSymbolName(name);
    if (module !== undefined) {
      return this.resolveModuleQualified(module, symbol);
    }

    const exact = this.symbols.find((entry) => entry.name === name);
    if (exact !== undefined) return exact;
    const upper = name.toUpperCase();
    return this.symbols.find((entry) => entry.name.toUpperCase() === upper);
  }

  resolveModuleQualified(module: string, symbol: string): UnifiedSymbol | undefined {
    const symUpper = symbol.toUpperCase();
    const matches = this.symbols.filter((entry) =>
      entry.name.toUpperCase() === symUpper && moduleNameMatches(entry.module, module),
    );
    if (matches.length === 0) return undefined;
    if (matches.length === 1) return matches[0];
    return matches.sort((a, b) => a.linear - b.linear)[0];
  }

  nearest(linear: number): { symbol: UnifiedSymbol; delta: number } | undefined {
    const target = linear >>> 0;
    let best: { symbol: UnifiedSymbol; delta: number } | undefined;
    for (const symbol of this.symbols) {
      if (symbol.linear > target) continue;
      const delta = (target - symbol.linear) >>> 0;
      if (symbol.size !== undefined && symbol.size > 0 && delta >= symbol.size) continue;
      if (best === undefined || delta < best.delta) best = { symbol, delta };
    }
    return best;
  }

  describe(linear: number): string | undefined {
    const found = this.nearest(linear);
    if (found === undefined) return undefined;
    const suffix = found.delta === 0 ? "" : `+${hex(found.delta)}`;
    const module = found.symbol.module === undefined ? "" : `${found.symbol.module}:`;
    return `${module}${found.symbol.name}${suffix}`;
  }
}

export function loadSidecarSymbols(path: string, loadLinear: number, segmentStarts: Map<string, number>): UnifiedSymbol[] {
  const parsed = JSON.parse(readFileSync(path, "utf8")) as SidecarFile;
  const symbols = parsed.symbols ?? [];
  return symbols.flatMap((symbol) => {
    let linear = symbol.linear;
    if (linear === undefined) {
      if (symbol.segmentName === undefined || symbol.offset === undefined) return [];
      const segmentStart = segmentStarts.get(symbol.segmentName.toUpperCase());
      if (segmentStart === undefined) return [];
      linear = (loadLinear + segmentStart + symbol.offset) >>> 0;
    }
    return [{
      name: symbol.name,
      linear,
      offset: symbol.offset ?? linear,
      source: "exe16-sidecar" as const,
      space: symbol.segmentName ?? "linear",
      module: symbol.source,
      explanation: `${symbol.name} from ${path} -> ${hex(linear)}`,
    }];
  });
}
