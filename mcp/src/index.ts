#!/usr/bin/env node
/**
 * DOSBox-X Debug MCP Server
 *
 * Exposes the DOSBox-X debug socket as MCP tools so Cursor agents can
 * launch, inspect, step through, and modify DOS programs interactively.
 *
 * Typical session (cold start to symbol):
 *   1. dosbox_launch()                            — start DOSBox (shows window)
 *   2. dosbox_load_and_run_to("vbe32.com", {      — atomic: load + bp + run
 *        name: "call_hi",                           stops exactly at the symbol,
 *        symFile: "build/vbe32.sym" })              no ordering mistakes possible
 *   3. dosbox_inspect()                           — regs + next instructions
 *   4. dosbox_step()                              — step; returns regs + disasm
 *   5. dosbox_run_to({ name: "vbe_try_mode" })    — run to hi-code symbol
 *   6. dosbox_trace_far()                         — inspect far call/jmp/retf
 *   7. dosbox_text_screen()                       — see what's on screen
 *   8. dosbox_stop()                              — tear down
 *
 * Manual alternative (when you need the entry stop):
 *   dosbox_debug_load_program(breakAtEntry:true) → dosbox_bp_set → dosbox_continue
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { mkdirSync, readFileSync } from "fs";
import { dirname } from "path";
import { z } from "zod";
import * as db from "./dosbox.js";
import {
  parseD32File,
  sectionName,
  sectionRuntimeAddress,
  type D32Module,
} from "./d32x.js";
import {
  hex,
  loadSymFile,
  resolveSymbol,
  type ResolvedSymbol,
  type SymFile,
} from "./symbols.js";
import { scrapeDosErrors } from "./dosErrors.js";
import { parseElf32Symbols } from "./elf.js";
import {
  loadConfMounts,
  type Mount,
} from "./mounts.js";
import {
  moduleScanners,
  resolveProbeName,
  scannerMagicPattern,
  type ModuleProbeResult,
} from "./formats.js";
import {
  describeMapAddress,
  effectiveLoadLinear,
  loadLinkMapFile,
  resolveLinkMapSymbol,
  type LinkMapFile,
  type LinkMapLoadInfo,
  type LinkMapResolution,
} from "./linkmap.js";
import {
  coalesceMemoryDiff,
  hexDataToBytes,
  readLinearMemoryChunked,
} from "./snapshot.js";
import {
  SymbolIndex,
  breakpointMatchOff,
  loadSidecarSymbols,
  parseQualifiedSymbolName,
  type UnifiedSymbol,
} from "./symbolIndex.js";
import { symbolFromSocket, symbolsFromSocketList } from "./socketSymbols.js";
import { j } from "./report.js";
import {
  debuggerStopBlockDecision,
  isDefaultCriticalStopEvent,
  shellIdleResumeDecision,
} from "./stopState.js";

const server = new McpServer({
  name: "dosbox-debug",
  version: "2.0.0",
});

// ─── Helpers ──────────────────────────────────────────────────────────────── //

function isErr(resp: db.JsonObject): boolean {
  return resp["status"] === "error" || resp["status"] === "blocked";
}

// Strip undefined values from an object so the debug socket doesn't choke
function clean(obj: Record<string, unknown>): db.JsonObject {
  return Object.fromEntries(
    Object.entries(obj).filter(([, v]) => v !== undefined),
  ) as db.JsonObject;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function blockedShellResponse(decision: ReturnType<typeof debuggerStopBlockDecision>): db.JsonObject {
  return clean({
    status: "blocked",
    blocked: true,
    msg: decision.reason ?? "guest is stopped in the debugger",
    reason: decision.reason,
    shellReady: false,
    socketStatus: decision.status,
    lastStop: decision.lastStop,
    notifications: decision.notifications,
  });
}

async function latestStopForStatus(statusResp: db.JsonObject): Promise<db.JsonObject | undefined> {
  const shouldFetch =
    statusResp["state"] === "stopped" ||
    statusResp["socketFrozen"] === true ||
    statusResp["socketFreezeRequested"] === true ||
    statusResp["debuggerFrozen"] === true ||
    statusResp["hasLastStop"] === true;

  if (!shouldFetch) return undefined;
  const stop = await db.sendCommand({ cmd: "last_stop" }, undefined, 1_000).catch(() => undefined);
  if (stop?.["event"] === "stopped") {
    lastStopEvent = stop;
    lastStopRegisters = parseRegistersFromRecord(stop);
    return stop;
  }
  if (stop?.["event"] === "process_exit") {
    lastProcessExit = stop;
    return stop;
  }
  return undefined;
}

async function ensureShellReady(timeoutMs = 5_000, autoContinue = true, respectFrozenStop = true): Promise<db.JsonObject> {
  const deadline = Date.now() + timeoutMs;
  let lastResp: db.JsonObject | undefined;
  let lastStatus: db.JsonObject | undefined;
  while (Date.now() <= deadline) {
    lastResp = await db.sendCommand({ cmd: "wait_for_shell", timeoutMs: 0 }, undefined, 1_000)
      .catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
    if (lastResp["status"] === "ok" && lastResp["shellReady"] === true) return lastResp;

    const statusResp = await db.sendCommand({ cmd: "status" }, undefined, 1_000).catch(() => undefined);
    if (statusResp?.["status"] === "ok") {
      lastStatus = statusResp;
      const lastStop = await latestStopForStatus(statusResp);
      const decision = debuggerStopBlockDecision({
        status: statusResp,
        lastStop,
        notifications: notificationSummary(),
        respectFrozenStop,
      });
      if (decision.blocked) return blockedShellResponse(decision);
    }
    if (statusResp?.["status"] === "ok" && statusResp["shellReady"] === true) {
      return { ...statusResp, shellReady: true };
    }
    if (autoContinue && statusResp?.["status"] === "ok" && statusResp["state"] === "stopped") {
      await db.sendCommand({ cmd: "continue" }, undefined, 2_000).catch(() => undefined);
    }
    await sleep(200);
  }
  throw new Error(`DOS shell was not ready within ${timeoutMs}ms: ${j({ waitForShell: lastResp, socketStatus: lastStatus })}`);
}

const explicitInterruptBreakpoints = new Set<number>();

async function resumeShellIdleStopIfSafe(respectFrozenStop = true): Promise<db.JsonObject | undefined> {
  const statusResp = await db.sendCommand({ cmd: "status" }, undefined, 1_000).catch(() => undefined);
  if (statusResp?.["status"] !== "ok") return undefined;

  const lastStop = await latestStopForStatus(statusResp);
  const decision = shellIdleResumeDecision({
    status: statusResp,
    lastStop,
    notifications: notificationSummary(),
    respectFrozenStop,
  });
  if (decision.blocked) return blockedShellResponse(decision);
  if (!decision.shouldResume) return undefined;

  const resp = await db.sendCommand({ cmd: "continue" }, undefined, 2_000).catch((e) => ({
    status: "error",
    msg: (e as Error).message,
  }) as db.JsonObject);
  return {
    status: isErr(resp) ? "error" : "ok",
    shellIdleResumed: true,
    reason: decision.reason,
    continue: resp,
  };
}

async function prepareShellForDosCommand(timeoutMs = 5_000, respectFrozenStop = true): Promise<db.JsonObject> {
  const shell = await ensureShellReady(Math.min(5_000, timeoutMs), true, respectFrozenStop);
  if (shell["status"] === "blocked") return shell;
  const resume = await resumeShellIdleStopIfSafe(respectFrozenStop);
  if (resume?.["status"] === "blocked" || resume?.["status"] === "error") return resume;
  return resume === undefined ? shell : { ...shell, shellIdleResume: resume };
}

function sendDosCommand(command: string, timeoutMs = 30_000, breakOnInterrupts = false): Promise<db.JsonObject> {
  return db.sendCommand({
    cmd: "dos_cmd",
    command,
    autoContinueInt3: !breakOnInterrupts && !explicitInterruptBreakpoints.has(3),
  }, undefined, timeoutMs);
}

async function sendDosCommandWhenShellReady(command: string, timeoutMs = 30_000, respectFrozenStop = true): Promise<db.JsonObject> {
  const shell = await prepareShellForDosCommand(Math.min(5_000, timeoutMs), respectFrozenStop);
  if (shell["status"] === "blocked") return shell;
  return sendDosCommand(command, timeoutMs);
}

let loadedSymbols: SymFile | undefined;
let loadedSymbolsPath: string | undefined;
let loadedMap: LinkMapFile | undefined;
let loadedMapPath: string | undefined;
let currentLoadSegment: number | undefined;
let currentLoadSegmentFresh = false;
let currentLoadInfo: NormalizedLoadInfo | undefined;
let guestMounts: Mount[] = [];
/** Symbols the emulator parsed for itself; mirrored here, never re-parsed. */
let socketSymbols: UnifiedSymbol[] = [];
let socketSymbolsKey: string | undefined;
let autoDebugInfoNote: string | undefined;
let lastStopRegisters: Record<string, number> | undefined;
let lastStopEvent: db.JsonObject | undefined;
let lastProcessExit: db.JsonObject | undefined;

type CriticalNotification = db.JsonObject & {
  id: number;
  unread: boolean;
  receivedAt: string;
  severity: "critical";
  source: string;
};

const pendingNotifications: CriticalNotification[] = [];
let nextNotificationId = 1;
let lastExitNotificationKey: string | undefined;

function notificationKey(event: db.JsonObject): string {
  const name = String(event["event"] ?? "unknown");
  const reason = String(event["reason"] ?? "");
  const exitCode = String(event["exit_code"] ?? "");
  const psp = String(event["psp"] ?? "");
  const vector = String(event["vector"] ?? "");
  const linear = String(event["linear"] ?? event["watch_linear"] ?? "");
  return [name, reason, exitCode, psp, vector, linear].join(":");
}

function enqueueCriticalNotification(event: db.JsonObject, source = "socket"): void {
  const key = notificationKey(event);
  if (event["event"] === "process_exit") {
    if (key === lastExitNotificationKey) return;
    lastExitNotificationKey = key;
  }
  pendingNotifications.push({
    ...event,
    id: nextNotificationId++,
    unread: true,
    receivedAt: new Date().toISOString(),
    severity: "critical",
    source,
  });
  if (pendingNotifications.length > 64) pendingNotifications.splice(0, pendingNotifications.length - 64);
}

function unreadNotifications(): CriticalNotification[] {
  return pendingNotifications.filter((entry) => entry.unread).map((entry) => ({ ...entry }));
}

function notificationSummary(): db.JsonObject {
  return {
    unread: unreadNotifications(),
    unreadCount: unreadNotifications().length,
    latest: pendingNotifications.length === 0 ? undefined : { ...pendingNotifications[pendingNotifications.length - 1] },
  };
}

function ackNotifications(ids?: number[]): db.JsonObject {
  const wanted = ids === undefined ? undefined : new Set(ids);
  let acked = 0;
  for (const entry of pendingNotifications) {
    if (!entry.unread) continue;
    if (wanted !== undefined && !wanted.has(entry.id)) continue;
    entry.unread = false;
    acked++;
  }
  return { status: "ok", acked, unreadCount: unreadNotifications().length };
}

async function harvestLatchedCriticalEvents(): Promise<void> {
  const exitInfo = await db.sendCommand({ cmd: "get_last_exit" }, undefined, 1_000)
    .then((r) => r["status"] === "ok" && r["available"] === true ? r : undefined)
    .catch(() => undefined);
  if (exitInfo !== undefined) {
    lastProcessExit = {
      event: "process_exit",
      exit_code: exitInfo["exit_code"],
      psp: exitInfo["psp"],
      tsr: exitInfo["tsr"],
      abnormal: exitInfo["abnormal"],
    };
    enqueueCriticalNotification(lastProcessExit, "socket-latched");
  }
}

// Latch process_exit and critical stop async events from the socket.
db.addAsyncEventListener((event) => {
  if (event["event"] === "process_exit") {
    lastProcessExit = event;
    enqueueCriticalNotification(event);
  } else if (event["event"] === "stopped") {
    const interruptNumber = parseRegisterNumber(event["int"]);
    if (isDefaultCriticalStopEvent(event) || (interruptNumber !== undefined && explicitInterruptBreakpoints.has(interruptNumber))) {
      lastStopEvent = event;
      lastStopRegisters = parseRegistersFromRecord(event);
      enqueueCriticalNotification(event);
    }
  } else if (event["event"] === "socket_disconnect") {
    enqueueCriticalNotification(event, "mcp-session");
  }
});
const symbolIndex = new SymbolIndex();
const d32Modules = new Map<string, { module: D32Module; base: number }>();
const loadedElfFiles = new Map<string, { file: string; base: number }>();
const memSnapshots = new Map<string, { addr: number; len: number; bytes: Uint8Array }>();
const discoveredModules = new Map<string, { probe: ModuleProbeResult; file?: string; registered: boolean }>();
let moduleSearchPaths: string[] = [];
let nextBpSpecId = 1;

type DeferredBreakpointSpec = {
  id: number;
  name: string;
  offsetDelta: number;
  once: boolean;
  loadSegment?: number;
  armedLinear?: number;
  status: "pending" | "armed";
  explanation?: string;
  warning?: string;
};

const deferredBreakpointSpecs = new Map<number, DeferredBreakpointSpec>();

type DLine = { addr: string; bytes: string; mnemonic: string };

type NormalizedLoadInfo = LinkMapLoadInfo & {
  program?: string;
  isCom?: boolean;
  pspSeg?: number;
  entryCS?: number;
  entryIP?: number;
  entryLinear?: number;
  initialSS?: number;
  initialSP?: number;
  initialStackLinear?: number;
  imageEndLinear?: number;
  mzSignature?: number;
  mzExtraBytes?: number;
  mzPages?: number;
  headerParagraphs?: number;
  headerBytes?: number;
  relocationCount?: number;
  relocationTableOffset?: number;
  initCS?: number;
  initIP?: number;
  initSS?: number;
  initSP?: number;
  checksum?: number;
  overlay?: number;
  minAlloc?: number;
  maxAlloc?: number;
  imageSizeBytes?: number;
  warnings: string[];
};

type ResolvedAddress = (ResolvedSymbol | LinkMapResolution) & {
  source: "sym" | "map" | "d32-debug" | "exe16-sidecar" | "elf";
};

function parseRegisterNumber(value: unknown): number | undefined {
  if (typeof value === "number") return value >>> 0;
  if (typeof value !== "string") return undefined;
  const parsed = Number.parseInt(value, value.toLowerCase().startsWith("0x") ? 16 : 10);
  return Number.isFinite(parsed) ? parsed >>> 0 : undefined;
}

async function readU32Linear(addr: number): Promise<number> {
  const resp = await db.sendCommand({ cmd: "mem_read_linear", addr, len: 4 });
  if (resp["status"] !== "ok" || typeof resp["data"] !== "string") {
    throw new Error(`Unable to read ${hex(addr)}: ${j(resp)}`);
  }

  const bytes = resp["data"].match(/.{1,2}/g) ?? [];
  if (bytes.length !== 4 || bytes.some((byte) => byte === "PF" || byte === "??")) {
    throw new Error(`Unable to read all bytes at ${hex(addr)}: ${resp["data"]}`);
  }

  return bytes.reduce((value, byte, index) => {
    return value | (Number.parseInt(byte, 16) << (index * 8));
  }, 0) >>> 0;
}

async function readRegisters(): Promise<Record<string, number>> {
  const resp = await db.sendCommand({ cmd: "regs" });
  if (resp["status"] !== "ok") {
    throw new Error(`Unable to read registers: ${j(resp)}`);
  }

  const registers: Record<string, number> = {};
  for (const [key, value] of Object.entries(resp)) {
    const parsed = parseRegisterNumber(value);
    if (parsed !== undefined) registers[key.toUpperCase()] = parsed;
  }
  return registers;
}

