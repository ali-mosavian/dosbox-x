/*
 * debug_socket.cpp - Socket-based debug interface for AI automation
 *
 * Protocol: JSON over TCP, newline-delimited
 *
 * Commands (send as JSON):
 *   {"cmd":"break"}           - Break execution
 *   {"cmd":"continue"}        - Continue execution  
 *   {"cmd":"step"}            - Single step
 *   {"cmd":"step_over"}       - Step over (step past call)
 *   {"cmd":"bp_set","seg":X,"off":Y}  - Set breakpoint
 *   {"cmd":"bp_clear","seg":X,"off":Y} - Clear breakpoint
 *   {"cmd":"bp_list"}         - List breakpoints
 *   {"cmd":"regs"}            - Get registers
 *   {"cmd":"regs_set","reg":"EAX","val":X} - Set register
 *   {"cmd":"mem_read","seg":X,"off":Y,"len":Z} - Read memory
 *   {"cmd":"mem_write","seg":X,"off":Y,"data":"hex"} - Write memory
 *   {"cmd":"disasm","seg":X,"off":Y,"count":Z} - Disassemble
 *   {"cmd":"status"}          - Get debugger status
 *
 * Responses (JSON):
 *   {"status":"ok",...}       - Success with optional data
 *   {"status":"error","msg":"..."} - Error
 *
 * Notifications (async):
 *   {"event":"stopped","reason":"breakpoint","seg":X,"off":Y}
 *   {"event":"stopped","reason":"step"}
 */

#include "dosbox.h"

#if C_DEBUG

#include "debug_socket.h"
#include "debug.h"
#include "cpu.h"
#include "regs.h"
#include "paging.h"
#include "mem.h"
#include "bios.h"
#include "shell.h"
#include "dos_inc.h"

// Forward declarations for CPU functions
extern Bitu CPU_SIDT_base(void);
extern Bitu CPU_SIDT_limit(void);

// Forward declarations
extern void DEBUG_ShowMsg(const char *format,...);
Bitu DasmI386(char* buffer, PhysPt pc, uint32_t cur_ip, bool bit32);
#define LOG_MSG DEBUG_ShowMsg

// CBreakpoint is defined in debug.cpp - we can access it since we're in the same library
// Define the enum (must match debug.cpp) and forward declare the class with needed methods
enum EBreakpoint { BKPNT_UNKNOWN, BKPNT_PHYSICAL, BKPNT_INTERRUPT, BKPNT_MEMORY, BKPNT_MEMORY_PROT, BKPNT_MEMORY_LINEAR, BKPNT_MEMORY_FREEZE };

class CBreakpoint {
public:
    static CBreakpoint* FindPhysBreakpoint(uint16_t seg, uint32_t off, bool once);
    static CBreakpoint* AddBreakpoint(uint16_t seg, uint32_t off, bool once);
    static CBreakpoint* AddBreakpointByAddr(PhysPt addr, bool once);
    static CBreakpoint* AddIntBreakpoint(uint8_t intNum, uint16_t ah, uint16_t al, bool once);
    static bool DeleteBreakpointByAddr(PhysPt addr);
    static void DeleteAll();
    static void ShowList();
    static std::string ToJSON();  // returns JSON array of all breakpoints
    void Activate(bool active);
};

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "keyboard.h"
#include <errno.h>
#include <string.h>
#include <string>
#include <sstream>
#include <vector>
#include <cstdio>

struct LinearExecBreakpoint {
    uint32_t linear = 0;
    bool once = false;
    bool active = true;
    bool suppress_current = false;
    bool has_match_seg = false;
    uint16_t match_seg = 0;
    bool has_match_off = false;
    uint32_t match_off = 0;
};

// External declarations from debug.cpp
extern bool exitLoop;
extern bool debugging;
extern bool mustCompleteInstruction;
extern bool inhibit_int_breakpoint;
extern int32_t DEBUG_Run(int32_t amount, bool quickexit);
extern uint64_t GetAddress(uint16_t seg, uint32_t offset);

// Host event pump (defined in the SDL frontend)
void GFX_Events(void);

// In-place "frozen" stop state.
//
// Unlike the GUI/terminal debugger, a socket stop must NOT unwind the host C++
// stack: doing so corrupts re-entrant DOSBOX_RunMachine() invocations and the
// PM/DPMI task state that CWSDPMI relies on. Instead, when a socket breakpoint
// matches, we block in place inside the CPU core (DEBUG_Socket_FreezeWait),
// servicing only socket commands + host events, never running the CPU and never
// touching the loop handler / cycle counters / flags. "continue" clears this
// flag and the original CPU-core invocation resumes from the exact same
// instruction boundary -- a transparent, Bochs-style stop/resume.
static volatile bool socket_freeze_wait = false;

// Single-step countdown. Set to 2 by the "step" command; the pre-instruction
// guard in CPU_Core_Normal_Run decrements it each pass. When it reaches 1 the
// current instruction executes normally; when it reaches 0 the guard emits a
// "step" stopped event and calls FreezeWait — so exactly one instruction runs.
static volatile int socket_step_arm = 0;

// Forward declarations
bool ParseCommand(char* str);
bool IsDebuggerRunwatch(void);
Bitu DasmI386(char* buffer, PhysPt pc, uint32_t cur_ip, bool bit32);

// Socket state
static int server_socket = -1;
static int client_socket = -1;
static int socket_port = 0;
static std::string recv_buffer;
static bool gdb_mode = false;  // true = GDB RSP, false = JSON
static uint8_t last_exception_num = 0xFF;
static uint32_t last_exception_error = 0;
static std::vector<LinearExecBreakpoint> linear_exec_breakpoints;
static std::string last_stop_event_json;
static std::string current_response_id_json;

// Simple JSON helpers (no external dependencies)
static std::string json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

static std::string json_obj_start() { return "{"; }
static std::string json_obj_end() { return "}"; }
static std::string json_str(const char* key, const char* val) {
    return "\"" + std::string(key) + "\":\"" + json_escape(val) + "\"";
}
static std::string json_str(const char* key, const std::string& val) {
    return "\"" + std::string(key) + "\":\"" + json_escape(val) + "\"";
}
static std::string json_num(const char* key, long long val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", val);
    return "\"" + std::string(key) + "\":" + buf;
}
static std::string json_hex(const char* key, uint32_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\"0x%08X\"", val);
    return "\"" + std::string(key) + "\":" + buf;
}
static std::string json_bool(const char* key, bool val) {
    return "\"" + std::string(key) + "\":" + (val ? "true" : "false");
}

