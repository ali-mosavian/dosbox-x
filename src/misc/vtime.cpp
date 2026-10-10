/*
 * vtime.cpp - see include/vtime.h.
 */

#include "dosbox.h"
#include "vtime.h"
#include "cpu.h"
#include "pic.h"
#include "mem.h"
#include "bios.h"

#include "control.h"
#include "setup.h"

#include <time.h>

extern bool ticksLocked;
extern bool sync_time;
void cmos_sync_time(time_t t);

bool vtime_skip_idle = false;

namespace {

/* Reads of a clock this close together, in cycles, are a program waiting on it. */
const int64_t NEAR_CYCLES = 2000;
/* How many in a row. */
const unsigned int STREAK = 16;
/* A program that asks for the time and gets the same answer again and again is waiting for it to change. The
 * BIOS and DOS clocks change every 55 ms, so a wait costs this many reads per emulated millisecond. */
const unsigned int REPEATS = 3;

uint64_t skipped = 0;
uint64_t reads = 0;
int64_t last_reading = -1;
unsigned int streak = 0;
unsigned int repeats = 0;
uint32_t last_ticks = 0xffffffffu;
bool turned_on = false;

/* Cycles since the machine started, by the PIC's own count. */
int64_t cycles_now() {
    return (int64_t)PIC_Ticks * (int64_t)CPU_CycleMax + (int64_t)PIC_TickIndexND();
}

} // namespace

void VTIME_ClockRead(void) {
    reads++;
    if (!vtime_skip_idle) return;

    const int64_t now = cycles_now();
    if (last_reading >= 0 && now - last_reading < NEAR_CYCLES) streak++;
    else streak = 0;
    last_reading = now;

    const uint32_t ticks = mem_readd(BIOS_TIMER);
    if (ticks == last_ticks) repeats++;
    else repeats = 0;
    last_ticks = ticks;
    if (streak < STREAK && repeats < REPEATS) return;

    /* Give the rest of this millisecond to the wait: the PIC then moves to the next one. */
    streak = 0;
    repeats = 0;
    skipped++;
    CPU_Cycles = 0;
    CPU_CycleLeft = 0;
}

bool VTIME_Configured(void) {
    const Section_prop *section = static_cast<Section_prop *>(control->GetSection("cpu"));
    return section != NULL && section->Get_bool("virtual time");
}

int64_t VTIME_StartTime(int64_t host_now) {
    const Section_prop *section = static_cast<Section_prop *>(control->GetSection("cpu"));
    if (section == NULL || !section->Get_bool("virtual time")) return host_now;
    return (int64_t)section->Get_int("virtual time start");
}

void VTIME_ConfigInit(void) {
    const Section_prop *section = static_cast<Section_prop *>(control->GetSection("cpu"));
    if (section == NULL || !section->Get_bool("virtual time")) return;
    VTIME_Enable((uint32_t)section->Get_int("virtual time rate"),section->Get_bool("virtual idle skip"),0);
}

void VTIME_Enable(uint32_t cycles_per_ms, bool skip_idle, int64_t epoch) {
    CPU_CycleAutoAdjust = false;
    CPU_CyclePercUsed = 100;
    CPU_CycleMax = (cpu_cycles_count_t)(cycles_per_ms ? cycles_per_ms : 25000);
    ticksLocked = true;
    sync_time = false;
    vtime_skip_idle = skip_idle;
    skipped = 0;
    last_reading = -1;
    streak = 0;
    repeats = 0;
    last_ticks = 0xffffffffu;
    turned_on = true;

    if (epoch != 0) {
        cmos_sync_time((time_t)epoch);
        /* The BIOS counts 18.2065 ticks a second since midnight, in the zone cmos_sync_time used. */
        const time_t at = (time_t)epoch;
        const struct tm local = *localtime(&at);
        const double seconds = local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec;
        mem_writed(BIOS_TIMER, (uint32_t)(seconds * 18.2065));
    }
}

void VTIME_Disable(void) {
    vtime_skip_idle = false;
    if (turned_on) ticksLocked = false;
    turned_on = false;
}

uint64_t VTIME_SkippedMs(void) { return skipped; }
uint64_t VTIME_ClockReads(void) { return reads; }
