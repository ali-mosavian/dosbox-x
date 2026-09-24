/*
 * dosrun: serve jobs from one booted machine. See include/dosrun.h.
 *
 * Job, on stdin, ended by a line holding only ".":
 *   :ms N      stop after N emulated milliseconds
 *   :wall N    stop after N host seconds
 *   :nowatch   do not stop on a crash
 *   :keep      on a clean exit, keep the machine: the child becomes the server
 *              and later jobs fork from its state -- mounts, current
 *              directory, environment, anything resident
 *   :pop       alone in a job: drop the kept machine, back to the one before
 *   :cwd, :env, :drives, :ls [PATTERN]
 *              queries, answered in order with the command lines
 *   anything else is a shell command line, run in order
 *
 * Events, one JSON object per line on DOSRUN_FD:
 *   {"ev":"ready"}                        from the server, once booted
 *   {"ev":"line","text":...}             a line of screen output, as it is finished
 *   {"ev":"out","handle":N,"file":...,"text":...}
 *                                        bytes written to standard output or error
 *                                        redirected to a file, sent within an
 *                                        emulated millisecond of being written
 *   {"ev":"screen","rows":[...]}         the screen when the job ends
 *   {"ev":"crash","kind":...,"at":{...}} execution went where code cannot be
 *   {"ev":"state",...}                   registers, source line and last branches,
 *                                        unless the job simply exited
 *   {"ev":"end","reason":...,...}        exit | crash | limit | wall, with emulated ms,
 *                                        host CPU ms and CS:EIP
 *   {"ev":"query","q":...,"result":...}  a query's answer, from DOS's own state
 *   {"ev":"done","status":N,"signal":N}  from the server, once the child is reaped;
 *                                        with "kept":true from a :keep child
 *                                        that now serves
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
 *   - a program's data: memory its debug info or link map lays out as data,
 *     or its load image outside any code, while the program still owns it
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

#include <algorithm>
#include <deque>
#include <string>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

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
/* Redirected output not yet sent: programs write it a byte at a time. */
uint16_t out_handle = 0;
std::string out_file, out_text;
bool watch = true;
bool keep = false;
bool forked = false; /* this process shares an older one's host state */
unsigned depth = 0;  /* kept machines below this one */
double fork_cpu_ms = 0; /* a child's rusage starts at zero on Linux, not everywhere */
uint32_t booted_ivt[8];
uint32_t owned_lo = 1, owned_hi = 0; /* the last owned block found, [lo, hi) */

/* The call stack, as execution built it: a frame lives while its return
 * address is still on the stack. Knows no instruction, so it holds calls,
 * INTs and interrupted code alike. */
struct Frame {
    uint16_t psp; /* the process that called */
    uint16_t ss;
    uint32_t slot; /* linear address of the return address */
    uint32_t site_cs, site; /* the instruction that called */
    uint32_t callee_cs, callee;
};
std::vector<Frame> frames;
const size_t max_frames = 1u << 16;
size_t dropped_frames = 0; /* the oldest, past max_frames */

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

