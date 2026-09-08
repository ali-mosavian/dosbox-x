import type { JsonObject } from "./dosbox.js";

export type StopBlockDecision = {
  blocked: boolean;
  reason?: string;
  status?: JsonObject;
  lastStop?: JsonObject;
  notifications?: JsonObject;
};

export type ShellIdleResumeDecision = StopBlockDecision & {
  shouldResume: boolean;
};

const intentionalStopReasons = new Set([
  "breakpoint",
  "exception",
  "linear_exec_breakpoint",
  "process_exit",
  "program_entry",
  "step",
  "watchpoint",
]);

function pendingNotificationCount(notifications: JsonObject | undefined): number {
  const count = notifications?.["unreadCount"];
  if (typeof count === "number") return count;
  const unread = notifications?.["unread"];
  return Array.isArray(unread) ? unread.length : 0;
}

function stopReason(lastStop: JsonObject | undefined): string | undefined {
  if (lastStop === undefined) return undefined;
  const event = lastStop?.["event"];
  if (event === "process_exit") return "process_exit";
  if (event !== "stopped") return undefined;
  return typeof lastStop["reason"] === "string" ? lastStop["reason"] : "stopped";
}

function stopInterruptNumber(lastStop: JsonObject | undefined): number | undefined {
  const raw = lastStop?.["int"];
  if (typeof raw === "number") return raw;
  if (typeof raw !== "string") return undefined;
  const parsed = Number.parseInt(raw, raw.toLowerCase().startsWith("0x") ? 16 : 10);
  return Number.isFinite(parsed) ? parsed : undefined;
}

export function isDefaultCriticalStopEvent(lastStop: JsonObject | undefined, opts: {
  breakOnInterrupts?: boolean;
  breakOnExit?: boolean;
} = {}): boolean {
  const reason = stopReason(lastStop);
  if (reason === undefined) return false;
  if (reason === "process_exit") {
    return opts.breakOnExit === true || lastStop?.["abnormal"] === true;
  }
  if (reason === "interrupt") {
    if (opts.breakOnInterrupts === true) return true;
    return stopInterruptNumber(lastStop) !== 3;
  }
  return intentionalStopReasons.has(reason);
}

export function debuggerStopBlockDecision(args: {
  status?: JsonObject;
  lastStop?: JsonObject;
  notifications?: JsonObject;
  respectFrozenStop?: boolean;
  breakOnInterrupts?: boolean;
}): StopBlockDecision {
  const { status, lastStop, notifications, respectFrozenStop = true, breakOnInterrupts = false } = args;
  if (!respectFrozenStop) return { blocked: false };

  if (pendingNotificationCount(notifications) > 0) {
    return {
      blocked: true,
      reason: "pending critical debugger notification",
      status,
      lastStop,
      notifications,
    };
  }

  const state = status?.["state"];
  const socketFrozen = status?.["socketFrozen"] === true;
  const freezeRequested = status?.["socketFreezeRequested"] === true;
  const reason = stopReason(lastStop);

  if (reason !== undefined && (isDefaultCriticalStopEvent(lastStop, { breakOnInterrupts, breakOnExit: status?.["breakOnExit"] === true }) || socketFrozen || freezeRequested)) {
    return {
      blocked: true,
      reason: `guest stopped at ${reason}`,
      status,
      lastStop,
      notifications,
    };
  }

  if (socketFrozen || freezeRequested) {
    return {
      blocked: true,
      reason: "guest is frozen in the debug socket",
      status,
      lastStop,
      notifications,
    };
  }

  return { blocked: false };
}

export function shellIdleResumeDecision(args: {
  status?: JsonObject;
  lastStop?: JsonObject;
  notifications?: JsonObject;
  respectFrozenStop?: boolean;
  breakOnInterrupts?: boolean;
}): ShellIdleResumeDecision {
  const { status, lastStop, notifications, respectFrozenStop = true, breakOnInterrupts = false } = args;
  if (!respectFrozenStop) return { blocked: false, shouldResume: false };

  const block = debuggerStopBlockDecision({
    status,
    lastStop,
    notifications,
    respectFrozenStop,
    breakOnInterrupts,
  });
  if (block.blocked) return { ...block, shouldResume: false };

  const shellReady = status?.["shellReady"] === true;
  const state = status?.["state"];
  const socketFrozen = status?.["socketFrozen"] === true;
  const freezeRequested = status?.["socketFreezeRequested"] === true;
  const debuggerFrozen = status?.["debuggerFrozen"] === true;
  const hasLastStop = status?.["hasLastStop"] === true || lastStop !== undefined;

  if (shellReady && state === "stopped" && debuggerFrozen && !socketFrozen && !freezeRequested) {
    return {
      blocked: false,
      shouldResume: true,
      reason: hasLastStop
        ? "shell is debugger-stopped with only a non-critical latched event"
        : "shell is debugger-stopped without a latched debug event",
      status,
      lastStop,
      notifications,
    };
  }

  return { blocked: false, shouldResume: false };
}