// Parse simple JSON value (handles optional whitespace after :)
static bool json_get_string(const std::string& json, const char* key, std::string& out) {
    std::string search = "\"" + std::string(key) + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.length();
    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    // Must be a string starting with "
    if (pos >= json.length() || json[pos] != '"') return false;
    pos++; // skip opening quote
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

static bool json_get_int(const std::string& json, const char* key, long long& out) {
    std::string search = "\"" + std::string(key) + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.length();
    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    // Handle hex string "0x..."
    if (pos + 1 < json.length() && json[pos] == '"') {
        pos++;
        if (json.substr(pos, 2) == "0x" || json.substr(pos, 2) == "0X") {
            out = strtoll(json.c_str() + pos, nullptr, 16);
            return true;
        }
    }
    // Handle number
    out = strtoll(json.c_str() + pos, nullptr, 0);
    return true;
}

static bool json_get_id_field(const std::string& json, std::string& out) {
    std::string search = "\"id\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.length()) return false;

    if (json[pos] == '"') {
        pos++;
        size_t end = json.find("\"", pos);
        if (end == std::string::npos) return false;
        out = json_str("id", json.substr(pos, end - pos));
        return true;
    }

    size_t end = pos;
    while (end < json.length() && json[end] != ',' && json[end] != '}' &&
           json[end] != ' ' && json[end] != '\t' && json[end] != '\r' && json[end] != '\n') {
        end++;
    }
    if (end == pos) return false;
    out = "\"id\":" + json.substr(pos, end - pos);
    return true;
}

// GDB RSP checksum calculation
static uint8_t gdb_checksum(const std::string& data) {
    uint8_t sum = 0;
    for (char c : data) {
        sum += (uint8_t)c;
    }
    return sum;
}

// Send GDB RSP packet
static void send_gdb_packet(const std::string& data) {
    if (client_socket < 0) return;
    uint8_t checksum = gdb_checksum(data);
    char packet[4096];
    int len = snprintf(packet, sizeof(packet), "$%s#%02x", data.c_str(), checksum);
    if (len > 0 && len < (int)sizeof(packet)) {
        send(client_socket, packet, len, 0);
    }
}

// Send GDB RSP ACK
static void send_gdb_ack(bool ok) {
    if (client_socket < 0) return;
    send(client_socket, ok ? "+" : "-", 1, 0);
}

// Send response to client (JSON or GDB RSP)
static void send_response(const std::string& json) {
    if (client_socket < 0) return;
    if (gdb_mode) {
        // In GDB mode, this shouldn't be called for JSON responses
        // But if it is, send as GDB packet
        send_gdb_packet(json);
    } else {
        std::string out = json;
        if (!current_response_id_json.empty() && out.size() > 1 && out[0] == '{') {
            out.insert(1, current_response_id_json + ",");
        }
        std::string msg = out + "\n";
        send(client_socket, msg.c_str(), msg.length(), 0);
    }
}

// Send error response
static void send_error(const char* msg) {
    send_response("{" + json_str("status", "error") + "," + json_str("msg", msg) + "}");
}

// Send OK response with optional data
static void send_ok(const std::string& extra = "") {
    std::string resp = "{" + json_str("status", "ok");
    if (!extra.empty()) resp += "," + extra;
    resp += "}";
    send_response(resp);
}

static void send_and_latch_stop(const std::string& json) {
    last_stop_event_json = json;
    send_response(json);
}

// Get all registers as JSON
static std::string get_registers_json() {
    std::ostringstream ss;
    ss << json_hex("EAX", reg_eax) << ","
       << json_hex("EBX", reg_ebx) << ","
       << json_hex("ECX", reg_ecx) << ","
       << json_hex("EDX", reg_edx) << ","
       << json_hex("ESI", reg_esi) << ","
       << json_hex("EDI", reg_edi) << ","
       << json_hex("EBP", reg_ebp) << ","
       << json_hex("ESP", reg_esp) << ","
       << json_hex("EIP", reg_eip) << ","
       << json_num("CS", SegValue(SegNames::cs)) << ","
       << json_num("DS", SegValue(SegNames::ds)) << ","
       << json_num("ES", SegValue(SegNames::es)) << ","
       << json_num("SS", SegValue(SegNames::ss)) << ","
       << json_num("FS", SegValue(SegNames::fs)) << ","
       << json_num("GS", SegValue(SegNames::gs)) << ","
       << json_hex("FLAGS", reg_flags);
    return ss.str();
}

// Translate a linear (virtual) address to a physical address via the page tables.
// Returns false if the page is not present.  When paging is disabled the
// linear and physical address are identical.
static bool LinearToPhysical(uint32_t linear, uint32_t& physical) {
    if (!paging.enabled) {
        physical = linear;
        return true;
    }
    uint32_t dir_idx = (linear >> 22) & 0x3FFu;
    uint32_t tbl_idx = (linear >> 12) & 0x3FFu;
    uint32_t page_off = linear & 0xFFFu;

    PhysPt pde_addr = (PhysPt)((paging.base.page << 12u) + dir_idx * 4u);
    X86PageEntry pde;
    pde.load = phys_readd(pde_addr);
    if (!pde.block.p) return false;

    PhysPt pte_addr = (PhysPt)(((Bitu)pde.block.base << 12u) + tbl_idx * 4u);
    X86PageEntry pte;
    pte.load = phys_readd(pte_addr);
    if (!pte.block.p) return false;

    physical = (pte.block.base << 12u) + page_off;
    return true;
}

static std::string selector_descriptor_json(const char* key, uint16_t sel) {
    Descriptor desc;
    if (!cpu.gdt.GetDescriptor((Bitu)sel, desc)) {
        return "\"" + std::string(key) + "\":{" +
               json_hex("sel", sel) + "," +
               json_bool("valid", false) + "}";
    }

    uint8_t type_byte = desc.saved.seg.type;
    bool is_system = !(type_byte & 0x10);
    bool is_code = !is_system && (type_byte & 0x08);
    const char* seg_type = is_system ? "system" : (is_code ? "code" : "data");
    bool is_ldt_sel = (sel & 0x04u) != 0;

    return "\"" + std::string(key) + "\":{" +
           json_hex("sel", sel) + "," +
           json_bool("valid", true) + "," +
           json_str("table", is_ldt_sel ? "ldt" : "gdt") + "," +
           json_hex("base", (uint32_t)desc.GetBase()) + "," +
           json_hex("limit", (uint32_t)desc.GetLimit()) + "," +
           json_num("dpl", desc.saved.seg.dpl) + "," +
           json_str("seg_type", seg_type) + "," +
           json_num("type_byte", type_byte) + "," +
           json_bool("present", desc.saved.seg.p != 0) + "," +
           json_bool("big", desc.saved.seg.big != 0) + "," +
           json_bool("granularity", desc.saved.seg.g != 0) +
           "}";
}

static bool selector_decode_info(uint16_t sel, uint32_t off, uint32_t& linear, bool& bit32) {
    linear = (uint32_t)GetAddress(sel, off);

    if (!cpu.pmode) {
        bit32 = false;
        return true;
    }

    Descriptor desc;
    if (!cpu.gdt.GetDescriptor((Bitu)sel, desc)) {
        return false;
    }

    const uint8_t type_byte = desc.saved.seg.type;
    const bool is_system = (type_byte & 0x10) == 0;
    const bool is_code = !is_system && (type_byte & 0x08) != 0;
    if (!is_code || desc.saved.seg.p == 0) {
        return false;
    }

    bit32 = desc.saved.seg.big != 0;
    return true;
}

static std::string stop_reason_name(const char* reason, int int_num) {
    if (int_num == 0x0E) return "page_fault";
    if (int_num == 0x0D) return "gpf";
    if (int_num >= 0) return "interrupt";
    return reason ? reason : "stopped";
}

static std::string stop_context_json(const char* reason, uint16_t seg, uint32_t off, int int_num = -1) {
    uint32_t linear = (uint32_t)GetAddress(seg, off);
    uint32_t physical = 0;
    bool physical_present = LinearToPhysical(linear, physical);

    std::ostringstream ss;
    ss << json_str("reason", stop_reason_name(reason, int_num)) << ","
       << json_hex("linear", linear) << ","
       << json_bool("physical_present", physical_present) << ",";

    if (physical_present) {
        ss << json_hex("physical", physical) << ",";
    }

    ss << json_hex("CR2", (uint32_t)paging.cr2) << ","
       << json_hex("CR3", (uint32_t)paging.cr3) << ","
       << json_bool("paging", paging.enabled) << ","
       << selector_descriptor_json("cs_desc", seg);

    if (int_num >= 0) {
        ss << "," << json_num("int", int_num);
    }
    if (int_num == last_exception_num) {
        ss << "," << json_hex("exception_error", last_exception_error);
    }

    return ss.str();
}

static std::string linear_exec_breakpoints_json() {
    std::string arr = "[";
    bool first = true;
    int nr = 0;
    for (const auto& bp : linear_exec_breakpoints) {
        if (!first) arr += ",";
        first = false;
        arr += "{" +
               json_num("index", nr) + "," +
               json_str("type", "exec_linear") + "," +
               json_hex("linear", bp.linear) + "," +
               json_bool("has_match_seg", bp.has_match_seg) + "," +
               json_hex("match_seg", bp.match_seg) + "," +
               json_bool("has_match_off", bp.has_match_off) + "," +
               json_hex("match_off", bp.match_off) + "," +
               json_bool("active", bp.active) + "," +
               json_bool("once", bp.once) +
               "}";
        nr++;
    }
    arr += "]";
    return arr;
}

static void add_linear_exec_breakpoint(
    uint32_t linear,
    bool once,
    bool has_match_seg = false,
    uint16_t match_seg = 0,
    bool has_match_off = false,
    uint32_t match_off = 0
) {
    for (auto& bp : linear_exec_breakpoints) {
        if (bp.linear == linear) {
            bp.once = once;
            bp.active = true;
            bp.has_match_seg = has_match_seg;
            bp.match_seg = match_seg;
            bp.has_match_off = has_match_off;
            bp.match_off = match_off;
            return;
        }
    }
    LinearExecBreakpoint bp;
    bp.linear = linear;
    bp.once = once;
    bp.active = true;
    bp.has_match_seg = has_match_seg;
    bp.match_seg = match_seg;
    bp.has_match_off = has_match_off;
    bp.match_off = match_off;
    linear_exec_breakpoints.push_back(bp);
}

static bool clear_linear_exec_breakpoint(uint32_t linear) {
    for (auto it = linear_exec_breakpoints.begin(); it != linear_exec_breakpoints.end(); ++it) {
        if (it->linear == linear) {
            linear_exec_breakpoints.erase(it);
            return true;
        }
    }
    return false;
}

// Helper function to implement StepOver logic
static bool DoStepOver() {
    exitLoop = false;
    uint16_t cs = SegValue(SegNames::cs);
    PhysPt start = (PhysPt)GetAddress(cs, reg_eip);
    char dline[200];
    Bitu size = DasmI386(dline, start, reg_eip, cpu.code.big);
    
    if (strstr(dline, "call") || strstr(dline, "int") || strstr(dline, "loop") || strstr(dline, "rep")) {
        // Set temporary breakpoint at next instruction
        uint32_t next_eip = (uint32_t)(reg_eip + size);
        if (!CBreakpoint::FindPhysBreakpoint(cs, next_eip, true)) {
            CBreakpoint::AddBreakpoint(cs, next_eip, true);
        }
        return true;
    }
    return false;
}

// Freeze-safe variant of DoStepOver: returns the linear address of the first
// instruction past the current one when stepping over a call/int/loop/rep,
// or 0 when a plain single-step is correct. Does NOT add a physical breakpoint
// (which would use the crash-prone legacy resume path).
static uint32_t GetStepOverLinear() {
    uint16_t cs_val = SegValue(SegNames::cs);
    PhysPt start = (PhysPt)GetAddress(cs_val, reg_eip);
    char dline[200];
    Bitu size = DasmI386(dline, start, reg_eip, cpu.code.big);
    if (strstr(dline, "call") || strstr(dline, "int") || strstr(dline, "loop") || strstr(dline, "rep")) {
        return (uint32_t)GetAddress(cs_val, (uint32_t)(reg_eip + size));
    }
    return 0;
}

// Get all registers in GDB RSP format (little-endian hex, 32-bit each)
static std::string get_registers_gdb() {
    std::ostringstream ss;
    // Format: eax, ecx, edx, ebx, esp, ebp, esi, edi, eip, eflags, cs, ss, ds, es, fs, gs
    // Each register is 8 hex chars (32 bits) in little-endian
    auto to_hex_le = [](uint32_t val) -> std::string {
        char buf[9];
        snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", 
                 val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF);
        return buf;
    };
    
    ss << to_hex_le(reg_eax) << to_hex_le(reg_ecx) << to_hex_le(reg_edx) << to_hex_le(reg_ebx)
       << to_hex_le(reg_esp) << to_hex_le(reg_ebp) << to_hex_le(reg_esi) << to_hex_le(reg_edi)
       << to_hex_le(reg_eip) << to_hex_le(reg_flags)
       << to_hex_le(SegValue(SegNames::cs)) << to_hex_le(SegValue(SegNames::ss))
       << to_hex_le(SegValue(SegNames::ds)) << to_hex_le(SegValue(SegNames::es))
       << to_hex_le(SegValue(SegNames::fs)) << to_hex_le(SegValue(SegNames::gs));
    return ss.str();
}

// Get screen text in GDB RSP format (hex-encoded JSON-like)
static std::string get_screen_text_gdb() {
    // Read dimensions from BIOS Data Area (BDA)
    uint8_t cols_lo, cols_hi, rows_minus1;
    mem_readb_checked(0x44A, &cols_lo);
    mem_readb_checked(0x44B, &cols_hi);
    mem_readb_checked(0x484, &rows_minus1);
    
    int cols = cols_lo | (cols_hi << 8);
    int rows = rows_minus1 + 1;
    
    // Sanity checks
    if (cols <= 0 || cols > 160) cols = 80;
    if (rows <= 0 || rows > 60) rows = 25;
    
    // Check video mode
    uint8_t video_mode;
    mem_readb_checked(0x449, &video_mode);
    PhysPt video_base = 0xB8000;  // Default to color text
    if (video_mode == 0x07) {
        video_base = 0xB0000;  // Monochrome
    }
    
    // Build screen text
    std::string screen_text;
    std::vector<std::string> lines;
    
    for (int row = 0; row < rows; row++) {
        std::string line;
        for (int col = 0; col < cols; col++) {
            PhysPt cell_addr = video_base + (row * cols + col) * 2;
            uint8_t ch;
            if (mem_readb_checked(cell_addr, &ch)) {
                ch = '?';
            }
            if (ch < 32 || ch > 126) {
                ch = ' ';
            }
            line += (char)ch;
        }
        
        // Trim trailing whitespace
        size_t last_non_ws = line.find_last_not_of(" \0");
        if (last_non_ws != std::string::npos) {
            line = line.substr(0, last_non_ws + 1);
        } else {
            line.clear();
        }
        
        lines.push_back(line);
    }
    
    // Remove trailing empty lines
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    
    // Build result with newlines
    for (size_t i = 0; i < lines.size(); i++) {
        screen_text += lines[i];
        if (i < lines.size() - 1) {
            screen_text += "\n";
        }
    }
    
    // Hex-encode the result for GDB RSP (since it may contain special chars)
    std::ostringstream hex_result;
    hex_result << "text=";
    for (char c : screen_text) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)c);
        hex_result << hex;
    }
    hex_result << ";cols=" << cols << ";rows=" << rows;
    
    return hex_result.str();
}

