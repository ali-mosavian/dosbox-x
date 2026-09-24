/*
 * dosrun: serve jobs from one booted machine. See include/dosrun.h.
 *
 * Job, on stdin, ended by a line holding only ".":
 *   :ms N      stop after N emulated milliseconds
 *   :wall N    stop after N host seconds
 *   :nowatch   do not stop on a crash
 *   anything else is a shell command line, run in order
 *
 * Events, one JSON object per line on DOSRUN_FD:
 *   {"ev":"ready"}                        from the server, once booted
 *   {"ev":"line","text":...}             a line of screen output, as it is finished
 *   {"ev":"screen","rows":[...]}         the screen when the job ends
 *   {"ev":"crash","kind":...,"at":{...}} execution went where code cannot be
 *   {"ev":"state",...}                   registers, source line and last branches,
 *                                        unless the job simply exited
 *   {"ev":"end","reason":...,...}        exit | crash | limit | wall, with emulated ms and CS:EIP
 *   {"ev":"done","status":N,"signal":N}  from the server, once the child is reaped
 *
 * The transcript follows the cursor and the BIOS scroll, not any output call:
 * a program may write video memory directly, but its cursor still moves and
 * its lines still leave through INT10_ScrollWindow. A row is finished when
 * the cursor moves below it or it scrolls off. Only full-width scrolls count;
 * a partial one is drawing inside a window, not line output. The cursor is
 * sampled each emulated millisecond and before every scroll, so a program
 * that moves down and back up within one millisecond without scrolling loses
 * those rows from the transcript; the screen event still has them.
 *
 * A crash is caught where execution lands, checked by the normal core after
 * every transfer and on entering a new page, in real mode:
 *   - memory no one owns: a free DOS block, an MCB header, video memory, or
 *     conventional memory past the end of the chain
 *   - empty memory: four bytes of 00 or of FF, which no code starts with
 *   - a fault (#DE, #BR, #UD, #NM) whose vector is still the one the booted
 *     machine had, so the program never meant to handle it
 *   - HLT with interrupts off, which nothing wakes
 * A program that runs code in memory it never allocated is reported too;
 * :nowatch turns the checks off for it.
 */

#include "dosbox.h"
#include "dosrun.h"
#include "mem.h"
#include "regs.h"
#include "dos_inc.h"
#include "ints/int10.h"
#include "cpu.h"
#include "debug/debug_socket.h"
#include "debug/debug_symstore.h"

#include <deque>
#include <string>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern bool ticksLocked;
Bitu FillFlags(void);
void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result);

namespace {

int out_fd = -2; /* -2: not yet looked up, -1: dosrun off */
bool child = false;
std::deque<std::string> lines;
uint64_t limit_ms = 0, ran_ms = 0;
volatile sig_atomic_t wall_expired = 0;
int upto = 0; /* rows above this one are in the transcript */
bool watch = true;
uint32_t booted_ivt[8];
uint32_t owned_lo = 1, owned_hi = 0; /* the last owned block found, [lo, hi) */

bool active() {
    if (out_fd == -2) {
        const char *fd = getenv("DOSRUN_FD");
        out_fd = fd ? atoi(fd) : -1;
    }
    return out_fd >= 0;
}

void emit(const std::string &json) {
    std::string line = json + "\n";
    for (size_t done = 0; done < line.size();) {
        ssize_t n = write(out_fd, line.data() + done, line.size() - done);
        if (n <= 0) _exit(3); /* the reader is gone */
        done += (size_t)n;
    }
}

/* Screen bytes are CP437; each one goes out as the code point of that byte. */
std::string quoted(const std::string &s) {
    std::string q = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { q += '\\'; q += (char)c; }
        else if (c >= 0x20 && c < 0x7f) q += (char)c;
        else { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); q += u; }
    }
    return q + "\"";
}

uint8_t page() { return real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE); }
int rows() { return real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1; }
int cols() { return real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS); }
int cursor_row() { return real_readb(BIOSMEM_SEG, BIOSMEM_CURSOR_POS + page() * 2 + 1); }

std::string row_text(int row) {
    std::string text;
    for (int col = 0; col < cols(); col++) {
        uint16_t cell = 0;
        ReadCharAttr((uint16_t)col, (uint16_t)row, page(), &cell);
        uint8_t c = (uint8_t)(cell & 0xff);
        text += (char)(c ? c : ' ');
    }
    return text.erase(text.find_last_not_of(' ') + 1);
}

void emit_row(int row, bool blank_too) {
    std::string text = row_text(row);
    if (blank_too || !text.empty()) emit("{\"ev\":\"line\",\"text\":" + quoted(text) + "}");
}

