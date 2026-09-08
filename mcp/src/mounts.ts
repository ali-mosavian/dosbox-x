import { readFileSync } from "fs";

/**
 * MOUNT lines from a DOSBox-X config.
 *
 * All that is left of the old host-file locator: the emulator reads a
 * program's debug info through its own DOS filesystem now, so nothing has to
 * map a guest path back to a host file. These are kept because the tools
 * report which drives are mounted.
 */

export interface Mount {
  drive: string;
  hostPath: string;
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