// Process GDB RSP command
static void process_gdb_command(const std::string& cmd) {
    if (cmd.empty()) {
        send_gdb_packet("");
        return;
    }
    
    char c = cmd[0];
    std::string args = cmd.substr(1);
    
    switch (c) {
        case 'g':  // Read all registers
            send_gdb_packet(get_registers_gdb());
            break;
            
        case 'G':  // Write all registers (not fully implemented - would need parsing)
            send_gdb_packet("OK");
            break;
            
        case 'm': {  // Read memory: m<addr>,<len>
            size_t comma = args.find(',');
            if (comma != std::string::npos) {
                uint32_t addr = (uint32_t)strtoul(args.substr(0, comma).c_str(), nullptr, 16);
                uint32_t len = (uint32_t)strtoul(args.substr(comma + 1).c_str(), nullptr, 16);
                
                std::ostringstream mem_data;
                for (uint32_t i = 0; i < len; i++) {
                    uint8_t byte = mem_readb(addr + i);
                    char hex[3];
                    snprintf(hex, sizeof(hex), "%02x", byte);
                    mem_data << hex;
                }
                send_gdb_packet(mem_data.str());
            } else {
                send_gdb_packet("E01");  // Error
            }
            break;
        }
        
        case 'M': {  // Write memory: M<addr>,<len>:<data>
            size_t comma1 = args.find(',');
            size_t colon = args.find(':');
            if (comma1 != std::string::npos && colon != std::string::npos) {
                uint32_t addr = (uint32_t)strtoul(args.substr(0, comma1).c_str(), nullptr, 16);
                uint32_t len = (uint32_t)strtoul(args.substr(comma1 + 1, colon - comma1 - 1).c_str(), nullptr, 16);
                std::string data = args.substr(colon + 1);
                
                if (data.length() == len * 2) {  // Hex data
                    for (uint32_t i = 0; i < len; i++) {
                        std::string byte_str = data.substr(i * 2, 2);
                        uint8_t byte = (uint8_t)strtoul(byte_str.c_str(), nullptr, 16);
                        mem_writeb(addr + i, byte);
                    }
                    send_gdb_packet("OK");
                } else {
                    send_gdb_packet("E02");  // Error
                }
            } else {
                send_gdb_packet("E03");  // Error
            }
            break;
        }
        
        case 's':  // Single step
            if (!IsDebuggerRunwatch()) {
                exitLoop = false;
                mustCompleteInstruction = true;
                DEBUG_Run(1, true);
                mustCompleteInstruction = false;
                send_gdb_packet("S05");  // SIGTRAP
            } else {
                send_gdb_packet("E04");  // Error: already running
            }
            break;
            
        case 'c':  // Continue
        case 'C':  // Continue with signal (ignore signal)
            if (!IsDebuggerRunwatch()) {
                char runcmd[] = "RUN";
                ParseCommand(runcmd);
                // Don't send response immediately - will send stop packet when breakpoint hits
            } else {
                send_gdb_packet("E05");  // Error: already running
            }
            break;
            
        case 'Z': {  // Set breakpoint: Z<type>,<addr>,<kind>
            size_t comma1 = args.find(',');
            size_t comma2 = args.find(',', comma1 + 1);
            if (comma1 != std::string::npos && comma2 != std::string::npos) {
                int type = (int)strtol(args.substr(0, comma1).c_str(), nullptr, 10);
                uint32_t addr = (uint32_t)strtoul(args.substr(comma1 + 1, comma2 - comma1 - 1).c_str(), nullptr, 16);
                // kind is ignored for software breakpoints
                
                if (type == 0) {  // Software breakpoint
                    CBreakpoint::AddBreakpointByAddr(addr, false);
                    send_gdb_packet("OK");
                } else {
                    send_gdb_packet("");  // Not supported
                }
            } else {
                send_gdb_packet("E06");  // Error
            }
            break;
        }
        
        case 'z': {  // Remove breakpoint: z<type>,<addr>,<kind>
            size_t comma1 = args.find(',');
            size_t comma2 = args.find(',', comma1 + 1);
            if (comma1 != std::string::npos && comma2 != std::string::npos) {
                int type = (int)strtol(args.substr(0, comma1).c_str(), nullptr, 10);
                uint32_t addr = (uint32_t)strtoul(args.substr(comma1 + 1, comma2 - comma1 - 1).c_str(), nullptr, 16);
                
                if (type == 0) {  // Software breakpoint
                    CBreakpoint::DeleteBreakpointByAddr(addr);
                    send_gdb_packet("OK");
                } else {
                    send_gdb_packet("");  // Not supported
                }
            } else {
                send_gdb_packet("E07");  // Error
            }
            break;
        }
        
        case '?':  // Halt reason
            if (IsDebuggerRunwatch()) {
                send_gdb_packet("S05");  // SIGTRAP (running)
            } else {
                send_gdb_packet("S05");  // SIGTRAP (stopped)
            }
            break;
            
        case 'q':  // Query commands
            if (args == "Supported") {
                send_gdb_packet("PacketSize=4096;qXfer:features:read+;qRcmd+");
            } else if (args.find("Xfer:features:read:") == 0) {
                send_gdb_packet("l");  // Empty (no features XML)
            } else if (args.find("Rcmd.") == 0) {
                // GDB monitor command: qRcmd.<hex-encoded-command>
                // GDB hex-encodes the entire monitor command (e.g., "dos_cmd cls" -> hex)
                std::string hex_cmd = args.substr(5);  // Skip "Rcmd."
                std::string command;
                // Decode hex string to ASCII
                for (size_t i = 0; i < hex_cmd.length(); i += 2) {
                    if (i + 1 < hex_cmd.length()) {
                        std::string byte_str = hex_cmd.substr(i, 2);
                        char byte = (char)strtoul(byte_str.c_str(), nullptr, 16);
                        command += byte;
                    }
                }
                
                // Parse command: "dos_cmd <command>" or "text_screen" or "screenshot"
                if (command.find("dos_cmd ") == 0) {
                    // Execute DOS command: monitor dos_cmd <command>
                    std::string dos_command = command.substr(8);  // Skip "dos_cmd "
                    
                    if (dos_command.length() > 255) {
                        send_gdb_packet("E01");  // Command too long
                    } else if (first_shell == nullptr) {
                        send_gdb_packet("E02");  // Shell not initialized
                    } else {
                        // Save error state
                        uint16_t old_errorcode = dos.errorcode;
                        uint8_t old_return_code = dos.return_code;
                        dos.errorcode = 0;
                        
                        // Execute command
                        char cmd_buffer[CMD_MAXLINE];
                        strncpy(cmd_buffer, dos_command.c_str(), CMD_MAXLINE - 1);
                        cmd_buffer[CMD_MAXLINE - 1] = 0;
                        first_shell->DoCommand(cmd_buffer);
                        
                        // Return result
                        char result[128];
                        snprintf(result, sizeof(result), "OK;errorcode=%04x;return_code=%02x", 
                                 dos.errorcode, dos.return_code);
                        send_gdb_packet(result);
                    }
                } else if (command == "text_screen") {
                    // Get screen text dump: monitor text_screen
                    std::string screen_text = get_screen_text_gdb();
                    send_gdb_packet(screen_text);
                } else if (command == "screenshot") {
                    // Trigger screenshot: monitor screenshot
                    extern void CAPTURE_ScreenShotEvent(bool pressed);
                    CAPTURE_ScreenShotEvent(true);
                    send_gdb_packet("OK;Screenshot triggered");
                } else if (command.find("bp_int ") == 0) {
                    // Set interrupt breakpoint: monitor bp_int <int_num> [ah=<val>] [al=<val>]
                    // Example: monitor bp_int 0x0E
                    //          monitor bp_int 0x21 ah=0x4C
                    std::string args = command.substr(7);  // Skip "bp_int "
                    
                    // Parse interrupt number (hex or decimal)
                    size_t space = args.find(' ');
                    std::string int_str = (space != std::string::npos) ? args.substr(0, space) : args;
                    uint8_t int_num = (uint8_t)strtoul(int_str.c_str(), nullptr, 0);
                    
                    // Parse optional AH/AL values
                    uint16_t ah_val = 0x100;  // 0x100 = BPINT_ALL
                    uint16_t al_val = 0x100;
                    
                    if (space != std::string::npos) {
                        std::string rest = args.substr(space + 1);
                        // Look for "ah=0xXX" or "ah=XX"
                        size_t ah_pos = rest.find("ah=");
                        if (ah_pos != std::string::npos) {
                            size_t ah_end = rest.find_first_of(" \t", ah_pos + 3);
                            std::string ah_str = (ah_end != std::string::npos) ? 
                                rest.substr(ah_pos + 3, ah_end - ah_pos - 3) : 
                                rest.substr(ah_pos + 3);
                            ah_val = (uint16_t)strtoul(ah_str.c_str(), nullptr, 0);
                        }
                        
                        // Look for "al=0xXX" or "al=XX"
                        size_t al_pos = rest.find("al=");
                        if (al_pos != std::string::npos) {
                            size_t al_end = rest.find_first_of(" \t", al_pos + 3);
                            std::string al_str = (al_end != std::string::npos) ? 
                                rest.substr(al_pos + 3, al_end - al_pos - 3) : 
                                rest.substr(al_pos + 3);
                            al_val = (uint16_t)strtoul(al_str.c_str(), nullptr, 0);
                        }
                    }
                    
                    // Set interrupt breakpoint
                    CBreakpoint::AddIntBreakpoint(int_num, ah_val, al_val, false);
                    
                    char result[128];
                    snprintf(result, sizeof(result), "OK;INT %02X breakpoint set", int_num);
                    if (ah_val != 0x100) {
                        char temp[64];
                        snprintf(temp, sizeof(temp), ";AH=%02X", ah_val);
                        strcat(result, temp);
                    }
                    if (al_val != 0x100) {
                        char temp[64];
                        snprintf(temp, sizeof(temp), ";AL=%02X", al_val);
                        strcat(result, temp);
                    }
                    send_gdb_packet(result);
                } else if (command == "bp_list_int") {
                    // List all interrupt breakpoints: monitor bp_list_int
                    // Use ShowList which prints to debug console
                    CBreakpoint::ShowList();
                    send_gdb_packet("OK;Breakpoint list printed to debug console");
                } else {
                    send_gdb_packet("E03");  // Unknown monitor command
                }
            } else {
                send_gdb_packet("");  // Unknown query
            }
            break;
            
        case '\x03':  // Ctrl+C - break
            if (IsDebuggerRunwatch()) {
                exitLoop = true;
                send_gdb_packet("S05");  // SIGTRAP
            } else {
                send_gdb_packet("S05");  // Already stopped
            }
            break;
            
        default:
            send_gdb_packet("");  // Empty response = not supported
            break;
    }
}