void follow_cursor() {
    int cur = cursor_row();
    for (int row = upto; row < cur; row++) emit_row(row, true);
    upto = cur;
}

std::string hex(uint32_t value) {
    char b[16];
    snprintf(b, sizeof b, "\"%X\"", value);
    return b;
}

/* Where CS:linear is: address, nearest symbol and source line. */
std::string place(uint32_t cs, uint32_t linear) {
    char at[32];
    if (cpu.pmode) snprintf(at, sizeof at, "%04X:%08X", cs, linear);
    else snprintf(at, sizeof at, "%04X:%04X", cs, (linear - (cs << 4)) & 0xFFFFu);
    std::string json = "{\"at\":" + quoted(at) + ",\"linear\":" + hex(linear);
    std::string symbol = DEBUG_Symbols().Describe(linear);
    if (!symbol.empty()) json += ",\"symbol\":" + quoted(symbol);
    if (const DebugSourceLine *line = DEBUG_Symbols().LineAt(linear))
        json += ",\"file\":" + quoted(line->file) + ",\"line\":" + std::to_string(line->line);
    return json + "}";
}

std::string state() {
    FillFlags();
    std::string json = "{\"ev\":\"state\",\"regs\":{";
    const std::pair<const char *, uint32_t> regs[] = {
        {"eax", reg_eax}, {"ebx", reg_ebx}, {"ecx", reg_ecx}, {"edx", reg_edx}, {"esi", reg_esi},
        {"edi", reg_edi}, {"ebp", reg_ebp}, {"esp", reg_esp}, {"eip", reg_eip}, {"eflags", (uint32_t)reg_flags},
        {"cs", SegValue(cs)}, {"ds", SegValue(ds)}, {"es", SegValue(es)}, {"ss", SegValue(ss)},
        {"fs", SegValue(fs)}, {"gs", SegValue(gs)}};
    for (const auto &reg : regs) json += (reg.first == regs[0].first ? "\"" : ",\"") + std::string(reg.first) + "\":" + hex(reg.second);
    json += "},\"where\":" + place(SegValue(cs), SegPhys(cs) + reg_eip) + ",\"branches\":[";
    bool first = true;
    for (const BranchEntry &b : DEBUG_Socket_TraceRecent(16)) {
        json += (first ? "" : ",") + std::string("{\"from\":") + place(b.from_cs, b.from_linear) +
                ",\"to\":" + place(b.to_cs, b.to_linear) + "}";
        first = false;
    }
    return json + "]}";
}

[[noreturn]] void finish(const char *reason);

[[noreturn]] void crash(const char *kind, uint32_t cs, uint32_t linear) {
    emit("{\"ev\":\"crash\",\"kind\":" + quoted(kind) + ",\"at\":" + place(cs, linear) + "}");
    finish("crash");
}

/* The DOS chain's verdict on a paragraph: 1 owned, 0 free or a header, -1 past its end. */
int chain_owner(uint16_t first, uint16_t para) {
    for (uint32_t m = first, blocks = 0; blocks < 4096; blocks++) {
        DOS_MCB mcb((uint16_t)m);
        uint8_t type = mcb.GetType();
        if (type != 'M' && type != 'Z') return 1; /* a broken chain is DOS's report, not this one */
        uint32_t start = m + 1, end = start + mcb.GetSize();
        if (para == m) return 0;
        if (para >= start && para < end) {
            if (mcb.GetPSPSeg() == 0) return 0;
            owned_lo = start << 4;
            owned_hi = end << 4;
            return 1;
        }
        if (type == 'Z' || end > 0xFFFF) return -1;
        m = end;
    }
    return 1;
}

bool owned(uint32_t linear) {
    if (linear >= 0xA0000 && linear < 0xC0000) return false; /* video memory is never code */
    if (linear >= 0xF0000 || (linear >= owned_lo && linear < owned_hi)) return true;
    uint16_t para = (uint16_t)(linear >> 4);
    if (para < dos.firstMCB) return true; /* the IVT, BIOS data and the DOS kernel */
    int conventional = chain_owner(dos.firstMCB, para);
    if (conventional >= 0) return conventional == 1;
    if (linear < 0xA0000) return false; /* conventional memory past the chain */
    uint16_t umb = dos_infoblock.GetStartOfUMBChain();
    /* upper memory outside the UMBs is ROM, the EMS frame or DOS's own */
    return (umb != 0xFFFF ? chain_owner(umb, para) : -1) != 0;
}

