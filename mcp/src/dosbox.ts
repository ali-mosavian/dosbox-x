/**
 * DOSBox-X debug socket client and process manager.
 *
 * Protocol: newline-delimited JSON over TCP.
 *
 * On connect the server sends:  {"event":"connected","state":"running"|"stopped"}
 * Commands are sent as:         {"cmd":"...", ...args}\n
 * Responses are one of:
 *   {"status":"ok", ...data}           — success
 *   {"status":"error","msg":"..."}     — failure
 *   {"event":"stopped","reason":"...","addr":"SEG:OFF",...regs}  — async stop event
 *
 * Because the server only accepts one client at a time, and commands are
 * synchronous from the server's perspective, we open a fresh connection per
 * command (same pattern as tools/dbg_cmd.py).
 */

import net from "net";
import { spawn, spawnSync, type ChildProcess } from "child_process";
import { existsSync, readFileSync, unlinkSync } from "fs";
import { resolve, dirname } from "path";
import { fileURLToPath } from "url";
import { isDefaultCriticalStopEvent } from "./stopState.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

export const PROJECT_ROOT = resolve(__dirname, "../..");
// mcp/bin/dosbox-x is the bundled copy; fall back to the in-tree debug build.
// DOSBOX_MCP_BIN overrides both, which is how a freshly built emulator gets
// exercised without overwriting the bundled copy a live session is running.
export const DEFAULT_BIN = process.env["DOSBOX_MCP_BIN"] ?? (existsSync(resolve(__dirname, "../bin/dosbox-x"))
  ? resolve(__dirname, "../bin/dosbox-x")
  : resolve(PROJECT_ROOT, "src/dosbox-x"));
// macOS .app wrapper — launched via open(1) to get proper Cocoa/SDL GUI context.
export const MACOS_APP_BUNDLE = resolve(__dirname, "../bin/dosbox-debug.app");
export const DEFAULT_CONF = process.env["DOSBOX_MCP_CONF"] ?? resolve(PROJECT_ROOT, "dosbox-x.reference.conf");
/* DOSBOX_MCP_PORT keeps a second server off the port a live session is using,
 * which is also the only way to exercise this one while one is running. */
export const DEFAULT_PORT = Number.parseInt(process.env["DOSBOX_MCP_PORT"] ?? "2159", 10) || 2159;

export type JsonObject = Record<string, unknown>;

// ─── Process state ────────────────────────────────────────────────────────── //

let _proc: ChildProcess | null = null;
let _macPid: number | null = null;   // PID from pid-file when launched via open(1)
let _port: number = DEFAULT_PORT;

export function getPort(): number {
  return _port;
}

/** Path to the pid file written by the .app wrapper script. */
function pidFilePath(port: number): string {
  return `/tmp/dosbox-debug-${port}.pid`;
}

/** Try to read the PID that the .app wrapper wrote to /tmp. */
function readMacPid(port: number): number | null {
  try {
    const txt = readFileSync(pidFilePath(port), "utf8").trim();
    const n = parseInt(txt, 10);
    return isNaN(n) ? null : n;
  } catch {
    return null;
  }
}

/** Returns true if a macOS PID is still alive. */
function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

export function isAlive(): boolean {
  syncMacPidFromFile(_port);
  if (_macPid !== null) return pidAlive(_macPid);
  return _proc !== null && _proc.exitCode === null && !_proc.killed;
}

/** Poll until the debug socket accepts a connection or the deadline passes. */
async function awaitSocket(port: number, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      await new Promise<void>((res, rej) => {
        const s = net.createConnection(port, "127.0.0.1");
        s.setTimeout(400);
        s.on("connect", () => {
          s.destroy();
          res();
        });
        s.on("timeout", () => {
          s.destroy();
          rej(new Error("timeout"));
        });
        s.on("error", rej);
      });
      return true;
    } catch {
      await sleep(200);
    }
  }
  return false;
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

function clearPidFile(port: number): void {
  try { unlinkSync(pidFilePath(port)); } catch { /* ignore */ }
}

function syncMacPidFromFile(port: number): void {
  if (_macPid !== null && pidAlive(_macPid)) return;

  const pid = readMacPid(port);
  if (pid === null || pid <= 0) {
    _macPid = null;
    return;
  }

  if (pidAlive(pid)) {
    _macPid = pid;
  } else {
    _macPid = null;
    clearPidFile(port);
  }
}