// Process a command
static void process_command(const std::string& json) {
    std::string cmd;
    if (!json_get_string(json, "cmd", cmd)) {
        send_error("Missing 'cmd' field");
        return;
    }

    if (cmd == "status") {
        std::string state = IsDebuggerRunwatch() ? "running" : "stopped";
        send_ok(json_str("state", state) + "," + json_num("port", socket_port));
        return;
    }

    if (cmd == "last_stop") {
        if (!last_stop_event_json.empty()) {
            send_response(last_stop_event_json);
        } else {
            send_error("No latched stop event");
        }
        return;
    }

    if (cmd == "break") {
        if (IsDebuggerRunwatch()) {
            exitLoop = true;
            send_ok(json_str("msg", "Break requested"));
        } else {
            send_ok(json_str("msg", "Already stopped"));
        }
        return;
    }

    if (cmd == "continue" || cmd == "run") {
        if (socket_freeze_wait) {
            // We are blocked in-place inside the CPU core. Just release the
            // freeze; the original core invocation resumes transparently. Do
            // NOT run the RUN trampoline / swap loops / touch cycles.
            last_stop_event_json.clear();
            socket_freeze_wait = false;
            send_ok(json_str("msg", "Continuing"));
        } else if (!IsDebuggerRunwatch()) {
            last_stop_event_json.clear();
            char runcmd[] = "RUN";
            ParseCommand(runcmd);
            send_ok(json_str("msg", "Continuing"));
        } else {
            send_ok(json_str("msg", "Already running"));
        }
        return;
    }

    if (cmd == "step") {
        if (socket_freeze_wait) {
            // Freeze-model path: arm one-instruction step and release the freeze.
            // The pre-instruction guard in CPU_Core_Normal_Run decrements the
            // counter each iteration: arm=2→1 (execute), 1→0 (re-freeze "step").
            socket_step_arm = 2;
            last_stop_event_json.clear();
            socket_freeze_wait = false;
            send_ok(json_str("msg", "Stepping"));
        } else if (!IsDebuggerRunwatch()) {
            // Legacy path (GUI/terminal debug loop stopped state).
            exitLoop = false;
            mustCompleteInstruction = true;
            DEBUG_Run(1, true);
            mustCompleteInstruction = false;
            send_ok(json_str("msg", "Stepped"));
        } else {
            send_error("Cannot step while running - break first");
        }
        return;
    }

    if (cmd == "step_over") {
        if (socket_freeze_wait) {
            // Freeze-model path: for call/int/loop/rep set a one-shot linear_exec
            // breakpoint at the return point (freeze-safe resume), otherwise single-step.
            uint32_t next_lin = GetStepOverLinear();
            if (next_lin != 0) {
                LinearExecBreakpoint bp;
                bp.linear = next_lin;
                bp.once = true;
                bp.active = true;
                bp.suppress_current = false;
                linear_exec_breakpoints.push_back(bp);
                socket_freeze_wait = false;
                send_ok(json_str("msg", "Stepping over"));
            } else {
                socket_step_arm = 2;
                socket_freeze_wait = false;
                send_ok(json_str("msg", "Stepping over"));
            }
        } else if (!IsDebuggerRunwatch()) {
            // Legacy path.
            if (DoStepOver()) {
                mustCompleteInstruction = true;
                inhibit_int_breakpoint = true;
                DEBUG_Run(1, false);
                inhibit_int_breakpoint = false;
                mustCompleteInstruction = false;
            } else {
                exitLoop = false;
                mustCompleteInstruction = true;
                DEBUG_Run(1, true);
                mustCompleteInstruction = false;
            }
            send_ok(json_str("msg", "Stepped over"));
        } else {
            send_error("Cannot step while running - break first");
        }
        return;
    }

    if (cmd == "regs") {
        send_ok(get_registers_json());
        return;
    }

    if (cmd == "regs_set") {
        std::string reg;
        long long val;
        if (!json_get_string(json, "reg", reg) || !json_get_int(json, "val", val)) {
            send_error("Missing 'reg' or 'val'");
            return;
        }
        // Set register
        if (reg == "EAX") reg_eax = (uint32_t)val;
        else if (reg == "EBX") reg_ebx = (uint32_t)val;
        else if (reg == "ECX") reg_ecx = (uint32_t)val;
        else if (reg == "EDX") reg_edx = (uint32_t)val;
        else if (reg == "ESI") reg_esi = (uint32_t)val;
        else if (reg == "EDI") reg_edi = (uint32_t)val;
        else if (reg == "EBP") reg_ebp = (uint32_t)val;
        else if (reg == "ESP") reg_esp = (uint32_t)val;
        else if (reg == "EIP") reg_eip = (uint32_t)val;
        else if (reg == "AX") reg_ax = (uint16_t)val;
        else if (reg == "BX") reg_bx = (uint16_t)val;
        else if (reg == "CX") reg_cx = (uint16_t)val;
        else if (reg == "DX") reg_dx = (uint16_t)val;
        else if (reg == "AL") reg_al = (uint8_t)val;
        else if (reg == "AH") reg_ah = (uint8_t)val;
        else if (reg == "BL") reg_bl = (uint8_t)val;
        else if (reg == "BH") reg_bh = (uint8_t)val;
        else if (reg == "CL") reg_cl = (uint8_t)val;
        else if (reg == "CH") reg_ch = (uint8_t)val;
        else if (reg == "DL") reg_dl = (uint8_t)val;
        else if (reg == "DH") reg_dh = (uint8_t)val;
        else {
            send_error("Unknown register");
            return;
        }
        send_ok();
        return;
    }

    if (cmd == "bp_set") {
        long long seg = -1, off = -1, addr_linear = -1;
        
        // Check for linear address first, then seg:off
        if (json_get_int(json, "addr", addr_linear)) {
            // Linear/physical address breakpoint - use internal API directly
            PhysPt phys_addr = (PhysPt)addr_linear;
            CBreakpoint::AddBreakpointByAddr(phys_addr, false);
            send_ok(json_str("msg", "Breakpoint set") + "," + json_hex("addr", phys_addr));
            return;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            // seg:off format - use existing ParseCommand interface
            char bpcmd[64];
            snprintf(bpcmd, sizeof(bpcmd), "BP %04X:%08X", (uint16_t)seg, (uint32_t)off);
            ParseCommand(bpcmd);
            send_ok(json_str("msg", "Breakpoint set"));
            return;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }
    }

    if (cmd == "bp_set_linear_exec") {
        long long linear = -1;
        long long once_val = 0;
        long long match_seg_val = -1;
        long long match_off_val = -1;
        if (!json_get_int(json, "linear", linear)) {
            send_error("Missing 'linear' address");
            return;
        }
        json_get_int(json, "once", once_val);
        const bool has_match_seg = json_get_int(json, "match_seg", match_seg_val);
        const bool has_match_off = json_get_int(json, "match_off", match_off_val);
        uint32_t linear_u32 = (uint32_t)linear;
        add_linear_exec_breakpoint(
            linear_u32,
            once_val != 0,
            has_match_seg,
            (uint16_t)match_seg_val,
            has_match_off,
            (uint32_t)match_off_val
        );
        send_ok(json_str("msg", "Linear execution breakpoint set") + "," +
                json_hex("linear", linear_u32) + "," +
                json_bool("has_match_seg", has_match_seg) + "," +
                json_hex("match_seg", (uint16_t)match_seg_val) + "," +
                json_bool("has_match_off", has_match_off) + "," +
                json_hex("match_off", (uint32_t)match_off_val) + "," +
                json_bool("once", once_val != 0));
        return;
    }

    if (cmd == "bp_clear_linear_exec") {
        long long linear = -1;
        if (!json_get_int(json, "linear", linear)) {
            send_error("Missing 'linear' address");
            return;
        }
        uint32_t linear_u32 = (uint32_t)linear;
        if (clear_linear_exec_breakpoint(linear_u32)) {
            send_ok(json_str("msg", "Linear execution breakpoint cleared") + "," +
                    json_hex("linear", linear_u32));
        } else {
            send_error("Linear execution breakpoint not found");
        }
        return;
    }

    if (cmd == "bp_clear") {
        long long seg = -1, off = -1, addr_linear = -1;
        
        // Check for linear address first, then seg:off
        if (json_get_int(json, "addr", addr_linear)) {
            // Linear/physical address breakpoint - use internal API directly
            PhysPt phys_addr = (PhysPt)addr_linear;
            bool deleted = CBreakpoint::DeleteBreakpointByAddr(phys_addr);
            if (deleted) {
                send_ok(json_str("msg", "Breakpoint cleared") + "," + json_hex("addr", phys_addr));
            } else {
                send_error("Breakpoint not found at address");
            }
            return;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            // seg:off format - use existing ParseCommand interface
            char bpcmd[64];
            snprintf(bpcmd, sizeof(bpcmd), "BPDEL %04X:%08X", (uint16_t)seg, (uint32_t)off);
            ParseCommand(bpcmd);
            send_ok(json_str("msg", "Breakpoint cleared"));
            return;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }
    }

    if (cmd == "bp_list") {
        std::string arr = CBreakpoint::ToJSON();
        // Wrap in a response with a count field
        long long count = 0;
        for (size_t i = 0; i < arr.size(); i++)
            if (arr[i] == '{') count++;
        send_ok("\"breakpoints\":" + arr + "," +
                "\"linear_exec_breakpoints\":" + linear_exec_breakpoints_json() + "," +
                json_num("count", count + (long long)linear_exec_breakpoints.size()));
        return;
    }

    if (cmd == "bp_clear_all") {
        CBreakpoint::DeleteAll();
        linear_exec_breakpoints.clear();
        send_ok(json_str("msg", "All breakpoints cleared"));
        return;
    }

    if (cmd == "bp_on_load") {
        // Break when next program is loaded (before it starts executing)
        // Uses DOSBox-X's built-in debugger_break_on_exec mechanism
        extern bool debugger_break_on_exec;
        debugger_break_on_exec = true;
        send_ok(json_str("msg", "Will break at entry point when next program loads"));
        return;
    }

    if (cmd == "bp_on_load_clear") {
        // Cancel break-on-load
        extern bool debugger_break_on_exec;
        debugger_break_on_exec = false;
        send_ok(json_str("msg", "Break on program load disabled"));
        return;
    }

    if (cmd == "bp_on_load_status") {
        // Check if break-on-load is enabled
        extern bool debugger_break_on_exec;
        send_ok(json_bool("enabled", debugger_break_on_exec));
        return;
    }

    if (cmd == "catch_int3" || cmd == "bp_int3") {
        // Convenience command to catch INT 3 (software breakpoint instruction)
        // INT 3 is the one-byte breakpoint instruction (opcode 0xCC)
        // This is what debuggers typically use to set breakpoints in code
        CBreakpoint::AddIntBreakpoint(3, 0x100, 0x100, false);  // BPINT_ALL for AH/AL
        send_ok(json_str("msg", "INT 3 breakpoint enabled - will catch INT 3 instructions"));
        return;
    }

    if (cmd == "catch_int3_clear" || cmd == "bp_int3_clear") {
        // Clear INT 3 interrupt breakpoint using debugger command
        char cmd_str[] = "BPINTCLEAR 03";
        ParseCommand(cmd_str);
        send_ok(json_str("msg", "INT 3 breakpoint clear command executed"));
        return;
    }

    if (cmd == "bp_enable" || cmd == "bp_disable") {
        // bp_enable/disable by index is not reliably supported via the socket
        // (inline getter methods are not exported).  Use bp_clear + bp_set instead,
        // or use the DOSBox debugger console directly.
        send_error("bp_enable/disable not supported via socket; use bp_clear+bp_set or the debugger console");
        return;
    }

    if (cmd == "get_linear_addr") {
        // Get current linear address from CS:EIP
        uint16_t cs = SegValue(SegNames::cs);
        uint32_t eip = reg_eip;
        uint64_t linear = GetAddress(cs, eip);
        send_ok(json_hex("linear_addr", linear) + "," + 
                json_num("CS", cs) + "," + json_hex("EIP", eip));
        return;
    }

    if (cmd == "mem_read") {
        long long seg, off, len, addr_linear;
        if (!json_get_int(json, "len", len)) {
            send_error("Missing 'len'");
            return;
        }
        if (len > 4096) len = 4096; // Limit
        
        PhysPt addr;
        // Check for linear address first, then seg:off
        if (json_get_int(json, "addr", addr_linear)) {
            addr = (PhysPt)addr_linear;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            addr = ((uint32_t)seg << 4) + (uint32_t)off;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }
        
        std::string hex;
        for (int i = 0; i < len; i++) {
            uint8_t val;
            if (mem_readb_checked(addr + i, &val)) {
                hex += "??";
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", val);
                hex += buf;
            }
        }
        send_ok(json_str("data", hex));
        return;
    }

    if (cmd == "mem_write") {
        long long seg, off, addr_linear;
        std::string data;
        if (!json_get_string(json, "data", data)) {
            send_error("Missing 'data'");
            return;
        }
        
        PhysPt addr;
        if (json_get_int(json, "addr", addr_linear)) {
            addr = (PhysPt)addr_linear;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            addr = ((uint32_t)seg << 4) + (uint32_t)off;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }
        
        for (size_t i = 0; i + 1 < data.length(); i += 2) {
            uint8_t val = (uint8_t)strtol(data.substr(i, 2).c_str(), nullptr, 16);
            phys_writeb(addr + i/2, val);
        }
        send_ok();
        return;
    }

    if (cmd == "disasm") {
        long long seg, off, count, addr_linear;
        if (!json_get_int(json, "count", count)) count = 10;
        if (count > 100) count = 100;

        uint32_t linear;
        uint32_t current_off = 0;
        bool use_linear = false;
        bool bit32 = cpu.code.big;
        
        if (json_get_int(json, "addr", addr_linear)) {
            linear = (uint32_t)addr_linear;
            use_linear = true;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            if (!selector_decode_info((uint16_t)seg, (uint32_t)off, linear, bit32)) {
                send_error("Invalid, non-present, or non-code selector");
                return;
            }
            current_off = (uint32_t)off;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }

        std::string result = "\"lines\":[";
        bool first = true;
        
        for (int i = 0; i < count; i++) {
            char buffer[256];
            const uint32_t shown_ip = use_linear ? linear : current_off;
            Bitu inst_len = DasmI386(buffer, (PhysPt)linear, shown_ip, bit32);
            if (inst_len == 0) inst_len = 1;
            
            if (!first) result += ",";
            first = false;
            
            char addrstr[32];
            if (use_linear) {
                snprintf(addrstr, sizeof(addrstr), "%08X", linear);
            } else {
                snprintf(addrstr, sizeof(addrstr), "%04X:%08X", (uint16_t)seg, current_off);
            }
            
            // Read instruction bytes
            std::string bytes_hex;
            for (Bitu b = 0; b < inst_len && b < 15; b++) {
                uint8_t value;
                if (mem_readb_checked((LinearPt)(linear + b), &value)) {
                    bytes_hex += "??";
                } else {
                    char byte_str[4];
                    snprintf(byte_str, sizeof(byte_str), "%02X", value);
                    bytes_hex += byte_str;
                }
            }
            
            result += "{" + json_str("addr", addrstr) + "," + 
                      json_str("bytes", bytes_hex.c_str()) + "," +
                      json_num("len", (long long)inst_len) + "," +
                      json_str("mnemonic", buffer) + "}";
            linear += (uint32_t)inst_len;
            if (!use_linear) current_off += (uint32_t)inst_len;
        }
        result += "]";
        send_ok(result);
        return;
    }

    if (cmd == "disasm_context") {
        // Disassemble N before + M after a linear address using back-disassembly.
        // Response: {"status":"ok","lines":[...],"current_idx":N}
        // lines[current_idx] is the instruction at `addr`.
        long long addr_linear, before_ll, after_ll;
        if (!json_get_int(json, "addr", addr_linear)) {
            send_error("Need 'addr' (linear)");
            return;
        }
        if (!json_get_int(json, "before", before_ll)) before_ll = 3;
        if (!json_get_int(json, "after",  after_ll))  after_ll  = 10;
        if (before_ll > 20) before_ll = 20;
        if (after_ll  > 50) after_ll  = 50;
        int before_count = (int)before_ll;
        int after_count  = (int)after_ll;

        PhysPt pc = (PhysPt)(uint32_t)addr_linear;

        // Back-disassembly: scan forward from pc-64, recording each instruction.
        // Find the rightmost instruction that ends exactly at pc.
        const uint32_t LOOKBACK = 64;
        PhysPt scan_start = (pc > LOOKBACK) ? pc - LOOKBACK : 0;
        const uint16_t cur_cs = SegValue(SegNames::cs);
        const PhysPt cur_pc = (PhysPt)GetAddress(cur_cs, reg_eip);
        if (!cpu.pmode && pc == cur_pc && cur_cs < 0xF000 && reg_eip >= 0x100) {
            // DOS .COM programs begin at CS:0100; bytes before that are the PSP,
            // not code. Clamping avoids bogus back-disassembly through PSP zeros.
            const PhysPt com_start = (PhysPt)GetAddress(cur_cs, 0x100);
            if (scan_start < com_start) scan_start = com_start;
        }

        struct InsnRecord { PhysPt addr; Bitu len; char mnemonic[256]; };
        std::vector<InsnRecord> scan;
        scan.reserve(32);

        PhysPt cur = scan_start;
        while (cur < pc) {
            InsnRecord rec;
            rec.addr = cur;
            rec.len  = DasmI386(rec.mnemonic, cur, cur, cpu.code.big);
            if (rec.len == 0) rec.len = 1;
            scan.push_back(rec);
            cur += rec.len;
        }

        // Find the last record whose end == pc.
        int aligned_idx = -1;
        for (int i = (int)scan.size() - 1; i >= 0; i--) {
            if (scan[i].addr + scan[i].len == pc) {
                aligned_idx = i;
                break;
            }
        }

        int slice_start = (aligned_idx >= 0)
            ? std::max(0, aligned_idx - before_count + 1)
            : (int)scan.size();
        int slice_end = (aligned_idx >= 0) ? aligned_idx + 1 : (int)scan.size();

        // Helper: serialize one instruction to JSON object.
        auto insn_json = [&](PhysPt a, const char* mnem) -> std::string {
            char addrstr[32];
            snprintf(addrstr, sizeof(addrstr), "%08X", (uint32_t)a);
            char buf[256];
            Bitu len = DasmI386(buf, a, a, cpu.code.big);
            if (len == 0) len = 1;
            char bytes_hex[64]; bytes_hex[0] = '\0';
            for (Bitu b = 0; b < len && b < 15; b++) {
                char bs[4];
                snprintf(bs, sizeof(bs), "%02X", mem_readb(a + b));
                strcat(bytes_hex, bs);
            }
            return "{" + json_str("addr", addrstr) + "," +
                         json_str("bytes", bytes_hex) + "," +
                         json_num("len", (long long)len) + "," +
                         json_str("mnemonic", mnem) + "}";
        };

        std::string lines_json = "\"lines\":[";
        bool first = true;
        int current_idx = 0;

        for (int i = slice_start; i < slice_end; i++) {
            if (!first) lines_json += ",";
            first = false;
            lines_json += insn_json(scan[i].addr, scan[i].mnemonic);
            current_idx++;
        }

        PhysPt a = pc;
        for (int i = 0; i < after_count; i++) {
            if (!first) lines_json += ",";
            first = false;
            char mnem[256];
            Bitu len = DasmI386(mnem, a, a, cpu.code.big);
            if (len == 0) len = 1;
            lines_json += insn_json(a, mnem);
            a += len;
        }
        lines_json += "]";

        send_ok(lines_json + "," + json_num("current_idx", current_idx));
        return;
    }

    if (cmd == "screenshot") {
        // Trigger screenshot capture
        extern void CAPTURE_ScreenShotEvent(bool pressed);
        CAPTURE_ScreenShotEvent(true);
        send_ok(json_str("msg", "Screenshot triggered - check capture folder"));
        return;
    }

    if (cmd == "mem_dump") {
        // Dump memory region to file
        long long seg, off, len, addr_linear;
        std::string filename;
        if (!json_get_int(json, "len", len)) {
            send_error("Missing 'len'");
            return;
        }
        if (!json_get_string(json, "file", filename)) {
            filename = "memdump.bin";
        }
        
        PhysPt addr;
        if (json_get_int(json, "addr", addr_linear)) {
            addr = (PhysPt)addr_linear;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            addr = ((uint32_t)seg << 4) + (uint32_t)off;
        } else {
            send_error("Need 'addr' (linear) or 'seg'+'off'");
            return;
        }
        
        FILE* f = fopen(filename.c_str(), "wb");
        if (!f) {
            send_error("Cannot create dump file");
            return;
        }
        
        for (long long i = 0; i < len; i++) {
            uint8_t val;
            if (mem_readb_checked(addr + i, &val)) {
                val = 0xFF;  // Fill unreadable with FF
            }
            fwrite(&val, 1, 1, f);
        }
        fclose(f);
        
        char msg[256];
        snprintf(msg, sizeof(msg), "Dumped %lld bytes from 0x%08X to %s", len, (unsigned)addr, filename.c_str());
        send_ok(json_str("msg", msg) + "," + json_str("file", filename) + "," + json_num("size", len) + "," + json_hex("addr", addr));
        return;
    }

    if (cmd == "mem_search") {
        // Search for byte pattern in memory range
        // Parameters:
        //   addr: start linear address
        //   len: length to search
        //   pattern: hex string of bytes to find (e.g., "CD21" for INT 21h)
        //   max_results: optional, default 100
        long long start_addr, search_len, max_results;
        std::string pattern;
        
        if (!json_get_int(json, "addr", start_addr)) {
            send_error("Missing 'addr' (start address)");
            return;
        }
        if (!json_get_int(json, "len", search_len)) {
            send_error("Missing 'len' (search length)");
            return;
        }
        if (!json_get_string(json, "pattern", pattern)) {
            send_error("Missing 'pattern' (hex string)");
            return;
        }
        if (!json_get_int(json, "max_results", max_results)) max_results = 100;
        if (max_results > 1000) max_results = 1000;
        if (search_len > 0x10000000) search_len = 0x10000000; // Max 256MB
        
        // Parse pattern hex string into bytes
        std::vector<uint8_t> pattern_bytes;
        for (size_t i = 0; i + 1 < pattern.length(); i += 2) {
            char hex[3] = {pattern[i], pattern[i+1], 0};
            char* end;
            unsigned long val = strtoul(hex, &end, 16);
            if (*end != 0) {
                send_error("Invalid hex pattern");
                return;
            }
            pattern_bytes.push_back((uint8_t)val);
        }
        
        if (pattern_bytes.empty()) {
            send_error("Empty pattern");
            return;
        }
        
        // Search for pattern
        std::string results = "\"matches\":[";
        bool first = true;
        long long found_count = 0;
        PhysPt addr = (PhysPt)start_addr;
        PhysPt end_addr = addr + (PhysPt)search_len - pattern_bytes.size();
        
        while (addr <= end_addr && found_count < max_results) {
            bool match = true;
            for (size_t i = 0; i < pattern_bytes.size() && match; i++) {
                uint8_t val;
                if (mem_readb_checked(addr + i, &val)) {
                    match = false;
                } else if (val != pattern_bytes[i]) {
                    match = false;
                }
            }
            
            if (match) {
                if (!first) results += ",";
                first = false;
                
                char addr_str[16];
                snprintf(addr_str, sizeof(addr_str), "%08X", (uint32_t)addr);
                results += "\"" + std::string(addr_str) + "\"";
                found_count++;
                addr += pattern_bytes.size(); // Skip past this match
            } else {
                addr++;
            }
        }
        results += "]";
        
        send_ok(results + "," + 
                json_num("count", found_count) + "," +
                json_hex("start", (uint32_t)start_addr) + "," +
                json_num("searched", search_len) + "," +
                json_str("pattern", pattern));
        return;
    }

    if (cmd == "bp_search") {
        // Search for byte pattern and set breakpoints at all matches
        // Like mem_search but also sets breakpoints
        // Parameters:
        //   addr: start linear address
        //   len: length to search
        //   pattern: hex string of bytes to find (e.g., "CD21" for INT 21h)
        //   max_results: optional, default 50 (to avoid setting too many breakpoints)
        long long start_addr, search_len, max_results;
        std::string pattern;
        
        if (!json_get_int(json, "addr", start_addr)) {
            send_error("Missing 'addr' (start address)");
            return;
        }
        if (!json_get_int(json, "len", search_len)) {
            send_error("Missing 'len' (search length)");
            return;
        }
        if (!json_get_string(json, "pattern", pattern)) {
            send_error("Missing 'pattern' (hex string)");
            return;
        }
        if (!json_get_int(json, "max_results", max_results)) max_results = 50;
        if (max_results > 200) max_results = 200; // Lower limit than mem_search
        if (search_len > 0x10000000) search_len = 0x10000000; // Max 256MB
        
        // Parse pattern hex string into bytes
        std::vector<uint8_t> pattern_bytes;
        for (size_t i = 0; i + 1 < pattern.length(); i += 2) {
            char hex[3] = {pattern[i], pattern[i+1], 0};
            char* end;
            unsigned long val = strtoul(hex, &end, 16);
            if (*end != 0) {
                send_error("Invalid hex pattern");
                return;
            }
            pattern_bytes.push_back((uint8_t)val);
        }
        
        if (pattern_bytes.empty()) {
            send_error("Empty pattern");
            return;
        }
        
        // Search for pattern and set breakpoints
        std::string results = "\"breakpoints\":[";
        bool first = true;
        long long found_count = 0;
        long long bp_count = 0;
        PhysPt addr = (PhysPt)start_addr;
        PhysPt end_addr = addr + (PhysPt)search_len - pattern_bytes.size();
        
        while (addr <= end_addr && found_count < max_results) {
            bool match = true;
            for (size_t i = 0; i < pattern_bytes.size() && match; i++) {
                uint8_t val;
                if (mem_readb_checked(addr + i, &val)) {
                    match = false;
                } else if (val != pattern_bytes[i]) {
                    match = false;
                }
            }
            
            if (match) {
                if (!first) results += ",";
                first = false;
                
                char addr_str[16];
                snprintf(addr_str, sizeof(addr_str), "%08X", (uint32_t)addr);
                results += "\"" + std::string(addr_str) + "\"";
                
                // Set breakpoint at this address
                CBreakpoint::AddBreakpointByAddr(addr, false);
                bp_count++;
                
                found_count++;
                addr += pattern_bytes.size(); // Skip past this match
            } else {
                addr++;
            }
        }
        results += "]";
        
        send_ok(results + "," + 
                json_num("count", found_count) + "," +
                json_num("bp_set", bp_count) + "," +
                json_hex("start", (uint32_t)start_addr) + "," +
                json_num("searched", search_len) + "," +
                json_str("pattern", pattern));
        return;
    }

    if (cmd == "vga_read") {
        // Read VGA memory directly (for screen capture)
        long long off, len;
        if (!json_get_int(json, "off", off)) off = 0;
        if (!json_get_int(json, "len", len)) len = 0x10000; // 64KB default
        if (len > 0x40000) len = 0x40000; // Max 256KB
        
        // VGA memory starts at 0xA0000
        PhysPt addr = 0xA0000 + (uint32_t)off;
        std::string hex;
        for (long long i = 0; i < len; i++) {
            uint8_t val;
            if (mem_readb_checked(addr + i, &val)) {
                hex += "??";
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", val);
                hex += buf;
            }
        }
        send_ok(json_str("data", hex) + "," + json_num("offset", off) + "," + json_num("len", len));
        return;
    }

    if (cmd == "text_screen") {
        // Dump text mode video memory as parsed text
        // Read dimensions from BIOS Data Area (BDA)
        // 0x40:0x4A (0x44A) = columns (word)
        // 0x40:0x84 (0x484) = rows - 1 (byte, EGA/VGA)
        
        uint8_t cols_lo, cols_hi, rows_minus1;
        mem_readb_checked(0x44A, &cols_lo);
        mem_readb_checked(0x44B, &cols_hi);
        mem_readb_checked(0x484, &rows_minus1);
        
        int cols = cols_lo | (cols_hi << 8);
        int rows = rows_minus1 + 1;
        
        // Sanity checks
        if (cols <= 0 || cols > 160) cols = 80;
        if (rows <= 0 || rows > 60) rows = 25;
        
        // Text mode video memory at 0xB8000 (color) or 0xB0000 (mono)
        // Check video mode from BDA 0x449 to determine base address
        uint8_t video_mode;
        mem_readb_checked(0x449, &video_mode);
        
        PhysPt video_base = 0xB8000;  // Default to color text
        if (video_mode == 0x07) {
            video_base = 0xB0000;  // Monochrome
        }
        
        // Build the screen text
        std::string screen_text;
        std::vector<std::string> lines;
        
        for (int row = 0; row < rows; row++) {
            std::string line;
            for (int col = 0; col < cols; col++) {
                // Each cell is 2 bytes: character + attribute
                PhysPt cell_addr = video_base + (row * cols + col) * 2;
                uint8_t ch;
                if (mem_readb_checked(cell_addr, &ch)) {
                    ch = '?';
                }
                // Convert non-printable ASCII to space for JSON compatibility
                // Keep only printable ASCII (32-126)
                if (ch < 32 || ch > 126) {
                    ch = ' ';
                }
                line += (char)ch;
            }
            
            // Trim trailing whitespace (spaces, nulls)
            size_t last_non_ws = line.find_last_not_of(" \0");
            if (last_non_ws != std::string::npos) {
                line = line.substr(0, last_non_ws + 1);
            } else {
                line.clear();  // All whitespace
            }
            
            lines.push_back(line);
        }
        
        // Remove trailing empty lines
        while (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
        }
        
        // Build result with newlines
        for (size_t i = 0; i < lines.size(); i++) {
            screen_text += lines[i];
            if (i < lines.size() - 1) {
                screen_text += "\n";
            }
        }
        
        send_ok(json_str("text", screen_text) + "," + 
                json_num("cols", cols) + "," + 
                json_num("rows", rows) + "," +
                json_num("video_mode", video_mode) + "," +
                json_hex("video_base", video_base));
        return;
    }

    if (cmd == "dos_cmd" || cmd == "exec") {
        // Execute a DOS command directly via the shell
        // This bypasses keyboard buffer and executes immediately
        // Note: If bp_on_load is enabled and an external program is run,
        // a breakpoint event will be sent via DEBUG_Socket_NotifyBreakpoint
        // before this function returns - the client should handle this.
        std::string command;
        if (!json_get_string(json, "command", command)) {
            send_error("Missing 'command'");
            return;
        }
        
        // Limit command length (DOS command line limit)
        if (command.length() > 255) {
            send_error("Command too long (max 255 chars)");
            return;
        }
        
        // Check if shell is available
        if (first_shell == nullptr) {
            send_error("Shell not initialized");
            return;
        }
        
        // Save current error state
        uint16_t old_errorcode = dos.errorcode;
        uint8_t old_return_code = dos.return_code;
        
        // Clear error state before execution
        dos.errorcode = 0;
        
        // Clear pending breakpoint flag - if bp_on_load triggers,
        // DEBUG_Socket_NotifyBreakpoint will send the event
        g_exec_breakpoint_pending = false;
        
        // Copy command to mutable buffer (DoCommand modifies it)
        char cmd_buffer[CMD_MAXLINE];
        strncpy(cmd_buffer, command.c_str(), CMD_MAXLINE - 1);
        cmd_buffer[CMD_MAXLINE - 1] = 0;
        
        // Execute via shell - this is what DOSBox uses internally for -c option
        // If bp_on_load is active and an external program is loaded,
        // DEBUG_Socket_NotifyBreakpoint will be called and the breakpoint
        // event will be sent BEFORE this function returns.
        first_shell->DoCommand(cmd_buffer);
        
        // If a bp_on_load breakpoint was hit, the notification was already sent
        // and execution stopped. Don't send a duplicate response.
        if (g_exec_breakpoint_pending) {
            g_exec_breakpoint_pending = false;
            // Response already sent by DEBUG_Socket_NotifyBreakpoint
            return;
        }
        
        // Capture results
        uint16_t errorcode = dos.errorcode;
        uint8_t return_code = dos.return_code;
        
        // Build response
        send_ok(json_str("msg", "Command executed") + "," +
                json_num("errorcode", errorcode) + "," +
                json_num("return_code", return_code) + "," +
                json_str("command", command));
        return;
    }

    if (cmd == "key" || cmd == "keypress") {
        // Inject a single key or special key
        std::string key;
        long long scancode = -1, ascii = -1;
        
        if (json_get_string(json, "key", key)) {
            // Named key
            uint16_t keycode = 0;
            if (key == "enter" || key == "return") keycode = 0x1C0D;
            else if (key == "esc" || key == "escape") keycode = 0x011B;
            else if (key == "space") keycode = 0x3920;
            else if (key == "tab") keycode = 0x0F09;
            else if (key == "backspace" || key == "bs") keycode = 0x0E08;
            else if (key == "up") keycode = 0x4800;
            else if (key == "down") keycode = 0x5000;
            else if (key == "left") keycode = 0x4B00;
            else if (key == "right") keycode = 0x4D00;
            else if (key == "home") keycode = 0x4700;
            else if (key == "end") keycode = 0x4F00;
            else if (key == "pgup" || key == "pageup") keycode = 0x4900;
            else if (key == "pgdn" || key == "pagedown") keycode = 0x5100;
            else if (key == "insert" || key == "ins") keycode = 0x5200;
            else if (key == "delete" || key == "del") keycode = 0x5300;
            else if (key == "f1") keycode = 0x3B00;
            else if (key == "f2") keycode = 0x3C00;
            else if (key == "f3") keycode = 0x3D00;
            else if (key == "f4") keycode = 0x3E00;
            else if (key == "f5") keycode = 0x3F00;
            else if (key == "f6") keycode = 0x4000;
            else if (key == "f7") keycode = 0x4100;
            else if (key == "f8") keycode = 0x4200;
            else if (key == "f9") keycode = 0x4300;
            else if (key == "f10") keycode = 0x4400;
            else if (key == "y") keycode = 0x1579;  // For Y/N prompts
            else if (key == "n") keycode = 0x316E;  // For Y/N prompts
            else if (key.length() == 1) {
                // Single character
                char c = key[0];
                static const uint8_t scancodes[128] = {
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x0E, 0x0F, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                    0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28,
                    0x0A, 0x0B, 0x09, 0x0D, 0x33, 0x0C, 0x34, 0x35,
                    0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                    0x09, 0x0A, 0x27, 0x27, 0x33, 0x0D, 0x34, 0x35,
                    0x03, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,
                    0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
                    0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,
                    0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x07, 0x0C,
                    0x29, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,
                    0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
                    0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,
                    0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x29, 0x00
                };
                if (c >= 0 && c < 128) {
                    keycode = ((uint16_t)scancodes[(int)c] << 8) | (uint8_t)c;
                }
            }
            
            if (keycode != 0) {
                // Use phys_read/write to bypass guest page tables (DPMI write-
                // protects low memory; mem_writew would trigger a page fault).
                const PhysPt tail_addr = 0x41C;  // BIOS_KEYBOARD_BUFFER_TAIL
                const PhysPt head_addr = 0x41A;  // BIOS_KEYBOARD_BUFFER_HEAD
                uint16_t tail = phys_readw(tail_addr);
                uint16_t head = phys_readw(head_addr);
                uint16_t ttail = tail + 2;
                if (ttail >= 0x3e) ttail = 0x1e;
                if (ttail != head) {
                    phys_writew(0x400 + tail, keycode);   // write into ring buffer
                    phys_writew(tail_addr, ttail);         // advance tail
                }
                send_ok(json_str("msg", "Key injected") + "," + json_str("key", key));
            } else {
                send_error("Unknown key name");
            }
            return;
        }
        
        // Raw scancode/ascii
        if (json_get_int(json, "scancode", scancode) && json_get_int(json, "ascii", ascii)) {
            uint16_t keycode = ((uint16_t)scancode << 8) | ((uint8_t)ascii);
            BIOS_AddKeyToBuffer(keycode);
            send_ok(json_str("msg", "Key injected") + "," + json_hex("keycode", keycode));
            return;
        }
        
        send_error("Need 'key' name or 'scancode'+'ascii'");
        return;
    }

    if (cmd == "key_hw") {
        // Inject a key via the hardware 8042 keyboard controller simulation.
        // Unlike "key" (which writes to BIOS buffer), this goes through
        // KEYBOARD_AddKey → sets the 8042 OBF bit so port-0x64 polling detects it.
        std::string key;
        if (!json_get_string(json, "key", key)) {
            send_error("Need 'key' field");
            return;
        }
        KBD_KEYS kbd_key = KBD_NONE;
        if (key == "enter" || key == "return") kbd_key = KBD_enter;
        else if (key == "esc" || key == "escape") kbd_key = KBD_esc;
        else if (key == "space") kbd_key = KBD_space;
        else if (key == "a") kbd_key = KBD_a;
        else if (key == "b") kbd_key = KBD_b;
        else if (key == "c") kbd_key = KBD_c;
        else if (key == "y") kbd_key = KBD_y;
        else if (key == "n") kbd_key = KBD_n;
        if (kbd_key == KBD_NONE) {
            send_error("Unknown key name for key_hw");
            return;
        }
        // Simulate press then release
        KEYBOARD_AddKey(kbd_key, true);
        KEYBOARD_AddKey(kbd_key, false);
        send_ok(json_str("msg", "HW key injected") + "," + json_str("key", key));
        return;
    }

    if (cmd == "get_idtr") {
        // Get IDTR (Interrupt Descriptor Table Register) - base and limit
        Bitu idt_base = CPU_SIDT_base();
        Bitu idt_limit = CPU_SIDT_limit();
        send_ok(json_hex("base", idt_base) + "," + json_hex("limit", idt_limit));
        return;
    }

    if (cmd == "get_idt_entry") {
        // Get IDT entry for a specific interrupt number
        long long int_num;
        if (!json_get_int(json, "int", int_num)) {
            send_error("Missing 'int' (interrupt number)");
            return;
        }
        if (int_num < 0 || int_num > 255) {
            send_error("Interrupt number must be 0-255");
            return;
        }

        // Read IDT descriptor
        Descriptor gate;
        if (!cpu.idt.GetDescriptor((Bitu)(int_num << 3), gate)) {
            send_error("Failed to read IDT entry");
            return;
        }

        // Get handler address from gate descriptor
        Bitu gate_sel = gate.GetSelector();
        Bitu gate_off = gate.GetOffset();
        
        // Calculate linear address of handler
        // For interrupt gates, we need to resolve the selector to get the base
        // For now, return selector:offset and let the caller resolve it
        send_ok(json_hex("selector", gate_sel) + "," + 
                json_hex("offset", gate_off) + "," +
                json_num("int_num", int_num));
        return;
    }

    if (cmd == "catch_int" || cmd == "bp_int") {
        // Set interrupt breakpoint (catches INT instruction or exception)
        // This uses the existing interrupt breakpoint system which works for both
        // software interrupts (INT instruction) and CPU exceptions (page fault, GPF, etc.)
        long long int_num;
        long long ah_val = -1, al_val = -1;
        
        if (!json_get_int(json, "int", int_num)) {
            send_error("Missing 'int' (interrupt number)");
            return;
        }
        if (int_num < 0 || int_num > 255) {
            send_error("Interrupt number must be 0-255");
            return;
        }
        
        // Optional: filter by AH/AL values (for INT 21h, etc.)
        json_get_int(json, "ah", ah_val);
        json_get_int(json, "al", al_val);
        
        // Use BPINT_ALL (0x100) if not specified
        uint16_t ah = (ah_val >= 0) ? (uint16_t)ah_val : 0x100;
        uint16_t al = (al_val >= 0) ? (uint16_t)al_val : 0x100;
        
        // Add interrupt breakpoint
        CBreakpoint::AddIntBreakpoint((uint8_t)int_num, ah, al, false);
        
        std::ostringstream msg;
        msg << "Interrupt breakpoint set for INT " << std::hex << int_num;
        if (ah_val >= 0) msg << " AH=" << ah_val;
        if (al_val >= 0) msg << " AL=" << al_val;
        
        send_ok(json_str("msg", msg.str()) + "," +
                json_num("int_num", int_num) + "," +
                json_num("ah", ah_val >= 0 ? ah_val : -1) + "," +
                json_num("al", al_val >= 0 ? al_val : -1));
        return;
    }

    if (cmd == "catch_int_handler") {
        // Set breakpoint at interrupt handler entry point (alternative to catch_int)
        // This sets a breakpoint at the handler address, not on the INT instruction
        long long int_num;
        if (!json_get_int(json, "int", int_num)) {
            send_error("Missing 'int' (interrupt number)");
            return;
        }
        if (int_num < 0 || int_num > 255) {
            send_error("Interrupt number must be 0-255");
            return;
        }

        // Read IDT entry
        Descriptor gate;
        if (!cpu.idt.GetDescriptor((Bitu)(int_num << 3), gate)) {
            send_error("Failed to read IDT entry");
            return;
        }

        Bitu gate_sel = gate.GetSelector();
        Bitu gate_off = gate.GetOffset();
        
        // Resolve selector to linear address
        // For interrupt gates in protected mode, we need to resolve the CS selector
        Descriptor cs_desc;
        if (!cpu.gdt.GetDescriptor(gate_sel, cs_desc)) {
            send_error("Failed to resolve CS selector from gate");
            return;
        }
        
        PhysPt handler_base = cs_desc.GetBase();
        PhysPt handler_addr = handler_base + gate_off;
        
        // Set breakpoint at handler address
        CBreakpoint::AddBreakpointByAddr(handler_addr, false);
        send_ok(json_str("msg", "Breakpoint set at interrupt handler") + "," +
                json_num("int_num", int_num) + "," +
                json_hex("handler_addr", handler_addr) + "," +
                json_hex("selector", gate_sel) + "," +
                json_hex("offset", gate_off));
        return;
    }

    if (cmd == "selinfo") {
        // Return GDT/LDT descriptor info for a selector value or name.
        // sel can be a number (0x1C, 28, …) or a segment name (cs/ds/es/ss/fs/gs).
        long long sel_val = -1;
        std::string sel_name;

        if (json_get_string(json, "sel", sel_name)) {
            // Named register
            if      (sel_name == "cs" || sel_name == "CS") sel_val = SegValue(SegNames::cs);
            else if (sel_name == "ds" || sel_name == "DS") sel_val = SegValue(SegNames::ds);
            else if (sel_name == "es" || sel_name == "ES") sel_val = SegValue(SegNames::es);
            else if (sel_name == "ss" || sel_name == "SS") sel_val = SegValue(SegNames::ss);
            else if (sel_name == "fs" || sel_name == "FS") sel_val = SegValue(SegNames::fs);
            else if (sel_name == "gs" || sel_name == "GS") sel_val = SegValue(SegNames::gs);
            else {
                // Try as hex/decimal number string
                sel_val = strtoll(sel_name.c_str(), nullptr, 0);
            }
        } else if (!json_get_int(json, "sel", sel_val)) {
            send_error("Need 'sel': selector value (number) or name (cs/ds/es/ss/fs/gs)");
            return;
        }

        // Determine table from TI bit: bit 2 of selector = 0→GDT, 1→LDT
        bool is_ldt_sel = ((uint32_t)sel_val & 0x04) != 0;
        const char* table = is_ldt_sel ? "ldt" : "gdt";

        Descriptor desc;
        if (!cpu.gdt.GetDescriptor((Bitu)sel_val, desc)) {
            send_error("Selector not found (GetDescriptor failed)");
            return;
        }

        uint8_t type_byte = desc.saved.seg.type;
        bool is_system   = !(type_byte & 0x10);  // S bit: 0=system, 1=code/data
        bool is_code     = !is_system && (type_byte & 0x08);
        bool present     = desc.saved.seg.p != 0;
        uint8_t dpl      = desc.saved.seg.dpl;
        bool big         = desc.saved.seg.big != 0;    // D/B bit
        bool granularity = desc.saved.seg.g != 0;      // G bit (0=byte, 1=4KB)
        uint32_t base    = (uint32_t)desc.GetBase();
        uint32_t limit   = (uint32_t)desc.GetLimit();
        const char* seg_type =
            is_system ? "system" : (is_code ? "code" : "data");

        send_ok(
            json_hex("sel",         (uint32_t)sel_val)  + "," +
            json_str("table",       table)               + "," +
            json_hex("base",        base)                + "," +
            json_hex("limit",       limit)               + "," +
            json_num("dpl",         (long long)dpl)      + "," +
            json_str("seg_type",    seg_type)            + "," +
            json_num("type_byte",   (long long)type_byte)+ "," +
            json_bool("present",    present)             + "," +
            json_bool("big",        big)                 + "," +
            json_bool("granularity",granularity)
        );
        return;
    }

    if (cmd == "cr_read") {
        // Return control registers: CR0 (mode flags), CR2 (last page fault addr),
        // CR3 (page directory base), CR4.
        // CR0 bits: bit 0=PE (prot mode), bit 31=PG (paging enabled).
        uint32_t cr0 = (uint32_t)cpu.cr0;
        uint32_t cr2 = (uint32_t)paging.cr2;
        uint32_t cr3 = (uint32_t)paging.cr3;
        uint32_t cr4 = (uint32_t)cpu.cr4;
        send_ok(
            json_hex("CR0", cr0) + "," +
            json_hex("CR2", cr2) + "," +
            json_hex("CR3", cr3) + "," +
            json_hex("CR4", cr4) + "," +
            json_bool("PE",     (cr0 & 0x01) != 0) + "," +
            json_bool("PG",     (cr0 & 0x80000000u) != 0) + "," +
            json_bool("paging_enabled", paging.enabled)
        );
        return;
    }

    if (cmd == "mem_read_linear") {
        // Like mem_read but treats 'addr' as a linear (virtual) address and
        // walks the page tables to reach physical memory.  When paging is
        // disabled the result is identical to mem_read.
        long long lin_addr, len;
        if (!json_get_int(json, "addr", lin_addr)) {
            send_error("Missing 'addr' (linear address)");
            return;
        }
        if (!json_get_int(json, "len", len)) {
            send_error("Missing 'len'");
            return;
        }
        if (len > 4096) len = 4096;

        std::string hex;
        for (long long i = 0; i < len; i++) {
            uint32_t phys = 0;
            if (!LinearToPhysical((uint32_t)(lin_addr + i), phys)) {
                hex += "PF";  // Page fault — not present
            } else {
                const uint8_t val = physdev_readb((PhysPt64)phys);
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", val);
                hex += buf;
            }
        }
        send_ok(json_str("data", hex) + "," +
                json_hex("linear_addr", (uint32_t)lin_addr) + "," +
                json_num("len", len) + "," +
                json_bool("paging", paging.enabled));
        return;
    }

    send_error("Unknown command");
}

