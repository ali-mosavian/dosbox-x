/*
 * debug_profile.h - where a program's executed instructions and memory
 * operands went, by function and source line, self and inclusive.
 *
 * The core calls DEBUG_ProfileEnter only when execution leaves the address
 * range it is already attributing to, and DEBUG_ProfileTransfer on every
 * control transfer: nothing per instruction but one range test. Names are
 * resolved when the report is made, from the symbol store.
 */

#ifndef DOSBOX_DEBUG_PROFILE_H
#define DOSBOX_DEBUG_PROFILE_H

#include "dosbox.h"

#if C_DEBUG

#include <stdint.h>

#include <string>

/* While profiling, the core compares each instruction's address against this
 * range, and calls DEBUG_ProfileEnter when it falls outside. */
extern bool debug_profiling;
extern uint32_t debug_profile_begin,debug_profile_length;

/* Begins counting from now; what an earlier run counted is dropped. */
void DEBUG_ProfileStart(void);

/* Stops, keeping the counts for the report. */
void DEBUG_ProfileStop(void);

/* The symbols or the program changed: the next instruction is attributed afresh. */
void DEBUG_ProfileInvalidate(void);

/* Execution is at `linear`, outside the range being attributed to. */
void DEBUG_ProfileEnter(uint32_t linear);

/* Execution jumped from `from` (SP `fromSp` before it, in the code segment at `fromBase`) to `to` (in the one at
 * `toBase`): a call pushes a frame, a return pops one. */
void DEBUG_ProfileTransfer(uint32_t from,uint32_t fromSp,uint32_t fromBase,uint32_t to,uint32_t toBase);

/* The counts so far, as the JSON members of a "profile" object. */
std::string DEBUG_ProfileJson(void);

#endif

#endif