function trackedPid(port: number = _port): number | null {
  syncMacPidFromFile(port);
  return _macPid ?? _proc?.pid ?? null;
}

async function socketStatus(port: number, timeoutMs: number): Promise<JsonObject | null> {
  try {
    const resp = await sendCommand({ cmd: "status" }, port, timeoutMs);
    return resp["status"] === "ok" ? resp : null;
  } catch {
    return null;
  }
}

async function awaitResponsiveSocket(port: number, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const resp = await socketStatus(port, Math.min(800, Math.max(100, deadline - Date.now())));
    if (resp !== null) return true;
    await sleep(200);
  }
  return false;
}

async function cleanupStaleTrackedProcess(port: number): Promise<void> {
  disconnectSession(port);
  syncMacPidFromFile(port);
  const pid = _macPid ?? _proc?.pid ?? null;
  if (pid !== null && pidAlive(pid)) {
    try { process.kill(pid, "SIGKILL"); } catch { /* ignore */ }
  }
  if (_proc !== null) {
    try { _proc.kill("SIGKILL"); } catch { /* ignore */ }
  }
  _proc = null;
  _macPid = null;
  killPort(port, "SIGKILL");
  clearPidFile(port);
  for (let i = 0; i < 10; i++) {
    if (!awaitResponsiveSocket(port, 100)) return;
    await sleep(100);
  }
}

// ─── Process lifecycle ────────────────────────────────────────────────────── //

export interface LaunchOptions {
  binPath?: string;
  conf?: string;
  port?: number;
  /** Extra DOSBox CLI args, e.g. ["-c", "mount c /tmp", "-c", "c:"] */
  extraArgs?: string[];
  /**
   * Run headless (no GUI window). Sets SDL_VIDEODRIVER=dummy so DOSBox
   * starts without creating a display. Useful for automated test runs where
   * the autoexec does all the work. Note: dos_cmd / dosbox_text_screen still
   * work, but SDL keyboard injection may be unreliable in headless mode.
   */
  headless?: boolean;
}

export interface LaunchResult {
  pid: number | null;
  socketUp: boolean;
  msg: string;
}

