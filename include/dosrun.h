/*
 * dosrun: serve jobs from one booted machine.
 *
 * With DOSRUN_FD set, the shell's first wait for console input becomes a job
 * loop: read a job from stdin, fork, and let the child run it from the booted
 * state. The child streams events as JSON lines to DOSRUN_FD and exits when
 * the job ends. See src/misc/dosrun.cpp for the job and event formats.
 */

#ifndef DOSBOX_DOSRUN_H
#define DOSBOX_DOSRUN_H

#include <stdint.h>

/* The shell wants a console line. True when dosrun supplied one. */
bool DOSRUN_ShellInput(char *line, unsigned int size);

/* True in a job's child. It shares the server's host state, so it must not
 * touch the host front end: macOS kills a forked child that enters a run loop. */
bool DOSRUN_Child(void);

/* One emulated millisecond passed. */
void DOSRUN_Tick(void);

/* INT10_ScrollWindow is about to move or clear rows [rul,rlr] x [cul,clr];
 * nlines < 0 scrolls up, 0 clears. */
void DOSRUN_BeforeScroll(uint8_t rul, uint8_t cul, uint8_t rlr, uint8_t clr, int8_t nlines, uint8_t page);

/* True in a job's child: the core then reports where execution lands. */
extern bool dosrun_watch;

/* Execution arrived at CS:linear, after a transfer or on entering a page. */
void DOSRUN_Executes(uint32_t cs, uint32_t linear);

/* The CPU raised exception `which`. */
void DOSRUN_Exception(uint8_t which);

/* HLT is about to execute. */
void DOSRUN_Halt(void);

#endif
