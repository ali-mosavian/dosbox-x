import { existsSync, readFileSync, readdirSync, statSync } from "fs";
import { basename, isAbsolute, join, resolve } from "path";

import { mzFingerprint, parseMzHeader } from "./mzexe.js";

export interface Mount {
  drive: string;
  hostPath: string;
}

export interface GuestProgramPath {
  drive?: string;
  /** Path components below the drive root, e.g. ["BUILD", "QRENDER.EXE"]. */
  parts: string[];
  file: string;
}

/**
 * loadInfo fields that come from the MZ header DOS actually loaded.
 *
 * Matching by basename alone picks the wrong QRENDER.EXE the moment two build
 * directories are mounted, which is this project's normal state.
 */
export interface ProgramFingerprint {
  mzExtraBytes?: number;
  mzPages?: number;
  relocationCount?: number;
  headerParagraphs?: number;
  minAlloc?: number;
  maxAlloc?: number;
  initSS?: number;
  initSP?: number;
  initIP?: number;
  initCS?: number;
  relocationTableOffset?: number;
}

export interface LocatedProgram {
  file: string;
  matchedBy: "fingerprint" | "name";
  candidates: number;
}

// MOUNT flags that take a following value, so it is not mistaken for the path.
const VALUE_FLAGS = new Set(["-t", "-label", "-size", "-freesize", "-usecd", "-ioctl", "-fs", "-bootdrive", "-nl", "-o"]);

function tokenize(line: string): string[] {
  return (line.match(/"[^"]*"|\S+/g) ?? []).map((token) => token.replace(/^"|"$/g, ""));
}

function parseMountLine(line: string): Mount | undefined {
  const tokens = tokenize(line);
  if (tokens.length < 3 || tokens[0].toLowerCase() !== "mount") return undefined;

  let drive: string | undefined;
  for (let i = 1; i < tokens.length; i++) {
    const token = tokens[i];
    if (token.startsWith("-")) {
      if (VALUE_FLAGS.has(token.toLowerCase())) i++;
      continue;
    }
    if (drive === undefined) {
      if (!/^[a-zA-Z]:?$/.test(token)) return undefined;
      drive = token[0].toUpperCase();
      continue;
    }
    return { drive, hostPath: token };
  }
  return undefined;
}

/** MOUNT lines from a DOSBox-X config's [autoexec] section. */
export function parseMounts(confText: string): Mount[] {
  const mounts: Mount[] = [];
  let inAutoexec = false;
  for (const rawLine of confText.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (line.startsWith("[")) {
      inAutoexec = line.toLowerCase().startsWith("[autoexec]");
      continue;
    }
    if (!inAutoexec || line.startsWith("#")) continue;
    const mount = parseMountLine(line);
    if (mount !== undefined) mounts.push(mount);
  }
  return mounts;
}

export function loadConfMounts(confFile: string): Mount[] {
  try {
    return parseMounts(readFileSync(confFile, "utf8"));
  } catch {
    return [];
  }
}

export function parseGuestPath(program: string): GuestProgramPath {
  const cleaned = program.trim().replace(/^"|"$/g, "");
  const drive = /^([a-zA-Z]):/.exec(cleaned)?.[1]?.toUpperCase();
  const withoutDrive = drive === undefined ? cleaned : cleaned.slice(2);
  const parts = withoutDrive.split(/[\\/]+/).filter((part) => part.length > 0 && part !== ".");
  return { drive, parts, file: parts[parts.length - 1] ?? "" };
}

function listExecutables(dir: string): string[] {
  try {
    return readdirSync(dir)
      .filter((entry) => /\.(exe|com)$/i.test(entry))
      .map((entry) => join(dir, entry));
  } catch {
    return [];
  }
}

function directoryOf(path: string): string | undefined {
  try {
    return statSync(path).isDirectory() ? path : undefined;
  } catch {
    return undefined;
  }
}

/**
 * Every host file that could be the program the guest launched.
 *
 * The guest path is a hint, not an answer: DOS hands the debugger whatever
 * string it was EXEC'd with, which may be relative or a shell fallback.
 */
export function candidateFiles(program: string, mounts: Mount[], searchPaths: string[]): string[] {
  const guest = parseGuestPath(program);
  if (guest.file === "") return [];

  const roots: string[] = [];
  for (const mount of mounts) {
    if (guest.drive === undefined || mount.drive === guest.drive) roots.push(mount.hostPath);
  }

  const found: string[] = [];
  const push = (path: string): void => {
    const full = resolve(path);
    if (existsSync(full) && !found.includes(full)) found.push(full);
  };

  for (const root of roots) {
    push(join(root, ...guest.parts));
    push(join(root, guest.file));
  }
  for (const searchPath of searchPaths) {
    const dir = directoryOf(searchPath);
    if (dir === undefined) {
      if (basename(searchPath).toUpperCase() === guest.file.toUpperCase()) push(searchPath);
      continue;
    }
    push(join(dir, ...guest.parts));
    push(join(dir, guest.file));
    for (const file of listExecutables(dir)) {
      if (basename(file).toUpperCase() === guest.file.toUpperCase()) push(file);
    }
  }
  // A host path handed straight in, which is what dosbox_run_program takes.
  if (isAbsolute(program)) push(program);

  return found;
}

function fingerprintOf(candidate: ProgramFingerprint): string | undefined {
  const header = {
    extraBytes: candidate.mzExtraBytes,
    pages: candidate.mzPages,
    relocationCount: candidate.relocationCount,
    headerParagraphs: candidate.headerParagraphs,
    minAlloc: candidate.minAlloc,
    maxAlloc: candidate.maxAlloc,
    initSS: candidate.initSS,
    initSP: candidate.initSP,
    initIP: candidate.initIP,
    initCS: candidate.initCS,
    relocationTableOffset: candidate.relocationTableOffset,
  };
  if (Object.values(header).some((value) => value === undefined)) return undefined;
  return mzFingerprint(header as Parameters<typeof mzFingerprint>[0]);
}

export function locateProgram(
  program: string,
  mounts: Mount[],
  searchPaths: string[],
  fingerprint?: ProgramFingerprint,
): LocatedProgram | undefined {
  const candidates = candidateFiles(program, mounts, searchPaths);
  if (candidates.length === 0) return undefined;

  const wanted = fingerprint === undefined ? undefined : fingerprintOf(fingerprint);
  if (wanted !== undefined) {
    for (const file of candidates) {
      try {
        const header = parseMzHeader(readFileSync(file));
        if (header !== undefined && mzFingerprint(header) === wanted) {
          return { file, matchedBy: "fingerprint", candidates: candidates.length };
        }
      } catch {
        // An unreadable candidate is not a match; keep looking.
      }
    }
    return undefined;
  }

  return { file: candidates[0], matchedBy: "name", candidates: candidates.length };
}