export async function launch(opts: LaunchOptions = {}): Promise<LaunchResult> {
  const port = opts.port ?? DEFAULT_PORT;
  _port = port;

  const existingPid = trackedPid(port);
  const existingPidAlive = existingPid !== null && pidAlive(existingPid);
  const existingSocketResponsive = await awaitResponsiveSocket(port, 1000);
  if (existingPidAlive && existingSocketResponsive) {
    return {
      pid: existingPid,
      socketUp: true,
      msg: `Already running (pid ${existingPid}); debug socket responsive on :${port}. Call stop() first to restart.`,
    };
  }

  if (existingPidAlive || existingSocketResponsive) {
    await cleanupStaleTrackedProcess(port);
  }

  const bin = opts.binPath ?? DEFAULT_BIN;
  const conf = opts.conf ?? DEFAULT_CONF;
  const extra = opts.extraArgs ?? [];

  if (!existsSync(bin)) {
    throw new Error(`DOSBox binary not found: ${bin}`);
  }
  if (!existsSync(conf)) {
    throw new Error(`Config file not found: ${conf}`);
  }

  _proc = null;
  _macPid = null;

  if (!opts.headless && process.platform === "darwin" && existsSync(MACOS_APP_BUNDLE)) {
    // On macOS, use open(1) to give DOSBox-X a proper Cocoa/SDL foreground
    // application context.  Direct spawn from a background Node.js process
    // makes SDL segfault because Cocoa requires a foreground app context.
    //
    // The .app wrapper script writes its PID to /tmp/dosbox-debug-<port>.pid
    // so we can track and kill the process later.
    //
    // Environment variable DOSBOX_DEBUG_PORT is set inside the wrapper script
    // (hardcoded to 2159).  For non-default ports, set DOSBOX_DEBUG_PORT in
    // the shell environment before starting Cursor.

    // Remove stale pid file
    clearPidFile(port);

    const openProc = spawn(
      "open",
      ["-n", "-a", MACOS_APP_BUNDLE, "--args", "-conf", conf, ...extra],
      { cwd: PROJECT_ROOT, stdio: "ignore", detached: true },
    );
    openProc.unref();

    // Wait for the debug socket AND the pid file in parallel (both can take
    // several seconds on a cold start; 15 s covers slow machines).
    const LAUNCH_TIMEOUT_MS = 15_000;
    const [socketUp] = await Promise.all([
      awaitResponsiveSocket(port, LAUNCH_TIMEOUT_MS),
      // Poll for the pid file alongside the socket wait.
      (async () => {
        const deadline = Date.now() + LAUNCH_TIMEOUT_MS;
        while (Date.now() < deadline && _macPid === null) {
          await sleep(200);
          const p = readMacPid(port);
          if (p !== null && p > 0) _macPid = p;
        }
      })(),
    ]);

    // One final pid-file read in case the file appeared right as the loop ended.
    if (_macPid === null) {
      const p = readMacPid(port);
      if (p !== null && p > 0) _macPid = p;
    }

    const pid = _macPid;
    if (!socketUp) {
      return {
        pid,
        socketUp: false,
        msg: `DOSBox launched (pid ${pid}) but debug socket did not open on :${port} within ${LAUNCH_TIMEOUT_MS / 1000} s — check the conf and binary.`,
      };
    }
    return {
      pid,
      socketUp: true,
      msg: `Launched via open (pid ${pid}), debug socket up on :${port}`,
    };
  }

  // Non-macOS or headless: direct spawn.
  const spawnEnv: NodeJS.ProcessEnv = {
    ...process.env,
    DOSBOX_DEBUG_PORT: String(port),
  };
  if (opts.headless) {
    spawnEnv["SDL_VIDEODRIVER"] = "dummy";
  }

  _proc = spawn(bin, ["-conf", conf, ...extra], {
    cwd: PROJECT_ROOT,
    env: spawnEnv,
    stdio: "ignore",
    detached: false,
  });

  // Capture pid immediately — _proc may become null before the await below
  // if DOSBox exits very quickly (e.g. short autoexec that terminates fast).
  const pid = _proc.pid ?? null;

  _proc.on("exit", () => {
    _proc = null;
  });

  const LAUNCH_TIMEOUT_MS = 15_000;
  const socketUp = await awaitResponsiveSocket(port, LAUNCH_TIMEOUT_MS);
  if (!socketUp) {
    return {
      pid,
      socketUp: false,
      msg: `DOSBox launched (pid ${pid}) but debug socket did not open on :${port} within ${LAUNCH_TIMEOUT_MS / 1000} s — check the conf and binary.`,
    };
  }
  return {
    pid,
    socketUp: true,
    msg: `Launched (pid ${pid}), debug socket up on :${port}`,
  };
}

/** Kill any process currently listening on `port` with the given signal. */
function killPort(port: number, sig: NodeJS.Signals): void {
  try {
    const lsof = spawnSync("lsof", ["-ti", `:${port}`], { encoding: "utf8" });
    if (lsof.stdout) {
      const pids = lsof.stdout.trim().split("\n")
        .map(Number).filter((n) => !isNaN(n) && n > 0);
      for (const p of pids) {
        try { process.kill(p, sig); } catch { /* already gone */ }
      }
    }
  } catch { /* lsof unavailable */ }
}

export async function stop(): Promise<string> {
  const port = _port;
  const pid = trackedPid(port);
  const wasMacOpen = _macPid !== null;   // capture before clearing
  disconnectSession(port);

  // Clear tracking state so isAlive() returns false immediately.
  _macPid = null;
  if (_proc) {
    try { _proc.kill("SIGTERM"); } catch { /* ignore */ }
    _proc = null;
  }

  if (pid === null) {
    killPort(port, "SIGKILL");
    clearPidFile(port);
    return "DOSBox is not running.";
  }

  // On macOS launched via open(1) / .app wrapper, SIGTERM triggers DOSBox's
  // SDL quit handler which shows a "quit while program running?" dialog.
  // Skip straight to SIGKILL — it bypasses all signal handlers and the dialog
  // never appears.  For direct-spawned processes (non-macOS / headless) try a
  // brief SIGTERM grace period first so DOSBox can flush its config.
  const useSigkillDirectly = wasMacOpen;

  if (!useSigkillDirectly) {
    // Step 1: SIGTERM grace period (non-macOS / headless only).
    try { process.kill(pid, "SIGTERM"); } catch { /* already gone */ }
    for (let i = 0; i < 5; i++) {
      await sleep(100);
      if (!pidAlive(pid)) {
        return `DOSBox stopped (pid ${pid}).`;
      }
    }
  }

  // Step 2: SIGKILL the tracked pid — instant, no dialog.
  try { process.kill(pid, "SIGKILL"); } catch { /* ignore */ }

  // Step 3: also SIGKILL anything else still holding our port
  // (handles orphaned dosbox-x children if the wrapper was the tracked pid).
  killPort(port, "SIGKILL");
  clearPidFile(port);

  // Step 4: wait up to 2 s for the port to be released.
  for (let i = 0; i < 10; i++) {
    await sleep(200);
    const stillUp = await awaitResponsiveSocket(port, 100).catch(() => false);
    if (!stillUp) break;
  }

  return `DOSBox stopped (pid ${pid}).`;
}