void flush_output() {
    if (out_text.empty()) return;
    emit("{\"ev\":\"out\",\"handle\":" + std::to_string(out_handle) + ",\"file\":" + quoted(out_file) +
         ",\"text\":" + quoted(out_text) + "}");
    out_text.clear();
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
    json += "},\"where\":" + place(SegValue(cs), SegPhys(cs) + reg_eip) + ",\"stack\":[";
    for (size_t i = frames.size(); i-- > 0 && frames.size() - i <= 32;)
        json += (i + 1 == frames.size() ? "" : ",") + std::string("{\"site\":") + place(frames[i].site_cs, frames[i].site) +
                ",\"callee\":" + place(frames[i].callee_cs, frames[i].callee) + "}";
    json += "],\"depth\":" + std::to_string(frames.size() + dropped_frames) + ",\"branches\":[";
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

bool owned(uint32_t linear) {
    if (linear < 0x500) return false; /* the interrupt table and BIOS data are never code */
    if (linear >= 0xA0000 && linear < 0xC0000) return false; /* video memory is never code */
    if (linear >= 0xF0000 || (linear >= owned_lo && linear < owned_hi)) return true;
    uint16_t para = (uint16_t)(linear >> 4), owner = 0, start = 0, end = 0;
    if (para < dos.firstMCB) return true; /* the IVT, BIOS data and the DOS kernel */
    if (DOS_MemoryBlockAt(para, owner, start, end)) {
        if (owner) {
            owned_lo = (uint32_t)start << 4;
            owned_hi = (uint32_t)end << 4;
        }
        return owner != 0;
    }
    /* past the chain: in conventional memory no one's; above it ROM, the EMS
     * frame or DOS's own */
    return linear >= 0xC0000;
}

void report(const char *reason);

[[noreturn]] void finish(const char *reason) {
    report(reason);
    _exit(0); /* the child shares the server's host state; run no destructors */
}

void report(const char *reason) {
    flush_output();
    follow_cursor();
    emit_row(upto, false);
    std::string screen = "{\"ev\":\"screen\",\"rows\":[";
    for (int row = 0; row < rows(); row++) screen += (row ? "," : "") + quoted(row_text(row));
    emit(screen + "]}");
    if (strcmp(reason, "exit")) emit(state());
    /* host CPU time, not wall time: other load on the host does not count */
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    const double cpu_ms = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e3 +
                          (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e3 - fork_cpu_ms;
    char end[200];
    snprintf(end, sizeof end, "{\"ev\":\"end\",\"reason\":\"%s\",\"exit_code\":%u,\"ms\":%llu,\"cpu_ms\":%.1f,\"cs\":%u,\"eip\":%u}",
             reason, (unsigned)dos.return_code, (unsigned long long)ran_ms, cpu_ms, (unsigned)SegValue(cs), (unsigned)reg_eip);
    emit(end);
}

void on_alarm(int) { wall_expired = 1; }

/* Unbuffered: a kept child takes over reading, and a stdio buffer would
 * leave each process a different copy of what was read ahead. */
bool read_line(std::string &line) {
    line.clear();
    char c;
    for (;;) {
        ssize_t n = read(0, &c, 1);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line += c;
    }
}

/* False at end of input. */
bool read_job(unsigned &wall, bool &pop) {
    lines.clear();
    limit_ms = 0;
    wall = 0;
    watch = true;
    keep = false;
    pop = false;
    std::string buf;
    while (read_line(buf)) {
        if (buf == ".") return true;
        if (!buf.compare(0, 4, ":ms ")) limit_ms = strtoull(buf.c_str() + 4, NULL, 10);
        else if (!buf.compare(0, 6, ":wall ")) wall = (unsigned)strtoul(buf.c_str() + 6, NULL, 10);
        else if (buf == ":nowatch") watch = false;
        else if (buf == ":keep") keep = true;
        else if (buf == ":pop") pop = true;
        else lines.push_back(buf);
    }
    return false;
}

std::string two(unsigned value) {
    char b[8];
    snprintf(b, sizeof b, "%02u", value % 100);
    return b;
}

/* A query line's answer, as JSON, or empty when the line is not a query. */
std::string query(const std::string &line) {
    const std::string name = line.substr(1, line.find(' ') == std::string::npos ? std::string::npos : line.find(' ') - 1);
    const std::string arg = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
    std::string result;
    if (name == "cwd") {
        char dir[DOS_PATHLENGTH] = {0};
        const uint8_t drive = DOS_GetDefaultDrive();
        DOS_GetCurrentDir(0, dir, false);
        result = quoted(std::string(1, (char)('A' + drive)) + ":\\" + dir);
    } else if (name == "env") {
        result = "{";
        PhysPt at = PhysMake(DOS_PSP(dos.psp()).GetEnvironment(), 0);
        for (bool first = true; mem_readb(at); first = false) {
            std::string entry;
            for (uint8_t c; (c = mem_readb(at++)) != 0;) entry += (char)c;
            const size_t eq = entry.find('=');
            result += (first ? "" : ",") + quoted(entry.substr(0, eq)) + ":" +
                      quoted(eq == std::string::npos ? "" : entry.substr(eq + 1));
        }
        result += "}";
    } else if (name == "drives") {
        result = "{";
        for (int i = 0, n = 0; i < DOS_DRIVES; i++)
            if (Drives[i]) result += (n++ ? "," : "") + quoted(std::string(1, (char)('A' + i))) + ":" + quoted(Drives[i]->GetInfo());
        result += "}";
    } else if (name == "ls") {
        /* the search leaves no trace: its own DTA, and DOS's error code as it was */
        const uint16_t saved_error = dos.errorcode;
        const RealPt saved_dta = dos.dta();
        dos.dta(dos.tables.tempdta);
        result = "[";
        bool more = DOS_FindFirst(arg.empty() ? "*.*" : arg.c_str(), DOS_ATTR_DIRECTORY | DOS_ATTR_HIDDEN | DOS_ATTR_SYSTEM);
        for (int n = 0; more; n++, more = DOS_FindNext()) {
            char fname[DOS_NAMELENGTH_ASCII], lname[LFN_NAMELENGTH + 1];
            uint32_t size, hsize;
            uint16_t date, time;
            uint8_t attr;
            DOS_DTA(dos.dta()).GetResult(fname, lname, size, hsize, date, time, attr);
            result += (n ? "," : "") + std::string("{\"name\":") + quoted(fname) + ",\"size\":" + std::to_string(size) +
                      ",\"attr\":" + std::to_string(attr) + ",\"dir\":" + ((attr & DOS_ATTR_DIRECTORY) ? "true" : "false") +
                      ",\"date\":\"" + std::to_string(((date >> 9) & 0x7f) + 1980) + "-" + two((date >> 5) & 0xf) + "-" +
                      two(date & 0x1f) + "\",\"time\":\"" + two(time >> 11) + ":" + two((time >> 5) & 0x3f) + ":" +
                      two((time & 0x1f) * 2) + "\"}";
        }
        result += "]";
        dos.dta(saved_dta);
        dos.errorcode = saved_error;
    } else {
        return "";
    }
    return "{\"ev\":\"query\",\"q\":" + quoted(name) + (arg.empty() ? "" : ",\"arg\":" + quoted(arg)) +
           ",\"result\":" + result + "}";
}

void serve() {
    for (;;) {
        unsigned wall;
        bool pop;
        if (!read_job(wall, pop)) _exit(0);
        if (pop) {
            if (depth) _exit(0); /* the server below reports it, as its child's end */
            emit("{\"ev\":\"done\",\"status\":-1,\"signal\":0,\"error\":\"nothing kept to pop\"}");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            child = true;
            forked = true;
            struct rusage usage;
            getrusage(RUSAGE_SELF, &usage);
            fork_cpu_ms = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e3 +
                          (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e3;
            ticksLocked = true; /* emulated time runs free of host time */
            upto = cursor_row();
            lines.push_front("@echo off"); /* a job runs as a batch file does */
            for (unsigned vec = 0; vec < 8; vec++) booted_ivt[vec] = real_readd(0, vec * 4);
            DEBUG_Socket_TraceEnable();
            frames.clear();
            dropped_frames = 0;
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
    if (!child) {
        emit("{\"ev\":\"ready\"}");
        serve();
    }
    for (;;) {
        while (!lines.empty() && lines.front()[0] == ':') {
            const std::string answer = query(lines.front());
            emit(answer.empty() ? "{\"ev\":\"query\",\"q\":" + quoted(lines.front()) + ",\"error\":\"unknown\"}" : answer);
            lines.pop_front();
        }
        if (!lines.empty()) break;
        if (!keep) finish("exit");
        /* Kept: this machine serves from here, at the prompt it reached. */
        report("exit");
        emit("{\"ev\":\"done\",\"status\":0,\"signal\":0,\"kept\":true}");
        child = false;
        dosrun_watch = false;
        alarm(0);
        wall_expired = 0;
        ran_ms = 0;
        depth++;
        serve(); /* returns in the child of the next job */
    }
    snprintf(line, size, "%s", lines.front().c_str());
    lines.pop_front();
    return true;
}

bool dosrun_watch = false;

bool DOSRUN_Forked(void) { return forked; }

void DOSRUN_Executes(uint32_t cs, uint32_t linear) {
    if (cpu.pmode) return;
    uint32_t head = mem_readd(linear);
    if (head == 0 || head == 0xFFFFFFFFu) crash("empty memory", cs, linear);
    if (!owned(linear)) crash("unowned memory", cs, linear);
    if (DEBUG_Symbols().ProgramDataAt(linear)) crash("program data", cs, linear);
}

void DOSRUN_Transferred(uint32_t from_cs, uint32_t from_linear, uint32_t from_sp, uint32_t cs, uint32_t linear) {
    if (cpu.pmode) return;
    const uint16_t ss = SegValue(::ss), sp = reg_sp;
    const uint32_t top = ((uint32_t)ss << 4) + sp;
    /* A call leaves on the stack an address just past the instruction that
     * made it: a CALL's own end, or where an interrupt broke in. */
    const uint32_t pushed = ((from_cs & 0xFFFFu) << 4) + mem_readw(top);
    const bool call = sp < (uint16_t)from_sp && pushed > from_linear && pushed - from_linear <= 15;
    /* A call can unwind a little, after code popped its own return address,
     * but only a stack that wrapped around its segment unwinds by half of it. */
    if (call && !frames.empty() && frames.back().ss == ss && top >= frames.back().slot + 0x8000)
        crash("stack overflow", cs, linear);
    /* A frame ends when its return address is popped, or overwritten. */
    while (!frames.empty() && frames.back().ss == ss && (frames.back().slot < top || (call && frames.back().slot == top)))
        frames.pop_back();
    if (!call) return;
    if (frames.size() == max_frames) {
        frames.erase(frames.begin(), frames.begin() + max_frames / 2);
        dropped_frames += max_frames / 2;
    }
    frames.push_back(Frame{dos.psp(), ss, top, from_cs, from_linear, cs, linear});
}

void DOSRUN_Terminated(uint16_t pspseg) {
    frames.erase(std::remove_if(frames.begin(), frames.end(), [pspseg](const Frame &f) { return f.psp == pspseg; }),
                 frames.end());
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

void DOSRUN_Wrote(uint16_t entry, const char *name, bool device, const uint8_t *data, uint16_t n) {
    /* on a device, the output is on the screen and in the transcript already */
    if (!child || device || (entry != 1 && entry != 2) || n == 0) return;
    std::string file = name ? name : "";
    if (entry != out_handle || file != out_file) flush_output();
    out_handle = entry;
    out_file = file;
    out_text.append((const char *)data, n);
    if (out_text.size() >= 4096) flush_output();
}

void DOSRUN_Tick(void) {
    if (!child) return;
    ran_ms++;
    flush_output();
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
