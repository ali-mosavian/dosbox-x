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

// Send a notification that execution stopped
void DEBUG_Socket_NotifyStopped(const char* reason);

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

