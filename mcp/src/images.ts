import { existsSync, readdirSync } from "fs";
import { extname, resolve } from "path";

import { parseD32File } from "./d32x.js";
import { d32HeaderFingerprint, d32RuntimeSize, type ModuleProbeResult } from "./formats.js";

type JsonObject = Record<string, unknown>;

/** One program the emulator watches for, as its `images` command reports it. */
export interface WatchedImage {
  program: string;
  objects: number;
  complete: boolean;
  placed: { object: number; linear: number }[];
}

/**
 * What `images` answered, or undefined when the emulator predates the command: a build without it says
 * "Unknown command", and the caller falls back to searching memory itself. Any other failure is real.
 */
export function watchedImages(resp: JsonObject): WatchedImage[] | undefined {
  if (resp["status"] === "error" && /unknown command/i.test(String(resp["msg"] ?? ""))) return undefined;
  if (resp["status"] !== "ok" || !Array.isArray(resp["images"])) {
    throw new Error(`images failed: ${JSON.stringify(resp)}`);
  }
  return (resp["images"] as JsonObject[]).map((image) => ({
    program: String(image["program"]),
    objects: Number(image["objects"]),
    complete: image["complete"] === true,
    placed: ((image["placed"] as JsonObject[] | undefined) ?? []).map((one) => ({
      object: Number(one["object"]),
      linear: Number.parseInt(String(one["linear"]).replace(/^0x/i, ""), 16) >>> 0,
    })),
  }));
}

/** The host file a guest path names, by its file name, in a search path's directory or as the path itself. */
export function findD32ByName(guestPath: string, searchPaths: string[]): string | undefined {
  const wanted = guestPath.split(/[\\/]/).pop()?.toLowerCase();
  if (!wanted || extname(wanted) !== ".d32") return undefined;

  for (const searchPath of searchPaths) {
    const resolved = resolve(searchPath);
    if (!existsSync(resolved)) continue;
    try {
      const entry = readdirSync(resolved).find((name) => name.toLowerCase() === wanted);
      if (entry !== undefined) return resolve(resolved, entry);
    } catch {
      if (resolved.toLowerCase().endsWith(`/${wanted}`)) return resolved;
    }
  }
  return undefined;
}

export interface ImageModule {
  probe: ModuleProbeResult;
  file: string;
}

/**
 * The D32 modules among the emulator's images, each with the host file it was built from. The emulator numbers a
 * module's code, data and BSS objects 1, 2 and 3; the code object sits `codeOffset` into the module.
 */
export function d32ModulesFromImages(images: WatchedImage[], searchPaths: string[]): ImageModule[] {
  const modules: ImageModule[] = [];
  for (const image of images) {
    const file = findD32ByName(image.program, searchPaths);
    const code = image.placed.find((one) => one.object === 1);
    if (file === undefined || code === undefined) continue;

    const module = parseD32File(file);
    const base = (code.linear - module.header.codeOffset) >>> 0;
    modules.push({
      file,
      probe: {
        name: module.name,
        base,
        size: d32RuntimeSize(module.header),
        formatId: "d32x",
        fingerprint: d32HeaderFingerprint(module.header),
      },
    });
  }
  return modules;
}
