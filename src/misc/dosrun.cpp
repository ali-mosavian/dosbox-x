/*
 * dosrun: serve jobs from one booted machine. See include/dosrun.h.
 *
 * Job, on stdin, ended by a line holding only ".":
 *   :ms N      stop after N emulated milliseconds
 *   :wall N    stop after N host seconds
 *   anything else is a shell command line, run in order
 *
 * Events, one JSON object per line on DOSRUN_FD:
 *   {"ev":"ready"}                        from the server, once booted
 *   {"ev":"line","text":...}             a line of screen output, as it is finished
 *   {"ev":"screen","rows":[...]}         the screen when the job ends
 *   {"ev":"end","reason":...,...}        exit | limit | wall, with emulated ms and CS:EIP
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
 */

#include "dosbox.h"
#include "dosrun.h"
#include "mem.h"
#include "regs.h"
#include "dos_inc.h"
#include "ints/int10.h"

#include <deque>
#include <string>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern bool ticksLocked;
void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result);

namespace {

int out_fd = -2; /* -2: not yet looked up, -1: dosrun off */
bool child = false;
std::deque<std::string> lines;
uint64_t limit_ms = 0, ran_ms = 0;
volatile sig_atomic_t wall_expired = 0;
int upto = 0; /* rows above this one are in the transcript */

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

[[noreturn]] void finish(const char *reason) {
    follow_cursor();
    emit_row(upto, false);
    std::string screen = "{\"ev\":\"screen\",\"rows\":[";
    for (int row = 0; row < rows(); row++) screen += (row ? "," : "") + quoted(row_text(row));
    emit(screen + "]}");
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
    char buf[1024];
    while (fgets(buf, sizeof buf, stdin)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (!strcmp(buf, ".")) return true;
        if (!strncmp(buf, ":ms ", 4)) limit_ms = strtoull(buf + 4, NULL, 10);
        else if (!strncmp(buf, ":wall ", 6)) wall = (unsigned)strtoul(buf + 6, NULL, 10);
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

bool DOSRUN_Child(void) { return child; }

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