bool DEBUG_Socket_Init(int port) {
    if (server_socket >= 0) {
        DEBUG_Socket_Shutdown();
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        LOG_MSG("DEBUG_Socket: Failed to create socket");
        return false;
    }

    // Allow reuse
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Non-blocking
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_MSG("DEBUG_Socket: Failed to bind to port %d", port);
        close(server_socket);
        server_socket = -1;
        return false;
    }

    if (listen(server_socket, 1) < 0) {
        LOG_MSG("DEBUG_Socket: Failed to listen");
        close(server_socket);
        server_socket = -1;
        return false;
    }

    socket_port = port;
    LOG_MSG("DEBUG_Socket: Listening on port %d", port);
    return true;
}

void DEBUG_Socket_Shutdown(void) {
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    socket_port = 0;
    recv_buffer.clear();
    gdb_mode = false;
}

bool DEBUG_Socket_CheckCommands(void) {
    if (server_socket < 0) return false;

    // Accept new connections
    if (client_socket < 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket >= 0) {
            // Set non-blocking
            int flags = fcntl(client_socket, F_GETFL, 0);
            fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
            LOG_MSG("DEBUG_Socket: Client connected");
            
            // Reset protocol mode for new connection
            gdb_mode = false;
            
            // Send initial state (JSON format - will switch to GDB if first packet is GDB RSP)
            std::string state = IsDebuggerRunwatch() ? "running" : "stopped";
            send_response("{" + json_str("event", "connected") + "," + json_str("state", state) + "}");
        }
    }

    if (client_socket < 0) return false;

    // Read data
    char buf[1024];
    ssize_t n = recv(client_socket, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = 0;
        recv_buffer += buf;
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Disconnected
        LOG_MSG("DEBUG_Socket: Client disconnected");
        close(client_socket);
        client_socket = -1;
        recv_buffer.clear();
        return false;
    }

    // Process packets (GDB RSP or JSON)
    bool processed = false;
    
    if (gdb_mode) {
        // GDB RSP mode: look for $...#xx packets
        size_t dollar = recv_buffer.find('$');
        if (dollar != std::string::npos) {
            size_t hash = recv_buffer.find('#', dollar + 1);
            if (hash != std::string::npos && hash + 2 < recv_buffer.length()) {
                // Extract packet: $data#xx
                std::string packet_data = recv_buffer.substr(dollar + 1, hash - dollar - 1);
                std::string checksum_str = recv_buffer.substr(hash + 1, 2);
                recv_buffer.erase(0, hash + 3);
                
                // Verify checksum
                uint8_t expected_checksum = (uint8_t)strtoul(checksum_str.c_str(), nullptr, 16);
                uint8_t actual_checksum = gdb_checksum(packet_data);
                
                if (expected_checksum == actual_checksum) {
                    send_gdb_ack(true);  // ACK
                    process_gdb_command(packet_data);
                    processed = true;
                } else {
                    send_gdb_ack(false);  // NAK
                }
            }
        }
    } else {
        // JSON mode: look for newline-delimited JSON
        // Also check if first packet is GDB RSP format to auto-detect
        if (recv_buffer.find('$') == 0) {
            // Looks like GDB RSP - switch mode
            gdb_mode = true;
            LOG_MSG("DEBUG_Socket: Auto-detected GDB RSP protocol");
        } else {
            // Process JSON lines
            size_t pos;
            while ((pos = recv_buffer.find('\n')) != std::string::npos) {
                std::string line = recv_buffer.substr(0, pos);
                recv_buffer.erase(0, pos + 1);
                
                if (!line.empty()) {
                    json_get_id_field(line, current_response_id_json);
                    process_command(line);
                    current_response_id_json.clear();
                    processed = true;
                }
            }
        }
    }

    return processed;
}