export interface StatusResult {
  processRunning: boolean;
  pid: number | null;
  pidAlive: boolean;
  socketPort: number;
  socketUp: boolean;
  socketResponsive: boolean;
}

export async function status(): Promise<StatusResult> {
  const pid = trackedPid(_port);
  const pidIsAlive = pid !== null && pidAlive(pid);
  const socketResponsive = await awaitResponsiveSocket(_port, 1000).catch(() => false);

  if (!pidIsAlive) {
    _macPid = null;
    if (_proc !== null && (_proc.exitCode !== null || _proc.killed)) _proc = null;
    clearPidFile(_port);
  }

  return {
    processRunning: pidIsAlive && socketResponsive,
    pid: pidIsAlive ? pid : null,
    pidAlive: pidIsAlive,
    socketPort: _port,
    socketUp: socketResponsive,
    socketResponsive,
  };
}

// ─── Socket command client ────────────────────────────────────────────────── //

type PendingCommand = {
  id: number;
  cmd: JsonObject;
  timeoutMs: number;
  autoContinueInt3: boolean;
  ignoredStops: JsonObject[];
  resolve: (value: JsonObject) => void;
  reject: (error: Error) => void;
  timer?: NodeJS.Timeout;
};

type StopWaiter = {
  resolve: (value: JsonObject) => void;
  reject: (error: Error) => void;
  timer?: NodeJS.Timeout;
};

function stopIntNumber(event: JsonObject): number | undefined {
  const raw = event["int"];
  if (typeof raw === "number") return raw;
  if (typeof raw !== "string") return undefined;
  const parsed = Number.parseInt(raw, raw.toLowerCase().startsWith("0x") ? 16 : 10);
  return Number.isFinite(parsed) ? parsed : undefined;
}

function isIgnoredInt3Stop(event: JsonObject): boolean {
  return event["event"] === "stopped" &&
    event["reason"] === "interrupt" &&
    stopIntNumber(event) === 3;
}

class DosboxSession {
  private socket: net.Socket | null = null;
  private connecting: Promise<void> | null = null;
  private buffer = "";
  private nextId = 1;
  private queue: PendingCommand[] = [];
  private pending: PendingCommand | null = null;
  private stopWaiters: StopWaiter[] = [];
  private lastStop: JsonObject | undefined;
  private connectedState: string | undefined;

  constructor(private readonly port: number) {}

  disconnect(): void {
    this.rejectPending(new Error("DOSBox session disconnected"));
    this.rejectStopWaiters(new Error("DOSBox session disconnected"));
    if (this.socket !== null) {
      this.socket.destroy();
      this.socket = null;
    }
    this.connecting = null;
    this.buffer = "";
    this.connectedState = undefined;
  }

  async sendCommand(cmd: JsonObject, timeoutMs = 8000): Promise<JsonObject> {
    await this.ensureConnected(timeoutMs);
    return new Promise<JsonObject>((resolve, reject) => {
      const id = this.nextId++;
      const autoContinueInt3 = cmd["autoContinueInt3"] === true;
      const socketCmd: JsonObject = { ...cmd, id };
      delete socketCmd["autoContinueInt3"];
      const pending: PendingCommand = {
        id,
        cmd: socketCmd,
        timeoutMs,
        autoContinueInt3,
        ignoredStops: [],
        resolve,
        reject,
      };
      pending.timer = setTimeout(() => {
        if (this.pending?.id === id) {
          this.pending = null;
          this.processQueue();
        } else {
          this.queue = this.queue.filter((entry) => entry.id !== id);
        }
        reject(new Error(`Timeout after ${timeoutMs}ms for: ${JSON.stringify(cmd)}`));
      }, timeoutMs);
      this.queue.push(pending);
      this.processQueue();
    });
  }