async function withTimeout<T>(label: string, timeoutMs: number, work: Promise<T>): Promise<T> {
  let timer: NodeJS.Timeout | undefined;
  try {
    return await Promise.race([
      work,
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label} timed out after ${timeoutMs}ms`)), timeoutMs);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

function parseRegistersFromRecord(resp: Record<string, unknown>): Record<string, number> {
  const registers: Record<string, number> = {};
  for (const [key, value] of Object.entries(resp)) {
    const parsed = parseRegisterNumber(value);
    if (parsed !== undefined) registers[key.toUpperCase()] = parsed;
  }
  return registers;
}

function parseBoolean(value: unknown): boolean | undefined {
  if (typeof value === "boolean") return value;
  if (typeof value === "number") return value !== 0;
  if (typeof value !== "string") return undefined;
  const normalized = value.toLowerCase();
  if (normalized === "true" || normalized === "yes" || normalized === "1") return true;
  if (normalized === "false" || normalized === "no" || normalized === "0") return false;
  return undefined;
}

function normalizeLoadInfo(record: unknown): NormalizedLoadInfo | undefined {
  if (record === undefined || record === null || typeof record !== "object") return undefined;
  const raw = record as Record<string, unknown>;
  const loadSeg = parseRegisterNumber(raw["loadSeg"]);
  const loadLinear = parseRegisterNumber(raw["loadLinear"]);
  const isCom = parseBoolean(raw["isCom"]);
  const entryCS = parseRegisterNumber(raw["entryCS"]);
  if (
    raw["program"] === undefined &&
    loadSeg === undefined &&
    loadLinear === undefined &&
    isCom === undefined &&
    entryCS === undefined
  ) {
    return undefined;
  }
  const warnings: string[] = [];
  const computedLoadSeg = loadSeg ?? (loadLinear === undefined ? undefined : loadLinear >>> 4);
  const computedLoadLinear = loadLinear ?? (loadSeg === undefined ? undefined : (loadSeg << 4) >>> 0);

  if (loadSeg !== undefined && loadLinear !== undefined && ((loadSeg << 4) >>> 0) !== loadLinear) {
    warnings.push(`loadSeg ${hex(loadSeg)} does not match loadLinear ${hex(loadLinear)}`);
  }
  if (isCom !== true && loadSeg !== undefined && entryCS !== undefined && loadSeg === entryCS) {
    warnings.push("EXE loadSeg equals entryCS; verify socket loadInfo because EXE entry CS is not the load segment");
  }

  return {
    program: typeof raw["program"] === "string" ? raw["program"] : undefined,
    isCom,
    pspSeg: parseRegisterNumber(raw["pspSeg"]),
    loadSeg: computedLoadSeg,
    loadLinear: computedLoadLinear,
    entryCS,
    entryIP: parseRegisterNumber(raw["entryIP"]),
    entryLinear: parseRegisterNumber(raw["entryLinear"]),
    initialSS: parseRegisterNumber(raw["initialSS"]),
    initialSP: parseRegisterNumber(raw["initialSP"]),
    initialStackLinear: parseRegisterNumber(raw["initialStackLinear"]),
    imageEndLinear: parseRegisterNumber(raw["imageEndLinear"]),
    mzSignature: parseRegisterNumber(raw["mzSignature"]),
    mzExtraBytes: parseRegisterNumber(raw["mzExtraBytes"]),
    mzPages: parseRegisterNumber(raw["mzPages"]),
    headerParagraphs: parseRegisterNumber(raw["headerParagraphs"]),
    headerBytes: parseRegisterNumber(raw["headerBytes"]),
    relocationCount: parseRegisterNumber(raw["relocationCount"]),
    relocationTableOffset: parseRegisterNumber(raw["relocationTableOffset"]),
    initCS: parseRegisterNumber(raw["initCS"]),
    initIP: parseRegisterNumber(raw["initIP"]),
    initSS: parseRegisterNumber(raw["initSS"]),
    initSP: parseRegisterNumber(raw["initSP"]),
    checksum: parseRegisterNumber(raw["checksum"]),
    overlay: parseRegisterNumber(raw["overlay"]),
    minAlloc: parseRegisterNumber(raw["minAlloc"]),
    maxAlloc: parseRegisterNumber(raw["maxAlloc"]),
    imageSizeBytes: parseRegisterNumber(raw["imageSizeBytes"]),
    warnings,
  };
}

function loadInfoFromResponse(resp: db.JsonObject): NormalizedLoadInfo | undefined {
  return normalizeLoadInfo(resp["loadInfo"]) ?? normalizeLoadInfo(resp);
}

async function fetchLoadInfoFallback(): Promise<NormalizedLoadInfo | undefined> {
  const resp = await db.sendCommand({ cmd: "get_load_info" }, undefined, 2_000).catch(() => undefined);
  if (resp === undefined || isErr(resp)) return undefined;
  return loadInfoFromResponse(resp);
}

async function captureStopState(resp: db.JsonObject): Promise<string | undefined> {
  if (resp["event"] !== "stopped") return undefined;

  lastStopEvent = resp;
  lastStopRegisters = parseRegistersFromRecord(resp);
  const loadInfo = loadInfoFromResponse(resp) ?? await fetchLoadInfoFallback();
  if (loadInfo !== undefined) {
    currentLoadInfo = loadInfo;
    if (loadInfo.loadSeg !== undefined) {
      currentLoadSegment = loadInfo.isCom === true && loadInfo.entryCS !== undefined
        ? loadInfo.entryCS
        : loadInfo.loadSeg;
      currentLoadSegmentFresh = true;
    }
    await syncSocketSymbols();
    const source = loadInfoFromResponse(resp) === undefined ? "get_load_info" : "loadInfo";
    const segmentText = currentLoadSegment === undefined ? "unknown" : hex(currentLoadSegment);
    return `loadInfo → loadSegment ${segmentText} (${source})`;
  }

  const fallbackSegment = parseRegisterNumber(resp["CS"]);
  if (fallbackSegment !== undefined) {
    currentLoadSegment = fallbackSegment;
    currentLoadInfo = {
      isCom: true,
      loadSeg: fallbackSegment,
      loadLinear: fallbackSegment << 4,
      entryCS: fallbackSegment,
      entryIP: parseRegisterNumber(resp["EIP"]) ?? parseRegisterNumber(resp["IP"]),
      warnings: ["No socket loadInfo was available; treating entry CS as COM load segment."],
    };
    currentLoadSegmentFresh = true;
    rebuildSymbolIndex();
    return `loadSegment → ${hex(currentLoadSegment)} (COM fallback CS)`;
  }

  return undefined;
}

async function fetchLastStopFallback(): Promise<db.JsonObject | undefined> {
  const resp = await db.sendCommand({ cmd: "last_stop" }, undefined, 1_000).catch(() => undefined);
  if (resp === undefined) return undefined;
  if (resp["event"] === "process_exit") {
    lastProcessExit = resp;
    enqueueCriticalNotification(resp, "socket-latched");
    return resp;
  }
  if (resp["event"] !== "stopped") return undefined;
  lastStopEvent = resp;
  lastStopRegisters = parseRegistersFromRecord(resp);
  return resp;
}

async function resolveLoadedSymbol(name: string, loadSegment?: number) {
  return (await resolveLoadedSymbolWithWarning(name, loadSegment)).resolved;
}

async function effectiveMapLoadInfo(): Promise<NormalizedLoadInfo> {
  if (currentLoadInfo !== undefined) return currentLoadInfo;
  const fetched = await fetchLoadInfoFallback();
  if (fetched !== undefined) {
    currentLoadInfo = fetched;
    await syncSocketSymbols();
    return fetched;
  }
  throw new Error("Resolving LINK map addresses requires loadInfo from a break-at-entry stop or get_load_info");
}

async function resolveLoadedMapSymbol(name: string): Promise<ResolvedAddress> {
  if (loadedMap === undefined) throw new Error("No LINK map loaded. Pass mapFile or call dosbox_map({op:'load'}).");
  const resolved = resolveLinkMapSymbol(loadedMap, name, await effectiveMapLoadInfo());
  return { ...resolved, source: "map" };
}

function addMapSymbolsToIndex(): void {
  if (loadedMap === undefined || currentLoadInfo === undefined) return;
  const loadLinear = effectiveLoadLinear(currentLoadInfo);
  const seenPublics = new Set<string>();
  for (const publicSymbol of loadedMap.publics.values()) {
    if (seenPublics.has(publicSymbol.name)) continue;
    seenPublics.add(publicSymbol.name);
    symbolIndex.add({
      name: publicSymbol.name,
      linear: (loadLinear + publicSymbol.mapOffset) >>> 0,
      offset: publicSymbol.offset,
      source: "map",
      space: "map",
      section: loadedMap.segments.find((segment) =>
        publicSymbol.mapOffset >= segment.start && publicSymbol.mapOffset <= segment.stop
      )?.name,
      explanation: `${publicSymbol.name} = loadLinear ${hex(loadLinear)} + map offset ${hex(publicSymbol.mapOffset)}`,
    });
  }
}

function addSymFileToIndex(): void {
  if (loadedSymbols === undefined) return;
  for (const symbol of loadedSymbols.symbols.values()) {
    const isFlat = symbol.tags.includes("flat");
    const linear = isFlat
      ? symbol.offset
      : currentLoadSegment === undefined
        ? undefined
        : ((currentLoadSegment << 4) + symbol.offset) >>> 0;
    if (linear === undefined) continue;
    symbolIndex.add({
      name: symbol.name,
      linear,
      offset: symbol.offset,
      source: "sym",
      space: isFlat ? "flat" : "com",
      size: symbol.attrs["size"] === undefined ? undefined : Number.parseInt(symbol.attrs["size"], 0),
      explanation: `${symbol.name} from ${loadedSymbolsPath ?? ".sym"} -> ${hex(linear)}`,
    });
  }
}

function addD32ModuleToIndex(module: D32Module, base: number): void {
  for (const symbol of module.debugSymbols) {
    symbolIndex.add({
      name: symbol.name,
      linear: sectionRuntimeAddress(module, base, symbol.section, symbol.value),
      offset: symbol.value,
      source: "d32-debug",
      space: sectionName(symbol.section),
      section: sectionName(symbol.section),
      size: symbol.size,
      module: module.name,
      explanation: `${module.name}:${symbol.name} = module base ${hex(base)} + ${sectionName(symbol.section)}+${hex(symbol.value)}`,
    });
  }
  for (const exp of module.exports) {
    symbolIndex.add({
      name: exp.name,
      linear: sectionRuntimeAddress(module, base, exp.section, exp.value),
      offset: exp.value,
      source: "d32-debug",
      space: sectionName(exp.section),
      section: sectionName(exp.section),
      module: module.name,
      explanation: `${module.name}:${exp.name} export = module base ${hex(base)} + ${sectionName(exp.section)}+${hex(exp.value)}`,
    });
  }
}

function addElfSymbolsToIndex(): void {
  for (const { file, base } of loadedElfFiles.values()) {
    try {
      symbolIndex.addMany(parseElf32Symbols(file, base));
    } catch {
      // Stale ELF registration is ignored until reload succeeds.
    }
  }
}

/**
 * Mirror the emulator's symbol store.
 *
 * DOSBox-X reads a program's debug info through its own DOS filesystem as
 * EXEC loads it, which is the only place the running program's identity is
 * certain -- no mount table to walk, no MZ fingerprint to match, and image
 * and zip drives work like any other. This asks for the result rather than
 * parsing the file a second time here.
 */
async function syncSocketSymbols(force = false): Promise<void> {
  const key = `${currentLoadInfo?.program ?? ""}@${currentLoadInfo?.loadLinear ?? ""}`;
  if (!force && key === socketSymbolsKey) return;

  const resp = await db.sendCommand({ cmd: "sym_list", limit: 1_000_000 }, undefined, 20_000)
    .catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
  if (resp["status"] !== "ok" || !Array.isArray(resp["symbols"])) {
    autoDebugInfoNote = `emulator symbols unavailable: ${String(resp["msg"] ?? "no sym_list support")}`;
    socketSymbols = [];
    socketSymbolsKey = key;
    rebuildSymbolIndex();
    return;
  }

  socketSymbols = symbolsFromSocketList(resp as Record<string, unknown>);
  socketSymbolsKey = key;

  const total = parseRegisterNumber(resp["total"]) ?? socketSymbols.length;
  const lines = parseRegisterNumber(resp["lines"]) ?? 0;
  autoDebugInfoNote = total === 0
    ? "the emulator found no debug info in the loaded program"
    : `${total} symbols and ${lines} line records from the emulator`;
  rebuildSymbolIndex();
}

async function currentLinearAddress(): Promise<number | undefined> {
  const resp = await db.sendCommand({ cmd: "get_linear_addr" })
    .catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
  if (resp["status"] !== "ok") return undefined;
  const text = String(resp["linear_addr"] ?? resp["linear"] ?? "");
  return text === "" ? undefined : Number.parseInt(text.replace(/^0x/i, ""), 16) >>> 0;
}

/** The source line covering an address, from the emulator's line tables. */
async function autoDebugSourceLine(linear: number): Promise<string | undefined> {
  const resp = await db.sendCommand({ cmd: "where", linear }, undefined, 5_000)
    .catch(() => ({ status: "error" }) as db.JsonObject);
  if (resp["status"] !== "ok" || typeof resp["file"] !== "string") return undefined;
  return `${resp["file"]}:${String(resp["line"] ?? "")}`;
}

function rebuildSymbolIndex(): void {
  symbolIndex.clear();
  symbolIndex.addMany(socketSymbols);
  addMapSymbolsToIndex();
  addSymFileToIndex();
  addElfSymbolsToIndex();
  for (const { module, base } of d32Modules.values()) addD32ModuleToIndex(module, base);
  void reResolveDeferredBreakpoints();
}

function clearSnapshotState(): void {
  memSnapshots.clear();
}

function clearDeferredBreakpointState(): void {
  deferredBreakpointSpecs.clear();
  nextBpSpecId = 1;
}

function clearMcpDebugSessionState(opts: {
  preserveSymbols?: boolean;
  preserveNotifications?: boolean;
} = {}): void {
  currentLoadSegment = undefined;
  currentLoadSegmentFresh = false;
  currentLoadInfo = undefined;
  socketSymbols = [];
  socketSymbolsKey = undefined;
  autoDebugInfoNote = undefined;
  lastStopRegisters = undefined;
  lastStopEvent = undefined;
  lastProcessExit = undefined;
  explicitInterruptBreakpoints.clear();
  clearSnapshotState();
  clearDeferredBreakpointState();
  discoveredModules.clear();
  moduleSearchPaths = [];
  if (opts.preserveNotifications !== true) {
    pendingNotifications.length = 0;
    lastExitNotificationKey = undefined;
  }
  if (opts.preserveSymbols !== true) {
    loadedSymbols = undefined;
    loadedSymbolsPath = undefined;
    loadedMap = undefined;
    loadedMapPath = undefined;
    loadedElfFiles.clear();
    d32Modules.clear();
  }
  rebuildSymbolIndex();
}

async function readLinearBytesChunked(addr: number, len: number): Promise<Uint8Array> {
  return readLinearMemoryChunked(async (chunkAddr, chunkLen) => {
    const resp = await db.sendCommand({ cmd: "mem_read_linear", addr: chunkAddr, len: chunkLen });
    if (resp["status"] !== "ok" || typeof resp["data"] !== "string") {
      throw new Error(`mem_read_linear ${hex(chunkAddr)} failed: ${j(resp)}`);
    }
    return hexDataToBytes(resp["data"]);
  }, addr, len);
}

async function resolveBreakpointSpecName(name: string, loadSegment?: number): Promise<{
  resolved?: ResolvedAddress;
  warning?: string;
  error?: string;
}> {
  const indexed = symbolIndex.resolve(name);
  if (indexed !== undefined) {
    return {
      resolved: {
        name: indexed.name,
        requested: name,
        space: indexed.space,
        linear: indexed.linear,
        offset: indexed.offset,
        explanation: indexed.explanation,
        source: indexed.source,
      } as ResolvedAddress,
      warning: undefined,
    };
  }

  try {
    const { resolved, warning } = await resolveLoadedSymbolWithWarning(name, loadSegment);
    return { resolved, warning };
  } catch (e) {
    return { error: (e as Error).message };
  }
}

async function clearArmedDeferredBreakpoint(spec: DeferredBreakpointSpec): Promise<void> {
  if (spec.armedLinear === undefined) return;
  await db.sendCommand({ cmd: "bp_clear_linear_exec", linear: spec.armedLinear }).catch(() => undefined);
  spec.armedLinear = undefined;
}

async function armDeferredBreakpointSpec(spec: DeferredBreakpointSpec): Promise<void> {
  const { resolved, warning, error } = await resolveBreakpointSpecName(spec.name, spec.loadSegment);
  if (resolved === undefined) {
    await clearArmedDeferredBreakpoint(spec);
    spec.status = "pending";
    spec.explanation = undefined;
    spec.warning = warning ?? error;
    return;
  }

  const linear = (resolved.linear + spec.offsetDelta) >>> 0;
  if (spec.armedLinear !== undefined && spec.armedLinear !== linear) {
    await clearArmedDeferredBreakpoint(spec);
  }
  if (spec.armedLinear === linear) {
    spec.status = "armed";
    spec.explanation = resolved.explanation;
    spec.warning = warning;
    return;
  }

  const matchOff = breakpointMatchOff(resolved);
  const resp = await db.sendCommand(clean({
    cmd: "bp_set_linear_exec",
    linear,
    match_off: matchOff,
    once: spec.once ? 1 : 0,
  }));
  if (isErr(resp)) {
    spec.status = "pending";
    spec.warning = `bp_set_linear_exec failed: ${j(resp)}`;
    return;
  }

  spec.armedLinear = linear;
  spec.status = "armed";
  spec.explanation = `${spec.name} -> ${hex(linear)} (${resolved.explanation})`;
  spec.warning = warning;
}

async function reResolveDeferredBreakpoints(): Promise<void> {
  for (const spec of deferredBreakpointSpecs.values()) {
    await armDeferredBreakpointSpec(spec);
  }
}

async function registerDeferredBreakpointSpec(args: {
  name: string;
  offsetDelta?: number;
  once?: boolean;
  loadSegment?: number;
}): Promise<DeferredBreakpointSpec> {
  const spec: DeferredBreakpointSpec = {
    id: nextBpSpecId++,
    name: args.name,
    offsetDelta: args.offsetDelta ?? 0,
    once: args.once ?? false,
    loadSegment: args.loadSegment,
    status: "pending",
  };
  deferredBreakpointSpecs.set(spec.id, spec);
  await armDeferredBreakpointSpec(spec);
  return spec;
}

async function appendDosErrorReport(parts: string[]): Promise<string[]> {
  const screen = await textScreenSummary();
  const dosErrors = scrapeDosErrors(screen.fullText ?? screen.text ?? "");
  if (dosErrors.length > 0) {
    parts.push(`dosErrors: ${j(dosErrors)}`);
  }
  return dosErrors;
}

function segmentStartsByName(): Map<string, number> {
  const starts = new Map<string, number>();
  if (loadedMap === undefined) return starts;
  for (const segment of loadedMap.segments) {
    starts.set(segment.name.toUpperCase(), segment.start);
  }
  return starts;
}

async function resolveLoadedSymbolWithWarning(name: string, loadSegment?: number) {
  const indexed = symbolIndex.resolve(name);
  if (indexed !== undefined) {
    const qualified = parseQualifiedSymbolName(name);
    return {
      resolved: {
        name: indexed.name,
        requested: name,
        space: indexed.space,
        linear: indexed.linear,
        offset: indexed.offset,
        explanation: indexed.explanation,
        source: indexed.source,
        ...(qualified.module !== undefined ? { module: qualified.module } : {}),
      } as ResolvedAddress,
      warning: undefined,
    };
  }

  // The emulator's store answers names the mirrored index cannot spell:
  // "module!name" matches there with any extension stripped, and here only
  // .d32 is. Asking it is also how a symbol that arrived after the last
  // mirror still resolves.
  const fromSocket = await db.sendCommand({ cmd: "sym", name }, undefined, 5_000)
    .catch(() => ({ status: "error" }) as db.JsonObject);
  if (fromSocket["status"] === "ok") {
    const symbol = symbolFromSocket(fromSocket);
    if (symbol !== undefined) {
      return {
        resolved: {
          name: symbol.name,
          requested: name,
          space: symbol.space,
          linear: symbol.linear,
          offset: symbol.offset,
          explanation: symbol.explanation,
          source: symbol.source,
        } as ResolvedAddress,
        warning: undefined,
      };
    }
  }

  let symError: Error | undefined;
  if (loadedSymbols !== undefined) {
    try {
      const effectiveLoadSegment = loadSegment ?? currentLoadSegment;
      const registers = lastStopRegisters ?? await readRegisters().catch(() => undefined);
      const resolved = await resolveSymbol(loadedSymbols, name, {
        loadSegment: effectiveLoadSegment,
        registers,
        readU32: readU32Linear,
      });

      const warning = loadSegment === undefined &&
        resolved.space === "com" &&
        currentLoadSegment !== undefined &&
        !currentLoadSegmentFresh
        ? `WARNING: loadSegment ${hex(currentLoadSegment)} is cached from a previous or unverified DOSBox session; load the program with breakAtEntry:true or pass loadSegment explicitly.`
        : undefined;

      return { resolved: { ...resolved, source: "sym" as const }, warning };
    } catch (e) {
      symError = e as Error;
    }
  }

  if (loadedMap !== undefined) {
    return { resolved: await resolveLoadedMapSymbol(name), warning: undefined };
  }

  if (symError !== undefined) throw symError;
  throw new Error("No symbols loaded. Pass symFile/mapFile or call dosbox_sym/dosbox_map load first.");
}

// Format registers + disasm into a single inspect block (shared by step + inspect)
function formatRegsBlock(r: Record<string, unknown>): string {
  const seg = (v: unknown) => (typeof v === "number" ? v.toString(16).padStart(4, "0") : String(v));
  return [
    "── Registers ──────────────────────────────────",
    `EAX=${r["EAX"]}  EBX=${r["EBX"]}  ECX=${r["ECX"]}  EDX=${r["EDX"]}`,
    `ESI=${r["ESI"]}  EDI=${r["EDI"]}  EBP=${r["EBP"]}  ESP=${r["ESP"]}`,
    `EIP=${r["EIP"]}  FLAGS=${r["FLAGS"]}`,
    `CS=${seg(r["CS"])}  DS=${seg(r["DS"])}  ES=${seg(r["ES"])}  ` +
    `SS=${seg(r["SS"])}  FS=${seg(r["FS"])}  GS=${seg(r["GS"])}`,
  ].join("\n");
}

function formatDisasmBlock(disasmResp: db.JsonObject): string {
  const lines = disasmResp["lines"] as DLine[] | undefined;
  if (!Array.isArray(lines)) return "";
  return [
    "── Disassembly ─────────────────────────────────",
    ...lines.map((l, i) => {
      const prefix = i === 0 ? "=>" : "  ";
      return `${prefix} ${l.addr}  ${l.bytes.padEnd(20)}  ${l.mnemonic}`;
    }),
  ].join("\n");
}

function symbolDisplayName(name: string, space: string): string {
  return space === "com" ? name : `${space}:${name}`;
}

async function buildSymbolLabels(registers: Record<string, number> | undefined): Promise<Map<number, string[]>> {
  const labels = new Map<number, string[]>();

  if (loadedSymbols !== undefined) {
    const callbacks = {
      loadSegment: currentLoadSegment,
      registers,
      readU32: readU32Linear,
    };

    for (const symbol of loadedSymbols.symbols.values()) {
      if (!symbol.tags.some((tag) => tag.startsWith("code"))) continue;

      try {
        const resolved = await resolveSymbol(loadedSymbols, symbol.name, callbacks);
        const names = labels.get(resolved.linear) ?? [];
        names.push(symbolDisplayName(symbol.name, resolved.space));
        labels.set(resolved.linear, names);
      } catch {
        // Some relocation spaces intentionally need registers that may not exist
        // yet (for example copied high-code labels before EBX is initialized).
      }
    }
  }

  if (loadedMap !== undefined && currentLoadInfo !== undefined) {
    const seenPublics = new Set<string>();
    for (const publicSymbol of loadedMap.publics.values()) {
      if (seenPublics.has(publicSymbol.name)) continue;
      seenPublics.add(publicSymbol.name);
      try {
        const linear = (effectiveLoadLinear(currentLoadInfo) + publicSymbol.mapOffset) >>> 0;
        const names = labels.get(linear) ?? [];
        names.push(`map:${publicSymbol.name}`);
        labels.set(linear, names);
      } catch {
        // MAP labels need a load base; absent until the program is loaded.
      }
    }
  }

  return labels;
}

function labelForAddress(labels: Map<number, string[]>, linear: number): string | undefined {
  const names = labels.get(linear >>> 0);
  if (names === undefined || names.length === 0) return undefined;
  return names.join(", ");
}

function addSymbolAlias(map: Map<number, string>, addr: number, label: string): void {
  const key = addr >>> 0;
  const existing = map.get(key);
  map.set(key, existing === undefined ? label : `${existing}, ${label}`);
}

async function buildMemorySymbols(registers: Record<string, number> | undefined): Promise<Map<number, string>> {
  const memory = new Map<number, string>();
  if (loadedSymbols === undefined) return memory;

  const callbacks = {
    loadSegment: currentLoadSegment,
    registers,
    readU32: readU32Linear,
  };

  for (const symbol of loadedSymbols.symbols.values()) {
    if (symbol.tags.some((tag) => tag.startsWith("code"))) continue;

    try {
      const resolved = await resolveSymbol(loadedSymbols, symbol.name, callbacks);
      const label = symbolDisplayName(symbol.name, resolved.space);

      // DOSBox's disassembler prints direct memory operands as effective
      // offsets (for COM/flat data labels), not always as linear addresses.
      addSymbolAlias(memory, symbol.offset, label);
      addSymbolAlias(memory, resolved.linear, label);
    } catch {
      // Data in relocation spaces can depend on registers not yet available.
    }
  }

  return memory;
}

function annotateBranchTarget(mnemonic: string, labels: Map<number, string[]>): string {
  const op = mnemonic.trimStart().split(/\s+/, 1)[0]?.toLowerCase() ?? "";
  const isBranchLike =
    op === "call" ||
    op === "jmp" ||
    op === "jcxz" ||
    op.startsWith("j") ||
    op.startsWith("loop");
  if (!isBranchLike) return mnemonic;

  return mnemonic.replace(/\b([0-9A-Fa-f]{8})\b/, (target) => {
    const label = labelForAddress(labels, Number.parseInt(target, 16));
    return label === undefined ? target : `${label} (${target.toUpperCase()})`;
  });
}

function annotateMemoryOperands(mnemonic: string, memory: Map<number, string>): string {
  return mnemonic.replace(/\[([0-9A-Fa-f]{4,8})\]/g, (operand, addrText: string) => {
    const label = memory.get(Number.parseInt(addrText, 16) >>> 0);
    return label === undefined ? operand : `[${label} (${addrText.toUpperCase()})]`;
  });
}

async function selectorBase(seg: number): Promise<number> {
  try {
    const si = await db.sendCommand({ cmd: "selinfo", sel: seg });
    if (si["status"] === "ok" && typeof si["base"] === "string") {
      return Number.parseInt(si["base"].replace(/^0x/i, ""), 16) >>> 0;
    }
  } catch {
    // selinfo is only meaningful in protected mode; real mode falls through.
  }
  return (seg << 4) >>> 0;
}

function linearFromDisasmAddress(addr: string, segmentBase?: number): number | undefined {
  const linearAddress = /^[0-9A-Fa-f]{8}$/.exec(addr);
  if (linearAddress !== null) return Number.parseInt(addr, 16) >>> 0;

  const segOffAddress = /^[0-9A-Fa-f]+:([0-9A-Fa-f]+)$/.exec(addr);
  if (segOffAddress !== null && segmentBase !== undefined) {
    return (segmentBase + Number.parseInt(segOffAddress[1], 16)) >>> 0;
  }

  return undefined;
}

async function formatDisasmLines(
  lines: DLine[],
  currentIdx: number | undefined,
  registers: Record<string, number> | undefined,
  segmentBase?: number,
): Promise<string[]> {
  const [labels, memory] = await Promise.all([
    buildSymbolLabels(registers),
    buildMemorySymbols(registers),
  ]);

  return lines.map((l, i) => {
    const linear = linearFromDisasmAddress(l.addr, segmentBase);
    const label = linear === undefined
      ? undefined
      : labelForAddress(labels, linear) ?? symbolIndex.describe(linear) ?? describeMapAddress(loadedMap, currentLoadInfo, linear);
    const prefix = i === currentIdx ? "=>" : "  ";
    const mnemonic = annotateMemoryOperands(annotateBranchTarget(l.mnemonic, labels), memory);
    const location = label === undefined ? "" : `${label}:`;
    const locationPrefix = location === "" ? "" : ` ${location.padEnd(24)}`;
    return `${prefix}${locationPrefix} ${l.addr}  ${l.bytes.padEnd(20)}  ${mnemonic}`;
  });
}

/**
 * Fetch regs + disasm centered on CS:EIP and return a formatted inspect block.
 * Shows `countBefore` instructions before the current PC (with alignment
 * verification — skipped silently if disassembly is misaligned) and
 * `countAfter` instructions at and after it, with `=>` marking the current PC.
 */
async function inspectAtCurrentPC(countBefore = 3, countAfter = 10): Promise<string> {
  const [regsResp, linResp] = await Promise.all([
    db.sendCommand({ cmd: "regs" }),
    db.sendCommand({ cmd: "get_linear_addr" }),
  ]);

  const linearStr = linResp["status"] === "ok"
    ? String(linResp["linear_addr"] ?? linResp["linear"] ?? "")
    : "";
  const pc = linearStr ? parseInt(linearStr.replace("0x", ""), 16) : undefined;

  const parts: string[] = [];
  const registers = regsResp["status"] === "ok"
    ? parseRegistersFromRecord(regsResp as Record<string, unknown>)
    : undefined;
  if (regsResp["status"] === "ok") parts.push(formatRegsBlock(regsResp as Record<string, unknown>));

  if (pc === undefined) return parts.join("\n");

  // Single round-trip: C++ back-disassembly finds aligned predecessors internally.
  const ctxResp = await db.sendCommand({
    cmd: "disasm_context",
    addr: pc,
    before: countBefore,
    after: countAfter,
  });

  type DLine = { addr: string; bytes: string; mnemonic: string };

  if (ctxResp["status"] === "ok") {
    const lines = ctxResp["lines"] as DLine[] ?? [];
    const currentIdx = typeof ctxResp["current_idx"] === "number"
      ? (ctxResp["current_idx"] as number)
      : 0;

    if (lines.length > 0) {
      const formattedLines = await formatDisasmLines(lines, currentIdx, registers);
      parts.push([
        "── Disassembly ─────────────────────────────────",
        ...formattedLines,
      ].join("\n"));
    }
  }

  return parts.join("\n");
}

/** Render a hex-dump from raw data string. */
function formatHexDump(data: string, baseAddr: number): string {
  const lines: string[] = [];
  for (let i = 0; i < data.length; i += 32) {
    const chunk = data.slice(i, i + 32);
    const tokens = chunk.match(/.{1,2}/g) ?? [];
    const addr = (baseAddr + i / 2).toString(16).padStart(8, "0").toUpperCase();
    const hexPart = tokens.join(" ").padEnd(47);
    const asciiPart = tokens.map((b) => {
      if (b === "PF" || b === "??") return "·";
      const c = parseInt(b, 16);
      return c >= 32 && c <= 126 ? String.fromCharCode(c) : ".";
    }).join("");
    lines.push(`${addr}  ${hexPart}  ${asciiPart}`);
  }
  return lines.join("\n");
}

function parseFaultLines(screenText: string): string[] {
  return screenText
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) =>
      /general protection fault|page fault|invalid opcode|exception/i.test(line)
    );
}

async function textScreenSummary(): Promise<{ text?: string; fullText?: string; faults: string[] }> {
  const resp = await db.sendCommand({ cmd: "text_screen" }).catch(() => undefined);
  if (resp === undefined || resp["status"] !== "ok" || typeof resp["text"] !== "string") {
    return { faults: [] };
  }
  const fullText = resp["text"] as string;
  const lines = fullText.split(/\r?\n/).filter((line) => line.trim().length > 0);
  return { text: lines.slice(-10).join("\n"), fullText, faults: parseFaultLines(fullText) };
}

async function activeSegmentSummary(registers: Record<string, number> | undefined): Promise<string[]> {
  if (registers === undefined) return [];
  const segNames = ["CS", "DS", "ES", "SS"] as const;
  const lines: string[] = [];
  for (const name of segNames) {
    const sel = registers[name];
    if (sel === undefined) continue;
    const info = await db.sendCommand({ cmd: "selinfo", sel }).catch(() => undefined);
    if (info === undefined || info["status"] !== "ok") {
      lines.push(`${name}=${hex(sel)}`);
      continue;
    }
    lines.push(
      `${name}=${hex(sel)} base=${info["base"]} limit=${info["limit"]} ` +
      `type=${info["seg_type"]} d/b=${info["big"] ? 32 : 16}`,
    );
  }
  return lines;
}

async function stackSummary(registers: Record<string, number> | undefined): Promise<string | undefined> {
  const ss = registers?.["SS"];
  const esp = registers?.["ESP"];
  if (ss === undefined || esp === undefined) return undefined;
  const base = await selectorBase(ss);
  const linear = (base + esp) >>> 0;
  const resp = await db.sendCommand({ cmd: "mem_read_linear", addr: linear, len: 64 }).catch(() => undefined);
  if (resp === undefined || resp["status"] !== "ok" || typeof resp["data"] !== "string") return undefined;
  return formatHexDump(resp["data"] as string, linear);
}

async function buildContextReport(useLastStop = true): Promise<string> {
  const lastStop = useLastStop ? (lastStopEvent ?? await fetchLastStopFallback()) : undefined;
  const [regsResp, screen] = await Promise.all([
    db.sendCommand({ cmd: "regs" }).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject),
    textScreenSummary(),
  ]);
  const registers = regsResp["status"] === "ok"
    ? parseRegistersFromRecord(regsResp as Record<string, unknown>)
    : lastStopRegisters;
  const linearResp = await db.sendCommand({ cmd: "get_linear_addr" }).catch(() => undefined);
  const linearText = linearResp?.["status"] === "ok"
    ? String(linearResp["linear_addr"] ?? linearResp["linear"] ?? "")
    : "";
  const linear = linearText === "" ? undefined : Number.parseInt(linearText.replace(/^0x/i, ""), 16) >>> 0;

  const [segments, stack, inspect] = await Promise.all([
    activeSegmentSummary(registers),
    stackSummary(registers),
    inspectAtCurrentPC(5, 12).catch((e) => `inspect failed: ${(e as Error).message}`),
  ]);

  const dosErrors = scrapeDosErrors(screen.fullText ?? screen.text ?? "");
  const parts: string[] = [];
  parts.push("── Stop Context ────────────────────────────────");
  if (lastStop !== undefined) parts.push(`lastStop: ${j(lastStop)}`);
  if (lastProcessExit !== undefined) {
    parts.push(`processExit: code=${lastProcessExit["exit_code"]} psp=${lastProcessExit["psp"]} ` +
      `tsr=${lastProcessExit["tsr"]} abnormal=${lastProcessExit["abnormal"]}`);
  }
  if (linear !== undefined) {
    parts.push(`where: ${symbolIndex.describe(linear) ?? describeMapAddress(loadedMap, currentLoadInfo, linear) ?? hex(linear)}`);
  }
  if (screen.faults.length > 0) parts.push(`screenFaults: ${screen.faults.join(" | ")}`);
  if (dosErrors.length > 0) parts.push(`dosErrors: ${j(dosErrors)}`);
  if (segments.length > 0) parts.push("── Selectors ───────────────────────────────────", ...segments);
  if (inspect.length > 0) parts.push(inspect);
  if (stack !== undefined) parts.push("── Stack ───────────────────────────────────────", stack);
  if (screen.text !== undefined) parts.push("── Screen Tail ─────────────────────────────────", screen.text);
  return parts.join("\n");
}

function readModuleU32(module: D32Module, fileBytes: Buffer, section: number, off: number): number {
  const fileOff = section === 0
    ? module.header.codeOffset + off
    : section === 1
      ? module.header.dataOffset + off
      : undefined;
  if (fileOff === undefined || fileOff + 4 > fileBytes.length) return 0;
  return fileBytes.readUInt32LE(fileOff);
}

async function verifyD32Relocs(module: D32Module, base: number): Promise<Array<Record<string, unknown>>> {
  const fileBytes = readFileSync(module.path);
  const results: Array<Record<string, unknown>> = [];
  for (const [index, reloc] of module.relocs.entries()) {
    const patchAddr = sectionRuntimeAddress(module, base, reloc.patchSec, reloc.patchOff);
    const targetAddr = sectionRuntimeAddress(module, base, reloc.targetSec, reloc.targetOff);
    const addend = readModuleU32(module, fileBytes, reloc.patchSec, reloc.patchOff);
    const live = await readU32Linear(patchAddr).catch(() => undefined);
    let expected: number | undefined;
    if (reloc.kind === 1) expected = (addend + targetAddr) >>> 0;
    if (reloc.kind === 2) expected = (addend + targetAddr - patchAddr) >>> 0;
    if (reloc.kind === 3) expected = (addend + sectionRuntimeAddress(module, base, reloc.patchSec, 0)) >>> 0;
    results.push({
      index,
      kind: reloc.kind === 1 ? "ABS32" : reloc.kind === 2 ? "REL32" : reloc.kind === 3 ? "RELATIVE" : reloc.kind,
      patch: `${sectionName(reloc.patchSec)}+${hex(reloc.patchOff)} @ ${hex(patchAddr)}`,
      target: `${sectionName(reloc.targetSec)}+${hex(reloc.targetOff)} @ ${hex(targetAddr)}`,
      addend: hex(addend),
      expected: expected === undefined ? undefined : hex(expected),
      live: live === undefined ? "unreadable" : hex(live),
      ok: expected !== undefined && live === expected,
    });
  }
  return results;
}

// ─── 1. Process lifecycle ─────────────────────────────────────────────────── //

server.tool(
  "dosbox_launch",
  "Launch DOSBox-X with the debug socket. Must be called before any other debug tool. " +
  "Returns the process PID and confirms the socket is up. " +
  "conf: path to the DOSBox-X config file. " +
  "skipStartupFaults: convenience option to arm catch_exceptions with skip filters after launch " +
  "(e.g. [{vec:14,count:1}] to skip one CWSDPMI startup page fault).",
  {
    conf: z.string().optional().describe("Path to DOSBox-X config file."),
    headless: z.boolean().optional().describe("Run without a display window (SDL_VIDEODRIVER=dummy)."),
    skipStartupFaults: z.array(z.object({
      vec: z.number().int().min(0).max(31).describe("Exception vector (0-31)."),
      count: z.number().int().min(1).describe("Skip count (times to ignore before stopping)."),
    })).optional().describe(
      "Post-launch convenience: arm catch_exceptions with skip filters. " +
      "E.g. [{vec:14,count:1}] skips one page fault (CWSDPMI startup PF).",
    ),
  },
  async ({ conf, headless, skipStartupFaults }) => {
    // db.launch waits 15 s for the socket; giving up sooner than it does
    // reported every slow-but-successful launch as a failure.
    const result = await withTimeout("dosbox_launch", 20_000, db.launch({ conf, headless }));
    clearMcpDebugSessionState();
    // Only for reporting which drives are mounted; the emulator reads a
    // program's debug info through its own filesystem, so nothing here has to
    // turn a guest path into a host one.
    guestMounts = loadConfMounts(conf ?? db.DEFAULT_CONF);
    const parts: string[] = [j(result)];
    if (guestMounts.length > 0) {
      parts.push(`mounts → ${guestMounts.map((mount) => `${mount.drive}:=${mount.hostPath}`).join(" ")}`);
    }
    if (skipStartupFaults !== undefined && skipStartupFaults.length > 0 && result.socketUp) {
      // Arm catch_exceptions with skip filters immediately after launch.
      const vectors = [...new Set(skipStartupFaults.map((f) => f.vec))];
      const skip = skipStartupFaults.map((f) => ({ vec: f.vec, count: f.count }));
      const catchResp = await db.sendCommand(
        { cmd: "catch_exceptions", vectors, skip },
        undefined,
        4_000,
      ).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
      parts.push(`catch_exceptions (skipStartupFaults) → ${j(catchResp)}`);
    }
    return { content: [{ type: "text", text: parts.join("\n") }] };
  },
);

server.tool(
  "dosbox_stop",
  "Kill the running DOSBox-X process. Cleans up the PID file.",
  {},
  async () => {
    const result = await withTimeout("dosbox_stop", 5_000, db.stop());
    clearMcpDebugSessionState();
    return { content: [{ type: "text", text: result }] };
  },
);

server.tool(
  "dosbox_status",
  "Check whether DOSBox is running and the debug socket is responsive. " +
  "Also includes latched process exit info and socket state flags (breakOnExit, traceEnabled, etc.).",
  {},
  async () => {
    const st = await withTimeout("dosbox_status", 3_000, db.status());
    if (st.socketResponsive) await harvestLatchedCriticalEvents();
    // Augment with socket-level status fields (breakOnExit, traceEnabled, etc.) and last exit
    const socketSt = st.socketResponsive
      ? await db.sendCommand({ cmd: "status" }, undefined, 2_000).catch(() => undefined)
      : undefined;
    // Process exit: use latched value or fall back to get_last_exit
    const exitInfo = lastProcessExit ?? (st.socketResponsive
      ? await db.sendCommand({ cmd: "get_last_exit" }, undefined, 2_000)
          .then((r) => r["status"] === "ok" && r["available"] === true ? r : undefined)
          .catch(() => undefined)
      : undefined);
    return {
      content: [{
        type: "text",
        text: j({
          ...st,
          socketStatus: socketSt !== undefined && socketSt["status"] === "ok" ? socketSt : undefined,
          processExit: exitInfo !== undefined
            ? {
              exit_code: exitInfo["exit_code"],
              psp: exitInfo["psp"],
              tsr: exitInfo["tsr"],
              abnormal: exitInfo["abnormal"],
            }
            : undefined,
          notifications: notificationSummary(),
        }),
      }],
    };
  },
);

server.tool(
  "dosbox_notifications",
  "List or acknowledge MCP-latched critical debugger notifications. " +
  "Critical notifications include process_exit, stopped breakpoints/steps/exceptions/watchpoints, and socket disconnects.",
  {
    op: z.enum(["list", "ack"]).describe("list returns notifications; ack marks unread notifications read."),
    ids: z.array(z.number().int()).optional().describe("Notification ids to ack. Omit to ack all unread."),
    includeRead: z.boolean().optional().describe("For list, include already acknowledged notifications (default false)."),
  },
  async ({ op, ids, includeRead = false }) => {
    if (op === "ack") {
      return { content: [{ type: "text", text: j(ackNotifications(ids)) }] };
    }
    return {
      content: [{
        type: "text",
        text: j({
          status: "ok",
          notifications: includeRead ? pendingNotifications : unreadNotifications(),
          unreadCount: unreadNotifications().length,
        }),
      }],
    };
  },
);

server.tool(
  "dosbox_reset",
  "Reset the emulated DOSBox machine/session without killing the host DOSBox process. " +
  "Clears stale MCP state by default because loadInfo, symbols, modules, breakpoints, watchpoints, trace, and reverse history no longer describe the new guest session.",
  {
    preserveSymbols: z.boolean().optional().describe("Keep MCP-loaded symbol files/modules across reset (default false)."),
    preserveNotifications: z.boolean().optional().describe("Keep pending MCP notifications across reset (default false)."),
    preserveExitPolicy: z.boolean().optional().describe("Keep socket break_on_exit setting across reset (default false)."),
    waitForShell: z.boolean().optional().describe("Wait for the DOS shell to become ready again after reset (default true)."),
    autoContinueToShell: z.boolean().optional().describe("If reset leaves the CPU stopped before the shell, continue once and wait again unless a debugger stop is frozen (default true)."),
    timeoutMs: z.number().int().min(100).max(30_000).optional().describe("Reset/shell wait timeout (default 10000)."),
  },
  async ({ preserveSymbols = false, preserveNotifications = false, preserveExitPolicy = false, waitForShell = true, autoContinueToShell = true, timeoutMs = 10_000 }) => {
    try {
      const before = await db.status();
      const resp = await db.sendCommand(
        { cmd: "machine_reset", preserveExitPolicy: preserveExitPolicy ? 1 : 0 },
        undefined,
        2_000,
      );
      clearMcpDebugSessionState({ preserveSymbols, preserveNotifications });
      let shellReady: db.JsonObject | undefined;
      if (waitForShell) {
        shellReady = await ensureShellReady(timeoutMs, autoContinueToShell).catch((e) => ({
          status: "error",
          msg: (e as Error).message,
        }) as db.JsonObject);
      }
      const clearResp = await db.sendCommand(
        { cmd: "clear_debug_state", preserveExitPolicy: preserveExitPolicy ? 1 : 0 },
        undefined,
        2_000,
      ).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
      const after = await db.status().catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
      return {
        content: [{
          type: "text",
          text: j({
            status: isErr(resp) ? "error" : "ok",
            socket: resp,
            socketClearAfterReset: clearResp,
            before,
            after,
            shellReady,
            cleared: {
              mcpLoadInfo: true,
              breakpoints: true,
              watchpoints: true,
              trace: true,
              reverse: true,
              snapshots: true,
              symbols: !preserveSymbols,
              notifications: !preserveNotifications,
            },
            limitations: "Uses DOSBox-X On_Software_CPU_Reset. The DOSBox host process and debug listener stay alive; host-side GUI/device state is not recreated from process start, so this is a guest reboot/reset, not a full process restart.",
            notifications: notificationSummary(),
          }),
        }],
        isError: isErr(resp),
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_exceptions",
  "First-class wrapper for socket catch_exceptions. arm/disarm/status first-chance exception catching.",
  {
    op: z.enum(["arm", "disarm", "status"]).describe("Operation."),
    vectors: z.array(z.number().int().min(0).max(31)).optional().describe("Exception vectors to catch (default [6,8,12,13,14] for arm)."),
    skip: z.array(z.object({
      vec: z.number().int().min(0).max(31),
      count: z.number().int(),
    })).optional().describe("Per-vector skip counts, e.g. [{vec:14,count:1}] for one startup page fault."),
    cr2Ignore: z.array(z.object({
      start: z.number().int().min(0),
      end: z.number().int().min(0),
    })).optional().describe("Page-fault CR2 ranges to ignore."),
  },
  async ({ op, vectors, skip, cr2Ignore }) => {
    const resp = op === "status"
      ? await db.sendCommand({ cmd: "catch_exceptions", op: "status" })
      : await db.sendCommand({
        cmd: "catch_exceptions",
        vectors: op === "disarm" ? [] : (vectors ?? [6, 8, 12, 13, 14]),
        skip,
        cr2_ignore: cr2Ignore,
      });
    return {
      content: [{ type: "text", text: j({ ...resp, notifications: notificationSummary() }) }],
      isError: isErr(resp),
    };
  },
);

server.tool(
  "dosbox_trace",
  "First-class wrapper for socket branch trace. start/stop/status/dump the branch ring.",
  {
    op: z.enum(["start", "stop", "status", "dump"]).describe("Operation."),
    last: z.number().int().min(1).max(8192).optional().describe("For dump, newest N entries."),
  },
  async ({ op, last }) => {
    const resp = await db.sendCommand(clean({ cmd: "trace", op, last }), undefined, op === "dump" ? 5_000 : 2_000);
    return {
      content: [{ type: "text", text: j({ ...resp, notifications: notificationSummary() }) }],
      isError: isErr(resp),
    };
  },
);

server.tool(
  "dosbox_exit_policy",
  "Control and inspect process-exit handling. enable/disable maps to break_on_exit; status includes get_last_exit.",
  {
    op: z.enum(["enable", "disable", "status"]).describe("Operation."),
  },
  async ({ op }) => {
    const policy = op === "status"
      ? await db.sendCommand({ cmd: "status" }, undefined, 2_000)
      : await db.sendCommand({ cmd: "break_on_exit", enable: op === "enable" ? 1 : 0 }, undefined, 2_000);
    const lastExit = await db.sendCommand({ cmd: "get_last_exit" }, undefined, 2_000)
      .catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);
    if (lastExit["status"] === "ok" && lastExit["available"] === true) {
      lastProcessExit = {
        event: "process_exit",
        exit_code: lastExit["exit_code"],
        psp: lastExit["psp"],
        tsr: lastExit["tsr"],
        abnormal: lastExit["abnormal"],
      };
      enqueueCriticalNotification(lastProcessExit, "socket-latched");
    }
    return {
      content: [{ type: "text", text: j({ policy, lastExit, notifications: notificationSummary() }) }],
      isError: isErr(policy),
    };
  },
);

function symbolCoverageSummary(): db.JsonObject {
  const symbols = symbolIndex.all();
  const bySource: Record<string, number> = {};
  for (const symbol of symbols) bySource[symbol.source] = (bySource[symbol.source] ?? 0) + 1;
  return {
    symbolCount: symbols.length,
    bySource,
    symFile: loadedSymbolsPath,
    mapFile: loadedMapPath,
    elfFiles: [...loadedElfFiles.values()].map((entry) => ({ file: entry.file, base: hex(entry.base) })),
    d32Modules: [...d32Modules.values()].map((entry) => ({
      name: entry.module.name,
      file: entry.module.path,
      base: hex(entry.base),
      debugSymbols: entry.module.debugSymbols.length,
      exports: entry.module.exports.length,
    })),
    discoveredModules: discoveredModules.size,
  };
}

function recommendedNextAction(state: {
  socketResponsive: boolean;
  socketStatus?: db.JsonObject;
  shellReady?: boolean;
  notifications: CriticalNotification[];
  processExit?: db.JsonObject;
  symbolCount: number;
  traceStatus?: db.JsonObject;
  reverseStatus?: db.JsonObject;
}): string {
  if (!state.socketResponsive) return "Launch DOSBox or fix the debug socket before debugging.";
  if (state.notifications.some((n) => n["event"] === "process_exit")) return "Inspect process_exit, text screen, and result files; ack notifications when handled.";
  if (state.notifications.some((n) => n["reason"] === "exception" || n["reason"] === "watchpoint")) return "Run dosbox_crash_report, then use reverse/trace to backtrack before the symptom.";
  if (state.notifications.some((n) => n["event"] === "stopped")) return "Inspect the stopped context or continue/step intentionally before running shell commands.";
  if (state.shellReady === false) return "Wait for shell readiness or reset/relaunch if the guest is stuck before the shell.";
  if (state.socketStatus?.["normalCoreHooksActive"] !== true) return "Use a core=normal debug config; trace/reverse/watchpoints depend on normal-core hooks.";
  if (state.traceStatus?.["enabled"] !== true || state.reverseStatus?.["enabled"] !== true) return "Run dosbox_debug_prepare before the interesting repro to arm trace and reverse checkpoints.";
  if (state.symbolCount === 0) return "Load MAP/.sym/ELF/D32 symbols or scan modules before interpreting addresses.";
  if (state.processExit !== undefined) return "A process exit is latched; inspect it before continuing.";
  return "Observability is armed; run to a controlled breakpoint or continue_report with a short timeout.";
}

server.tool(
  "dosbox_debug_prepare",
  "One-call hard-bug session setup: wait for shell, arm exceptions, start branch trace, start reverse checkpoints, optionally break on process exit, load/search symbols, and return a compact readiness report.",
  {
    waitForShell: z.boolean().optional().describe("Wait for DOS shell readiness first (default true)."),
    autoContinueToShell: z.boolean().optional().describe("If the CPU is stopped before the shell, continue once and wait again unless a debugger stop is frozen (default true)."),
    respectFrozenStop: z.boolean().optional().describe("Do not auto-continue while stopped at a debugger event (default true)."),
    shellTimeoutMs: z.number().int().min(0).max(30_000).optional().describe("Shell wait timeout (default 5000)."),
    exceptionVectors: z.array(z.number().int().min(0).max(31)).optional().describe("Exception vectors to catch (default [6,8,12,13,14])."),
    skipStartupFaults: z.boolean().optional().describe("Skip one startup page fault on vector 14 (default true)."),
    skip: z.array(z.object({
      vec: z.number().int().min(0).max(31),
      count: z.number().int(),
    })).optional().describe("Additional/override exception skip list."),
    cr2Ignore: z.array(z.object({
      start: z.number().int().min(0),
      end: z.number().int().min(0),
    })).optional().describe("Page-fault CR2 ranges to ignore."),
    startTrace: z.boolean().optional().describe("Start branch trace (default true)."),
    startReverse: z.boolean().optional().describe("Start reverse checkpoints (default true)."),
    reverseMode: z.enum(["branch", "instruction"]).optional().describe("Reverse checkpoint mode (default branch)."),
    maxCheckpoints: z.number().int().min(2).max(8192).optional().describe("Reverse checkpoint capacity (default 512)."),
    breakOnExit: z.boolean().optional().describe("Freeze on process exit (default false; explicit true preserves old behavior)."),
    symFile: z.string().optional().describe("Optional .sym file to load."),
    mapFile: z.string().optional().describe("Optional Microsoft LINK .MAP file to load."),
    elfFiles: z.array(z.object({
      file: z.string(),
      base: z.number().int().min(0),
    })).optional().describe("ELF symbol files and runtime bases to register."),
    d32SearchPaths: z.array(z.string()).optional().describe("Directories/files for D32 module scan registration."),
    scanD32Modules: z.boolean().optional().describe("Scan memory and auto-register D32 modules when search paths are supplied."),
    moduleRangeStart: z.number().int().min(0).optional().describe("D32 scan start (default 0x100000)."),
    moduleRangeEnd: z.number().int().min(0).optional().describe("D32 scan end (default 0x2000000)."),
    verifySymbol: z.string().optional().describe("Optional symbol to resolve as a readiness check."),
  },
  async ({
    waitForShell = true,
    autoContinueToShell = true,
    respectFrozenStop = true,
    shellTimeoutMs = 5_000,
    exceptionVectors = [6, 8, 12, 13, 14],
    skipStartupFaults = true,
    skip,
    cr2Ignore,
    startTrace = true,
    startReverse = true,
    reverseMode = "branch",
    maxCheckpoints = 512,
    breakOnExit = false,
    symFile,
    mapFile,
    elfFiles,
    d32SearchPaths,
    scanD32Modules = false,
    moduleRangeStart = 0x100000,
    moduleRangeEnd = 0x2000000,
    verifySymbol,
  }) => {
    const report: db.JsonObject = {};
    try {
      const effectiveSkip = skip ?? (skipStartupFaults ? [{ vec: 14, count: 1 }] : undefined);
      report["exceptions"] = await db.sendCommand({
        cmd: "catch_exceptions",
        vectors: exceptionVectors,
        skip: effectiveSkip,
        cr2_ignore: cr2Ignore,
      }, undefined, 4_000);
      if (waitForShell) {
        report["shell"] = await ensureShellReady(shellTimeoutMs, autoContinueToShell, respectFrozenStop);
        if ((report["shell"] as db.JsonObject)["status"] === "blocked") {
          return {
            content: [{
              type: "text",
              text: j({
                status: "blocked",
                setup: report,
                notifications: notificationSummary(),
                recommendedNextAction: "Inspect the current debug stop or continue/step intentionally before preparing the session.",
              }),
            }],
            isError: true,
          };
        }
      }
      if (startTrace) report["trace"] = await db.sendCommand({ cmd: "trace", op: "start" }, undefined, 2_000);
      if (startReverse) {
        report["reverse"] = await db.sendCommand({
          cmd: "reverse_trace",
          op: "start",
          mode: reverseMode,
          max_checkpoints: maxCheckpoints,
        }, undefined, 4_000);
      }
      if (breakOnExit) report["exitPolicy"] = await db.sendCommand({ cmd: "break_on_exit", enable: 1 }, undefined, 2_000);

      if (symFile !== undefined) {
        loadedSymbols = loadSymFile(symFile);
        loadedSymbolsPath = symFile;
      }
      if (mapFile !== undefined) {
        loadedMap = loadLinkMapFile(mapFile);
        loadedMapPath = mapFile;
      }
      if (elfFiles !== undefined) {
        for (const entry of elfFiles) loadedElfFiles.set(entry.file, { file: entry.file, base: entry.base });
      }
      rebuildSymbolIndex();

      if (d32SearchPaths !== undefined) moduleSearchPaths = d32SearchPaths;
      if (scanD32Modules) {
        report["modules"] = await scanDiscoveredModules(moduleRangeStart, moduleRangeEnd, moduleSearchPaths);
      }

      const socketStatus = await db.sendCommand({ cmd: "status" }, undefined, 2_000).catch(() => undefined);
      const traceStatus = await db.sendCommand({ cmd: "trace", op: "status" }, undefined, 2_000).catch(() => undefined);
      const reverseStatus = await db.sendCommand({ cmd: "reverse_trace", op: "status" }, undefined, 2_000).catch(() => undefined);
      const verify = verifySymbol === undefined
        ? undefined
        : symbolIndex.resolve(verifySymbol) ?? await resolveLoadedSymbol(verifySymbol).catch(() => undefined);
      await harvestLatchedCriticalEvents();
      const coverage = symbolCoverageSummary();
      return {
        content: [{
          type: "text",
          text: j({
            status: "ok",
            readiness: {
              shellReady: report["shell"] === undefined ? undefined : (report["shell"] as db.JsonObject)["shellReady"],
              normalCoreHooksActive: socketStatus?.["normalCoreHooksActive"],
              exceptionsArmed: (report["exceptions"] as db.JsonObject | undefined)?.["armed"],
              traceEnabled: traceStatus?.["enabled"] ?? socketStatus?.["traceEnabled"],
              reverseEnabled: reverseStatus?.["enabled"] ?? socketStatus?.["reverseTraceEnabled"],
              breakOnExit: socketStatus?.["breakOnExit"],
            },
            setup: report,
            symbolCoverage: coverage,
            verifySymbol: verify,
            notifications: notificationSummary(),
            recommendedNextAction: recommendedNextAction({
              socketResponsive: true,
              socketStatus,
              shellReady: report["shell"] === undefined ? undefined : (report["shell"] as db.JsonObject)["shellReady"] === true,
              notifications: unreadNotifications(),
              symbolCount: Number(coverage["symbolCount"] ?? 0),
              traceStatus,
              reverseStatus,
            }),
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}\n${j(report)}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_debug_state",
  "Canonical debugger state check: tells whether the agent is blind, shows pending notifications/process exit/last stop, trace/reverse/watchpoint/symbol/module coverage, and recommends the next action.",
  {
    ackNotifications: z.boolean().optional().describe("Acknowledge unread notifications after reading them (default false)."),
    includeContext: z.boolean().optional().describe("Include compact stop context when stopped (default false)."),
  },
  async ({ ackNotifications: ack = false, includeContext = false }) => {
    try {
      const process = await db.status();
      if (process.socketResponsive) await harvestLatchedCriticalEvents();
      const [socketStatus, shell, traceStatus, reverseStatus, bpList, wpList, lastExit] = process.socketResponsive
        ? await Promise.all([
          db.sendCommand({ cmd: "status" }, undefined, 2_000).catch(() => undefined),
          db.sendCommand({ cmd: "wait_for_shell", timeoutMs: 0 }, undefined, 1_000).catch(() => undefined),
          db.sendCommand({ cmd: "trace", op: "status" }, undefined, 2_000).catch(() => undefined),
          db.sendCommand({ cmd: "reverse_trace", op: "status" }, undefined, 2_000).catch(() => undefined),
          db.sendCommand({ cmd: "bp_list" }, undefined, 2_000).catch(() => undefined),
          db.sendCommand({ cmd: "wp_list" }, undefined, 2_000).catch(() => undefined),
          db.sendCommand({ cmd: "get_last_exit" }, undefined, 2_000).catch(() => undefined),
        ])
        : [undefined, undefined, undefined, undefined, undefined, undefined, undefined];
      const stop = process.socketResponsive ? (lastStopEvent ?? await fetchLastStopFallback()) : undefined;
      const coverage = symbolCoverageSummary();
      const context = includeContext && process.socketResponsive
        ? await buildContextReport(true).catch((e) => `context unavailable: ${(e as Error).message}`)
        : undefined;
      const notificationsBeforeAck = unreadNotifications();
      const recommendation = recommendedNextAction({
        socketResponsive: process.socketResponsive,
        socketStatus,
        shellReady: shell?.["shellReady"] === true,
        notifications: notificationsBeforeAck,
        processExit: lastExit?.["status"] === "ok" && lastExit["available"] === true ? lastExit : undefined,
        symbolCount: Number(coverage["symbolCount"] ?? 0),
        traceStatus,
        reverseStatus,
      });
      const ackResult = ack ? ackNotifications() : undefined;
      return {
        content: [{
          type: "text",
          text: j({
            status: "ok",
            process,
            shellReady: shell?.["shellReady"] ?? false,
            cpu: socketStatus === undefined ? undefined : {
              state: socketStatus["state"],
              running: socketStatus["cpuRunning"],
              frozen: socketStatus["socketFrozen"],
              debuggerFrozen: socketStatus["debuggerFrozen"],
              normalCoreHooksActive: socketStatus["normalCoreHooksActive"],
            },
            notifications: ack ? { beforeAck: notificationsBeforeAck, ack: ackResult, current: notificationSummary() } : notificationSummary(),
            processExit: lastExit?.["status"] === "ok" && lastExit["available"] === true ? lastExit : lastProcessExit,
            lastStop: stop,
            controls: {
              exceptions: {
                armed: socketStatus?.["catchExceptionsArmed"],
              },
              trace: traceStatus,
              reverse: reverseStatus,
              breakOnExit: socketStatus?.["breakOnExit"],
              breakpoints: bpList,
              watchpoints: wpList,
            },
            symbolCoverage: coverage,
            modules: {
              registered: [...d32Modules.values()].map((entry) => ({ name: entry.module.name, base: hex(entry.base), file: entry.module.path })),
              discovered: [...discoveredModules.values()].map((entry) => ({ ...entry.probe, file: entry.file, registered: entry.registered })),
            },
            recommendedNextAction: recommendation,
            context,
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_get_load_info",
  "Return the latest authoritative DOS EXEC load metadata from DOSBox-X. " +
  "The socket command is {cmd:\"get_load_info\"}; when available it returns " +
  "loadInfo with PSP, MZ image base, entry CS:IP, initial SS:SP, image size, relocation count/table offset, " +
  "and raw MZ header fields. DOSBox-X does not know Microsoft LINK segment names; MCP combines loadSeg/loadLinear " +
  "with parsed .MAP segment starts to compute per-segment runtime bases.",
  {},
  async () => {
    const resp = await db.sendCommand({ cmd: "get_load_info" });
    const loadInfo = loadInfoFromResponse(resp);
    if (loadInfo !== undefined) {
      currentLoadInfo = loadInfo;
      await syncSocketSymbols();
    }
    return { content: [{ type: "text", text: j({ ...resp, normalized: loadInfo }) }], isError: isErr(resp) };
  },
);

// ─── 2. DOS shell / program execution ────────────────────────────────────── //

server.tool(
  "dosbox_run_program",
  "Run a DOS command or program. When breakAtEntry is true, halts at the program " +
  "entry point before the first instruction and returns the full stopped event with loadInfo when available. " +
  "For shell-only commands (dir, cd, cwsdpmi -p …) omit breakAtEntry.",
  {
    command: z.string().max(255).describe(
      "DOS command or EXE/COM filename, e.g. 'vbe32.com' or 'cwsdpmi -p'.",
    ),
    breakAtEntry: z.boolean().optional().describe(
      "Stop at program entry point (sets bp_on_load before running).",
    ),
    respectFrozenStop: z.boolean().optional().describe("Do not issue the shell command while stopped at a debugger event (default true)."),
    breakOnInterrupts: z.boolean().optional().describe("Stop on plain interrupt stops including INT 3 instead of auto-continuing through INT 3 (default false)."),
    mapFile: z.string().optional().describe("Host path to a Microsoft LINK .MAP file to load for this program."),
  },
  async ({ command, breakAtEntry, respectFrozenStop = true, breakOnInterrupts = false, mapFile }) => {
    const parts: string[] = [];
    if (mapFile !== undefined) {
      loadedMap = loadLinkMapFile(mapFile);
      loadedMapPath = mapFile;
      rebuildSymbolIndex();
      parts.push(`map → ${mapFile} (${loadedMap.segments.length} segments, ${loadedMap.publics.size} public keys)`);
    }

    const shell = await prepareShellForDosCommand(5_000, respectFrozenStop);
    if (isErr(shell)) {
      parts.push(`shell → ${j(shell)}`);
      return { content: [{ type: "text", text: parts.join("\n\n") }], isError: true };
    }

    if (breakAtEntry) {
      const bpResp = await db.sendCommand({ cmd: "bp_on_load" });
      parts.push(`bp_on_load → ${j(bpResp)}`);
      if (isErr(bpResp)) {
        return { content: [{ type: "text", text: parts.join("\n") }], isError: true };
      }
    }

    const resp = await sendDosCommand(command, 30_000, breakOnInterrupts || breakAtEntry === true);
    parts.push(`dos_cmd → ${j(resp)}`);
    await appendDosErrorReport(parts);
    if (breakAtEntry) {
      const loadSegmentSummary = await captureStopState(resp);
      if (loadSegmentSummary !== undefined) parts.push(loadSegmentSummary);
    }

    return {
      content: [{ type: "text", text: parts.join("\n\n") }],
      isError: isErr(resp),
    };
  },
);

server.tool(
  "dosbox_debug_load_program",
  "Load symbols and run a DOS program, optionally stopping at entry. When stopped " +
  "at entry, DOSBox-X loadInfo is used to capture the current load segment for later " +
  "symbol resolution.",
  {
    command: z.string().max(255).describe("DOS command, typically the COM/EXE filename."),
    symFile: z.string().optional().describe("Host path to a custom .sym file."),
    mapFile: z.string().optional().describe("Host path to a Microsoft LINK .MAP file."),
    breakAtEntry: z.boolean().optional().describe("Stop at program entry before first instruction."),
    respectFrozenStop: z.boolean().optional().describe("Do not issue the shell command while stopped at a debugger event (default true)."),
    breakOnInterrupts: z.boolean().optional().describe("Stop on plain interrupt stops including INT 3 instead of auto-continuing through INT 3 (default false)."),
  },
  async ({ command, symFile, mapFile, breakAtEntry, respectFrozenStop = true, breakOnInterrupts = false }) => {
    const parts: string[] = [];
    try {
      if (symFile !== undefined) {
        loadedSymbols = loadSymFile(symFile);
        loadedSymbolsPath = symFile;
        rebuildSymbolIndex();
        parts.push(`symbols → ${loadedSymbols.module.name} (${loadedSymbols.symbols.size} symbols)`);
      }
      if (mapFile !== undefined) {
        loadedMap = loadLinkMapFile(mapFile);
        loadedMapPath = mapFile;
        rebuildSymbolIndex();
        parts.push(`map → ${mapFile} (${loadedMap.segments.length} segments, ${loadedMap.publics.size} public keys)`);
      }

      const shell = await prepareShellForDosCommand(5_000, respectFrozenStop);
      if (isErr(shell)) {
        parts.push(`shell → ${j(shell)}`);
        return { content: [{ type: "text", text: parts.join("\n\n") }], isError: true };
      }

      if (breakAtEntry === true) {
        const bpResp = await db.sendCommand({ cmd: "bp_on_load" });
        parts.push(`bp_on_load → ${j(bpResp)}`);
        if (isErr(bpResp)) {
          return { content: [{ type: "text", text: parts.join("\n\n") }], isError: true };
        }
      }

      const resp = await sendDosCommand(command, 30_000, breakOnInterrupts || breakAtEntry === true);
      parts.push(`dos_cmd → ${j(resp)}`);
      await appendDosErrorReport(parts);
      if (breakAtEntry === true) {
        const loadSegmentSummary = await captureStopState(resp);
        if (loadSegmentSummary !== undefined) parts.push(loadSegmentSummary);
      }

      return {
        content: [{ type: "text", text: parts.join("\n\n") }],
        isError: isErr(resp),
      };
    } catch (e) {
      return {
        content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }],
        isError: true,
      };
    }
  },
);

server.tool(
  "dosbox_load_and_run_to",
  "Atomic cold-start helper: load a program and run it to a named symbol in one step. " +
  "Equivalent to: load_program(breakAtEntry) → bp_set(name) → continue — but done within " +
  "a single tool call so there is no ordering mistake or race window between steps. " +
  "Works for code16 / low-segment symbols (e.g. call_hi). " +
  "For hi/copied-code symbols that need EBX, run to call_hi first then use dosbox_run_to.",
  {
    command: z.string().max(255).describe("DOS command to run (e.g. 'vbe32.com')."),
    name: z.string().describe("Symbol name to stop at (e.g. 'call_hi')."),
    symFile: z.string().optional().describe("Host path to .sym file (uses already-loaded symbols if omitted)."),
    mapFile: z.string().optional().describe("Host path to a Microsoft LINK .MAP file."),
    once: z.boolean().optional().describe("One-shot breakpoint (default true)."),
    timeoutMs: z.number().int().optional().describe("Max wait ms for the named breakpoint (default 30 000)."),
    respectFrozenStop: z.boolean().optional().describe("Do not issue the shell command while stopped at a debugger event (default true)."),
    breakOnInterrupts: z.boolean().optional().describe("Stop on plain interrupt stops including INT 3 instead of auto-continuing through INT 3 before the entry stop (default false)."),
  },
  async ({ command, name, symFile, mapFile, once = true, timeoutMs = 30_000, respectFrozenStop = true, breakOnInterrupts = false }) => {
    const parts: string[] = [];
    try {
      // Step 1: Load symbols if a file was provided.
      if (symFile !== undefined) {
        loadedSymbols = loadSymFile(symFile);
        loadedSymbolsPath = symFile;
        rebuildSymbolIndex();
        parts.push(`symbols → ${loadedSymbols.module.name} (${loadedSymbols.symbols.size} symbols)`);
      }
      if (mapFile !== undefined) {
        loadedMap = loadLinkMapFile(mapFile);
        loadedMapPath = mapFile;
        rebuildSymbolIndex();
        parts.push(`map → ${mapFile} (${loadedMap.segments.length} segments, ${loadedMap.publics.size} public keys)`);
      }

      const shell = await prepareShellForDosCommand(5_000, respectFrozenStop);
      if (isErr(shell)) {
        parts.push(`shell → ${j(shell)}`);
        return { content: [{ type: "text", text: parts.join("\n\n") }], isError: true };
      }

      // Step 2: Break at entry to capture the load segment.
      const bpLoadResp = await db.sendCommand({ cmd: "bp_on_load" });
      if (isErr(bpLoadResp)) {
        return { content: [{ type: "text", text: `bp_on_load failed: ${j(bpLoadResp)}` }], isError: true };
      }

      const entryResp = await sendDosCommand(command, 30_000, true);
      const loadSegmentSummary = await captureStopState(entryResp);
      if (loadSegmentSummary !== undefined) {
        parts.push(`entry stop → ${loadSegmentSummary}`);
      } else {
        // Program exited without an entry stop (e.g. bp_on_load not supported here).
        parts.push(`WARNING: no entry stop — ${j(entryResp)}`);
      }

      // Step 3: Resolve and set the named breakpoint while the CPU is still frozen.
      // (All within this single tool call — no gap for the debug loop to exit.)
      const resolved = await resolveLoadedSymbol(name);
      parts.push(`bp → ${name} = ${hex(resolved.linear)}  (${resolved.explanation})`);

      const bpSetResp = await db.sendCommand({
        cmd: "bp_set_linear_exec",
        linear: resolved.linear,
        match_off: breakpointMatchOff(resolved),
        once: once ? 1 : 0,
      });
      if (isErr(bpSetResp)) {
        return { content: [{ type: "text", text: `bp_set failed: ${j(bpSetResp)}` }], isError: true };
      }

      // Step 4: Resume and wait for the named breakpoint.
      const stopResp = await db.continueAndWait(undefined, timeoutMs);
      await captureStopState(stopResp);
      const inspect = await inspectAtCurrentPC(10);
      parts.push(inspect);

      return { content: [{ type: "text", text: parts.join("\n") }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_key",
  "Inject a keypress into the DOS guest via the hardware 8042 keyboard controller " +
  "(key_hw). Works reliably under DPMI / protected-mode programs. " +
  "Named keys: enter, esc, space, a-z, 0-9.",
  {
    key: z.string().describe("Named key: enter, esc, space, a single lowercase letter a-z, or a digit 0-9."),
  },
  async ({ key }) => {
    const resp = await db.sendCommand({ cmd: "key_hw", key });
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

// ─── 3. Symbols ───────────────────────────────────────────────────────────── //

server.tool(
  "dosbox_sym",
  "Manage loaded symbols. " +
  "op='load': load a .sym file (file required; optional loadSegment). " +
  "op='list': show all symbols and relocations. " +
  "op='resolve': resolve a symbol name to its runtime linear address.",
  {
    op: z.enum(["load", "list", "resolve"]).describe("Operation: load | list | resolve"),
    file: z.string().optional().describe("Host path to .sym file (op=load)."),
    name: z.string().optional().describe("Symbol name, e.g. 'call_hi' or 'hi:vbe_mode' (op=resolve)."),
    loadSegment: z.number().int().optional().describe("DOS COM load segment (op=load overrides; op=resolve overrides just this lookup)."),
  },
  async ({ op, file, name, loadSegment }) => {
    try {
      if (op === "load") {
        if (file === undefined) return { content: [{ type: "text", text: "ERROR: 'file' required for op=load" }], isError: true };
        loadedSymbols = loadSymFile(file);
        loadedSymbolsPath = file;
        if (loadSegment !== undefined) {
          currentLoadSegment = loadSegment;
          currentLoadSegmentFresh = false;
        }
        rebuildSymbolIndex();
        return {
          content: [{
            type: "text",
            text: j({
              status: "ok",
              file,
              module: loadedSymbols.module,
              symbol_count: loadedSymbols.symbols.size,
              relocations: loadedSymbols.relocs,
              loadSegment: currentLoadSegment === undefined ? undefined : hex(currentLoadSegment),
            }),
          }],
        };
      }

      if (op === "list") {
        if (loadedSymbols === undefined) return { content: [{ type: "text", text: "ERROR: No symbols loaded." }], isError: true };
        const symbols = [...loadedSymbols.symbols.values()].map((sym) => ({
          name: sym.name,
          offset: hex(sym.offset),
          tags: sym.tags,
          attrs: sym.attrs,
        }));
        return {
          content: [{
            type: "text",
            text: j({
              file: loadedSymbolsPath,
              module: loadedSymbols.module,
              loadSegment: currentLoadSegment === undefined ? undefined : hex(currentLoadSegment),
              relocations: loadedSymbols.relocs,
              symbols,
            }),
          }],
        };
      }

      // op === "resolve"
      if (name === undefined) return { content: [{ type: "text", text: "ERROR: 'name' required for op=resolve" }], isError: true };
      const { resolved, warning } = await resolveLoadedSymbolWithWarning(name, loadSegment);
      return {
        content: [{
          type: "text",
          text: [
            `${resolved.requested} -> ${hex(resolved.linear)}`,
            ...(warning === undefined ? [] : [`warning: ${warning}`]),
            `space: ${resolved.space}`,
            `offset: ${hex(resolved.offset)}`,
            `why: ${resolved.explanation}`,
          ].join("\n"),
        }],
      };
    } catch (e) {
      return {
        content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }],
        isError: true,
      };
    }
  },
);

server.tool(
  "dosbox_map",
  "Manage Microsoft LINK .MAP files. " +
  "op='load': load a .MAP file (file required). " +
  "op='list': show segments, groups, publics, and entry point. " +
  "op='resolve': resolve a public, entry, SEGMENT, SEGMENT+offset, SEGMENT:offset, or SSSS:OOOO.",
  {
    op: z.enum(["load", "list", "resolve"]).describe("Operation: load | list | resolve"),
    file: z.string().optional().describe("Host path to .MAP file (op=load)."),
    name: z.string().optional().describe("MAP symbol/spec, e.g. 'D32WRAP+0x250', 'D32WRAP:0', '_main', or 'entry'."),
  },
  async ({ op, file, name }) => {
    try {
      if (op === "load") {
        if (file === undefined) return { content: [{ type: "text", text: "ERROR: 'file' required for op=load" }], isError: true };
        loadedMap = loadLinkMapFile(file);
        loadedMapPath = file;
        rebuildSymbolIndex();
        return {
          content: [{
            type: "text",
            text: j({
              status: "ok",
              file,
              segment_count: loadedMap.segments.length,
              group_count: loadedMap.groups.length,
              public_key_count: loadedMap.publics.size,
              entryPoint: loadedMap.entryPoint,
              loadInfo: currentLoadInfo,
            }),
          }],
        };
      }

      if (op === "list") {
        if (loadedMap === undefined) return { content: [{ type: "text", text: "ERROR: No MAP loaded." }], isError: true };
        const publics = [...new Map([...loadedMap.publics.values()].map((sym) => [sym.name, sym])).values()];
        return {
          content: [{
            type: "text",
            text: j({
              file: loadedMapPath,
              loadInfo: currentLoadInfo,
              entryPoint: loadedMap.entryPoint,
              segments: loadedMap.segments,
              groups: loadedMap.groups,
              publics,
            }),
          }],
        };
      }

      if (name === undefined) return { content: [{ type: "text", text: "ERROR: 'name' required for op=resolve" }], isError: true };
      if (loadedMap === undefined) return { content: [{ type: "text", text: "ERROR: No MAP loaded." }], isError: true };
      const resolved = resolveLinkMapSymbol(loadedMap, name, await effectiveMapLoadInfo());
      return {
        content: [{
          type: "text",
          text: [
            `${resolved.requested} -> ${hex(resolved.linear)}`,
            "source: map",
            `space: ${resolved.space}`,
            `mapOffset: ${hex(resolved.mapOffset)}`,
            `offset: ${hex(resolved.offset)}`,
            `why: ${resolved.explanation}`,
          ].join("\n"),
        }],
      };
    } catch (e) {
      return {
        content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }],
        isError: true,
      };
    }
  },
);

server.tool(
  "dosbox_debuginfo",
  "Debug info the emulator read out of the program the guest loaded. DOSBox-X parses it " +
  "through its own DOS filesystem as EXEC loads the program -- CodeView, Borland TDINFO " +
  "including a .TDS sidecar, Watcom, or the program's .MAP as a fallback -- so this tool " +
  "reports that work rather than doing any of it. " +
  "op='status': what the emulator found and how much of it. " +
  "op='modules': the object modules those symbols name. " +
  "op='lines': source file and line for a linear address (or for the current CS:IP). " +
  "op='load': have the emulator read a host file too, an EXE carrying debug info or a LINK .MAP.",
  {
    op: z.enum(["status", "modules", "lines", "load"]).describe("Operation."),
    file: z.string().optional().describe("Host path for op=load."),
    addr: z.number().optional().describe("Linear address for op=lines; defaults to the current location."),
    loadSeg: z.number().optional().describe("Load segment for op=load; defaults to the running program's."),
  },
  async ({ op, file, addr, loadSeg }) => {
    try {
      if (op === "load") {
        if (file === undefined) return { content: [{ type: "text", text: "ERROR: 'file' required for op=load" }], isError: true };
        const segment = loadSeg ?? currentLoadInfo?.loadSeg ?? currentLoadSegment ?? 0;
        const resp = await db.sendCommand({ cmd: "sym_load", file, loadSeg: segment, program: file });
        if (resp["status"] !== "ok") {
          return { content: [{ type: "text", text: `ERROR: ${String(resp["msg"] ?? j(resp))}` }], isError: true };
        }
        await syncSocketSymbols(true);
        return { content: [{ type: "text", text: j({ ...resp, loadSeg: segment, note: autoDebugInfoNote }) }] };
      }

      if (op === "lines") {
        const target = addr ?? await currentLinearAddress();
        if (target === undefined) return { content: [{ type: "text", text: "ERROR: no address; pass addr" }], isError: true };
        return { content: [{ type: "text", text: j({ address: hex(target), source: await autoDebugSourceLine(target) }) }] };
      }

      await syncSocketSymbols(true);

      if (op === "modules") {
        const counts = new Map<string, number>();
        for (const symbol of socketSymbols) {
          if (symbol.module === undefined) continue;
          counts.set(symbol.module, (counts.get(symbol.module) ?? 0) + 1);
        }
        const modules = [...counts.entries()]
          .sort((a, b) => a[0].localeCompare(b[0]))
          .map(([name, symbols]) => ({ name, symbols }));
        return { content: [{ type: "text", text: j({ modules: modules.length, unnamed: socketSymbols.filter((symbol) => symbol.module === undefined).length, list: modules }) }] };
      }

      const bySource = new Map<string, number>();
      for (const symbol of socketSymbols) bySource.set(symbol.source, (bySource.get(symbol.source) ?? 0) + 1);
      return {
        content: [{
          type: "text",
          text: j({
            status: socketSymbols.length === 0 ? "none" : "ok",
            program: currentLoadInfo?.program,
            symbols: socketSymbols.length,
            bySource: Object.fromEntries(bySource),
            mounts: guestMounts,
            note: autoDebugInfoNote,
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_symbols",
  "Unified symbol index for MAP, .sym, D32 debug symbols, ELF symtab, and 16-bit sidecars. " +
  "op='list' lists indexed symbols; op='resolve' resolves exact names; " +
  "op='where' reports nearest symbol for a linear address; op='load-d32' registers D32 debug symbols; " +
  "op='load-elf' registers ELF32 symtab symbols at a runtime base; " +
  "op='load-exe16-sidecar' loads generated JSON sidecar symbols.",
  {
    op: z.enum(["list", "resolve", "where", "load-d32", "load-elf", "load-exe16-sidecar"]).describe("Operation."),
    name: z.string().optional().describe("Symbol name for op=resolve."),
    addr: z.number().optional().describe("Linear address for op=where."),
    file: z.string().optional().describe("D32 module or EXE16 sidecar JSON file."),
    base: z.number().optional().describe("Module base linear address for op=load-d32 or load-elf."),
  },
  async ({ op, name, addr, file, base }) => {
    try {
      if (op === "load-d32") {
        if (file === undefined || base === undefined) {
          return { content: [{ type: "text", text: "ERROR: file and base are required" }], isError: true };
        }
        const module = parseD32File(file);
        d32Modules.set(file, { module, base });
        rebuildSymbolIndex();
        return {
          content: [{ type: "text", text: j({
            status: "ok",
            file,
            base: hex(base),
            debugSymbols: module.debugSymbols.length,
            exports: module.exports.length,
          }) }],
        };
      }

      if (op === "load-elf") {
        if (file === undefined || base === undefined) {
          return { content: [{ type: "text", text: "ERROR: file and base are required" }], isError: true };
        }
        const symbols = parseElf32Symbols(file, base);
        loadedElfFiles.set(file, { file, base });
        rebuildSymbolIndex();
        return {
          content: [{ type: "text", text: j({
            status: "ok",
            file,
            base: hex(base),
            symbols: symbols.length,
          }) }],
        };
      }

      if (op === "load-exe16-sidecar") {
        if (file === undefined) return { content: [{ type: "text", text: "ERROR: file is required" }], isError: true };
        const loadLinear = currentLoadInfo === undefined ? 0 : effectiveLoadLinear(currentLoadInfo);
        symbolIndex.addMany(loadSidecarSymbols(file, loadLinear, segmentStartsByName()));
        return { content: [{ type: "text", text: j({ status: "ok", file, loadLinear: hex(loadLinear) }) }] };
      }

      if (op === "resolve") {
        if (name === undefined) return { content: [{ type: "text", text: "ERROR: name is required" }], isError: true };
        let resolved = symbolIndex.resolve(name);
        if (resolved === undefined) {
          // The emulator's store spells some names this index cannot, such as
          // "cvprobe!pr_fill": module matching there strips any extension.
          const reply = await db.sendCommand({ cmd: "sym", name }, undefined, 5_000)
            .catch(() => ({ status: "error" }) as db.JsonObject);
          if (reply["status"] === "ok") resolved = symbolFromSocket(reply);
        }
        if (resolved === undefined) return { content: [{ type: "text", text: `ERROR: unknown symbol ${name}` }], isError: true };
        return { content: [{ type: "text", text: j(resolved) }] };
      }

      if (op === "where") {
        if (addr === undefined) return { content: [{ type: "text", text: "ERROR: addr is required" }], isError: true };
        const nearest = symbolIndex.nearest(addr);
        return { content: [{ type: "text", text: nearest === undefined ? "no symbol" : j({
          address: hex(addr),
          symbol: nearest.symbol,
          delta: hex(nearest.delta),
          display: symbolIndex.describe(addr),
        }) }] };
      }

      return { content: [{ type: "text", text: j(symbolIndex.all()) }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_d32x_inspect_module",
  "Parse a .d32 module, optionally register its runtime base, and report exports, relocs, debug symbols, and live symbol disassembly.",
  {
    moduleFile: z.string().describe("Host path to .d32 module."),
    base: z.number().optional().describe("Runtime module base linear address."),
    symbol: z.string().optional().describe("Symbol/export to disassemble when base is provided."),
    count: z.number().int().min(1).max(100).optional().describe("Instructions to disassemble (default 12)."),
  },
  async ({ moduleFile, base, symbol, count = 12 }) => {
    try {
      const module = parseD32File(moduleFile);
      if (base !== undefined) {
        d32Modules.set(moduleFile, { module, base });
        rebuildSymbolIndex();
      }
      const found = symbol === undefined ? undefined : symbolIndex.resolve(symbol);
      const disasm = found !== undefined
        ? await db.sendCommand({ cmd: "disasm", addr: found.linear, count }).catch((e) => ({ status: "error", msg: (e as Error).message }))
        : undefined;
      return {
        content: [{ type: "text", text: j({
          file: moduleFile,
          base: base === undefined ? undefined : hex(base),
          header: module.header,
          exports: module.exports,
          relocs: module.relocs,
          debugSymbols: module.debugSymbols,
          symbol: found,
          disasm,
        }) }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_d32x_verify_relocs",
  "Compare a .d32 module's relocation records with live memory at the supplied module base.",
  {
    moduleFile: z.string().describe("Host path to .d32 module."),
    base: z.number().describe("Runtime module base linear address."),
  },
  async ({ moduleFile, base }) => {
    try {
      const module = parseD32File(moduleFile);
      d32Modules.set(moduleFile, { module, base });
      rebuildSymbolIndex();
      const relocs = await verifyD32Relocs(module, base);
      return {
        content: [{ type: "text", text: j({
          file: moduleFile,
          base: hex(base),
          ok: relocs.every((reloc) => reloc["ok"] === true),
          relocs,
        }) }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_d32x_where",
  "Resolve the current flat protected-mode address, or an explicit address, against registered D32 debug symbols.",
  {
    moduleFile: z.string().optional().describe("Optional .d32 file to parse before lookup."),
    base: z.number().optional().describe("Runtime module base when moduleFile is supplied."),
    addr: z.number().optional().describe("Linear address to resolve; defaults to current CS:EIP linear address."),
  },
  async ({ moduleFile, base, addr }) => {
    try {
      if (moduleFile !== undefined && base !== undefined) {
        const module = parseD32File(moduleFile);
        d32Modules.set(moduleFile, { module, base });
        rebuildSymbolIndex();
      }
      let linear = addr;
      if (linear === undefined) {
        const resp = await db.sendCommand({ cmd: "get_linear_addr" });
        if (resp["status"] !== "ok") return { content: [{ type: "text", text: j(resp) }], isError: true };
        linear = Number.parseInt(String(resp["linear_addr"] ?? resp["linear"]).replace(/^0x/i, ""), 16) >>> 0;
      }
      return { content: [{ type: "text", text: j({
        address: hex(linear),
        location: symbolIndex.describe(linear),
        nearest: symbolIndex.nearest(linear),
      }) }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_where",
  "Show the current execution location using registers, loadInfo, and loaded MAP/public symbols. " +
  "If name is provided, resolve that .sym/.MAP symbol/spec to a linear address.",
  {
    name: z.string().optional().describe("Optional symbol or MAP spec to resolve."),
    refreshLoadInfo: z.boolean().optional().describe("Fetch {cmd:'get_load_info'} before reporting."),
  },
  async ({ name, refreshLoadInfo = false }) => {
    try {
      if (refreshLoadInfo) {
        const refreshed = await fetchLoadInfoFallback();
        if (refreshed !== undefined) {
          currentLoadInfo = refreshed;
          await syncSocketSymbols();
        }
      }

      if (name !== undefined) {
        const { resolved, warning } = await resolveLoadedSymbolWithWarning(name);
        return {
          content: [{
            type: "text",
            text: [
              `${resolved.requested} -> ${hex(resolved.linear)}`,
              ...(warning === undefined ? [] : [`warning: ${warning}`]),
              `source: ${resolved.source}`,
              `space: ${resolved.space}`,
              `offset: ${hex(resolved.offset)}`,
              `why: ${resolved.explanation}`,
            ].join("\n"),
          }],
        };
      }

      const [regsResp, linear] = await Promise.all([
        db.sendCommand({ cmd: "regs" }).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject),
        currentLinearAddress(),
      ]);
      const registers = regsResp["status"] === "ok"
        ? parseRegistersFromRecord(regsResp as Record<string, unknown>)
        : undefined;
      const mapLocation = linear === undefined ? undefined : describeMapAddress(loadedMap, currentLoadInfo, linear);
      const symbolLocation = linear === undefined ? undefined : symbolIndex.describe(linear);

      return {
        content: [{
          type: "text",
          text: j({
            linear: linear === undefined ? undefined : hex(linear),
            registers,
            symbolLocation,
            sourceLine: linear === undefined ? undefined : await autoDebugSourceLine(linear),
            mapLocation,
            loadInfo: currentLoadInfo,
            debugInfo: autoDebugInfoNote,
            symFile: loadedSymbolsPath,
            mapFile: loadedMapPath,
          }),
        }],
      };
    } catch (e) {
      return {
        content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }],
        isError: true,
      };
    }
  },
);

// ─── 4. Execution control ─────────────────────────────────────────────────── //

server.tool(
  "dosbox_break",
  "Break execution (pause the emulated CPU). Equivalent to pressing Alt+F12.",
  {},
  async () => {
    const resp = await db.sendCommand({ cmd: "break" });
    return { content: [{ type: "text", text: j(resp) }] };
  },
);

server.tool(
  "dosbox_context",
  "One-call debug context: last/current stop, unified symbol location, selectors, disassembly, stack, and screen tail.",
  {
    useLastStop: z.boolean().optional().describe("Prefer the socket's latched last stop when available (default true)."),
  },
  async ({ useLastStop = true }) => {
    try {
      return { content: [{ type: "text", text: await buildContextReport(useLastStop) }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_continue_report",
  "Continue for up to timeoutMs<=2000, then return stop context plus screen fault lines. " +
  "Use this as the default D32X debug step.",
  {
    timeoutMs: z.number().int().min(1).max(2000).optional().describe("Max wait in ms (default 2000)."),
  },
  async ({ timeoutMs = 2000 }) => {
    try {
      const stopResp = await db.continueAndWait(undefined, timeoutMs).catch(async (e) => {
        const last = await fetchLastStopFallback();
        if (last !== undefined) return last;
        return { status: "timeout", msg: (e as Error).message } as db.JsonObject;
      });
      await captureStopState(stopResp);
      const report = await buildContextReport(true);
      return { content: [{ type: "text", text: `continue → ${j(stopResp)}\n${report}` }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_continue",
  "Resume execution and wait for the next breakpoint or exception. " +
  "Keeps the socket connection open so the stopped event is captured. " +
  "Returns the stopped event (with full registers) when the CPU halts.",
  {
    timeoutMs: z.number().int().optional().describe("Max wait in ms (default 30 000)."),
  },
  async ({ timeoutMs = 30_000 }) => {
    const resp = await db.continueAndWait(undefined, timeoutMs);
    await captureStopState(resp);
    const inspect = await inspectAtCurrentPC(10);
    return { content: [{ type: "text", text: inspect }] };
  },
);

server.tool(
  "dosbox_wait_for_stop",
  "Open a connection and wait for the next stopped event (breakpoint, exception, step). " +
  "Use after dosbox_raw {cmd:'continue'} when no open connection is waiting. " +
  "Prefer dosbox_continue which does continue+wait atomically.",
  {
    timeoutMs: z.number().int().optional().describe("Max wait in ms (default 30 000)."),
    context: z.boolean().optional().describe("Append buildContextReport output (default true)."),
  },
  async ({ timeoutMs = 30_000, context = true }) => {
    const resp = await db.waitForStop(undefined, timeoutMs);
    await captureStopState(resp);
    const parts = [j(resp)];
    if (context) parts.push(await buildContextReport(true));
    return { content: [{ type: "text", text: parts.join("\n") }] };
  },
);

server.tool(
  "dosbox_step",
  "Step N instructions (into or over calls/interrupts). " +
  "Always returns the register dump and next disassembled instructions after the last step. " +
  "count=1 (default) steps one instruction. over=true steps over calls/loops. " +
  "Uses the freeze-based stop model: transparent and crash-free across PM transitions.",
  {
    count: z.number().int().min(1).max(1000).optional().describe("Instructions to step (default 1)."),
    over: z.boolean().optional().describe("Step over calls/interrupts/loops (default false = step into)."),
    context: z.boolean().optional().describe("Append buildContextReport output (default true)."),
  },
  async ({ count = 1, over = false, context = true }) => {
    const cmd = over ? "step_over" : "step";

    for (let i = 0; i < count; i++) {
      const stepResp = await db.sendCommand({ cmd });
      if (isErr(stepResp)) {
        return { content: [{ type: "text", text: j(stepResp) }], isError: true };
      }
    }

    const prefix = count > 1 ? `stepped ${count}×\n` : "";
    const inspect = await inspectAtCurrentPC(10);
    const parts = [prefix + inspect];
    if (context) parts.push(await buildContextReport(false));
    return { content: [{ type: "text", text: parts.join("\n") }] };
  },
);

server.tool(
  "dosbox_run_to",
  "Set a breakpoint and run until it fires. Provide a linear address (addr) OR a loaded " +
  "symbol name (name). Symbols resolve via the current load segment / relocation context. " +
  "Uses exec_linear (freeze-safe) breakpoints — not legacy physical breakpoints.",
  {
    addr: z.number().optional().describe("Linear address to stop at."),
    name: z.string().optional().describe("Loaded symbol name (e.g. 'call_hi' or 'hi:vbe_mode')."),
    once: z.boolean().optional().describe("One-shot breakpoint (default true)."),
    loadSegment: z.number().int().optional().describe("Override COM load segment for symbol resolution."),
    timeoutMs: z.number().int().optional().describe("Max wait in ms (default 30 000)."),
  },
  async ({ addr, name, once = true, loadSegment, timeoutMs = 30_000 }) => {
    try {
      let linear: number;
      let why: string;
      let matchOff: number | undefined;

      if (name !== undefined) {
        const { resolved, warning } = await resolveLoadedSymbolWithWarning(name, loadSegment);
        linear = resolved.linear;
        matchOff = breakpointMatchOff(resolved);
        why = `${name} -> ${hex(linear)}  (${resolved.explanation})` +
          (warning === undefined ? "" : `\nwarning: ${warning}`);
      } else if (addr !== undefined) {
        linear = addr >>> 0;
        why = `addr ${hex(linear)}`;
      } else {
        return { content: [{ type: "text", text: "ERROR: provide addr or name" }], isError: true };
      }

      const bpResp = await db.sendCommand(clean({ cmd: "bp_set_linear_exec", linear, match_off: matchOff, once: once ? 1 : 0 }));
      if (isErr(bpResp)) {
        return { content: [{ type: "text", text: `bp_set failed: ${j(bpResp)}` }], isError: true };
      }

      const stopResp = await db.continueAndWait(undefined, timeoutMs);
      await captureStopState(stopResp);
      const inspect = await inspectAtCurrentPC(10);
      return {
        content: [{
          type: "text",
          text: [`run_to ${why}`, inspect].join("\n"),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

// ─── 5. Registers ─────────────────────────────────────────────────────────── //

server.tool(
  "dosbox_regs",
  "Read all CPU registers: EAX/EBX/ECX/EDX/ESI/EDI/EBP/ESP/EIP, " +
  "CS/DS/ES/SS/FS/GS, FLAGS.",
  {},
  async () => {
    const resp = await db.sendCommand({ cmd: "regs" });
    return { content: [{ type: "text", text: j(resp) }] };
  },
);

server.tool(
  "dosbox_reg_set",
  "Set a CPU register to a value. Supports EAX/EBX/ECX/EDX/ESI/EDI/EBP/ESP/EIP, " +
  "16-bit (AX/BX/CX/DX), and 8-bit (AL/AH/BL/BH/CL/CH/DL/DH).",
  {
    reg: z.string().describe("Register name (e.g. EAX, ESP, AH)."),
    val: z.number().describe("New value."),
  },
  async ({ reg, val }) => {
    const resp = await db.sendCommand({ cmd: "regs_set", reg, val });
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_inspect",
  "Combined snapshot: registers + disassembly at current CS:EIP. " +
  "The most common single action after stopping — shows where you are and what's next.",
  {
    count: z.number().int().min(1).max(50).optional().describe("Instructions to disassemble (default 12)."),
  },
  async ({ count = 12 }) => {
    const inspect = await inspectAtCurrentPC(count);
    return { content: [{ type: "text", text: inspect }] };
  },
);

server.tool(
  "dosbox_var",
  "Read a program variable by name. A name that is not a global is looked up as a " +
  "local or parameter of whatever is running at CS:EIP, read from the frame or the " +
  "register it lives in. Returns its bytes always, and a decoded value when the " +
  "program's debug info gave it a type. A BASIC array is followed through its " +
  "runtime descriptor to the elements, each decoded field by field.",
  {
    name: z.string().describe("Variable name, e.g. 'g_counter' or 'module!g_counter'."),
    len: z.number().int().min(1).max(4096).optional().describe(
      "Bytes to read. Defaults to the size the debug info gives, else 2.",
    ),
  },
  async ({ name, len }) => {
    const resp = await db.sendCommand(clean({ cmd: "var", name, len }));
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_locals",
  "Every local and parameter in scope where the CPU is stopped, innermost first, " +
  "with values. A frame variable is only where BP says it is once the function's " +
  "prologue has run, so the frame each was read through comes back with it.",
  {
    linear: z.number().int().optional().describe("Ask about another address instead of CS:EIP."),
  },
  async ({ linear }) => {
    const resp = await db.sendCommand(clean({ cmd: "locals", linear }));
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

// ─── 6. Breakpoints ───────────────────────────────────────────────────────── //

server.tool(
  "dosbox_bp_set",
  "Set a breakpoint or write watchpoint. " +
  "Execution kinds: 'exec_linear' (default, freeze-safe): addr or symbol name; " +
  "'exec': physical segment:offset breakpoint (addr or seg+off); " +
  "'int': interrupt breakpoint (intNum, optional ah/al filter). " +
  "Watchpoint kinds: 'write' or 'rw': arm a write watchpoint at linear addr or symbol name. " +
  "Requires core=normal. Provide length (bytes to watch, default 4). " +
  "Symbol names resolve via current load segment / relocation context. " +
  "deferred:true registers a reload-surviving symbolic spec (WinDbg bu semantics).",
  {
    addr: z.number().optional().describe("Linear address (exec_linear/write/rw) or physical address (exec)."),
    seg: z.number().optional().describe("Segment value (exec kind, with off)."),
    off: z.number().optional().describe("Offset value (exec kind, with seg)."),
    name: z.string().optional().describe("Loaded symbol name (resolves to exec_linear or write watchpoint)."),
    location: z.string().optional().describe(
      "gdb-style location resolved by the emulator: 'cvprobe.bas:17', 'pr_add', 'pr_add+0x10', '*0x82F4'. " +
      "A line with no code of its own moves to the next line that has some. exec_linear only.",
    ),
    once: z.boolean().optional().describe("One-shot breakpoint (default false)."),
    deferred: z.boolean().optional().describe("Register symbolic deferred breakpoint spec instead of failing when unresolved."),
    offsetDelta: z.number().int().optional().describe("Add to resolved symbol linear address (deferred or immediate)."),
    kind: z.enum(["exec_linear", "exec", "int", "write", "rw"]).optional().describe(
      "Breakpoint kind. 'write'/'rw' = write watchpoint via wp_set (requires core=normal).",
    ),
    intNum: z.number().int().min(0).max(255).optional().describe("Interrupt vector (kind=int)."),
    ah: z.number().int().optional().describe("AH filter (kind=int)."),
    al: z.number().int().optional().describe("AL filter (kind=int)."),
    loadSegment: z.number().int().optional().describe("Override COM load segment for symbol resolution."),
    length: z.number().int().min(1).max(65536).optional().describe("Watchpoint byte length (kind=write/rw, default 4)."),
  },
  async ({ addr, seg, off, name, location, once, deferred, offsetDelta, kind, intNum, ah, al, loadSegment, length }) => {
    try {
      const effectiveKind = kind ?? (
        intNum !== undefined ? "int" :
        (seg !== undefined || off !== undefined) ? "exec" :
        "exec_linear"
      );
      const onceVal = once === true ? 1 : 0;

      if (effectiveKind === "write" || effectiveKind === "rw") {
        let linear: number;
        let why: string;
        if (name !== undefined) {
          const { resolved, warning } = await resolveLoadedSymbolWithWarning(name, loadSegment);
          linear = (resolved.linear + (offsetDelta ?? 0)) >>> 0;
          why = `${name} -> ${hex(linear)}  (${resolved.explanation})` +
            (warning === undefined ? "" : `\nwarning: ${warning}`);
        } else if (addr !== undefined) {
          linear = ((addr >>> 0) + (offsetDelta ?? 0)) >>> 0;
          why = `addr ${hex(linear)}`;
        } else {
          return { content: [{ type: "text", text: "ERROR: provide addr or name for watchpoint" }], isError: true };
        }
        const len = length ?? 4;
        const resp = await db.sendCommand({ cmd: "wp_set", linear, len });
        return {
          content: [{ type: "text", text: `${why}\n${j(resp)}` }],
          isError: isErr(resp),
        };
      }

      if (effectiveKind === "int") {
        if (intNum === undefined) return { content: [{ type: "text", text: "ERROR: intNum required for kind=int" }], isError: true };
        const resp = await db.sendCommand(clean({ cmd: "bp_int", int: intNum, ah, al }));
        if (!isErr(resp)) explicitInterruptBreakpoints.add(intNum);
        return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
      }

      if (effectiveKind === "exec_linear") {
        if (location !== undefined) {
          // The emulator owns the line table, so it resolves the spec.
          const resp = await db.sendCommand(clean({ cmd: "bp_set_linear_exec", location, once: onceVal }));
          return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
        }

        if (deferred === true) {
          if (name === undefined) {
            return { content: [{ type: "text", text: "ERROR: deferred breakpoints require name" }], isError: true };
          }
          const spec = await registerDeferredBreakpointSpec({
            name,
            offsetDelta,
            once,
            loadSegment,
          });
          return {
            content: [{
              type: "text",
              text: j({
                status: "ok",
                deferred: true,
                spec,
              }),
            }],
          };
        }

        let linear: number;
        let why: string;
        let matchOff: number | undefined;
        if (name !== undefined) {
          const { resolved, warning } = await resolveLoadedSymbolWithWarning(name, loadSegment);
          linear = (resolved.linear + (offsetDelta ?? 0)) >>> 0;
          matchOff = breakpointMatchOff(resolved);
          why = `${name} -> ${hex(linear)}  (${resolved.explanation})` +
            (warning === undefined ? "" : `\nwarning: ${warning}`);
        } else if (addr !== undefined) {
          linear = ((addr >>> 0) + (offsetDelta ?? 0)) >>> 0;
          why = `addr ${hex(linear)}`;
        } else {
          return { content: [{ type: "text", text: "ERROR: provide addr or name for exec_linear bp" }], isError: true };
        }
        const resp = await db.sendCommand(clean({ cmd: "bp_set_linear_exec", linear, match_off: matchOff, once: onceVal }));
        return {
          content: [{ type: "text", text: `${why}\n${j(resp)}` }],
          isError: isErr(resp),
        };
      }

      const resp = await db.sendCommand(clean({ cmd: "bp_set", addr, seg, off }));
      return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_bp_clear",
  "Clear a breakpoint, watchpoint, or all breakpoints/watchpoints. " +
  "Provide addr (linear exec bp) OR seg+off (exec bp) OR intNum (interrupt bp) OR " +
  "watchpointSlot (watchpoint slot number from wp_list) OR all=true.",
  {
    addr: z.number().optional().describe("Linear address of an exec_linear bp to clear."),
    seg: z.number().optional().describe("Segment of an exec bp to clear."),
    off: z.number().optional().describe("Offset of an exec bp to clear."),
    intNum: z.number().int().min(0).max(255).optional().describe("Interrupt vector of an int bp to clear."),
    watchpointSlot: z.number().int().min(0).optional().describe("Watchpoint slot number (from wp_list) to clear."),
    allWatchpoints: z.boolean().optional().describe("Clear ALL watchpoints (wp_clear with no slot)."),
    all: z.boolean().optional().describe("Clear ALL breakpoints (exec, exec_linear, int). Does not clear watchpoints."),
  },
  async ({ addr, seg, off, intNum, watchpointSlot, allWatchpoints, all }) => {
    if (watchpointSlot !== undefined) {
      const resp = await db.sendCommand({ cmd: "wp_clear", slot: watchpointSlot });
      return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
    }
    if (allWatchpoints === true) {
      const resp = await db.sendCommand({ cmd: "wp_clear" });
      return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
    }
    if (all === true) {
      const resp = await db.sendCommand({ cmd: "bp_clear_all" });
      if (!isErr(resp)) explicitInterruptBreakpoints.clear();
      return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
    }
    if (addr !== undefined) {
      const resp = await db.sendCommand({ cmd: "bp_clear_linear_exec", linear: addr });
      return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
    }
    if (intNum !== undefined) {
      // Fall back to raw debugger command for int clear
      const resp = await db.sendCommand(clean({ cmd: "bp_int_clear", int: intNum }));
      if (!isErr(resp)) {
        explicitInterruptBreakpoints.delete(intNum);
        return { content: [{ type: "text", text: j(resp) }] };
      }
      return {
        content: [{
          type: "text",
          text: `Note: bp_int_clear not available via socket for int 0x${intNum.toString(16)}. ` +
                `Use dosbox_bp_list to find index, then dosbox_raw {cmd:'bp_disable', index:N}.`,
        }],
      };
    }
    // exec bp by seg+off or addr
    const resp = await db.sendCommand(clean({ cmd: "bp_clear", addr, seg, off }));
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_bp_list",
  "List socket breakpoints, watchpoints, and deferred symbolic breakpoint specs. " +
  "Includes exec/exec_linear/int breakpoints from bp_list, write watchpoints from wp_list, " +
  "and MCP-side deferred symbolic specs.",
  {},
  async () => {
    const [bpResp, wpResp] = await Promise.all([
      db.sendCommand({ cmd: "bp_list" }).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject),
      db.sendCommand({ cmd: "wp_list" }).catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject),
    ]);
    const deferred = [...deferredBreakpointSpecs.values()].map((spec) => ({
      id: spec.id,
      name: spec.name,
      offsetDelta: spec.offsetDelta,
      once: spec.once,
      status: spec.status,
      armedLinear: spec.armedLinear === undefined ? undefined : hex(spec.armedLinear),
      stale: spec.status === "pending" && spec.warning !== undefined,
      explanation: spec.explanation,
      warning: spec.warning,
    }));
    // Annotate watchpoints with symbolIndex descriptions
    const rawWatchpoints = Array.isArray(wpResp["watchpoints"]) ? wpResp["watchpoints"] as db.JsonObject[] : [];
    const watchpoints = rawWatchpoints.map((wp) => {
      const linear = typeof wp["linear"] === "string"
        ? Number.parseInt((wp["linear"] as string).replace(/^0x/i, ""), 16) >>> 0
        : typeof wp["linear"] === "number" ? wp["linear"] as number >>> 0 : undefined;
      return {
        ...wp,
        symbol: linear !== undefined ? (symbolIndex.describe(linear) ?? undefined) : undefined,
      };
    });
    return {
      content: [{
        type: "text",
        text: j({
          breakpoints: bpResp,
          watchpoints: { ...wpResp, watchpoints },
          deferred,
        }),
      }],
    };
  },
);

// ─── 7. Memory ────────────────────────────────────────────────────────────── //

server.tool(
  "dosbox_mem_read",
  "Read bytes from memory. By default uses linear addressing (walks page tables). " +
  "Provide addr (linear) OR seg+off. When seg+off is given the descriptor base is " +
  "resolved automatically (works correctly in both real-mode and protected-mode). " +
  "Returns 'PF' tokens for page-faulted bytes.",
  {
    addr: z.number().optional().describe("Linear address."),
    seg: z.number().optional().describe("Segment selector value (with off)."),
    off: z.number().optional().describe("Offset within segment (with seg)."),
    len: z.number().int().min(1).max(4096).describe("Bytes to read (max 4096)."),
    linear: z.boolean().optional().describe("Use linear addressing / page-walk (default true). Ignored when seg+off is provided."),
  },
  async ({ addr, seg, off, len, linear = true }) => {
    let linearAddr: number;

    if (seg !== undefined && off !== undefined) {
      // Resolve descriptor base so the address is correct in PM.
      linearAddr = (await selectorBase(seg)) + off;
    } else if (addr !== undefined) {
      linearAddr = addr;
    } else {
      return { content: [{ type: "text", text: "ERROR: provide addr or seg+off" }], isError: true };
    }

    const resp = await db.sendCommand(
      linear
        ? { cmd: "mem_read_linear", addr: linearAddr, len }
        : { cmd: "mem_read", addr: linearAddr, len },
    );
    if (resp["status"] === "ok" && typeof resp["data"] === "string") {
      return { content: [{ type: "text", text: formatHexDump(resp["data"] as string, linearAddr) }] };
    }
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_mem_write",
  "Write bytes to memory. data is a hex string e.g. 'DEADBEEF'. Provide addr OR seg+off.",
  {
    addr: z.number().optional().describe("Linear address."),
    seg: z.number().optional().describe("Segment value."),
    off: z.number().optional().describe("Offset value."),
    data: z.string().regex(/^[0-9A-Fa-f]+$/).describe("Hex string of bytes to write."),
  },
  async (args) => {
    const resp = await db.sendCommand(clean({ cmd: "mem_write", ...args }));
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_mem_search",
  "Search a memory range for a byte pattern. " +
  "Pattern mode: provide 'pattern' (hex bytes) with optional 'mask' (same length, 00=wildcard) and 'align'. " +
  "Value mode: provide 'value' + 'width' (1/2/4) to encode as little-endian pattern automatically. " +
  "Returns all match addresses.",
  {
    addr: z.number().describe("Start address (linear)."),
    len: z.number().int().describe("Bytes to scan."),
    pattern: z.string().optional().describe("Hex byte pattern, e.g. 'CD31' for INT 31h."),
    mask: z.string().optional().describe("Hex mask same length as pattern; 00=wildcard byte, FF=exact match."),
    align: z.number().int().min(1).optional().describe("Alignment constraint (default 1)."),
    maxResults: z.number().int().optional().describe("Max matches to return (default 100)."),
    value: z.number().optional().describe("Integer value for little-endian convenience search (use with width)."),
    width: z.number().int().min(1).max(4).optional().describe("Byte width for value mode: 1, 2, or 4."),
  },
  async ({ addr, len, pattern, mask, align, maxResults, value, width }) => {
    let effectivePattern = pattern;
    if (effectivePattern === undefined) {
      if (value === undefined || width === undefined) {
        return { content: [{ type: "text", text: "ERROR: provide pattern or value+width" }], isError: true };
      }
      // Encode value as little-endian hex
      const bytes: string[] = [];
      let v = value >>> 0;
      for (let i = 0; i < width; i++) {
        bytes.push((v & 0xff).toString(16).padStart(2, "0"));
        v >>>= 8;
      }
      effectivePattern = bytes.join("");
    }
    const resp = await db.sendCommand(
      clean({ cmd: "mem_search", addr, len, pattern: effectivePattern, mask, align, max_results: maxResults }),
    );
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_mem_snapshot",
  "Save, diff, or diff-file compare linear memory snapshots stored in the MCP process. " +
  "Snapshots are cleared on dosbox_launch/dosbox_stop.",
  {
    op: z.enum(["save", "diff", "diff-file"]).describe("save=read memory; diff=compare to saved id; diff-file=compare to host file slice."),
    id: z.string().optional().describe("Snapshot id for save/diff."),
    addr: z.number().optional().describe("Linear start address for save/diff."),
    len: z.number().int().min(1).optional().describe("Byte length for save/diff."),
    file: z.string().optional().describe("Host file for diff-file."),
    fileOffset: z.number().int().min(0).optional().describe("Offset within host file for diff-file."),
  },
  async ({ op, id, addr, len, file, fileOffset = 0 }) => {
    try {
      if (op === "save") {
        if (id === undefined || addr === undefined || len === undefined) {
          return { content: [{ type: "text", text: "ERROR: id, addr, and len are required for save" }], isError: true };
        }
        const bytes = await readLinearBytesChunked(addr, len);
        memSnapshots.set(id, { addr, len, bytes });
        return { content: [{ type: "text", text: j({ status: "ok", id, addr: hex(addr), len, bytes: bytes.length }) }] };
      }

      if (op === "diff") {
        if (id === undefined || addr === undefined || len === undefined) {
          return { content: [{ type: "text", text: "ERROR: id, addr, and len are required for diff" }], isError: true };
        }
        const saved = memSnapshots.get(id);
        if (saved === undefined) {
          return { content: [{ type: "text", text: `ERROR: unknown snapshot id ${id}` }], isError: true };
        }
        const live = await readLinearBytesChunked(addr, len);
        const compareLen = Math.min(saved.bytes.length, live.length, len);
        const diff = coalesceMemoryDiff(
          saved.bytes.subarray(0, compareLen),
          live.subarray(0, compareLen),
          addr,
        );
        return {
          content: [{
            type: "text",
            text: j({
              status: "ok",
              id,
              addr: hex(addr),
              len: compareLen,
              ...diff,
              note: diff.truncated
                ? `Output truncated to ${diff.ranges.length} ranges; ${diff.dirtyRangeCount} total dirty ranges, ${diff.totalDirtyBytes} dirty bytes.`
                : undefined,
            }),
          }],
        };
      }

      if (file === undefined || addr === undefined || len === undefined) {
        return { content: [{ type: "text", text: "ERROR: file, addr, and len are required for diff-file" }], isError: true };
      }
      const fileBytes = readFileSync(file);
      if (fileOffset + len > fileBytes.length) {
        return { content: [{ type: "text", text: "ERROR: file slice exceeds file size" }], isError: true };
      }
      const expected = new Uint8Array(fileBytes.subarray(fileOffset, fileOffset + len));
      const live = await readLinearBytesChunked(addr, len);
      const diff = coalesceMemoryDiff(expected, live.subarray(0, len), addr);
      return {
        content: [{
          type: "text",
          text: j({
            status: "ok",
            file,
            fileOffset,
            addr: hex(addr),
            len,
            ...diff,
            note: diff.truncated
              ? `Output truncated to ${diff.ranges.length} ranges; ${diff.dirtyRangeCount} total dirty ranges, ${diff.totalDirtyBytes} dirty bytes.`
              : undefined,
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_modules",
  "Format-pluggable module discovery and registration. scan uses mem_search + header probes; " +
  "register manually binds a host module file to a runtime base (same as dosbox_symbols load-d32).",
  {
    op: z.enum(["scan", "list", "register"]).describe("Operation."),
    rangeStart: z.number().optional().describe("Linear scan start for op=scan (default 0x100000)."),
    rangeEnd: z.number().optional().describe("Linear scan end for op=scan (default 0x2000000)."),
    searchPaths: z.array(z.string()).optional().describe(".d32 directories/files to match discovered headers against."),
    file: z.string().optional().describe("Host module file for op=register."),
    base: z.number().optional().describe("Runtime module base for op=register."),
  },
  async ({ op, rangeStart = 0x100000, rangeEnd = 0x2000000, searchPaths, file, base }) => {
    try {
      if (searchPaths !== undefined) moduleSearchPaths = searchPaths;

      if (op === "scan") {
        const discovered = await scanDiscoveredModules(rangeStart, rangeEnd, moduleSearchPaths);
        for (const entry of discovered) {
          const moduleFile = entry["file"];
          const moduleBase = entry["base"];
          if (typeof moduleFile === "string" && typeof moduleBase === "string") {
            await registerD32ModuleAtBase(moduleFile, Number.parseInt(moduleBase.replace(/^0x/i, ""), 16) >>> 0);
            entry["registered"] = true;
          }
        }
        return { content: [{ type: "text", text: j({ status: "ok", discovered, searchPaths: moduleSearchPaths }) }] };
      }

      if (op === "list") {
        return {
          content: [{
            type: "text",
            text: j({
              discovered: [...discoveredModules.entries()].map(([key, value]) => ({
                key,
                ...value.probe,
                base: hex(value.probe.base),
                size: hex(value.probe.size),
                file: value.file,
                registered: value.registered,
              })),
              registered: [...d32Modules.entries()].map(([path, entry]) => ({
                file: path,
                name: entry.module.name,
                base: hex(entry.base),
              })),
              searchPaths: moduleSearchPaths,
            }),
          }],
        };
      }

      if (file === undefined || base === undefined) {
        return { content: [{ type: "text", text: "ERROR: file and base are required for register" }], isError: true };
      }
      const module = await registerD32ModuleAtBase(file, base);
      return {
        content: [{
          type: "text",
          text: j({
            status: "ok",
            file,
            base: hex(base),
            name: module.name,
            debugSymbols: module.debugSymbols.length,
            exports: module.exports.length,
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

server.tool(
  "dosbox_addr",
  "Convert between seg:off, selector:off, linear, and module section offsets using selinfo/get_linear_addr and the symbol index.",
  {
    seg: z.number().optional().describe("Real-mode segment or selector value."),
    off: z.number().optional().describe("Offset within segment/selector."),
    selector: z.number().optional().describe("Protected-mode selector (alias for seg)."),
    linear: z.number().optional().describe("Flat linear address."),
    module: z.string().optional().describe("Registered module name for section+sectionOffset lookup."),
    section: z.enum(["code", "data", "bss"]).optional().describe("Module section name."),
    sectionOffset: z.number().optional().describe("Offset within module section."),
  },
  async ({ seg, off, selector, linear, module, section, sectionOffset }) => {
    try {
      let resolvedLinear: number | undefined = linear === undefined ? undefined : linear >>> 0;
      const effectiveSeg = selector ?? seg;
      const effectiveOff = off;

      if (module !== undefined && section !== undefined && sectionOffset !== undefined) {
        const moduleLinear = resolveModuleSectionOffset(module, section, sectionOffset);
        if (moduleLinear === undefined) {
          return { content: [{ type: "text", text: `ERROR: unknown module ${module}` }], isError: true };
        }
        resolvedLinear = moduleLinear;
      } else if (resolvedLinear === undefined && effectiveSeg !== undefined && effectiveOff !== undefined) {
        const segBaseAddr = await selectorBase(effectiveSeg);
        resolvedLinear = (segBaseAddr + effectiveOff) >>> 0;
      }

      if (resolvedLinear === undefined) {
        return { content: [{ type: "text", text: "ERROR: provide linear, seg+off/selector+off, or module+section+sectionOffset" }], isError: true };
      }

      const linResp = await db.sendCommand({ cmd: "get_linear_addr" }).catch(() => undefined);
      const currentLinear = linResp?.["status"] === "ok"
        ? Number.parseInt(String(linResp["linear_addr"] ?? linResp["linear"]).replace(/^0x/i, ""), 16) >>> 0
        : undefined;

      let segOff: { seg: number; off: number } | undefined;
      if (effectiveSeg !== undefined && effectiveOff !== undefined) {
        segOff = { seg: effectiveSeg, off: effectiveOff };
      } else if (currentLoadSegment !== undefined) {
        segOff = {
          seg: currentLoadSegment,
          off: (resolvedLinear - ((currentLoadSegment << 4) >>> 0)) >>> 0,
        };
      }

      let selectorInfo: db.JsonObject | undefined;
      if (effectiveSeg !== undefined) {
        selectorInfo = await db.sendCommand({ cmd: "selinfo", sel: effectiveSeg }).catch(() => undefined);
      }

      const moduleHits: Array<Record<string, unknown>> = [];
      for (const { module: d32Module, base: moduleBase } of d32Modules.values()) {
        for (const sectionName of ["code", "data", "bss"] as const) {
          const sectionId = sectionName === "code" ? 0 : sectionName === "data" ? 1 : 2;
          const sectionBase = sectionRuntimeAddress(d32Module, moduleBase, sectionId, 0);
          const sectionSize = sectionName === "code"
            ? d32Module.header.codeSize
            : sectionName === "data"
              ? d32Module.header.dataSize
              : d32Module.header.bssSize;
          if (resolvedLinear >= sectionBase && resolvedLinear < sectionBase + sectionSize) {
            moduleHits.push({
              module: d32Module.name,
              section: sectionName,
              sectionOffset: hex((resolvedLinear - sectionBase) >>> 0),
              base: hex(moduleBase),
            });
          }
        }
      }

      return {
        content: [{
          type: "text",
          text: j({
            linear: hex(resolvedLinear),
            segOff: segOff === undefined ? undefined : {
              seg: hex(segOff.seg),
              off: hex(segOff.off),
            },
            selector: effectiveSeg === undefined ? undefined : hex(effectiveSeg),
            selectorInfo,
            currentLinear: currentLinear === undefined ? undefined : hex(currentLinear),
            symbol: symbolIndex.describe(resolvedLinear),
            nearest: symbolIndex.nearest(resolvedLinear),
            mapLocation: describeMapAddress(loadedMap, currentLoadInfo, resolvedLinear),
            moduleHits,
          }),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

// ─── 8. Disassembly ───────────────────────────────────────────────────────── //

server.tool(
  "dosbox_disasm",
  "Disassemble instructions at an address. Provide addr (linear) OR seg + off. " +
  "Decodes relative to current CPU mode (16/32-bit based on CS descriptor D/B bit).",
  {
    addr: z.number().optional().describe("Linear address."),
    seg: z.number().optional().describe("Segment value."),
    off: z.number().optional().describe("Offset value."),
    count: z.number().int().min(1).max(100).optional().describe("Instructions to disassemble (default 10)."),
  },
  async (args) => {
    const resp = await db.sendCommand(clean({ cmd: "disasm", ...args }));
    if (resp["status"] === "ok") {
      const lines = resp["lines"] as DLine[] | undefined;
      if (Array.isArray(lines)) {
        const registers = lastStopRegisters ?? await readRegisters().catch(() => undefined);
        const base = args.seg === undefined ? undefined : await selectorBase(args.seg);
        const formattedLines = await formatDisasmLines(lines, undefined, registers, base);
        const text = formattedLines.join("\n");
        return { content: [{ type: "text", text }] };
      }
    }
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

// ─── 9. Screen / display ──────────────────────────────────────────────────── //

server.tool(
  "dosbox_text_screen",
  "Dump the current DOS text-mode screen (reads VGA text RAM at B800:0000). " +
  "Returns the visible characters as plain text lines.",
  {},
  async () => {
    const resp = await db.sendCommand({ cmd: "text_screen" });
    if (resp["status"] === "ok" && typeof resp["text"] === "string") {
      const meta = `[${resp["cols"]}×${resp["rows"]} mode ${resp["video_mode"]}]`;
      return { content: [{ type: "text", text: `${meta}\n${resp["text"]}` }] };
    }
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

server.tool(
  "dosbox_screenshot",
  "Trigger DOSBox to save a PNG screenshot. Optionally provide an absolute host path.",
  {
    path: z.string().optional().describe(
      "Optional absolute host path where the PNG should be saved. Parent directories are created if needed.",
    ),
  },
  async ({ path }) => {
    if (path !== undefined) {
      mkdirSync(dirname(path), { recursive: true });
    }
    const resp = await db.sendCommand({ cmd: "screenshot", ...(path !== undefined ? { path } : {}) });
    return { content: [{ type: "text", text: j(resp) }] };
  },
);

// ─── 10. Protected-mode info ─────────────────────────────────────────────── //

server.tool(
  "dosbox_pm_info",
  "Protected-mode snapshot: CR0/CR2/CR3, IDTR, linear address of CS:EIP. " +
  "Optional: decode a selector (sel) and/or show an IDT gate (intNum). " +
  "Essential for PM/DPMI debugging — check flat selectors, paging, page-fault address, etc.",
  {
    sel: z.union([z.number(), z.string()]).optional().describe(
      "Selector to decode: numeric value or register name (cs/ds/es/ss/fs/gs).",
    ),
    intNum: z.number().int().min(0).max(255).optional().describe("IDT gate to show (interrupt vector)."),
  },
  async ({ sel, intNum }) => {
    const parts: string[] = [];

    try {
      // Fetch CRs, linear addr, IDTR, and current register values in parallel.
      const [cr, lin, idtr, regs] = await Promise.all([
        db.sendCommand({ cmd: "cr_read" }),
        db.sendCommand({ cmd: "get_linear_addr" }),
        db.sendCommand({ cmd: "get_idtr" }),
        db.sendCommand({ cmd: "regs" }),
      ]);

      // CR registers + paging
      if (cr["status"] === "ok") {
        parts.push(
          "── CRs ─────────────────────────────────────────",
          `CR0  ${cr["CR0"]}   PE=${cr["PE"] ? 1 : 0}  PG=${cr["PG"] ? 1 : 0}`,
          `CR2  ${cr["CR2"]}   (last page-fault linear addr)`,
          `CR3  ${cr["CR3"]}   (page directory base)`,
          `CR4  ${cr["CR4"]}`,
          `paging: ${cr["paging_enabled"]}`,
        );
      }

      // Current linear address of CS:EIP
      if (lin["status"] === "ok") {
        parts.push(`CS:EIP linear → ${lin["linear_addr"] ?? lin["linear"]}`);
      }

      // IDTR
      if (idtr["status"] === "ok") {
        parts.push(
          "── IDTR ────────────────────────────────────────",
          `base=${idtr["base"]}  limit=${idtr["limit"]}`,
        );
      }

      // Active segment descriptors — always shown, fetched in parallel.
      if (regs["status"] === "ok") {
        const segNames = ["CS", "DS", "ES", "SS", "FS", "GS"] as const;
        // Only fetch selectors that are non-zero (FS/GS are often 0 in DPMI).
        const toFetch = segNames.filter((n) => {
          const v = regs[n];
          return typeof v === "number" && v !== 0;
        });

        const selInfos = await Promise.all(
          toFetch.map((n) =>
            db.sendCommand({ cmd: "selinfo", sel: regs[n] as number })
              .then((r) => ({ name: n, sel: regs[n] as number, info: r }))
              .catch(() => ({ name: n, sel: regs[n] as number, info: null })),
          ),
        );

        // Build compact one-line-per-segment table; collapse identical selectors.
        const seen = new Map<number, string>();
        const lines: string[] = [];
        for (const { name, sel: selVal, info } of selInfos) {
          const hex = `0x${selVal.toString(16).padStart(4, "0")}`;
          if (seen.has(selVal)) {
            lines.push(`${name.padEnd(2)}  ${hex}  (same as ${seen.get(selVal)})`);
            continue;
          }
          seen.set(selVal, name);
          if (!info || info["status"] !== "ok") {
            lines.push(`${name.padEnd(2)}  ${hex}  (decode failed)`);
            continue;
          }
          const table = String(info["table"] ?? "").toUpperCase();
          const type = String(info["seg_type"] ?? "");
          const big = info["big"] ? "32" : "16";
          const gran = info["granularity"] ? "×4KB" : "bytes";
          const flat = info["base"] === "0x00000000" && info["granularity"] ? "  flat" : "";
          lines.push(
            `${name.padEnd(2)}  ${hex}  ${table.padEnd(3)}  ${(type + big).padEnd(8)}` +
            `  base=${info["base"]}  limit=${info["limit"]}(${gran})  dpl=${info["dpl"]}${flat}`,
          );
        }
        if (lines.length > 0) {
          parts.push("── Active Segments ─────────────────────────────", ...lines);
        }
      }

      // Optional: IDT gate
      if (intNum !== undefined) {
        const gate = await db.sendCommand({ cmd: "get_idt_entry", int: intNum });
        if (gate["status"] === "ok") {
          parts.push(
            `── IDT[0x${intNum.toString(16)}] ──────────────────────────────`,
            `sel=${gate["sel"]}  offset=${gate["offset"]}`,
          );
        }
      }

      // Optional: arbitrary selector
      if (sel !== undefined) {
        const param = typeof sel === "number" ? { sel } : { sel: String(sel) };
        const si = await db.sendCommand({ cmd: "selinfo", ...param });
        if (si["status"] === "ok") {
          parts.push(
            "── Selector ────────────────────────────────────",
            `selector  ${si["sel"]}  (${String(si["table"]).toUpperCase()})`,
            `base      ${si["base"]}`,
            `limit     ${si["limit"]}${si["granularity"] ? "  (×4KB)" : "  (bytes)"}`,
            `dpl       ${si["dpl"]}`,
            `type      ${si["seg_type"]}`,
            `present   ${si["present"]}`,
            `D/B (big) ${si["big"]}  ← 32-bit segment when true`,
          );
        }
      }
    } catch (e) {
      parts.push(`ERROR: ${(e as Error).message}`);
    }

    return { content: [{ type: "text", text: parts.join("\n") }] };
  },
);

// ─── 11. Trace primitives (far call/ret, DPMI, RMCS) ─────────────────────── //

function hex4(v: number): string {
  return `0x${(v & 0xffff).toString(16).toUpperCase().padStart(4, "0")}`;
}

/** Read `len` bytes at a linear address; page-faulted bytes come back as -1. */
async function readBytesLinear(addr: number, len: number): Promise<number[]> {
  const resp = await db.sendCommand({ cmd: "mem_read_linear", addr, len });
  if (resp["status"] !== "ok" || typeof resp["data"] !== "string") {
    throw new Error(`mem_read_linear ${hex(addr)} failed: ${j(resp)}`);
  }
  const tokens = (resp["data"] as string).match(/.{1,2}/g) ?? [];
  return tokens.map((t) => (t === "PF" || t === "??" ? -1 : parseInt(t, 16)));
}

function leU16(b: number[], o: number): number {
  return ((b[o] & 0xff) | ((b[o + 1] & 0xff) << 8)) & 0xffff;
}
function leU32(b: number[], o: number): number {
  return (
    (b[o] & 0xff) |
    ((b[o + 1] & 0xff) << 8) |
    ((b[o + 2] & 0xff) << 16) |
    ((b[o + 3] & 0xff) << 24)
  ) >>> 0;
}

interface SelDesc {
  sel: number;
  base: number;
  limit: number;
  big: boolean;
  dpl: number;
  segType: string;
  present: boolean;
  gran: boolean;
  table: string;
}

/** Decode a selector (number) or segment register name ("cs".."gs") via selinfo. */
async function describeSelector(sel: number | string): Promise<SelDesc | undefined> {
  const resp = await db.sendCommand({ cmd: "selinfo", sel });
  if (resp["status"] !== "ok") return undefined;
  return {
    sel: parseRegisterNumber(resp["sel"]) ?? (typeof sel === "number" ? sel : 0),
    base: parseRegisterNumber(resp["base"]) ?? 0,
    limit: parseRegisterNumber(resp["limit"]) ?? 0,
    big: resp["big"] === true,
    dpl: typeof resp["dpl"] === "number" ? (resp["dpl"] as number) : -1,
    segType: String(resp["seg_type"] ?? "?"),
    present: resp["present"] === true,
    gran: resp["granularity"] === true,
    table: String(resp["table"] ?? "?"),
  };
}

/** Linear base of a segment register: descriptor base in PM, seg<<4 otherwise. */
async function segBase(name: string, regs: Record<string, number>): Promise<number> {
  const desc = await describeSelector(name);
  if (desc !== undefined) return desc.base >>> 0;
  return ((regs[name.toUpperCase()] ?? 0) << 4) >>> 0;
}

interface ModRMResult {
  eaOffset: number;
  segReg: string;
  length: number;
  text: string;
}

const R32 = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"];

/** Decode the effective offset of a ModRM memory operand (16- or 32-bit addressing). */
function decodeModRMOffset(
  b: number[],
  addr32: boolean,
  regs: Record<string, number>,
  segOverride: string | undefined,
): ModRMResult {
  const modrm = b[0] & 0xff;
  const mod = (modrm >> 6) & 3;
  const rm = modrm & 7;
  let len = 1;
  let off = 0;
  let seg = segOverride ?? "ds";
  let text = "";

  const dispStr = (d: number) => (d ? `+0x${(d >>> 0).toString(16)}` : "");

  if (!addr32) {
    if (mod === 0 && rm === 6) {
      off = leU16(b, 1);
      len += 2;
      text = `[0x${off.toString(16)}]`;
    } else {
      const BX = regs["EBX"] & 0xffff;
      const BP = regs["EBP"] & 0xffff;
      const SI = regs["ESI"] & 0xffff;
      const DI = regs["EDI"] & 0xffff;
      let val = 0;
      let label = "";
      switch (rm) {
        case 0: val = BX + SI; label = "bx+si"; break;
        case 1: val = BX + DI; label = "bx+di"; break;
        case 2: val = BP + SI; label = "bp+si"; seg = segOverride ?? "ss"; break;
        case 3: val = BP + DI; label = "bp+di"; seg = segOverride ?? "ss"; break;
        case 4: val = SI; label = "si"; break;
        case 5: val = DI; label = "di"; break;
        case 6: val = BP; label = "bp"; seg = segOverride ?? "ss"; break;
        default: val = BX; label = "bx"; break;
      }
      let disp = 0;
      if (mod === 1) { disp = (b[1] << 24) >> 24; len += 1; }
      else if (mod === 2) { disp = leU16(b, 1); len += 2; }
      off = (val + disp) & 0xffff;
      text = `[${label}${dispStr(disp)}]`;
    }
  } else {
    if (mod === 0 && rm === 5) {
      off = leU32(b, 1);
      len += 4;
      text = `[0x${off.toString(16)}]`;
    } else if (rm === 4) {
      const sib = b[1] & 0xff;
      len += 1;
      const scale = (sib >> 6) & 3;
      const index = (sib >> 3) & 7;
      const base = sib & 7;
      let val = 0;
      const bparts: string[] = [];
      if (base === 5 && mod === 0) {
        const disp = leU32(b, 2); len += 4; val += disp; bparts.push(`0x${(disp >>> 0).toString(16)}`);
      } else {
        val += regs[R32[base]] >>> 0; bparts.push(R32[base].toLowerCase());
        if (base === 4 || base === 5) seg = segOverride ?? "ss";
      }
      if (index !== 4) {
        val += (regs[R32[index]] >>> 0) * (1 << scale);
        bparts.push(`${R32[index].toLowerCase()}*${1 << scale}`);
      }
      if (mod === 1) { val += (b[len] << 24) >> 24; len += 1; }
      else if (mod === 2) { val += leU32(b, len); len += 4; }
      off = val >>> 0;
      text = `[${bparts.join("+")}]`;
    } else {
      const val = regs[R32[rm]] >>> 0;
      if (rm === 5) seg = segOverride ?? "ss";
      let disp = 0;
      if (mod === 1) { disp = (b[1] << 24) >> 24; len += 1; }
      else if (mod === 2) { disp = leU32(b, 1); len += 4; }
      off = (val + disp) >>> 0;
      text = `[${R32[rm].toLowerCase()}${dispStr(disp)}]`;
    }
  }
  return { eaOffset: off, segReg: seg, length: len, text };
}

const SEG_PREFIX: Record<number, string> = {
  0x26: "es", 0x2e: "cs", 0x36: "ss", 0x3e: "ds", 0x64: "fs", 0x65: "gs",
};

async function descLine(desc: SelDesc | undefined, sel: number): Promise<string> {
  if (desc === undefined) return `selector ${hex4(sel)} (no descriptor / real-mode)`;
  return (
    `selector ${hex4(sel)} (${desc.table}) base=${hex(desc.base)} ` +
    `limit=${hex(desc.limit)}${desc.gran ? " ×4K" : ""} D/B=${desc.big ? 1 : 0} ` +
    `dpl=${desc.dpl} ${desc.segType}${desc.present ? "" : " NOT-PRESENT"}`
  );
}

server.tool(
  "dosbox_trace_far",
  "Decode the far control-flow instruction at the current CS:EIP *before* it executes " +
  "(read-only). Handles `call far`/`jmp far` (direct ptr16:16/32 and indirect via memory) " +
  "and `retf`. Reports the pointer source, the bytes read, the target selector descriptor " +
  "(base/limit/D-B), the target linear address, and the expected return-frame width.",
  {},
  async () => {
    try {
      const regs = await readRegisters();
      const cs = regs["CS"] ?? 0;
      const eip = regs["EIP"] ?? 0;
      const csDesc = await describeSelector("cs");
      const csBase = csDesc?.base ?? (cs << 4);
      const csBig = csDesc?.big ?? false;
      const linear = (csBase + eip) >>> 0;
      const code = await readBytesLinear(linear, 12);

      let i = 0;
      let opsize = csBig;
      let addrsize = csBig;
      let segOverride: string | undefined;
      while (i < code.length) {
        const byte = code[i];
        if (byte === 0x66) { opsize = !csBig; i++; continue; }
        if (byte === 0x67) { addrsize = !csBig; i++; continue; }
        if (SEG_PREFIX[byte] !== undefined) { segOverride = SEG_PREFIX[byte]; i++; continue; }
        if (byte === 0xf0 || byte === 0xf2 || byte === 0xf3) { i++; continue; }
        break;
      }

      const offW = opsize ? 4 : 2;
      const frameW = opsize ? 8 : 4;
      const op = code[i];
      const lines: string[] = [];
      lines.push(
        `at ${hex4(cs)}:${hex(eip)} (linear ${hex(linear)}) CS D/B=${csBig ? 1 : 0}`,
      );

      const reportTarget = async (offset: number, sel: number, source: string) => {
        const tdesc = await describeSelector(sel);
        const tbase = tdesc?.base ?? (sel << 4);
        const tlin = (tbase + offset) >>> 0;
        lines.push(source);
        lines.push(`target ${hex4(sel)}:${hex(offset)}  ->  linear ${hex(tlin)}`);
        lines.push(await descLine(tdesc, sel));
      };

      if (op === 0x9a || op === 0xea) {
        const kind = op === 0x9a ? "call far" : "jmp far";
        const offset = opsize ? leU32(code, i + 1) : leU16(code, i + 1);
        const sel = leU16(code, i + 1 + offW);
        lines.push(`${kind} ${hex4(sel)}:${hex(offset)} (direct ptr${opsize ? "16:32" : "16:16"})`);
        await reportTarget(offset, sel, `immediate operand in code`);
        if (op === 0x9a) lines.push(`return frame: ${frameW} bytes (${opsize ? "EIP(4)+CS(4 padded)" : "IP(2)+CS(2)"})`);
      } else if (op === 0xff) {
        const reg = (code[i + 1] >> 3) & 7;
        if (reg !== 3 && reg !== 5) {
          lines.push(`opcode FF /${reg} at CS:EIP is not a far call/jmp.`);
          return { content: [{ type: "text", text: lines.join("\n") }] };
        }
        const kind = reg === 3 ? "call far" : "jmp far";
        const ea = decodeModRMOffset(code.slice(i + 1), addrsize, regs, segOverride);
        const base = await segBase(ea.segReg, regs);
        const ptrLin = (base + ea.eaOffset) >>> 0;
        const ptr = await readBytesLinear(ptrLin, offW + 2);
        const offset = opsize ? leU32(ptr, 0) : leU16(ptr, 0);
        const sel = leU16(ptr, offW);
        const rawHex = ptr.map((x) => (x < 0 ? "PF" : x.toString(16).padStart(2, "0"))).join(" ");
        lines.push(`${kind} ${ea.segReg}:${ea.text} (indirect, ptr${opsize ? "16:32" : "16:16"})`);
        lines.push(`pointer @ linear ${hex(ptrLin)}: ${rawHex}`);
        await reportTarget(offset, sel, `read from memory`);
        if (reg === 3) lines.push(`return frame: ${frameW} bytes (${opsize ? "EIP(4)+CS(4 padded)" : "IP(2)+CS(2)"})`);
      } else if (op === 0xcb || op === 0xca) {
        const ssBase = await segBase("ss", regs);
        const esp = regs["ESP"] ?? 0;
        const frameLin = (ssBase + esp) >>> 0;
        const frame = await readBytesLinear(frameLin, offW + 2);
        const offset = opsize ? leU32(frame, 0) : leU16(frame, 0);
        const sel = leU16(frame, offW);
        const rawHex = frame.map((x) => (x < 0 ? "PF" : x.toString(16).padStart(2, "0"))).join(" ");
        const imm = op === 0xca ? leU16(code, i + 1) : 0;
        lines.push(`retf${op === 0xca ? ` 0x${imm.toString(16)}` : ""} (operand size ${opsize ? "32" : "16"})`);
        lines.push(`frame @ SS:ESP linear ${hex(frameLin)}: ${rawHex}`);
        await reportTarget(offset, sel, `popped from stack`);
        lines.push(`pops ${frameW}${imm ? ` + ${imm}` : ""} bytes off the stack`);
      } else {
        lines.push(`opcode 0x${op.toString(16)} at CS:EIP is not a far call/jmp/retf.`);
      }

      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

/** Decode a 50-byte DPMI Real Mode Call Structure into readable lines. */
function decodeRmcs(b: number[]): string[] {
  const flags = leU16(b, 0x20);
  const fl = (mask: number, ch: string) => ((flags & mask) ? ch : "-");
  const flagStr =
    fl(0x800, "O") + fl(0x400, "D") + fl(0x200, "I") + fl(0x80, "S") +
    fl(0x40, "Z") + fl(0x10, "A") + fl(0x04, "P") + fl(0x01, "C");
  return [
    `EDI=${hex(leU32(b, 0x00))} ESI=${hex(leU32(b, 0x04))} EBP=${hex(leU32(b, 0x08))}`,
    `EBX=${hex(leU32(b, 0x10))} EDX=${hex(leU32(b, 0x14))} ECX=${hex(leU32(b, 0x18))} EAX=${hex(leU32(b, 0x1c))}`,
    `Flags=${hex4(flags)} [${flagStr}]`,
    `CS:IP=${hex4(leU16(b, 0x2c))}:${hex4(leU16(b, 0x2a))}   SS:SP=${hex4(leU16(b, 0x30))}:${hex4(leU16(b, 0x2e))}`,
    `DS=${hex4(leU16(b, 0x24))} ES=${hex4(leU16(b, 0x22))} FS=${hex4(leU16(b, 0x26))} GS=${hex4(leU16(b, 0x28))}`,
  ];
}

server.tool(
  "dosbox_rmcs_dump",
  "Pretty-print a DPMI Real Mode Call Structure (50 bytes). " +
  "Target resolution order: explicit 'addr' (linear) > 'seg'+'off' > symbol 'name' > " +
  "current ES:EDI (the RMCS pointer convention for INT 31h AX=0300h).",
  {
    addr: z.number().optional().describe("Linear address of the RMCS."),
    seg: z.number().optional().describe("Segment (with off) holding the RMCS."),
    off: z.number().optional().describe("Offset (with seg)."),
    name: z.string().optional().describe("Loaded symbol naming the RMCS."),
  },
  async ({ addr, seg, off, name }) => {
    try {
      const regs = await readRegisters();
      let linear: number;
      let how: string;
      if (addr !== undefined) {
        linear = addr >>> 0; how = `addr ${hex(linear)}`;
      } else if (seg !== undefined && off !== undefined) {
        const base = await describeSelector(seg);
        linear = ((base?.base ?? (seg << 4)) + off) >>> 0;
        how = `${hex4(seg)}:${hex(off)} -> ${hex(linear)}`;
      } else if (name !== undefined) {
        const resolved = await resolveLoadedSymbol(name);
        linear = resolved.linear; how = `${name} -> ${hex(linear)} (${resolved.explanation})`;
      } else {
        const esBase = await segBase("es", regs);
        linear = (esBase + (regs["EDI"] ?? 0)) >>> 0;
        how = `ES:EDI (${hex4(regs["ES"] ?? 0)}:${hex(regs["EDI"] ?? 0)}) -> ${hex(linear)}`;
      }
      const bytes = await readBytesLinear(linear, 50);
      return {
        content: [{
          type: "text",
          text: [`RMCS @ ${how}`, ...decodeRmcs(bytes)].join("\n"),
        }],
      };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

const DPMI_FUNCS: Record<number, string> = {
  0x0000: "Allocate LDT descriptors (CX=count)",
  0x0001: "Free LDT descriptor (BX=sel)",
  0x0006: "Get segment base (BX=sel) -> CX:DX",
  0x0007: "Set segment base (BX=sel, CX:DX=base)",
  0x0008: "Set segment limit (BX=sel, CX:DX=limit)",
  0x0009: "Set descriptor access rights (BX=sel, CX=rights)",
  0x000a: "Create alias descriptor (BX=sel)",
  0x000c: "Set descriptor (BX=sel, ES:EDI=descriptor)",
  0x0100: "Allocate DOS memory (BX=paragraphs) -> AX=seg, DX=sel",
  0x0101: "Free DOS memory (DX=sel)",
  0x0200: "Get real-mode interrupt vector (BL=int) -> CX:DX",
  0x0201: "Set real-mode interrupt vector (BL=int, CX:DX)",
  0x0300: "Simulate real-mode interrupt (BL=int, BH=flags, CX=words, ES:EDI=RMCS)",
  0x0301: "Call real-mode far proc (ES:EDI=RMCS)",
  0x0303: "Allocate real-mode callback",
  0x0501: "Allocate memory block (BX:CX=size) -> BX:CX=lin, SI:DI=handle",
  0x0502: "Free memory block (SI:DI=handle)",
  0x0503: "Resize memory block",
  0x0800: "Map physical address (BX:CX=phys, SI:DI=size) -> BX:CX=lin",
  0x0801: "Free physical mapping (BX:CX=lin)",
};

server.tool(
  "dosbox_trace_dpmi",
  "Decode the DPMI call described by the current registers (INT 31h, AX=function). " +
  "Names the function, shows the relevant register pairs (BX:CX, SI:DI, ES:EDI), and " +
  "for AX=0300h/0301h also dumps the Real Mode Call Structure at ES:EDI.",
  {},
  async () => {
    try {
      const regs = await readRegisters();
      const ax = (regs["EAX"] ?? 0) & 0xffff;
      const bx = (regs["EBX"] ?? 0) & 0xffff;
      const cx = (regs["ECX"] ?? 0) & 0xffff;
      const si = (regs["ESI"] ?? 0) & 0xffff;
      const di = (regs["EDI"] ?? 0) & 0xffff;

      const csBase = await segBase("cs", regs);
      const atLinear = (csBase + (regs["EIP"] ?? 0)) >>> 0;
      const op = await readBytesLinear(atLinear, 2).catch(() => [-1, -1]);
      const atInt31 = op[0] === 0xcd && op[1] === 0x31;

      const lines: string[] = [];
      lines.push(
        atInt31
          ? `at an INT 31h opcode (${hex4(regs["CS"] ?? 0)}:${hex(regs["EIP"] ?? 0)})`
          : `note: CS:EIP is NOT on an INT 31h opcode (decoding from current AX anyway)`,
      );
      lines.push(`AX=${hex4(ax)}  ${DPMI_FUNCS[ax] ?? "(unknown / not a recognized DPMI function)"}`);

      if (ax === 0x0300 || ax === 0x0301) {
        const intNo = bx & 0xff;
        if (ax === 0x0300) lines.push(`real-mode INT ${hex4(intNo)} (BH flags=0x${((regs["EBX"] ?? 0) >> 8 & 0xff).toString(16)}, CX words=${cx})`);
        const esBase = await segBase("es", regs);
        const rmcsLin = (esBase + (regs["EDI"] ?? 0)) >>> 0;
        lines.push(`RMCS @ ES:EDI (${hex4(regs["ES"] ?? 0)}:${hex(regs["EDI"] ?? 0)}) -> ${hex(rmcsLin)}`);
        const bytes = await readBytesLinear(rmcsLin, 50).catch(() => undefined);
        if (bytes) {
          const rax = leU32(bytes, 0x1c);
          if (intNo === 0x10) lines.push(`  VBE/INT10h AX=${hex4(rax & 0xffff)}`);
          lines.push(...decodeRmcs(bytes).map((l) => "  " + l));
        }
      } else if (ax === 0x0501) {
        lines.push(`size = (BX:CX) = ${hex(((bx << 16) | cx) >>> 0)} bytes`);
      } else if (ax === 0x0800 || ax === 0x0801) {
        lines.push(`phys/lin (BX:CX) = ${hex(((bx << 16) | cx) >>> 0)}, size (SI:DI) = ${hex(((si << 16) | di) >>> 0)}`);
      } else if (ax === 0x0100) {
        lines.push(`paragraphs (BX) = ${bx} (${bx * 16} bytes)`);
      }

      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

async function registerD32ModuleAtBase(file: string, base: number): Promise<D32Module> {
  const module = parseD32File(file);
  d32Modules.set(file, { module, base });
  rebuildSymbolIndex();
  return module;
}

async function scanDiscoveredModules(
  rangeStart: number,
  rangeEnd: number,
  searchPaths: string[],
): Promise<Array<Record<string, unknown>>> {
  const results: Array<Record<string, unknown>> = [];
  const seenBases = new Set<number>();

  for (const scanner of moduleScanners) {
    const pattern = scannerMagicPattern(scanner);
    const resp = await db.sendCommand(clean({
      cmd: "mem_search",
      addr: rangeStart,
      len: (rangeEnd - rangeStart) >>> 0,
      pattern,
      max_results: 256,
    }));
    if (resp["status"] !== "ok") {
      throw new Error(`mem_search failed: ${j(resp)}`);
    }

    const matches = Array.isArray(resp["matches"]) ? resp["matches"] as unknown[] : [];
    for (const match of matches) {
      const base = typeof match === "number"
        ? match >>> 0
        : typeof match === "string"
          ? Number.parseInt(match.replace(/^0x/i, ""), 16) >>> 0
          : undefined;
      if (base === undefined || seenBases.has(base)) continue;
      seenBases.add(base);

      const headerBytes = await readLinearBytesChunked(base, 72);
      const probe = scanner.headerProbe(headerBytes, base);
      if (probe === null) continue;

      const resolved = resolveProbeName(probe, headerBytes, searchPaths);
      const key = `${probe.formatId}@${hex(base)}`;
      discoveredModules.set(key, {
        probe: { ...probe, name: resolved.name },
        file: resolved.file,
        registered: resolved.file !== undefined && d32Modules.has(resolved.file),
      });

      results.push({
        key,
        format: probe.formatId,
        base: hex(base),
        size: hex(probe.size),
        name: resolved.name,
        file: resolved.file,
        fingerprint: probe.fingerprint,
        registered: resolved.file !== undefined && d32Modules.has(resolved.file),
      });
    }
  }

  return results;
}

function resolveModuleSectionOffset(moduleName: string, section: string, offset: number): number | undefined {
  const requested = moduleName.toUpperCase().replace(/\.D32$/i, "");
  for (const { module, base } of d32Modules.values()) {
    const candidate = module.name.toUpperCase().replace(/\.D32$/i, "");
    if (candidate !== requested) continue;
    const sectionId = section === "code" ? 0 : section === "data" ? 1 : section === "bss" ? 2 : -1;
    if (sectionId < 0) return undefined;
    return sectionRuntimeAddress(module, base, sectionId, offset);
  }
  return undefined;
}

// ─── 11. Crash report ────────────────────────────────────────────────────── //

/** Parse a linear hex address from a branch trace entry field (e.g. "0x00100000"). */
function parseBranchLinear(val: unknown): number | undefined {
  if (typeof val === "number") return val >>> 0;
  if (typeof val === "string") {
    const n = Number.parseInt((val as string).replace(/^0x/i, ""), 16);
    return Number.isFinite(n) ? n >>> 0 : undefined;
  }
  return undefined;
}

function annotateReverseCheckpoint(entry: db.JsonObject): db.JsonObject {
  const linear = parseBranchLinear(entry["linear"]);
  const fromLinear = parseBranchLinear(entry["from_linear"]);
  return clean({
    ...entry,
    where: linear === undefined ? undefined : symbolIndex.describe(linear) ?? describeMapAddress(loadedMap, currentLoadInfo, linear),
    fromWhere: fromLinear === undefined || fromLinear === 0
      ? undefined
      : symbolIndex.describe(fromLinear) ?? describeMapAddress(loadedMap, currentLoadInfo, fromLinear),
  });
}

function annotateReverseResponse(resp: db.JsonObject): db.JsonObject {
  const linear = parseBranchLinear(resp["linear"]);
  const checkpoints = Array.isArray(resp["checkpoints"])
    ? (resp["checkpoints"] as db.JsonObject[]).map(annotateReverseCheckpoint)
    : undefined;
  return clean({
    ...resp,
    where: linear === undefined ? undefined : symbolIndex.describe(linear) ?? describeMapAddress(loadedMap, currentLoadInfo, linear),
    checkpoints,
  });
}

server.tool(
  "dosbox_reverse",
  "Opt-in Level 2 reverse checkpoints for core=normal. " +
  "start enables branch checkpoints; back/forward/goto navigate retained checkpoints and leave DOSBox frozen. " +
  "This is best-effort CPU+RAM time travel, not device-state rollback.",
  {
    op: z.enum(["start", "stop", "status", "list", "back", "forward", "goto"]).describe("Operation."),
    max_checkpoints: z.number().int().min(2).max(8192).optional().describe("Maximum retained checkpoints for op=start (default 512)."),
    mode: z.enum(["branch", "instruction"]).optional().describe("Checkpoint mode for op=start. instruction is experimental and can grow memory quickly."),
    checkpoint: z.enum(["branch", "instruction"]).optional().describe("Legacy alias for mode."),
    last: z.number().int().min(0).optional().describe("For op=list, return only the newest N checkpoints."),
    steps: z.number().int().min(1).optional().describe("For op=back/forward, number of checkpoints to move (default 1)."),
    checkpoint_id: z.number().int().optional().describe("For op=goto, target checkpoint_id."),
  },
  async ({ op, max_checkpoints, checkpoint, mode, last, steps, checkpoint_id }) => {
    if (checkpoint !== undefined && mode !== undefined && checkpoint !== mode) {
      return {
        content: [{ type: "text", text: "ERROR: checkpoint and mode disagree" }],
        isError: true,
      };
    }
    const command = clean({
      cmd: "reverse_trace",
      op,
      max_checkpoints,
      mode: mode ?? checkpoint,
      last,
      steps,
      checkpoint_id,
    });
    const resp = await db.sendCommand(command, undefined, 10_000);
    const annotated = annotateReverseResponse(resp);
    return {
      content: [{ type: "text", text: j(annotated) }],
      isError: isErr(resp),
    };
  },
);

/** Build a single annotated branch trail line. */
function formatBranchEntry(
  idx: number,
  fromLinear: number | undefined,
  fromCs: number | undefined,
  toLinear: number | undefined,
  toCs: number | undefined,
  lastKnownGoodIdx: number | undefined,
): string {
  const fHex = fromLinear !== undefined ? hex(fromLinear) : "?";
  const tHex = toLinear !== undefined ? hex(toLinear) : "?";
  const fSym = fromLinear !== undefined ? (symbolIndex.describe(fromLinear) ?? "?") : "?";
  const tSym = toLinear !== undefined ? (symbolIndex.describe(toLinear) ?? "?") : "?";
  const fCs = fromCs !== undefined ? ` CS=${fromCs}` : "";
  const tCs = toCs !== undefined ? ` CS=${toCs}` : "";
  const marker = idx === lastKnownGoodIdx ? "  ← LAST_KNOWN_GOOD" : "";
  return `  [${idx}] ${fHex}${fCs} (${fSym}) -> ${tHex}${tCs} (${tSym})${marker}`;
}

server.tool(
  "dosbox_crash_report",
  "One-call crash analysis: faulting context, symbolicated branch trail, last-known-good heuristic, " +
  "stack window, and screen fault text. Works with any stop reason (exception/watchpoint/process_exit/exec). " +
  "Uses latched lastStopEvent, get_last_exit fallback, and trace dump. " +
  "Branch trace must be started before the crash with dosbox_debug_prepare or dosbox_trace({op:'start'}).",
  {
    trailDepth: z.number().int().min(1).max(8192).optional().describe(
      "Max branch trail entries to include (default 64).",
    ),
    context: z.boolean().optional().describe("Include full stop context with selectors/disasm/stack (default true)."),
  },
  async ({ trailDepth = 64, context = true }) => {
    try {
      const lines: string[] = ["── Crash Report ─────────────────────────────────"];

      // --- Stop event / reason ---
      const stopEvent = lastStopEvent ?? await fetchLastStopFallback();
      const exitInfo = lastProcessExit ?? await db.sendCommand({ cmd: "get_last_exit" }, undefined, 2_000)
        .then((r) => r["status"] === "ok" && r["available"] === true ? r : undefined)
        .catch(() => undefined);
      const eventName = typeof stopEvent?.["event"] === "string" ? stopEvent["event"] : undefined;
      const reason = typeof stopEvent?.["reason"] === "string"
        ? stopEvent["reason"]
        : eventName === "process_exit" || exitInfo !== undefined
          ? "process_exit"
          : "unknown";
      lines.push(`stopReason: ${reason}`);

      if (stopEvent !== undefined) {
        if (reason === "exception") {
          const vec = stopEvent["vector"];
          const ec = stopEvent["error_code"];
          const cr2 = stopEvent["CR2"] ?? stopEvent["cr2"];
          const faultCs = stopEvent["fault_cs"];
          const faultEip = stopEvent["fault_eip"];
          const isNested = stopEvent["is_nested"];
          lines.push(`exception: vector=${vec} error_code=${ec} CR2=${cr2}`);
          lines.push(`  fault at CS=${faultCs} EIP=${faultEip}${isNested ? " (NESTED FAULT)" : ""}`);
        } else if (reason === "watchpoint") {
          const wLin = stopEvent["watch_linear"];
          const wSz = stopEvent["watch_size"];
          const oldVal = stopEvent["old"];
          const newVal = stopEvent["new"];
          const culpritCs = stopEvent["culprit_cs"];
          const culpritEip = stopEvent["culprit_eip"];
          lines.push(`watchpoint: linear=${wLin} size=${wSz} old=${oldVal} new=${newVal}`);
          lines.push(`  culprit write at CS=${culpritCs} EIP=${culpritEip}`);
          const culpritLinear = parseBranchLinear(culpritEip);
          if (culpritLinear !== undefined) {
            const sym = symbolIndex.describe(culpritLinear);
            if (sym !== undefined) lines.push(`  culprit symbol: ${sym}`);
          }
        } else if (reason === "process_exit") {
          const ec = stopEvent["exit_code"];
          const psp = stopEvent["psp"];
          const tsr = stopEvent["tsr"];
          const abnormal = stopEvent["abnormal"];
          lines.push(`process exited: code=${ec} psp=${psp} tsr=${tsr} abnormal=${abnormal}`);
        }
      }

      // --- Process exit fallback ---
      if (exitInfo !== undefined && eventName !== "process_exit") {
        lines.push(`lastProcessExit: code=${exitInfo["exit_code"]} psp=${exitInfo["psp"]} ` +
          `tsr=${exitInfo["tsr"]} abnormal=${exitInfo["abnormal"]}`);
      }

      const reverseStatus = await db.sendCommand({ cmd: "reverse_trace", op: "status" }, undefined, 2_000)
        .catch(() => undefined);
      if (reverseStatus?.["status"] === "ok" && reverseStatus["enabled"] === true) {
        lines.push(
          `reverseTrace: checkpoint=${reverseStatus["checkpoint_id"]} ` +
          `cursor=${reverseStatus["cursor_index"]}/${reverseStatus["newest_index"]} ` +
          `forward=${reverseStatus["hasForwardHistory"]}`,
        );
        lines.push("  Tip: use dosbox_reverse {op:'back', steps:1} to rewind one checkpoint.");
      }

      // --- Branch trail ---
      lines.push("── Branch Trail (most recent first) ─────────────");
      const traceResp = await db.sendCommand({ cmd: "trace", op: "dump", last: trailDepth }, undefined, 5_000)
        .catch((e) => ({ status: "error", msg: (e as Error).message }) as db.JsonObject);

      if (traceResp["status"] !== "ok" || !Array.isArray(traceResp["entries"])) {
        lines.push(`  (no trace data: ${traceResp["msg"] ?? "trace not started or empty"})`);
        lines.push("  Tip: start trace before reloading with dosbox_debug_prepare or dosbox_trace({op:'start'})");
      } else {
        const entries = traceResp["entries"] as db.JsonObject[];
        const totalCount = typeof traceResp["count"] === "number" ? traceResp["count"] : entries.length;

        // Find lastKnownGood: most recent (lowest index) entry where from resolves to a known
        // symbol but to does NOT, or from is known and to is much further away.
        let lastKnownGoodIdx: number | undefined;
        for (let i = 0; i < entries.length; i++) {
          const entry = entries[i];
          const fromLinear = parseBranchLinear(entry["from"]);
          const toLinear = parseBranchLinear(entry["to"]);
          if (fromLinear === undefined || toLinear === undefined) continue;
          const fromNearest = symbolIndex.nearest(fromLinear);
          const toNearest = symbolIndex.nearest(toLinear);
          // Heuristic: from is in a known symbol, to is unknown
          if (fromNearest !== undefined && toNearest === undefined) {
            lastKnownGoodIdx = i;
            break;
          }
        }

        lines.push(`  total entries in ring: ${totalCount}, showing: ${entries.length}`);
        for (let i = 0; i < entries.length; i++) {
          const entry = entries[i];
          const fromLinear = parseBranchLinear(entry["from"]);
          const toLinear = parseBranchLinear(entry["to"]);
          const fromCs = typeof entry["from_cs"] === "number" ? entry["from_cs"] as number : undefined;
          const toCs = typeof entry["to_cs"] === "number" ? entry["to_cs"] as number : undefined;
          lines.push(formatBranchEntry(i, fromLinear, fromCs, toLinear, toCs, lastKnownGoodIdx));
        }

        if (lastKnownGoodIdx === undefined) {
          lines.push("  (lastKnownGood: not determined — all destinations known or no symbols loaded)");
        }
      }

      // --- Full stop context (optional) ---
      if (context) {
        const contextReport = await buildContextReport(true).catch((e) => `buildContextReport error: ${(e as Error).message}`);
        lines.push(contextReport);
      }

      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (e) {
      return { content: [{ type: "text", text: `ERROR: ${(e as Error).message}` }], isError: true };
    }
  },
);

// ─── 12. Drive mount / unmount ───────────────────────────────────────────── //

server.tool(
  "dosbox_mount",
  "Mount or unmount a DOS drive at runtime without touching the GUI. " +
  "op='mount': directory mount (equivalent to DOSBox 'mount <drive> <path> [-t <type>]'). " +
  "op='imgmount': floppy/CD-ROM/HDD image mount (equivalent to 'imgmount <drive> <path> -t <type> -fs <fs>'). " +
  "op='unmount': remove a drive (equivalent to 'mount -u <drive>'). " +
  "op='swap': swap the active disk image on an already-mounted drive — works while a DOS program is running (no shell required). " +
  "Without path, cycles to the next image in the pre-loaded swap list (same as GUI 'Swap disk'). " +
  "With path, replaces the active image with a new host file. " +
  "op='swap_list': pre-load an ordered list of disk images into the drive's swap list — works while a program is running. " +
  "Provide 'images' (array of absolute host paths). The first image becomes active immediately; " +
  "subsequent op='swap' calls cycle through the list. " +
  "drive is a single letter (A–Z). path is the host filesystem path for mount/imgmount/swap.",
  {
    op: z.enum(["mount", "imgmount", "unmount", "swap", "swap_list"]).describe(
      "'mount' = directory mount, 'imgmount' = image mount, 'unmount' = remove drive, " +
      "'swap' = cycle or replace the active disk image (works while a program is running), " +
      "'swap_list' = pre-load an ordered list of images into the swap list (works while a program is running).",
    ),
    drive: z.string().regex(/^[A-Za-z]$/).describe(
      "Drive letter (single letter, e.g. 'C', 'W', 'A').",
    ),
    path: z.string().optional().describe(
      "Host path to the directory or image file. Required for mount and imgmount. Optional for swap (omit to cycle to next in swap list).",
    ),
    images: z.array(z.string()).optional().describe(
      "Ordered list of absolute host paths to disk images. Required for swap_list. " +
      "The first image becomes active; op='swap' cycles through the list.",
    ),
    type: z.enum(["floppy", "cdrom", "hdd", "local"]).optional().describe(
      "Mount type. For imgmount: floppy|cdrom|hdd. For directory mount: local (default).",
    ),
    fs: z.string().optional().describe(
      "Filesystem type for imgmount (default 'fat').",
    ),
  },
  async ({ op, drive, path, images, type, fs }) => {
    const driveLetter = drive.toUpperCase();

    if (op === "swap") {
      // Direct socket command — bypasses the shell, safe while a program is running.
      const payload: Record<string, string> = { cmd: "floppy_swap", drive: driveLetter };
      if (path) payload.image = path;
      try {
        const resp = await db.sendCommand(payload);
        const ok = !isErr(resp);
        return {
          content: [{ type: "text", text: JSON.stringify({ status: ok ? "ok" : "error", msg: j(resp) }) }],
          isError: !ok,
        };
      } catch (e) {
        return { content: [{ type: "text", text: JSON.stringify({ status: "error", msg: (e as Error).message }) }], isError: true };
      }
    }

    if (op === "swap_list") {
      // Direct socket command — bypasses the shell, safe while a program is running.
      if (!images || images.length === 0) {
        return { content: [{ type: "text", text: JSON.stringify({ status: "error", msg: "images array is required for swap_list" }) }], isError: true };
      }
      try {
        const resp = await db.sendCommand({ cmd: "floppy_load_list", drive: driveLetter, images });
        const ok = !isErr(resp);
        return {
          content: [{ type: "text", text: JSON.stringify({ status: ok ? "ok" : "error", msg: j(resp) }) }],
          isError: !ok,
        };
      } catch (e) {
        return { content: [{ type: "text", text: JSON.stringify({ status: "error", msg: (e as Error).message }) }], isError: true };
      }
    }

    let command: string;
    if (op === "unmount") {
      command = `mount -u ${driveLetter}`;
    } else if (op === "imgmount") {
      if (!path) {
        return { content: [{ type: "text", text: JSON.stringify({ status: "error", msg: "path is required for imgmount" }) }], isError: true };
      }
      const mountType = type ?? "floppy";
      const mountFs = fs ?? "fat";
      command = `imgmount ${driveLetter} ${path} -t ${mountType} -fs ${mountFs}`;
    } else {
      // op === "mount"
      if (!path) {
        return { content: [{ type: "text", text: JSON.stringify({ status: "error", msg: "path is required for mount" }) }], isError: true };
      }
      const mountType = type ?? "local";
      command = mountType !== "local"
        ? `mount ${driveLetter} ${path} -t ${mountType}`
        : `mount ${driveLetter} ${path}`;
    }

    try {
      const resp = await sendDosCommandWhenShellReady(command, 10_000);
      const ok = !isErr(resp);
      if (ok) {
        // Keep the guest->host drive map current, or a program launched from a
        // runtime mount has no on-disk EXE to read debug info from.
        guestMounts = guestMounts.filter((mount) => mount.drive !== driveLetter);
        if (op !== "unmount" && path !== undefined) guestMounts.push({ drive: driveLetter, hostPath: path });
        rebuildSymbolIndex();
      }
      const result = { status: ok ? "ok" : "error", msg: j(resp) };
      return {
        content: [{ type: "text", text: JSON.stringify(result) }],
        isError: !ok,
      };
    } catch (e) {
      const result = { status: "error", msg: (e as Error).message };
      return { content: [{ type: "text", text: JSON.stringify(result) }], isError: true };
    }
  },
);

// ─── 13. Raw escape hatch ─────────────────────────────────────────────────── //

server.tool(
  "dosbox_raw",
  "Send any raw JSON command object to the debug socket. " +
  "Use when no other tool covers the needed operation. " +
  "The object must contain a 'cmd' string field.",
  {
    command: z.record(z.unknown()).describe("Full command JSON, e.g. {cmd:'vga_read',off:0,len:4096}."),
    timeoutMs: z.number().int().optional().describe("Response timeout in ms (default 8000)."),
  },
  async ({ command, timeoutMs = 8000 }) => {
    const resp = await db.sendCommand(command as db.JsonObject, undefined, timeoutMs);
    return { content: [{ type: "text", text: j(resp) }], isError: isErr(resp) };
  },
);

// ─── Start ───────────────────────────────────────────────────────────────── //

const transport = new StdioServerTransport();
await server.connect(transport);