void DEBUG_Socket_NotifyBreakpoint(uint16_t seg, uint32_t off) {
    if (client_socket < 0) return;
    if (gdb_mode) {
        // GDB RSP: Send stop packet (S05 = SIGTRAP)
        PhysPt addr = (PhysPt)GetAddress(seg, off);
        char stop_packet[64];
        snprintf(stop_packet, sizeof(stop_packet), "S05;thread:1;core:%08x", addr);
        send_gdb_packet(stop_packet);
    } else {
        // JSON mode
        char addr[32];
        snprintf(addr, sizeof(addr), "%04X:%08X", seg, off);
        send_and_latch_stop("{" + json_str("event", "stopped") + "," +
                            stop_context_json("breakpoint", seg, off) + "," +
                            json_str("addr", addr) + "," + get_registers_json() + "}");
    }
}

void DEBUG_Socket_NotifyInterrupt(uint8_t intNum, uint16_t seg, uint32_t off) {
    if (client_socket < 0) return;
    if (gdb_mode) {
        // GDB RSP: Send stop packet (S05 = SIGTRAP)
        PhysPt addr = (PhysPt)GetAddress(seg, off);
        char stop_packet[64];
        snprintf(stop_packet, sizeof(stop_packet), "S05;thread:1;core:%08x", addr);
        send_gdb_packet(stop_packet);
    } else {
        // JSON mode - include interrupt number
        char addr[32];
        snprintf(addr, sizeof(addr), "%04X:%08X", seg, off);
        send_and_latch_stop("{" + json_str("event", "stopped") + "," +
                            stop_context_json("interrupt", seg, off, intNum) + "," +
                            json_str("addr", addr) + "," + get_registers_json() + "}");
    }
}

