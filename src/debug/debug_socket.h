/*
 * debug_socket.h - Socket-based debug interface for AI automation
 *
 * This provides a TCP socket interface to the DOSBox-X debugger,
 * allowing external programs (like AI assistants) to:
 * - Set/clear breakpoints
 * - Single step
 * - Continue execution
 * - Read/write registers
 * - Read/write memory
 * - Disassemble code
 */

#ifndef DEBUG_SOCKET_H
#define DEBUG_SOCKET_H

#include "dosbox.h"

#if C_DEBUG

// Initialize the debug socket server
// Returns true if started successfully
bool DEBUG_Socket_Init(int port);

// Shutdown the debug socket server
void DEBUG_Socket_Shutdown(void);

// Check for and process socket commands
// Called from DEBUG_Loop
// Returns true if a command was processed
bool DEBUG_Socket_CheckCommands(void);

// Send a notification that a breakpoint was hit
void DEBUG_Socket_NotifyBreakpoint(uint16_t seg, uint32_t off);

// Send a notification that an interrupt breakpoint was hit (INT 3, INT n, etc.)
void DEBUG_Socket_NotifyInterrupt(uint8_t intNum, uint16_t seg, uint32_t off);
/* Non-stopping allocation trace: called on every INT before dispatch,
   logs only the DOS/EMS memory calls. Returns immediately when off. */
void DEBUG_Socket_TraceAlloc(uint8_t intNum);

// Send a notification that execution stopped
void DEBUG_Socket_NotifyStopped(const char* reason);

// Record the most recent CPU exception so stop events can report vector/error.
void DEBUG_Socket_RecordException(uint8_t intNum, uint32_t error);

// Record authoritative DOS EXEC load metadata for JSON socket clients.
void DEBUG_Socket_RecordLoadInfo(const char* program,
                                 bool isCom,
                                 uint16_t pspSeg,
                                 uint16_t loadSeg,
                                 uint16_t entryCS,
                                 uint16_t entryIP,
                                 uint16_t initialSS,
                                 uint16_t initialSP,
                                 uint32_t imageSizeBytes,
                                 uint16_t mzSignature,
                                 uint16_t mzExtraBytes,
                                 uint16_t mzPages,
                                 uint16_t headerParagraphs,
                                 uint16_t relocationCount,
                                 uint16_t relocationTableOffset,
                                 uint16_t initCS,
                                 uint16_t initIP,
                                 uint16_t initSS,
                                 uint16_t initSP,
                                 uint16_t checksum,
                                 uint16_t overlay,
                                 uint16_t minAlloc,
                                 uint16_t maxAlloc);

// Check socket-managed linear execution breakpoints before normal physical BPs.
bool DEBUG_Socket_CheckLinearExecBreakpoint(uint16_t seg, uint32_t off);

// Check DOSBox normal breakpoints using the socket freeze stop model.
bool DEBUG_Socket_CheckNormalBreakpoint(uint16_t seg, uint32_t off);

// Block in place inside the CPU core until the client continues. Transparent,
// Bochs-style stop: no host-stack unwind, no loop swap, no cycle/flag changes.
void DEBUG_Socket_FreezeWait(void);

// Decrement the step-instruction arm counter; returns true when it reaches
// zero (the pre-instruction guard should freeze and emit a "step" stop).
// Returns false if the counter was already zero (no step pending).
bool DEBUG_Socket_DecrStepArm(void);

// Check and emit pending watchpoint stop event; returns true if freeze is needed.
bool DEBUG_Socket_CheckWatchpointFreeze(void);

// First-chance exception filter: returns true if the exception should be caught
// (freeze the CPU before the IDT handler runs). is_nested = true when there is
// already an exception being serviced (double-fault context).
bool DEBUG_Socket_CheckException(uint8_t which, uint32_t error, bool is_nested);

// True when the socket first-chance filter is explicitly armed for this vector.
bool DEBUG_Socket_IsExceptionVectorCaught(uint8_t which);

// Notify the socket of a process exit event (called from DOS_Terminate).
void DEBUG_Socket_NotifyProcessExit(uint16_t pspseg, uint8_t exitcode, bool tsr);

// Branch trace: returns true when trace is enabled (called per-instruction).
bool DEBUG_Socket_TraceIsEnabled(void);

// Branch trace: record a non-sequential transfer (from_cs:from_linear -> to_cs:to_linear).
void DEBUG_Socket_TraceRecordBranch(uint32_t from_cs, uint32_t from_linear,
                                    uint32_t to_cs, uint32_t to_linear);

// Check if socket debugging is active
bool DEBUG_Socket_IsActive(void);

// Get current socket port (0 if not active)
int DEBUG_Socket_GetPort(void);

// Exec breakpoint pending - set by DEBUG_CheckExecuteBreakpoint when bp_on_load triggers
extern bool g_exec_breakpoint_pending;
extern uint16_t g_exec_breakpoint_seg;
extern uint32_t g_exec_breakpoint_off;

#endif // C_DEBUG

#endif // DEBUG_SOCKET_H

