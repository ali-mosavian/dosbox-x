import { hex } from "./symbols.js";
import type { UnifiedSymbol, UnifiedSymbolSource } from "./symbolIndex.js";

/**
 * The emulator's own symbol store, as {"cmd":"sym_list"} reports it.
 *
 * DOSBox-X reads a program's debug info through its own DOS filesystem as
 * EXEC loads it -- CodeView, Borland TDINFO, Watcom, or a .MAP beside the
 * program. That is the only place the running program's identity is certain:
 * no mount table to walk, no MZ header to fingerprint, and image and zip
 * drives work like any other. Nothing here parses a debug format; it converts
 * what the emulator already read.
 */

const SOURCES: Record<string, UnifiedSymbolSource> = {
  codeview: "codeview",
  tdinfo: "tdinfo",
  watcom: "watcom",
  map: "map",
};

/** Numbers arrive as JSON numbers or as "0x..." strings, per field. */
function socketNumber(value: unknown): number | undefined {
  if (typeof value === "number") return value >>> 0;
  if (typeof value !== "string") return undefined;
  const parsed = Number.parseInt(value, value.toLowerCase().startsWith("0x") ? 16 : 10);
  return Number.isFinite(parsed) ? parsed >>> 0 : undefined;
}

export function symbolFromSocket(entry: unknown): UnifiedSymbol | undefined {
  if (typeof entry !== "object" || entry === null) return undefined;
  const record = entry as Record<string, unknown>;

  const name = typeof record["name"] === "string" ? record["name"] : undefined;
  const linear = socketNumber(record["linear"]);
  if (name === undefined || name === "" || linear === undefined) return undefined;

  const source = SOURCES[String(record["source"] ?? "")] ?? "codeview";
  const segment = socketNumber(record["segment"]);
  const space = segment === undefined ? source : `seg${segment}`;
  return {
    name,
    linear,
    offset: socketNumber(record["offset"]) ?? linear,
    source,
    space,
    section: space,
    size: socketNumber(record["size"]),
    module: typeof record["module"] === "string" ? record["module"] : undefined,
    explanation: `${name} = ${hex(linear)}, ${source} symbols the emulator loaded`
      + (typeof record["program"] === "string" ? ` for ${record["program"]}` : ""),
  };
}

export function symbolsFromSocketList(resp: Record<string, unknown>): UnifiedSymbol[] {
  const symbols = resp["symbols"];
  if (!Array.isArray(symbols)) return [];
  return symbols
    .map(symbolFromSocket)
    .filter((symbol): symbol is UnifiedSymbol => symbol !== undefined);
}