void DEBUG_Socket_NotifyStopped(const char* reason) {
    if (client_socket < 0) return;
    if (gdb_mode) {
        // GDB RSP: Send stop packet (S05 = SIGTRAP)
        PhysPt addr = (PhysPt)GetAddress(SegValue(SegNames::cs), reg_eip);
        char stop_packet[64];
        snprintf(stop_packet, sizeof(stop_packet), "S05;thread:1;core:%08x", addr);
        send_gdb_packet(stop_packet);
    } else {
        // JSON mode
        uint16_t seg = SegValue(SegNames::cs);
        uint32_t off = reg_eip;
        send_and_latch_stop("{" + json_str("event", "stopped") + "," +
                            stop_context_json(reason, seg, off) + "," +
                            get_registers_json() + "}");
    }
}

void DEBUG_Socket_RecordException(uint8_t intNum, uint32_t error) {
    last_exception_num = intNum;
    last_exception_error = error;
}

bool DEBUG_Socket_DecrStepArm(void) {
    if (socket_step_arm == 0) return false;
    return (--socket_step_arm == 0);
}

bool DEBUG_Socket_CheckLinearExecBreakpoint(uint16_t seg, uint32_t off) {
    if (linear_exec_breakpoints.empty()) return false;

    uint32_t linear = (uint32_t)GetAddress(seg, off);
    for (auto it = linear_exec_breakpoints.begin(); it != linear_exec_breakpoints.end(); ++it) {
        if (it->linear != linear) {
            it->suppress_current = false;
            continue;
        }
        if (!it->active) continue;
        if (it->has_match_seg && it->match_seg != seg) continue;
        if (it->has_match_off && it->match_off != off) continue;
        if (it->suppress_current) continue;

        if (client_socket >= 0 && !gdb_mode) {
            char addr[32];
            snprintf(addr, sizeof(addr), "%04X:%08X", seg, off);
            send_and_latch_stop("{" + json_str("event", "stopped") + "," +
                                stop_context_json("linear_exec_breakpoint", seg, off) + "," +
                                json_hex("requested_linear", it->linear) + "," +
                                json_str("addr", addr) + "," + get_registers_json() + "}");
        } else if (client_socket >= 0 && gdb_mode) {
            char stop_packet[64];
            snprintf(stop_packet, sizeof(stop_packet), "S05;thread:1;core:%08x", linear);
            send_gdb_packet(stop_packet);
        }

        if (it->once) {
            linear_exec_breakpoints.erase(it);
        } else {
            it->suppress_current = true;
        }
        return true;
    }

    return false;
}

// Block in place until the client issues "continue" (or "break" handling
// clears the flag). Services only socket commands and host events. The caller
// (a CPU-core breakpoint guard) has already emitted the "stopped" event, so the
// client can react. On return, the CPU core resumes from the identical
// instruction boundary with all guest/host state untouched.
//
// Note: we intentionally do NOT exit on client disconnect. The MCP layer opens
// a fresh TCP connection per command, so a disconnect after delivering the
// "stopped" event is normal -- we must keep waiting for an explicit continue
// that may arrive on a subsequent connection.
void DEBUG_Socket_FreezeWait(void) {
    if (server_socket < 0) return;  // socket debugger not active: never block

    // We deliberately do NOT touch the debugger's `debugging`/`debug_running`
    // globals or the loop handler. The CPU core is simply parked here; all
    // socket read commands operate on the live guest state directly.
    socket_freeze_wait = true;
    while (socket_freeze_wait) {
        DEBUG_Socket_CheckCommands();
        GFX_Events();
        usleep(1000);  // 1ms; keeps host responsive without busy-spinning
    }
}

bool DEBUG_Socket_IsActive(void) {
    return server_socket >= 0;
}

int DEBUG_Socket_GetPort(void) {
    return socket_port;
}

#endif // C_DEBUG

