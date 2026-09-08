import { readFileSync } from "fs";

import { hex } from "./symbols.js";

export interface LinkMapSegment {
  start: number;
  stop: number;
  length: number;
  name: string;
  className: string;
}

export interface LinkMapGroup {
  name: string;
  segment: number;
  offset: number;
  mapOffset: number;
}

export interface LinkMapPublic {
  name: string;
  segment: number;
  offset: number;
  mapOffset: number;
}

export interface LinkMapEntryPoint {
  segment: number;
  offset: number;
  mapOffset: number;
}

export interface LinkMapFile {
  sourceName: string;
  segments: LinkMapSegment[];
  groups: LinkMapGroup[];
  publics: Map<string, LinkMapPublic>;
  entryPoint?: LinkMapEntryPoint;
}

export interface LinkMapLoadInfo {
  loadSeg?: number;
  loadLinear?: number;
}

export interface LinkMapResolution {
  requested: string;
  name: string;
  space: string;
  linear: number;
  offset: number;
  mapOffset: number;
  explanation: string;
  publicSymbol?: LinkMapPublic;
  segment?: LinkMapSegment;
}

function parseHex(text: string): number {
  const trimmed = text.trim();
  const withoutSuffix = trimmed.toUpperCase().endsWith("H")
    ? trimmed.slice(0, -1)
    : trimmed;
  const value = Number.parseInt(withoutSuffix, 16);
  if (!Number.isFinite(value)) {
    throw new Error(`Invalid LINK map hex value: ${text}`);
  }
  return value >>> 0;
}

function parseColonAddress(segmentText: string, offsetText: string): {
  segment: number;
  offset: number;
  mapOffset: number;
} {
  const segment = parseHex(segmentText);
  const offset = parseHex(offsetText);
  return {
    segment,
    offset,
    mapOffset: ((segment << 4) + offset) >>> 0,
  };
}

function publicKeys(name: string): string[] {
  return [name, name.toUpperCase()];
}

export function loadLinkMapFile(path: string): LinkMapFile {
  return parseLinkMap(readFileSync(path, "utf8"), path);
}