[[noreturn]] void finish(const char *reason) {
    follow_cursor();
    emit_row(upto, false);
    std::string screen = "{\"ev\":\"screen\",\"rows\":[";
    for (int row = 0; row < rows(); row++) screen += (row ? "," : "") + quoted(row_text(row));
    emit(screen + "]}");
    if (strcmp(reason, "exit")) emit(state());
    char end[160];
    snprintf(end, sizeof end, "{\"ev\":\"end\",\"reason\":\"%s\",\"exit_code\":%u,\"ms\":%llu,\"cs\":%u,\"eip\":%u}",
             reason, (unsigned)dos.return_code, (unsigned long long)ran_ms, (unsigned)SegValue(cs), (unsigned)reg_eip);
    emit(end);
    _exit(0); /* the child shares the server's host state; run no destructors */
}

void on_alarm(int) { wall_expired = 1; }

/* False at end of input. */
bool read_job(unsigned &wall) {
    lines.clear();
    limit_ms = 0;
    wall = 0;
    watch = true;
    char buf[1024];
    while (fgets(buf, sizeof buf, stdin)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (!strcmp(buf, ".")) return true;
        if (!strncmp(buf, ":ms ", 4)) limit_ms = strtoull(buf + 4, NULL, 10);
        else if (!strncmp(buf, ":wall ", 6)) wall = (unsigned)strtoul(buf + 6, NULL, 10);
        else if (!strcmp(buf, ":nowatch")) watch = false;
        else lines.push_back(buf);
    }
    return false;
}

void serve() {
    emit("{\"ev\":\"ready\"}");
    for (;;) {
        unsigned wall;
        if (!read_job(wall)) _exit(0);
        pid_t pid = fork();
        if (pid == 0) {
            child = true;
            ticksLocked = true; /* emulated time runs free of host time */
            upto = cursor_row();
            lines.push_front("@echo off"); /* a job runs as a batch file does */
            for (unsigned vec = 0; vec < 8; vec++) booted_ivt[vec] = real_readd(0, vec * 4);
            DEBUG_Socket_TraceEnable();
            dosrun_watch = watch;
            signal(SIGALRM, on_alarm);
            alarm(wall);
            return;
        }
        int status = 0;
        if (pid > 0) waitpid(pid, &status, 0);
        char done[96];
        snprintf(done, sizeof done, "{\"ev\":\"done\",\"status\":%d,\"signal\":%d}",
                 pid < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                 pid > 0 && WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        emit(done);
    }
}

}

bool DOSRUN_ShellInput(char *line, unsigned int size) {
    if (!active()) return false;
    if (!child) serve();
    if (lines.empty()) finish("exit");
    snprintf(line, size, "%s", lines.front().c_str());
    lines.pop_front();
    return true;
}

bool dosrun_watch = false;

bool DOSRUN_Child(void) { return child; }

void DOSRUN_Executes(uint32_t cs, uint32_t linear) {
    if (cpu.pmode) return;
    uint32_t head = mem_readd(linear);
    if (head == 0 || head == 0xFFFFFFFFu) crash("empty memory", cs, linear);
    if (!owned(linear)) crash("unowned memory", cs, linear);
}

void DOSRUN_Exception(uint8_t which) {
    static const bool fault[8] = {true, false, false, false, false, true, true, true}; /* #DE #BR #UD #NM */
    if (!dosrun_watch || cpu.pmode || which >= 8 || !fault[which]) return;
    if (real_readd(0, which * 4) != booted_ivt[which]) return; /* the program handles it */
    const char *names[8] = {"divide error", "", "", "", "", "bound range", "invalid opcode", "no FPU"};
    crash(names[which], SegValue(cs), SegPhys(cs) + reg_eip);
}

void DOSRUN_Halt(void) {
    if (!dosrun_watch) return;
    FillFlags();
    if (!GETFLAG(IF)) crash("halt with interrupts off", SegValue(cs), SegPhys(cs) + reg_eip);
}

void DOSRUN_Tick(void) {
    if (!child) return;
    ran_ms++;
    follow_cursor();
    if (limit_ms && ran_ms >= limit_ms) finish("limit");
    if (wall_expired) finish("wall");
}

void DOSRUN_BeforeScroll(uint8_t rul, uint8_t cul, uint8_t rlr, uint8_t clr, int8_t nlines, uint8_t) {
    if (!child || nlines > 0 || cul != 0 || clr + 1 < cols()) return;
    follow_cursor();
    int leaving = nlines == 0 ? rlr - rul + 1 : -nlines; /* negative is up */
    for (int row = rul; row <= rlr && row < rul + leaving; row++)
        if (row >= upto) emit_row(row, false);
    if (upto > rul && upto <= rlr + 1) upto = upto - leaving > rul ? upto - leaving : rul;
}
