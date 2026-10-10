/*
 * vtime: guest time that follows what the guest executes, not the host.
 *
 * DOSBox's clocks (PIT, BIOS tick, RTC) are driven by the cycles the CPU runs, so with a fixed cycle count per
 * emulated millisecond and no host throttle they are the same on every run. A program that only waits for a clock,
 * though, pays for every emulated millisecond in cycles: a 4 second delay at 25000 cycles per millisecond is 100
 * million instructions. With idle skipping, a program seen reading a clock again and again with almost nothing
 * between the reads has the rest of the millisecond skipped: the clock moves on as if it had run it.
 */

#ifndef DOSBOX_VTIME_H
#define DOSBOX_VTIME_H

#include <stdint.h>

/* True while a program that polls a clock has its idle time skipped. */
extern bool vtime_skip_idle;

/* The guest read a clock: BIOS or DOS time, the PIT, the RTC. */
void VTIME_ClockRead(void);

/* True when the configuration asks for virtual time: whatever waits on the host's clock, as the BIOS logo does,
 * would make the guest's timeline depend on how fast the host runs. */
bool VTIME_Configured(void);

/* The guest's clock at the start: the configured time with virtual time on, `host_now` otherwise. */
int64_t VTIME_StartTime(int64_t host_now);

/* Reads the [cpu] settings once the tick loop starts: with "virtual time" the rate, throttle and clock are set. */
void VTIME_ConfigInit(void);

/* Turns it on: a fixed cycles-per-millisecond, no host throttle, no host clock behind the CMOS, and idle skipping
 * when `skip_idle`. `epoch`, when not 0, is the time of day the guest starts from. */
void VTIME_Enable(uint32_t cycles_per_ms, bool skip_idle, int64_t epoch);

/* Back to the host's clocks; the cycle count stays as it was set. */
void VTIME_Disable(void);

/* Emulated milliseconds skipped so far, and the clock reads that caused it. */
uint64_t VTIME_SkippedMs(void);
uint64_t VTIME_ClockReads(void);

#endif