export function parseLinkMap(text: string, sourceName = "<memory>"): LinkMapFile {
  const segments: LinkMapSegment[] = [];
  const groups: LinkMapGroup[] = [];
  const publics = new Map<string, LinkMapPublic>();
  let entryPoint: LinkMapEntryPoint | undefined;
  let section: "none" | "segments" | "groups" | "publics" = "none";

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (line.length === 0) continue;

    if (/^Start\s+Stop\s+Length\s+Name\s+Class\b/i.test(line)) {
      section = "segments";
      continue;
    }
    if (/^Origin\s+Group\b/i.test(line)) {
      section = "groups";
      continue;
    }
    if (/Publics\s+by\s+Value/i.test(line)) {
      section = "publics";
      continue;
    }

    const entryMatch = /^Program\s+entry\s+point\s+at\s+([0-9A-Fa-f]+):([0-9A-Fa-f]+)\b/.exec(line);
    if (entryMatch !== null) {
      entryPoint = parseColonAddress(entryMatch[1], entryMatch[2]);
      section = "none";
      continue;
    }

    if (section === "segments") {
      const row = /^([0-9A-Fa-f]+H)\s+([0-9A-Fa-f]+H)\s+([0-9A-Fa-f]+H)\s+(\S+)\s+(\S+)/.exec(line);
      if (row !== null) {
        segments.push({
          start: parseHex(row[1]),
          stop: parseHex(row[2]),
          length: parseHex(row[3]),
          name: row[4],
          className: row[5],
        });
        continue;
      }
    }

    if (section === "groups") {
      const row = /^([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+(\S+)/.exec(line);
      if (row !== null) {
        const parsed = parseColonAddress(row[1], row[2]);
        groups.push({
          name: row[3],
          segment: parsed.segment,
          offset: parsed.offset,
          mapOffset: parsed.mapOffset,
        });
        continue;
      }
    }

    const publicRow = /^([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+(\S+)/.exec(line);
    if (section === "publics" && publicRow !== null) {
      const parsed = parseColonAddress(publicRow[1], publicRow[2]);
      const symbol: LinkMapPublic = {
        name: publicRow[3],
        segment: parsed.segment,
        offset: parsed.offset,
        mapOffset: parsed.mapOffset,
      };
      for (const key of publicKeys(symbol.name)) {
        if (!publics.has(key)) publics.set(key, symbol);
      }
    }
  }

  return {
    sourceName,
    segments,
    groups,
    publics,
    entryPoint,
  };
}

export function effectiveLoadLinear(loadInfo: LinkMapLoadInfo): number {
  if (loadInfo.loadLinear !== undefined) return loadInfo.loadLinear >>> 0;
  if (loadInfo.loadSeg !== undefined) return (loadInfo.loadSeg << 4) >>> 0;
  throw new Error("Resolving LINK map addresses requires loadInfo.loadLinear or loadInfo.loadSeg");
}

export function mapOffsetToLinear(loadInfo: LinkMapLoadInfo, mapOffset: number): number {
  return (effectiveLoadLinear(loadInfo) + mapOffset) >>> 0;
}

function parseNumberSpec(text: string): number {
  const trimmed = text.trim();
  if (/^0x[0-9A-Fa-f]+$/.test(trimmed)) return Number.parseInt(trimmed.slice(2), 16) >>> 0;
  if (/^[0-9A-Fa-f]+H$/i.test(trimmed)) return parseHex(trimmed);
  if (/^[0-9]+$/.test(trimmed)) return Number.parseInt(trimmed, 10) >>> 0;
  if (/^[0-9A-Fa-f]+$/i.test(trimmed)) return Number.parseInt(trimmed, 16) >>> 0;
  throw new Error(`Invalid MAP offset: ${text}`);
}

function findSegment(map: LinkMapFile, name: string): LinkMapSegment | undefined {
  const upper = name.toUpperCase();
  return map.segments.find((segment) => segment.name.toUpperCase() === upper);
}

function findPublic(map: LinkMapFile, name: string): LinkMapPublic | undefined {
  return map.publics.get(name) ?? map.publics.get(name.toUpperCase());
}

export function resolveLinkMapSymbol(
  map: LinkMapFile,
  requested: string,
  loadInfo: LinkMapLoadInfo,
): LinkMapResolution {
  const trimmed = requested.trim();
  const directAddress = /^([0-9A-Fa-f]+):([0-9A-Fa-f]+)$/.exec(trimmed);
  if (directAddress !== null) {
    const parsed = parseColonAddress(directAddress[1], directAddress[2]);
    const linear = mapOffsetToLinear(loadInfo, parsed.mapOffset);
    return {
      requested,
      name: trimmed,
      space: "map",
      linear,
      offset: parsed.offset,
      mapOffset: parsed.mapOffset,
      explanation: `${trimmed} = loadLinear ${hex(effectiveLoadLinear(loadInfo))} + map offset ${hex(parsed.mapOffset)} = ${hex(linear)}`,
    };
  }

  if (trimmed.toLowerCase() === "entry" || trimmed.toLowerCase() === "$entry") {
    if (map.entryPoint === undefined) {
      throw new Error("LINK map has no Program entry point line");
    }
    const linear = mapOffsetToLinear(loadInfo, map.entryPoint.mapOffset);
    return {
      requested,
      name: "entry",
      space: "map",
      linear,
      offset: map.entryPoint.offset,
      mapOffset: map.entryPoint.mapOffset,
      explanation: `entry = loadLinear ${hex(effectiveLoadLinear(loadInfo))} + map offset ${hex(map.entryPoint.mapOffset)} = ${hex(linear)}`,
    };
  }

  const plusSpec = /^([^:+\s]+)\s*\+\s*(.+)$/.exec(trimmed);
  const colonSpec = /^([^:+\s]+)\s*:\s*(.+)$/.exec(trimmed);
  const segmentSpec = plusSpec ?? colonSpec;
  if (segmentSpec !== null) {
    const segment = findSegment(map, segmentSpec[1]);
    if (segment === undefined) {
      throw new Error(`Unknown MAP segment: ${segmentSpec[1]}`);
    }
    const offset = parseNumberSpec(segmentSpec[2]);
    const mapOffset = (segment.start + offset) >>> 0;
    const linear = mapOffsetToLinear(loadInfo, mapOffset);
    return {
      requested,
      name: segment.name,
      space: "map",
      linear,
      offset,
      mapOffset,
      segment,
      explanation: `${segment.name}+${hex(offset)} = loadLinear ${hex(effectiveLoadLinear(loadInfo))} + segment ${hex(segment.start)} + offset ${hex(offset)} = ${hex(linear)}`,
    };
  }

  const segment = findSegment(map, trimmed);
  if (segment !== undefined) {
    const linear = mapOffsetToLinear(loadInfo, segment.start);
    return {
      requested,
      name: segment.name,
      space: "map",
      linear,
      offset: 0,
      mapOffset: segment.start,
      segment,
      explanation: `${segment.name} = loadLinear ${hex(effectiveLoadLinear(loadInfo))} + segment ${hex(segment.start)} = ${hex(linear)}`,
    };
  }

  const publicSymbol = findPublic(map, trimmed);
  if (publicSymbol !== undefined) {
    const linear = mapOffsetToLinear(loadInfo, publicSymbol.mapOffset);
    return {
      requested,
      name: publicSymbol.name,
      space: "map",
      linear,
      offset: publicSymbol.offset,
      mapOffset: publicSymbol.mapOffset,
      publicSymbol,
      explanation: `${publicSymbol.name} = loadLinear ${hex(effectiveLoadLinear(loadInfo))} + public map offset ${hex(publicSymbol.mapOffset)} = ${hex(linear)}`,
    };
  }

  throw new Error(`Unknown LINK map segment or public symbol: ${requested}`);
}

export function describeMapAddress(
  map: LinkMapFile | undefined,
  loadInfo: LinkMapLoadInfo | undefined,
  linear: number,
): string | undefined {
  if (map === undefined || loadInfo === undefined) return undefined;

  let loadLinear: number;
  try {
    loadLinear = effectiveLoadLinear(loadInfo);
  } catch {
    return undefined;
  }

  const mapOffset = (linear - loadLinear) >>> 0;
  const publicSymbol = [...map.publics.values()]
    .filter((symbol, index, all) => all.findIndex((entry) => entry.name === symbol.name) === index)
    .find((symbol) => symbol.mapOffset === mapOffset);
  if (publicSymbol !== undefined) return publicSymbol.name;

  const segment = map.segments
    .filter((entry) => entry.start <= mapOffset && mapOffset <= entry.stop)
    .sort((left, right) => right.start - left.start)[0];
  if (segment === undefined) return undefined;

  return `${segment.name}+${hex((mapOffset - segment.start) >>> 0)}`;
}