  async continueAndWait(timeoutMs = 30_000): Promise<JsonObject> {
    this.lastStop = undefined;
    await this.sendCommand({ cmd: "continue" }, timeoutMs);
    return this.waitForStop(timeoutMs);
  }

  async waitForStop(timeoutMs = 30_000): Promise<JsonObject> {
    await this.ensureConnected(timeoutMs);
    if (this.lastStop !== undefined) return this.lastStop;

    return new Promise<JsonObject>((resolve, reject) => {
      const waiter: StopWaiter = { resolve, reject };
      waiter.timer = setTimeout(() => {
        this.stopWaiters = this.stopWaiters.filter((entry) => entry !== waiter);
        this.sendCommand({ cmd: "last_stop" }, 1000)
          .then((resp) => {
            if ((resp as { event?: string }).event === "stopped") resolve(resp);
            else reject(new Error(`waitForStop timed out after ${timeoutMs}ms`));
          })
          .catch(() => reject(new Error(`waitForStop timed out after ${timeoutMs}ms`)));
      }, timeoutMs);
      this.stopWaiters.push(waiter);
    });
  }

  private async ensureConnected(timeoutMs: number): Promise<void> {
    if (this.socket !== null && !this.socket.destroyed) return;
    if (this.connecting !== null) return this.connecting;

    this.connecting = new Promise<void>((resolve, reject) => {
      const socket = new net.Socket();
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        socket.destroy();
        this.connecting = null;
        reject(new Error(`Socket connect timeout after ${timeoutMs}ms`));
      }, timeoutMs);

      socket.setEncoding("utf8");
      socket.on("connect", () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.socket = socket;
        this.connecting = null;
        resolve();
      });
      socket.on("data", (data: string) => this.handleData(data));
      socket.on("error", (err) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          this.connecting = null;
          reject(err);
          return;
        }
        this.handleDisconnect(err);
      });
      socket.on("close", () => this.handleDisconnect(new Error("DOSBox socket closed")));
      socket.connect(this.port, "127.0.0.1");
    });

    return this.connecting;
  }

  private handleData(data: string): void {
    this.buffer += data;
    for (;;) {
      const nl = this.buffer.indexOf("\n");
      if (nl === -1) break;
      const line = this.buffer.slice(0, nl).trim();
      this.buffer = this.buffer.slice(nl + 1);
      if (!line) continue;
      let msg: JsonObject;
      try {
        msg = JSON.parse(line) as JsonObject;
      } catch {
        continue;
      }
      this.handleMessage(msg);
    }
  }

  private handleMessage(msg: JsonObject): void {
    const event = (msg as { event?: string }).event;
    if (event === "connected") {
      this.connectedState = typeof msg["state"] === "string" ? msg["state"] : undefined;
      return;
    }
    if (event === "stopped") {
      const id = typeof msg["id"] === "number" ? msg["id"] : undefined;
      if (this.pending !== null && (id === this.pending.id || id === undefined) && this.shouldAutoContinueIgnoredStop(msg, this.pending)) {
        this.pending.ignoredStops.push(msg);
        this.socket?.write(JSON.stringify({ cmd: "continue", id: -this.pending.id }) + "\n");
        return;
      }

      this.lastStop = msg;
      this.resolveStopWaiters(msg);
      if (isDefaultCriticalStopEvent(msg)) {
        for (const cb of asyncEventListeners) {
          try { cb(msg); } catch { /* ignore listener errors */ }
        }
      }
      if (this.pending !== null && (id === this.pending.id || id === undefined)) {
        this.resolvePending(msg);
      }
      return;
    }
    if (event === "process_exit") {
      // Async event — deliver to registered listeners; do NOT resolve pending commands.
      for (const cb of asyncEventListeners) {
        try { cb(msg); } catch { /* ignore listener errors */ }
      }
      return;
    }

    const id = typeof msg["id"] === "number" ? msg["id"] : undefined;
    if (this.pending !== null && (id === this.pending.id || id === undefined)) {
      this.resolvePending(msg);
    }
  }

  private processQueue(): void {
    if (this.pending !== null || this.socket === null || this.socket.destroyed) return;
    const next = this.queue.shift();
    if (next === undefined) return;
    this.pending = next;
    this.socket.write(JSON.stringify(next.cmd) + "\n");
  }

  private resolvePending(resp: JsonObject): void {
    const pending = this.pending;
    if (pending === null) return;
    this.pending = null;
    if (pending.timer !== undefined) clearTimeout(pending.timer);
    const resolved = pending.ignoredStops.length === 0
      ? resp
      : {
        ...resp,
        autoContinued: true,
        ignoredStopCount: pending.ignoredStops.length,
        ignoredStops: pending.ignoredStops,
      };
    pending.resolve(resolved);
    this.processQueue();
  }

  private rejectPending(error: Error): void {
    if (this.pending !== null) {
      if (this.pending.timer !== undefined) clearTimeout(this.pending.timer);
      this.pending.reject(error);
      this.pending = null;
    }
    for (const entry of this.queue) {
      if (entry.timer !== undefined) clearTimeout(entry.timer);
      entry.reject(error);
    }
    this.queue = [];
  }

  private resolveStopWaiters(resp: JsonObject): void {
    const waiters = this.stopWaiters;
    this.stopWaiters = [];
    for (const waiter of waiters) {
      if (waiter.timer !== undefined) clearTimeout(waiter.timer);
      waiter.resolve(resp);
    }
  }

  private rejectStopWaiters(error: Error): void {
    const waiters = this.stopWaiters;
    this.stopWaiters = [];
    for (const waiter of waiters) {
      if (waiter.timer !== undefined) clearTimeout(waiter.timer);
      waiter.reject(error);
    }
  }

  private handleDisconnect(error: Error): void {
    if (this.socket !== null) {
      this.socket.destroy();
      this.socket = null;
    }
    this.connecting = null;
    this.buffer = "";
    this.connectedState = undefined;
    for (const cb of asyncEventListeners) {
      try {
        cb({
          event: "socket_disconnect",
          msg: error.message,
          port: this.port,
        });
      } catch { /* ignore listener errors */ }
    }
    this.rejectPending(error);
    this.rejectStopWaiters(error);
  }

  private shouldAutoContinueIgnoredStop(msg: JsonObject, pending: PendingCommand): boolean {
    if (!pending.autoContinueInt3) return false;
    if (pending.cmd["cmd"] !== "dos_cmd") return false;
    if (!isIgnoredInt3Stop(msg)) return false;
    return pending.ignoredStops.length < 32;
  }
}

