import assert from "node:assert/strict";
import test from "node:test";

import {
  debuggerStopBlockDecision,
  isDefaultCriticalStopEvent,
  shellIdleResumeDecision,
} from "./stopState.js";

test("debuggerStopBlockDecision blocks frozen breakpoint stops", () => {
  const decision = debuggerStopBlockDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: true,
      debuggerFrozen: true,
      hasLastStop: true,
    },
    lastStop: {
      event: "stopped",
      reason: "breakpoint",
      addr: "1234:00000010",
    },
  });

  assert.equal(decision.blocked, true);
  assert.equal(decision.reason, "guest stopped at breakpoint");
});

test("debuggerStopBlockDecision blocks pending critical notifications", () => {
  const decision = debuggerStopBlockDecision({
    status: {
      status: "ok",
      state: "running",
      shellReady: true,
      socketFrozen: false,
    },
    notifications: {
      unreadCount: 1,
      unread: [{ event: "stopped", reason: "watchpoint" }],
    },
  });

  assert.equal(decision.blocked, true);
  assert.equal(decision.reason, "pending critical debugger notification");
});

test("debuggerStopBlockDecision allows pre-shell stopped startup states", () => {
  const decision = debuggerStopBlockDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: false,
      socketFrozen: false,
      debuggerFrozen: true,
      hasLastStop: false,
    },
  });

  assert.equal(decision.blocked, false);
});

test("debuggerStopBlockDecision allows override for legacy auto-continue flows", () => {
  const decision = debuggerStopBlockDecision({
    respectFrozenStop: false,
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: true,
    },
    lastStop: {
      event: "stopped",
      reason: "linear_exec_breakpoint",
    },
  });

  assert.equal(decision.blocked, false);
});

test("shellIdleResumeDecision resumes shell-idle debugger stops without a latched stop", () => {
  const decision = shellIdleResumeDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: false,
      socketFreezeRequested: false,
      debuggerFrozen: true,
      hasLastStop: false,
    },
    notifications: {
      unreadCount: 0,
      unread: [],
    },
  });

  assert.equal(decision.blocked, false);
  assert.equal(decision.shouldResume, true);
});

test("shellIdleResumeDecision blocks intentional entry stops", () => {
  const decision = shellIdleResumeDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: true,
      debuggerFrozen: true,
      hasLastStop: true,
    },
    lastStop: {
      event: "stopped",
      reason: "program_entry",
      addr: "1234:00000000",
    },
    notifications: {
      unreadCount: 0,
      unread: [],
    },
  });

  assert.equal(decision.blocked, true);
  assert.equal(decision.shouldResume, false);
  assert.equal(decision.reason, "guest stopped at program_entry");
});

test("shellIdleResumeDecision resumes through stale process exit latch", () => {
  const decision = shellIdleResumeDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: false,
      socketFreezeRequested: false,
      debuggerFrozen: true,
      hasLastStop: true,
      breakOnExit: false,
    },
    lastStop: {
      event: "process_exit",
      exit_code: 0,
      tsr: true,
      abnormal: false,
    },
    notifications: {
      unreadCount: 0,
      unread: [],
    },
  });

  assert.equal(decision.blocked, false);
  assert.equal(decision.shouldResume, true);
  assert.equal(decision.reason, "shell is debugger-stopped with only a non-critical latched event");
});

test("debuggerStopBlockDecision blocks break-on-exit process exits", () => {
  const decision = debuggerStopBlockDecision({
    status: {
      status: "ok",
      state: "stopped",
      shellReady: true,
      socketFrozen: false,
      debuggerFrozen: true,
      hasLastStop: true,
      breakOnExit: true,
    },
    lastStop: {
      event: "process_exit",
      exit_code: 0,
      tsr: false,
      abnormal: false,
    },
    notifications: {
      unreadCount: 0,
      unread: [],
    },
  });

  assert.equal(decision.blocked, true);
  assert.equal(decision.reason, "guest stopped at process_exit");
});

test("isDefaultCriticalStopEvent ignores plain INT 3 by default", () => {
  assert.equal(isDefaultCriticalStopEvent({
    event: "stopped",
    reason: "interrupt",
    int: 3,
  }), false);
});

test("isDefaultCriticalStopEvent can opt into interrupt stops", () => {
  assert.equal(isDefaultCriticalStopEvent({
    event: "stopped",
    reason: "interrupt",
    int: 3,
  }, { breakOnInterrupts: true }), true);
});