const sessions = new Map<number, DosboxSession>();

// ─── Async event listeners ────────────────────────────────────────────────── //

type AsyncEventCallback = (event: JsonObject) => void;

const asyncEventListeners: AsyncEventCallback[] = [];

/** Register a callback invoked when an async event (e.g. process_exit) arrives. */
export function addAsyncEventListener(callback: AsyncEventCallback): void {
  asyncEventListeners.push(callback);
}

/** Remove a previously registered async event listener. */
export function removeAsyncEventListener(callback: AsyncEventCallback): void {
  const idx = asyncEventListeners.indexOf(callback);
  if (idx >= 0) asyncEventListeners.splice(idx, 1);
}

function sessionFor(port: number): DosboxSession {
  let session = sessions.get(port);
  if (session === undefined) {
    session = new DosboxSession(port);
    sessions.set(port, session);
  }
  return session;
}

function disconnectSession(port: number): void {
  sessions.get(port)?.disconnect();
  sessions.delete(port);
}

export function sendCommand(
  cmd: JsonObject,
  port: number = _port,
  timeoutMs = 8000,
): Promise<JsonObject> {
  return sessionFor(port).sendCommand(cmd, timeoutMs);
}

export function continueAndWait(
  port: number = _port,
  timeoutMs = 30_000,
): Promise<JsonObject> {
  return sessionFor(port).continueAndWait(timeoutMs);
}

export function waitForStop(
  port: number = _port,
  timeoutMs = 30_000,
): Promise<JsonObject> {
  return sessionFor(port).waitForStop(timeoutMs);
}
