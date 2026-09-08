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
 *   {"cmd":"get_load_info"}   - Get latest DOS EXEC COM/EXE load metadata
 *   {"cmd":"sym","name":"_main"} - Resolve a symbol name to an address. Accepts
 *     "module!name" and matches case-insensitively when the exact name misses.
 *   {"cmd":"sym_list","filter":"draw","limit":100} - List loaded symbols.
 *   {"cmd":"where"} - What is at CS:EIP: symbol, offset into it, source line.
 *     Takes {"linear":X} or {"seg":X,"off":Y} to ask about another address.
 *   {"cmd":"sym_load","file":"/host/path.map","loadSeg":X} - Load symbols from
 *     a host file, either a LINK .MAP or a program with debug info in it. Each
 *     program's symbols load by themselves as DOS EXEC runs it, so this is for
 *     files the guest cannot see.
 *   {"cmd":"sym_clear"} - Drop every symbol, or one program's with "program".
 *   {"cmd":"regs"}            - Get registers
 *   {"cmd":"regs_set","reg":"EAX","val":X} - Set register
 *   {"cmd":"mem_read","seg":X,"off":Y,"len":Z} - Read memory
 *   {"cmd":"alloc_trace","op":"start","file":"/host/path"} - log every DOS
 *     (INT 21h AH=48/49/4A) and EMS (INT 67h AH=43/45/47/48) memory call
 *     with its size and calling CS:IP. {"op":"stop"} closes it.
 *   {"cmd":"mem_dump","seg":X,"off":Y,"len":Z,"file":"/host/path"} - Read memory
 *     straight to a host file (binary). No hex, no response-size limit: for
 *     dumps far larger than mem_read's practical reply size.
 *   {"cmd":"mem_write","seg":X,"off":Y,"data":"hex"} - Write memory
 *   {"cmd":"disasm","seg":X,"off":Y,"count":Z} - Disassemble
 *   {"cmd":"status"}          - Get debugger status
 *   {"cmd":"floppy_swap","drive":"A"} - Cycle to next disk image in swap list
 *   {"cmd":"floppy_swap","drive":"A","image":"/path"} - Replace active image with new file
 *   {"cmd":"floppy_load_list","drive":"A","images":["/abs/Disk01.img","/abs/Disk02.img",...]}
 *     Pre-load a list of images into the drive's swap list (replicates imgmount multi-image
 *     behaviour). First image becomes active. Subsequent floppy_swap (no image) cycles through.
 *
 * Responses (JSON):
 *   {"status":"ok",...}       - Success with optional data
 *   {"status":"error","msg":"..."} - Error
 *
 * Notifications (async):
 *   {"event":"stopped","reason":"breakpoint","seg":X,"off":Y}
 *   {"event":"stopped","reason":"breakpoint",...,"loadInfo":{...}}
 *     loadInfo is present on bp_on_load entry stops when DOS EXEC metadata is
 *     available. Numeric fields are decimal JSON numbers:
 *     program,isCom,pspSeg,loadSeg,loadLinear,entryCS,entryIP,entryLinear,
 *     initialSS,initialSP,initialStackLinear,imageSizeBytes,imageEndLinear,
 *     and for EXE files raw MZ/load facts including mzSignature,mzExtraBytes,
 *     mzPages,headerParagraphs,headerBytes,relocationCount,
 *     relocationTableOffset,initCS,initIP,initSS,initSP,checksum,overlay,
 *     minAlloc,maxAlloc.
 *     Note: DOSBox-X reports the MZ image base and raw MZ loader facts, not
 *     Microsoft LINK segment names. MCP/debug tools should parse the .MAP file
 *     and add each MAP segment start to loadSeg/loadLinear for per-segment
 *     runtime bases.
 *   {"event":"stopped","reason":"step"}
 */

#include "dosbox.h"

#if C_DEBUG

#include "debug_socket.h"
#include "debug_symstore.h"
#include "debug.h"
#include "cpu.h"
#include "regs.h"
#include "paging.h"
#include "mem.h"
#include "bios.h"
#include "bios_disk.h"
#include "shell.h"
#include "dos_inc.h"
#include "../dos/drives.h"

// Forward declarations for CPU functions
extern Bitu CPU_SIDT_base(void);
extern Bitu CPU_SIDT_limit(void);

// Forward declaration for bios_disk.cpp swap helper
extern void swapInDrive(int drive, unsigned int position);
extern int swapInDisksSpecificDrive;

// Forward declarations
extern void DEBUG_ShowMsg(const char *format,...);
Bitu DasmI386(char* buffer, PhysPt pc, uint32_t cur_ip, bool bit32);
#define LOG_MSG DEBUG_ShowMsg
extern bool DOS_Shell_QueueCommandFromDebugger(const char* command);
extern bool DOS_Shell_HasQueuedCommandFromDebugger(void);
extern uint64_t DOS_Shell_DebuggerCommandsQueued(void);
extern uint64_t DOS_Shell_DebuggerCommandsWoken(void);
extern uint64_t DOS_Shell_DebuggerCommandsConsumed(void);
extern uint64_t DOS_Shell_DebuggerCommandsSubmitted(void);
extern const char* DOS_Shell_DebuggerLastConsumedCommand(void);
extern const char* DOS_Shell_DebuggerLastSubmittedCommand(void);

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

// On macOS, MSG_NOSIGNAL is not available; use SO_NOSIGPIPE on the socket instead.
// On Linux we pass MSG_NOSIGNAL to every send() so SIGPIPE is never raised.
#if defined(__APPLE__) || defined(__MACH__)
#define DEBUG_SOCKET_SEND_FLAGS 0
#else
#define DEBUG_SOCKET_SEND_FLAGS MSG_NOSIGNAL
#endif

// Move an fd to a number >= 100 so that it doesn't collide with the low-numbered
// file descriptors opened by disk-image code (imgmount / floppy_load_list).
// On systems where F_DUPFD is unavailable this is a no-op.
static int socket_bump_fd(int fd) {
#if defined(F_DUPFD)
    int high = fcntl(fd, F_DUPFD, 100);
    if (high >= 0) {
        close(fd);
        return high;
    }
#endif
    return fd;
}
#include <string.h>
#include <string>
#include <sstream>
#include <vector>
#include <cstdio>
#include <algorithm>

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
extern void DEBUG_ResumeNormalFromSocket(void);
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
static volatile bool socket_freeze_loop_active = false;
uint32_t debug_socket_normal_core_hooks_active = 0; // set by core_normal.cpp when socket hooks are executing

// Single-step countdown. Set to 2 by the "step" command; the pre-instruction
// guard in CPU_Core_Normal_Run decrements it each pass. When it reaches 1 the
// current instruction executes normally; when it reaches 0 the guard emits a
// "step" stopped event and calls FreezeWait — so exactly one instruction runs.
static volatile int socket_step_arm = 0;

// Forward declarations
bool ParseCommand(char* str);
bool IsDebuggerRunwatch(void);
bool IsDebuggerRunNormal(void);
Bitu DasmI386(char* buffer, PhysPt pc, uint32_t cur_ip, bool bit32);
void On_Software_CPU_Reset(void);

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
static std::string last_fault_stop_event_json;
static std::string current_response_id_json;

// -----------------------------------------------------------------------
// Write watchpoints
// -----------------------------------------------------------------------
struct DebugWatchpoint {
    uint32_t start  = 0;
    uint32_t end    = 0;  // exclusive (start + len)
    bool     active = false;
    uint32_t hit_count = 0;
};
static const int MAX_WATCHPOINTS = 16;
static DebugWatchpoint watchpoints[MAX_WATCHPOINTS];
uint32_t debug_watchpoint_count = 0;  // extern'd in paging.h; checked per write

struct WatchpointHit {
    uint32_t watch_linear = 0;
    uint32_t old_val      = 0;
    uint32_t new_val      = 0;
    uint32_t culprit_cs   = 0;
    uint32_t culprit_eip  = 0;
    uint32_t size         = 0;
};
static volatile bool watchpoint_pending_freeze = false;
static WatchpointHit  watchpoint_hit_context;

// -----------------------------------------------------------------------
// First-chance exception catching
// -----------------------------------------------------------------------
struct ExceptionSkip {
    uint8_t  vec        = 0;
    int      remaining  = 0;   // skips left; -1 = never
};
struct CR2IgnoreRange {
    uint32_t start = 0;
    uint32_t end   = 0;  // inclusive
};
static bool catch_exceptions_armed = false;
static bool catch_exceptions_vectors[32] = {};
static ExceptionSkip catch_exceptions_skip_list[32];
static int  catch_exceptions_skip_count = 0;
static std::vector<CR2IgnoreRange> catch_exceptions_cr2_ignore;
// "nested fault always stops" is implemented by checking is_nested arg

// Latched exception context for the stop event
struct ExceptionHitCtx {
    uint8_t  vec       = 0;
    uint32_t error     = 0;
    uint32_t cr2       = 0;
    uint16_t fault_cs  = 0;
    uint32_t fault_eip = 0;
};
static ExceptionHitCtx exception_hit_ctx;

// -----------------------------------------------------------------------
// Process exit events
// -----------------------------------------------------------------------
struct ProcessExitInfo {
    uint16_t psp       = 0;
    uint8_t  exit_code = 0;
    bool     tsr       = false;
    bool     abnormal  = false;
    bool     valid     = false;
};
static ProcessExitInfo last_process_exit;
static bool break_on_exit = false;

// -----------------------------------------------------------------------
// Branch trace ring buffer
// -----------------------------------------------------------------------
struct BranchEntry {
    uint32_t from_cs;
    uint32_t from_linear;
    uint32_t to_cs;
    uint32_t to_linear;
};
static const int BRANCH_RING_SIZE = 8192;
static BranchEntry branch_ring[BRANCH_RING_SIZE];
static uint32_t branch_ring_head  = 0;   // next write index (mod BRANCH_RING_SIZE)
static uint32_t branch_ring_count = 0;   // total entries ever written (capped at RING_SIZE)
static bool branch_trace_enabled = false;

// -----------------------------------------------------------------------
// Reverse checkpoint ring ("time travel lite")
// -----------------------------------------------------------------------
enum ReverseCheckpointMode {
    REVERSE_CHECKPOINT_BRANCH = 0,
    REVERSE_CHECKPOINT_INSTRUCTION = 1,
};

struct ReverseCpuState {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t esi = 0;
    uint32_t edi = 0;
    uint32_t ebp = 0;
    uint32_t esp = 0;
    uint32_t eip = 0;
    uint32_t flags = 0;
    Bitu seg_val[8] = {};
    PhysPt seg_phys[8] = {};
    PhysPt seg_limit[8] = {};
    bool seg_expanddown[8] = {};
    Bitu cpl = 0;
    Bitu mpl = 0;
    Bitu cr0 = 0;
    Bitu cr4 = 0;
    bool pmode = false;
    bool code_big = false;
    bool stack_big = false;
    uint32_t stack_mask = 0;
    uint32_t stack_notmask = 0;
    Bits direction = 0;
    bool trap_skip = false;
    bool paging_enabled = false;
    bool paging_wp = false;
    uint32_t paging_cr2 = 0;
    uint32_t paging_cr3 = 0;
    PageNum paging_base_page = 0;
    PhysPt paging_base_addr = 0;
};

struct ReverseMemDelta {
    uint32_t linear = 0;
    uint32_t physical = 0;
    uint8_t old_value = 0;
    uint8_t new_value = 0;
};

struct ReverseCheckpoint {
    uint64_t id = 0;
    ReverseCpuState cpu;
    uint32_t cs = 0;
    uint32_t eip = 0;
    uint32_t linear = 0;
    uint32_t from_cs = 0;
    uint32_t from_linear = 0;
    std::vector<ReverseMemDelta> deltas; // writes from this checkpoint to the next
    bool delta_overflow = false;
    uint64_t dropped_writes = 0;
};

static const size_t REVERSE_DEFAULT_MAX_CHECKPOINTS = 512;
static const size_t REVERSE_MIN_CHECKPOINTS = 2;
static const size_t REVERSE_MAX_CHECKPOINTS = 8192;
static const size_t REVERSE_MAX_DELTAS_PER_INTERVAL = 1024 * 1024;
static std::vector<ReverseCheckpoint> reverse_checkpoints;
static size_t reverse_cursor = 0;
static size_t reverse_max_checkpoints = REVERSE_DEFAULT_MAX_CHECKPOINTS;
static uint64_t reverse_next_checkpoint_id = 1;
static ReverseCheckpointMode reverse_mode = REVERSE_CHECKPOINT_BRANCH;
static bool reverse_trace_enabled = false;
static bool reverse_applying_delta = false;
static uint64_t reverse_total_dropped_writes = 0;
uint32_t debug_reverse_trace_active = 0; // extern'd in paging.h; checked per write

static bool LinearToPhysical(uint32_t linear, uint32_t& physical);
extern Bitu FillFlags(void);

struct DebugSocketLoadInfo {
    bool valid = false;
    std::string program;
    bool is_com = false;
    uint16_t psp_seg = 0;
    uint16_t load_seg = 0;
    uint16_t entry_cs = 0;
    uint16_t entry_ip = 0;
    uint16_t initial_ss = 0;
    uint16_t initial_sp = 0;
    uint32_t image_size_bytes = 0;
    uint16_t mz_signature = 0;
    uint16_t mz_extra_bytes = 0;
    uint16_t mz_pages = 0;
    uint16_t header_paragraphs = 0;
    uint16_t relocation_count = 0;
    uint16_t relocation_table_offset = 0;
    uint16_t init_cs = 0;
    uint16_t init_ip = 0;
    uint16_t init_ss = 0;
    uint16_t init_sp = 0;
    uint16_t checksum = 0;
    uint16_t overlay = 0;
    uint16_t min_alloc = 0;
    uint16_t max_alloc = 0;
};

static DebugSocketLoadInfo last_load_info;

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

// Parse a JSON array of strings: "key": ["str1","str2",...]
static bool json_get_string_array(const std::string& json, const char* key, std::vector<std::string>& out) {
    std::string search = "\"" + std::string(key) + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.length() || json[pos] != '[') return false;
    pos++; // skip '['
    out.clear();
    while (pos < json.length()) {
        while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' ||
               json[pos] == '\n' || json[pos] == '\r')) pos++;
        if (pos >= json.length()) break;
        if (json[pos] == ']') break;
        if (json[pos] == ',') { pos++; continue; }
        if (json[pos] != '"') return false;
        pos++;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return false;
        out.push_back(json.substr(pos, end - pos));
        pos = end + 1;
    }
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
        ssize_t sent = send(client_socket, msg.c_str(), msg.length(),
                            DEBUG_SOCKET_SEND_FLAGS);
        if (sent < 0) {
            // Socket is broken (EPIPE, EBADF, etc.) — close and wait for reconnect.
            LOG_MSG("DEBUG_Socket: send failed (errno %d): %s", errno, strerror(errno));
            close(client_socket);
            client_socket = -1;
        }
    }
}

// Send error response
// ---- symbols ----

static const char* symbol_source_name(DebugFormatId source) {
    switch (source) {
    case DEBUG_FORMAT_CODEVIEW: return "codeview";
    case DEBUG_FORMAT_TDINFO:   return "tdinfo";
    case DEBUG_FORMAT_WATCOM:   return "watcom";
    case DEBUG_FORMAT_MAP:      return "map";
    }
    return "unknown";
}

static std::string symbol_json(const DebugSymbol& symbol) {
    std::string out = json_str("name", symbol.name) + "," +
                      json_hex("linear", symbol.linear) + "," +
                      json_num("segment", symbol.segment) + "," +
                      json_hex("offset", symbol.offset) + "," +
                      json_str("source", symbol_source_name(symbol.source));
    if (symbol.hasSize) out += "," + json_num("size", symbol.size);
    if (!symbol.module.empty()) out += "," + json_str("module", symbol.module);
    if (!symbol.program.empty()) out += "," + json_str("program", symbol.program);
    return out;
}

// What is at an address: the symbol it falls inside, and the source line.
static std::string address_json(uint32_t linear) {
    DebugSymbolStore& store = DEBUG_Symbols();
    std::string out = json_hex("linear", linear);

    uint32_t delta = 0;
    const DebugSymbol* symbol = store.Nearest(linear, delta);
    if (symbol != NULL) {
        out += "," + json_str("symbol", symbol->name) +
               "," + json_num("delta", delta) +
               "," + json_str("description", store.Describe(linear)) +
               "," + json_str("source", symbol_source_name(symbol->source));
        if (!symbol->module.empty()) out += "," + json_str("module", symbol->module);
        if (!symbol->program.empty()) out += "," + json_str("program", symbol->program);
    }

    const DebugSourceLine* line = store.LineAt(linear);
    if (line != NULL) out += "," + json_str("file", line->file) + "," + json_num("line", line->line);
    return out;
}

static bool name_contains(const std::string& name, const std::string& needle) {
    if (needle.empty()) return true;
    std::string haystack = name;
    std::string want = needle;
    for (size_t i = 0; i < haystack.size(); i++) haystack[i] = (char)toupper((unsigned char)haystack[i]);
    for (size_t i = 0; i < want.size(); i++) want[i] = (char)toupper((unsigned char)want[i]);
    return haystack.find(want) != std::string::npos;
}

static void send_error(const char* msg) {
    send_response("{" + json_str("status", "error") + "," + json_str("msg", msg) + "}");
}

// Send OK response with optional data
static FILE*    alloc_trace_fp   = NULL;
static uint32_t alloc_trace_n    = 0;

static void send_ok(const std::string& extra = "") {
    std::string resp = "{" + json_str("status", "ok");
    if (!extra.empty()) resp += "," + extra;
    resp += "}";
    send_response(resp);
}

static void execute_dos_command_and_respond(const std::string& command) {
    // Clear error state before execution
    dos.errorcode = 0;

    // Clear pending breakpoint flag - if bp_on_load triggers,
    // DEBUG_Socket_NotifyBreakpoint will send the event
    g_exec_breakpoint_pending = false;

    // Copy command to mutable buffer (DoCommand modifies it)
    char cmd_buffer[CMD_MAXLINE];
    strncpy(cmd_buffer, command.c_str(), CMD_MAXLINE - 1);
    cmd_buffer[CMD_MAXLINE - 1] = 0;

    // Execute through the same shell parser used by the interactive prompt so
    // redirection, piping, batch files, and external programs all follow normal
    // DOS shell semantics. If bp_on_load is active and an external program is
    // loaded, DEBUG_Socket_NotifyBreakpoint will be called before this returns.
    first_shell->ParseLine(cmd_buffer);

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
}

static uint16_t bios_keycode_for_ascii(char c) {
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

    const uint8_t ascii = (uint8_t)c;
    if (ascii < 128 && scancodes[ascii] != 0) {
        return ((uint16_t)scancodes[ascii] << 8) | ascii;
    }

    return ascii;
}

static bool socket_cpu_running(void) {
    return !socket_freeze_loop_active && (IsDebuggerRunNormal() || IsDebuggerRunwatch());
}

static std::string socket_state_fields(void) {
    const bool debugger_runwatch = IsDebuggerRunwatch();
    const bool debugger_runnormal = IsDebuggerRunNormal();
    const bool frozen = socket_freeze_loop_active;
    const bool cpu_running = socket_cpu_running();
    std::string state = cpu_running ? "running" : "stopped";
    return json_str("state", state) + "," +
           json_bool("cpuRunning", cpu_running) + "," +
           json_bool("socketFrozen", frozen) + "," +
           json_bool("socketFreezeRequested", socket_freeze_wait) + "," +
           json_bool("debuggerFrozen", frozen || (!debugger_runwatch && !debugger_runnormal)) + "," +
           json_bool("debuggerRunwatch", debugger_runwatch) + "," +
           json_bool("debuggerRunnormal", debugger_runnormal) + "," +
           json_bool("normalCoreHooksActive", debug_socket_normal_core_hooks_active != 0) + "," +
           json_bool("shellReady", first_shell != nullptr);
}

static void send_and_latch_stop(const std::string& json) {
    last_stop_event_json = json;
    send_response(json);
}

static void send_and_latch_fault_stop(const std::string& json) {
    last_stop_event_json = json;
    last_fault_stop_event_json = json;
    send_response(json);
}

static bool latched_stop_blocks_dos_command(void) {
    if (!last_fault_stop_event_json.empty()) return true;
    if (last_stop_event_json.empty()) return false;

    // With breakOnExit disabled, process_exit is an async notification latched
    // for clients to poll; it is not a sticky debugger stop and must not block
    // issuing the next shell command from an idle prompt.
    return last_stop_event_json.find("\"event\":\"process_exit\"") == std::string::npos;
}

static std::string load_info_json_value() {
    std::ostringstream ss;
    ss << "{"
       << json_str("program", last_load_info.program) << ","
       << json_bool("isCom", last_load_info.is_com) << ","
       << json_num("pspSeg", last_load_info.psp_seg) << ","
       << json_num("loadSeg", last_load_info.load_seg) << ","
       << json_num("loadLinear", (uint32_t)last_load_info.load_seg << 4u) << ","
       << json_num("entryCS", last_load_info.entry_cs) << ","
       << json_num("entryIP", last_load_info.entry_ip) << ","
       << json_num("entryLinear", ((uint32_t)last_load_info.entry_cs << 4u) + last_load_info.entry_ip) << ","
       << json_num("initialSS", last_load_info.initial_ss) << ","
       << json_num("initialSP", last_load_info.initial_sp) << ","
       << json_num("initialStackLinear", ((uint32_t)last_load_info.initial_ss << 4u) + last_load_info.initial_sp) << ","
       << json_num("imageSizeBytes", last_load_info.image_size_bytes) << ","
       << json_num("imageEndLinear", ((uint32_t)last_load_info.load_seg << 4u) + last_load_info.image_size_bytes);

    if (!last_load_info.is_com) {
        ss << ","
           << json_num("mzSignature", last_load_info.mz_signature) << ","
           << json_num("mzExtraBytes", last_load_info.mz_extra_bytes) << ","
           << json_num("mzPages", last_load_info.mz_pages) << ","
           << json_num("headerParagraphs", last_load_info.header_paragraphs) << ","
           << json_num("headerBytes", (uint32_t)last_load_info.header_paragraphs << 4u) << ","
           << json_num("relocationCount", last_load_info.relocation_count) << ","
           << json_num("relocationTableOffset", last_load_info.relocation_table_offset) << ","
           << json_num("initCS", last_load_info.init_cs) << ","
           << json_num("initIP", last_load_info.init_ip) << ","
           << json_num("initSS", last_load_info.init_ss) << ","
           << json_num("initSP", last_load_info.init_sp) << ","
           << json_num("checksum", last_load_info.checksum) << ","
           << json_num("overlay", last_load_info.overlay) << ","
           << json_num("minAlloc", last_load_info.min_alloc) << ","
           << json_num("maxAlloc", last_load_info.max_alloc);
    }

    ss << "}";
    return ss.str();
}

static std::string load_info_json_field() {
    return "\"loadInfo\":" + load_info_json_value();
}

static uint32_t current_linear_pc(void) {
    return (uint32_t)(SegPhys(SegNames::cs) + reg_eip);
}

static ReverseCpuState reverse_capture_cpu(void) {
    FillFlags();
    ReverseCpuState state;
    state.eax = reg_eax;
    state.ebx = reg_ebx;
    state.ecx = reg_ecx;
    state.edx = reg_edx;
    state.esi = reg_esi;
    state.edi = reg_edi;
    state.ebp = reg_ebp;
    state.esp = reg_esp;
    state.eip = reg_eip;
    state.flags = reg_flags;
    for (int i = 0; i < 8; i++) {
        state.seg_val[i] = Segs.val[i];
        state.seg_phys[i] = Segs.phys[i];
        state.seg_limit[i] = Segs.limit[i];
        state.seg_expanddown[i] = Segs.expanddown[i];
    }
    state.cpl = cpu.cpl;
    state.mpl = cpu.mpl;
    state.cr0 = cpu.cr0;
    state.cr4 = cpu.cr4;
    state.pmode = cpu.pmode;
    state.code_big = cpu.code.big;
    state.stack_big = cpu.stack.big;
    state.stack_mask = cpu.stack.mask;
    state.stack_notmask = cpu.stack.notmask;
    state.direction = cpu.direction;
    state.trap_skip = cpu.trap_skip;
    state.paging_enabled = paging.enabled;
    state.paging_wp = paging.wp;
    state.paging_cr2 = paging.cr2;
    state.paging_cr3 = paging.cr3;
    state.paging_base_page = paging.base.page;
    state.paging_base_addr = paging.base.addr;
    return state;
}

static void reverse_restore_cpu(const ReverseCpuState& state) {
    reg_eax = state.eax;
    reg_ebx = state.ebx;
    reg_ecx = state.ecx;
    reg_edx = state.edx;
    reg_esi = state.esi;
    reg_edi = state.edi;
    reg_ebp = state.ebp;
    reg_esp = state.esp;
    reg_eip = state.eip;
    reg_flags = state.flags | 2u;
    for (int i = 0; i < 8; i++) {
        Segs.val[i] = state.seg_val[i];
        Segs.phys[i] = state.seg_phys[i];
        Segs.limit[i] = state.seg_limit[i];
        Segs.expanddown[i] = state.seg_expanddown[i];
    }
    cpu.cpl = state.cpl;
    cpu.mpl = state.mpl;
    cpu.cr0 = state.cr0;
    cpu.cr4 = state.cr4;
    cpu.pmode = state.pmode;
    cpu.code.big = state.code_big;
    cpu.stack.big = state.stack_big;
    cpu.stack.mask = state.stack_mask;
    cpu.stack.notmask = state.stack_notmask;
    cpu.direction = state.direction;
    cpu.trap_skip = state.trap_skip;
    paging.enabled = state.paging_enabled;
    paging.wp = state.paging_wp;
    paging.cr2 = state.paging_cr2;
    paging.cr3 = state.paging_cr3;
    paging.base.page = state.paging_base_page;
    paging.base.addr = state.paging_base_addr;
    PAGING_ClearTLB();
}

static ReverseCheckpoint reverse_make_checkpoint(uint32_t from_cs = 0, uint32_t from_linear = 0) {
    ReverseCheckpoint checkpoint;
    checkpoint.id = reverse_next_checkpoint_id++;
    checkpoint.cpu = reverse_capture_cpu();
    checkpoint.cs = SegValue(SegNames::cs);
    checkpoint.eip = reg_eip;
    checkpoint.linear = current_linear_pc();
    checkpoint.from_cs = from_cs;
    checkpoint.from_linear = from_linear;
    return checkpoint;
}

static void reverse_discard_future_if_needed(void) {
    if (!reverse_trace_enabled || reverse_checkpoints.empty()) return;
    if (reverse_cursor + 1 >= reverse_checkpoints.size()) return;
    reverse_checkpoints[reverse_cursor].deltas.clear();
    reverse_checkpoints[reverse_cursor].delta_overflow = false;
    reverse_checkpoints[reverse_cursor].dropped_writes = 0;
    reverse_checkpoints.erase(reverse_checkpoints.begin() + (long)(reverse_cursor + 1), reverse_checkpoints.end());
}

static void reverse_prune_old_checkpoints(void) {
    while (reverse_checkpoints.size() > reverse_max_checkpoints) {
        reverse_checkpoints.erase(reverse_checkpoints.begin());
        if (reverse_cursor > 0) reverse_cursor--;
    }
}

static void reverse_reset(size_t max_checkpoints) {
    reverse_trace_enabled = false;
    debug_reverse_trace_active = 0;
    reverse_applying_delta = false;
    reverse_total_dropped_writes = 0;
    reverse_next_checkpoint_id = 1;
    reverse_max_checkpoints = std::max(REVERSE_MIN_CHECKPOINTS,
        std::min(max_checkpoints, REVERSE_MAX_CHECKPOINTS));
    reverse_cursor = 0;
    reverse_checkpoints.clear();
}

static void clear_socket_debug_session_state(bool preserve_exit_policy) {
    socket_freeze_wait = false;
    socket_freeze_loop_active = false;
    socket_step_arm = 0;
    linear_exec_breakpoints.clear();
    last_stop_event_json.clear();
    last_fault_stop_event_json.clear();
    last_exception_num = 0xFF;
    last_exception_error = 0;
    last_load_info = DebugSocketLoadInfo();

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        watchpoints[i] = DebugWatchpoint();
    }
    debug_watchpoint_count = 0;
    watchpoint_pending_freeze = false;
    watchpoint_hit_context = WatchpointHit();

    catch_exceptions_armed = false;
    for (int i = 0; i < 32; i++) catch_exceptions_vectors[i] = false;
    catch_exceptions_skip_count = 0;
    catch_exceptions_cr2_ignore.clear();

    branch_trace_enabled = false;
    branch_ring_head = 0;
    branch_ring_count = 0;
    reverse_reset(reverse_max_checkpoints);

    CBreakpoint::DeleteAll();

    if (!preserve_exit_policy) {
        break_on_exit = false;
    }
    last_process_exit = ProcessExitInfo();
}

static void reverse_record_checkpoint(uint32_t from_cs, uint32_t from_linear) {
    if (!reverse_trace_enabled || reverse_mode != REVERSE_CHECKPOINT_BRANCH) return;
    reverse_discard_future_if_needed();
    reverse_checkpoints.push_back(reverse_make_checkpoint(from_cs, from_linear));
    reverse_cursor = reverse_checkpoints.empty() ? 0 : reverse_checkpoints.size() - 1;
    reverse_prune_old_checkpoints();
}

void DEBUG_Socket_ReverseInstructionCheckpoint(void) {
    if (!reverse_trace_enabled || reverse_mode != REVERSE_CHECKPOINT_INSTRUCTION) return;
    reverse_discard_future_if_needed();
    if (!reverse_checkpoints.empty() &&
        reverse_checkpoints[reverse_cursor].linear == current_linear_pc() &&
        reverse_checkpoints[reverse_cursor].cs == SegValue(SegNames::cs) &&
        reverse_checkpoints[reverse_cursor].eip == reg_eip) {
        return;
    }
    reverse_checkpoints.push_back(reverse_make_checkpoint(SegValue(SegNames::cs), current_linear_pc()));
    reverse_cursor = reverse_checkpoints.empty() ? 0 : reverse_checkpoints.size() - 1;
    reverse_prune_old_checkpoints();
}

static std::string reverse_checkpoint_json(const ReverseCheckpoint& checkpoint,
                                           size_t index,
                                           bool current,
                                           bool newest) {
    std::ostringstream ss;
    ss << "{"
       << json_num("index", (long long)index) << ","
       << json_num("checkpoint_id", (long long)checkpoint.id) << ","
       << json_bool("current", current) << ","
       << json_bool("newest", newest) << ","
       << json_num("CS", checkpoint.cs) << ","
       << json_hex("EIP", checkpoint.eip) << ","
       << json_hex("linear", checkpoint.linear) << ","
       << json_num("deltaBytesToNext", (long long)checkpoint.deltas.size()) << ","
       << json_bool("deltaOverflowToNext", checkpoint.delta_overflow) << ","
       << json_num("droppedWritesToNext", (long long)checkpoint.dropped_writes) << ","
       << json_hex("firstDeltaLinear", checkpoint.deltas.empty() ? 0 : checkpoint.deltas.front().linear) << ","
       << json_hex("lastDeltaLinear", checkpoint.deltas.empty() ? 0 : checkpoint.deltas.back().linear) << ","
       << json_num("from_cs", checkpoint.from_cs) << ","
       << json_hex("from_linear", checkpoint.from_linear)
       << "}";
    return ss.str();
}

static std::string reverse_status_fields(void) {
    std::ostringstream ss;
    const bool has_current = reverse_trace_enabled && !reverse_checkpoints.empty();
    ss << json_bool("enabled", reverse_trace_enabled) << ","
       << json_str("mode", reverse_mode == REVERSE_CHECKPOINT_BRANCH ? "branch" : "instruction") << ","
       << json_num("max_checkpoints", (long long)reverse_max_checkpoints) << ","
       << json_num("count", (long long)reverse_checkpoints.size()) << ","
       << json_num("cursor_index", has_current ? (long long)reverse_cursor : -1) << ","
       << json_bool("hasForwardHistory", has_current && reverse_cursor + 1 < reverse_checkpoints.size()) << ","
       << json_num("newest_index", reverse_checkpoints.empty() ? -1 : (long long)(reverse_checkpoints.size() - 1)) << ","
       << json_num("totalDroppedWrites", (long long)reverse_total_dropped_writes) << ","
       << json_str("limitations",
                   "Best-effort CPU + RAM rollback. Device/DMA/direct physical writes, timers, PIC/PIT/audio/video device state, host I/O, and non-memory side effects are not rolled back.");
    if (has_current) {
        const ReverseCheckpoint& cur = reverse_checkpoints[reverse_cursor];
        const ReverseCheckpoint& newest = reverse_checkpoints.back();
        ss << ","
           << json_num("checkpoint_id", (long long)cur.id) << ","
           << json_num("newest_checkpoint_id", (long long)newest.id) << ","
           << json_num("CS", cur.cs) << ","
           << json_hex("EIP", cur.eip) << ","
           << json_hex("linear", cur.linear);
    }
    return ss.str();
}

static bool reverse_interval_is_reversible(size_t interval, std::string& error) {
    if (interval >= reverse_checkpoints.size()) {
        error = "Invalid checkpoint interval";
        return false;
    }
    const ReverseCheckpoint& checkpoint = reverse_checkpoints[interval];
    if (checkpoint.delta_overflow) {
        std::ostringstream ss;
        ss << "Checkpoint interval " << (long long)checkpoint.id
           << " overflowed; rollback/forward would be incomplete";
        error = ss.str();
        return false;
    }
    return true;
}

static bool reverse_can_navigate_to(size_t target, std::string& error) {
    if (reverse_checkpoints.empty()) {
        error = "Reverse trace has no checkpoints";
        return false;
    }
    if (target >= reverse_checkpoints.size()) {
        error = "Target checkpoint is outside retained history";
        return false;
    }
    if (target < reverse_cursor) {
        for (size_t i = target; i < reverse_cursor; i++) {
            if (!reverse_interval_is_reversible(i, error)) return false;
        }
    } else {
        for (size_t i = reverse_cursor; i < target; i++) {
            if (!reverse_interval_is_reversible(i, error)) return false;
        }
    }
    return true;
}

static bool reverse_require_navigation_stopped(void) {
    if (socket_freeze_loop_active) return true;
    send_error("CPU must be stopped in the socket freeze loop before reverse navigation; set a socket breakpoint/watchpoint or break at program entry first");
    return false;
}

static void reverse_apply_interval(size_t interval, bool forward) {
    const std::vector<ReverseMemDelta>& deltas = reverse_checkpoints[interval].deltas;
    reverse_applying_delta = true;
    if (forward) {
        for (const ReverseMemDelta& delta : deltas) {
            physdev_writeb((PhysPt64)delta.physical, delta.new_value);
        }
    } else {
        for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
            physdev_writeb((PhysPt64)it->physical, it->old_value);
        }
    }
    reverse_applying_delta = false;
}

static std::string reverse_navigation_fields(const char* op, size_t old_cursor) {
    const ReverseCheckpoint& cur = reverse_checkpoints[reverse_cursor];
    const ReverseCheckpoint& newest = reverse_checkpoints.back();
    std::ostringstream ss;
    ss << json_str("op", op) << ","
       << json_num("checkpoint_id", (long long)cur.id) << ","
       << json_num("cursor_index", (long long)reverse_cursor) << ","
       << json_num("previous_cursor_index", (long long)old_cursor) << ","
       << json_num("newest_index", (long long)(reverse_checkpoints.size() - 1)) << ","
       << json_num("newest_checkpoint_id", (long long)newest.id) << ","
       << json_bool("hasForwardHistory", reverse_cursor + 1 < reverse_checkpoints.size()) << ","
       << json_num("CS", cur.cs) << ","
       << json_hex("EIP", cur.eip) << ","
       << json_hex("linear", cur.linear);
    return ss.str();
}

static bool reverse_navigate_to(size_t target, const char* op) {
    if (!reverse_trace_enabled) {
        send_error("reverse_trace is not enabled");
        return false;
    }
    std::string error;
    if (!reverse_can_navigate_to(target, error)) {
        send_error(error.c_str());
        return false;
    }
    const size_t old_cursor = reverse_cursor;
    while (reverse_cursor > target) {
        reverse_apply_interval(reverse_cursor - 1, false);
        reverse_cursor--;
    }
    while (reverse_cursor < target) {
        reverse_apply_interval(reverse_cursor, true);
        reverse_cursor++;
    }
    reverse_restore_cpu(reverse_checkpoints[reverse_cursor].cpu);
    send_ok(reverse_navigation_fields(op, old_cursor));
    return true;
}

static bool load_info_matches_entry(uint16_t seg, uint32_t off) {
    return last_load_info.valid &&
           last_load_info.entry_cs == seg &&
           last_load_info.entry_ip == (off & 0xFFFFu);
}

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
                                 uint16_t maxAlloc) {
    last_load_info.valid = true;
    // DOS_Execute receives the path string DOS used. It may be relative or a
    // shell-resolved fallback, so clients should treat it as a display hint.
    last_load_info.program = program ? program : "";
    last_load_info.is_com = isCom;
    last_load_info.psp_seg = pspSeg;
    last_load_info.load_seg = loadSeg;
    last_load_info.entry_cs = entryCS;
    last_load_info.entry_ip = entryIP;
    last_load_info.initial_ss = initialSS;
    last_load_info.initial_sp = initialSP;
    last_load_info.image_size_bytes = imageSizeBytes;
    last_load_info.mz_signature = mzSignature;
    last_load_info.mz_extra_bytes = mzExtraBytes;
    last_load_info.mz_pages = mzPages;
    last_load_info.header_paragraphs = headerParagraphs;
    last_load_info.relocation_count = relocationCount;
    last_load_info.relocation_table_offset = relocationTableOffset;
    last_load_info.init_cs = initCS;
    last_load_info.init_ip = initIP;
    last_load_info.init_ss = initSS;
    last_load_info.init_sp = initSP;
    last_load_info.checksum = checksum;
    last_load_info.overlay = overlay;
    last_load_info.min_alloc = minAlloc;
    last_load_info.max_alloc = maxAlloc;
}

// Forward declaration needed by watchpoint/exception implementations below
static std::string get_registers_json();

// -----------------------------------------------------------------------
// Watchpoint implementation
// -----------------------------------------------------------------------

void DEBUG_RecordReverseWrite(uint32_t address, uint32_t size, uint32_t val) {
    if (!reverse_trace_enabled || reverse_applying_delta || reverse_checkpoints.empty()) return;
    reverse_discard_future_if_needed();

    ReverseCheckpoint& checkpoint = reverse_checkpoints[reverse_cursor];
    if (checkpoint.delta_overflow) {
        checkpoint.dropped_writes += size;
        reverse_total_dropped_writes += size;
        return;
    }

    for (uint32_t i = 0; i < size; i++) {
        if (checkpoint.deltas.size() >= REVERSE_MAX_DELTAS_PER_INTERVAL) {
            checkpoint.delta_overflow = true;
            checkpoint.dropped_writes++;
            reverse_total_dropped_writes++;
            continue;
        }

        const uint32_t linear = address + i;
        uint32_t physical = 0;
        uint8_t old_value = 0;
        if (LinearToPhysical(linear, physical)) {
            old_value = physdev_readb((PhysPt64)physical);
        } else {
            old_value = mem_readb_inline((LinearPt)linear);
            physical = linear;
        }
        const uint8_t new_value = (uint8_t)((val >> (i * 8u)) & 0xFFu);

        auto existing = std::find_if(checkpoint.deltas.begin(), checkpoint.deltas.end(),
            [linear](const ReverseMemDelta& delta) {
                return delta.linear == linear;
            });
        if (existing != checkpoint.deltas.end()) {
            existing->new_value = new_value;
            existing->physical = physical;
            continue;
        }

        ReverseMemDelta delta;
        delta.linear = linear;
        delta.physical = physical;
        delta.old_value = old_value;
        delta.new_value = new_value;
        checkpoint.deltas.push_back(delta);
    }
}

// Called from paging.h mem_writeb/w/d_checked; address=linear, size=1/2/4.
// Runs inline-hot path — returns immediately when no watchpoints armed.
void DEBUG_CheckWriteWatch(uint32_t address, uint32_t size, uint32_t val) {
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!watchpoints[i].active) continue;
        // Overlap check: [address, address+size) vs [start, end)
        if (address >= watchpoints[i].end) continue;
        if ((address + size) <= watchpoints[i].start) continue;
        // Hit!
        watchpoints[i].hit_count++;
        // Read old value before the write completes (read path doesn't go through watchpoint hook)
        uint32_t old_val = 0;
        uint8_t  bv = 0;
        uint16_t wv = 0;
        uint32_t dv = 0;
        if (size == 1) {
            if (!mem_readb_checked(address, &bv)) old_val = bv;
        } else if (size == 2) {
            if (!mem_readw_checked(address, &wv)) old_val = wv;
        } else {
            if (!mem_readd_checked(address, &dv)) old_val = dv;
        }
        watchpoint_hit_context.watch_linear = address;
        watchpoint_hit_context.old_val      = old_val;
        watchpoint_hit_context.new_val      = val;
        watchpoint_hit_context.culprit_cs   = SegValue(cs);
        watchpoint_hit_context.culprit_eip  = reg_eip;
        watchpoint_hit_context.size         = size;
        watchpoint_pending_freeze = true;
        return;  // only latch the first hit per write
    }
}

// Called from core_normal.cpp per-instruction guard; returns true = freeze needed.
bool DEBUG_Socket_CheckWatchpointFreeze(void) {
    if (!watchpoint_pending_freeze) return false;
    watchpoint_pending_freeze = false;
    if (client_socket >= 0 && !gdb_mode) {
        uint16_t seg = SegValue(SegNames::cs);
        uint32_t off = reg_eip;
        std::ostringstream ss;
        ss << "{" << json_str("event", "stopped") << ","
           << json_str("reason", "watchpoint") << ","
           << json_hex("watch_linear", watchpoint_hit_context.watch_linear) << ","
           << json_num("watch_size",   (long long)watchpoint_hit_context.size) << ","
           << json_num("old",          (long long)watchpoint_hit_context.old_val) << ","
           << json_num("new",          (long long)watchpoint_hit_context.new_val) << ","
           << json_num("culprit_cs",   (long long)watchpoint_hit_context.culprit_cs) << ","
           << json_hex("culprit_eip",  watchpoint_hit_context.culprit_eip) << ","
           << json_hex("linear",       (uint32_t)GetAddress(seg, off)) << ","
           << get_registers_json() << "}";
        send_and_latch_stop(ss.str());
    }
    return true;
}

static void rebuild_watchpoint_count() {
    uint32_t n = 0;
    for (int i = 0; i < MAX_WATCHPOINTS; i++)
        if (watchpoints[i].active) n++;
    debug_watchpoint_count = n;
}

// -----------------------------------------------------------------------
// First-chance exception implementation
// -----------------------------------------------------------------------

bool DEBUG_Socket_IsExceptionVectorCaught(uint8_t which) {
    if (which >= 32) return false;
    return catch_exceptions_armed && catch_exceptions_vectors[which];
}

bool DEBUG_Socket_CheckException(uint8_t which, uint32_t error, bool is_nested) {
    if (!DEBUG_Socket_IsExceptionVectorCaught(which)) return false;

    // Nested faults for requested vectors always stop (never skip).
    if (!is_nested) {
        // Check CR2 ignore ranges for #PF (vec 14)
        if (which == 14) {
            uint32_t cr2 = (uint32_t)paging.cr2;
            for (const auto& r : catch_exceptions_cr2_ignore) {
                if (cr2 >= r.start && cr2 <= r.end) return false;
            }
        }

        // Check skip list
        for (int i = 0; i < catch_exceptions_skip_count; i++) {
            if (catch_exceptions_skip_list[i].vec == which &&
                catch_exceptions_skip_list[i].remaining > 0) {
                catch_exceptions_skip_list[i].remaining--;
                return false;
            }
        }
    }

    // Latch context (at write time reg_eip is the faulting EIP)
    exception_hit_ctx.vec       = which;
    exception_hit_ctx.error     = error;
    exception_hit_ctx.cr2       = (uint32_t)paging.cr2;
    exception_hit_ctx.fault_cs  = SegValue(cs);
    exception_hit_ctx.fault_eip = reg_eip;

    if (client_socket >= 0 && !gdb_mode) {
        std::ostringstream ss;
        ss << "{" << json_str("event", "stopped") << ","
           << json_str("reason", "exception") << ","
           << json_num("vector",       (long long)which) << ","
           << json_num("error_code",   (long long)error) << ","
           << json_hex("CR2",          exception_hit_ctx.cr2) << ","
           << json_num("fault_cs",     (long long)exception_hit_ctx.fault_cs) << ","
           << json_hex("fault_eip",    exception_hit_ctx.fault_eip) << ","
           << json_bool("is_nested",   is_nested) << ","
           << json_hex("linear",       (uint32_t)GetAddress(
                                           exception_hit_ctx.fault_cs,
                                           exception_hit_ctx.fault_eip)) << ","
           << get_registers_json() << "}";
        send_and_latch_stop(ss.str());
    }
    return true;
}

// -----------------------------------------------------------------------
// Process exit events
// -----------------------------------------------------------------------

void DEBUG_Socket_NotifyProcessExit(uint16_t pspseg, uint8_t exitcode, bool tsr) {
    last_process_exit.psp       = pspseg;
    last_process_exit.exit_code = exitcode;
    last_process_exit.tsr       = tsr;
    last_process_exit.abnormal  = false;
    last_process_exit.valid     = true;

    if (client_socket < 0 || gdb_mode) return;

    std::string evt = "{" +
        json_str("event",     "process_exit") + "," +
        json_num("exit_code", exitcode) + "," +
        json_num("psp",       pspseg) + "," +
        json_bool("tsr",      tsr) + "," +
        json_bool("abnormal", false) + "}";

    if (break_on_exit) {
        send_and_latch_stop(evt);
        DEBUG_Socket_FreezeWait();
    } else {
        // Async notification; also latch so MCP can poll
        last_stop_event_json = evt;
        send_response(evt);
    }
}

// -----------------------------------------------------------------------
// Branch trace ring
// -----------------------------------------------------------------------

bool DEBUG_Socket_TraceIsEnabled(void) {
    return branch_trace_enabled || reverse_trace_enabled;
}

void DEBUG_Socket_TraceRecordBranch(uint32_t from_cs, uint32_t from_linear,
                                    uint32_t to_cs,   uint32_t to_linear) {
    if (branch_trace_enabled) {
        uint32_t idx = branch_ring_head % BRANCH_RING_SIZE;
        branch_ring[idx].from_cs     = from_cs;
        branch_ring[idx].from_linear = from_linear;
        branch_ring[idx].to_cs       = to_cs;
        branch_ring[idx].to_linear   = to_linear;
        branch_ring_head++;
        if (branch_ring_count < (uint32_t)BRANCH_RING_SIZE) branch_ring_count++;
    }
    if (reverse_trace_enabled) {
        reverse_record_checkpoint(from_cs, from_linear);
    }
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
        std::string extra = socket_state_fields() + "," +
                json_num("port", socket_port) + "," +
                json_bool("hasLastStop", !last_stop_event_json.empty()) + "," +
                json_bool("hasLastFaultStop", !last_fault_stop_event_json.empty()) + "," +
                json_bool("hasLastExit", last_process_exit.valid) + "," +
                json_bool("shellCommandPending", DOS_Shell_HasQueuedCommandFromDebugger()) + "," +
                json_num("shellCommandsQueued", DOS_Shell_DebuggerCommandsQueued()) + "," +
                json_num("shellCommandWakes", DOS_Shell_DebuggerCommandsWoken()) + "," +
                json_num("shellCommandsConsumed", DOS_Shell_DebuggerCommandsConsumed()) + "," +
                json_num("shellCommandsSubmitted", DOS_Shell_DebuggerCommandsSubmitted()) + "," +
                json_str("shellLastConsumedCommand", DOS_Shell_DebuggerLastConsumedCommand()) + "," +
                json_str("shellLastSubmittedCommand", DOS_Shell_DebuggerLastSubmittedCommand()) + "," +
                json_bool("catchExceptionsArmed", catch_exceptions_armed) + "," +
                json_bool("traceEnabled", branch_trace_enabled) + "," +
                json_bool("reverseTraceEnabled", reverse_trace_enabled) + "," +
                json_num("reverseCheckpointCount", (long long)reverse_checkpoints.size()) + "," +
                json_num("reverseCursorIndex", reverse_trace_enabled && !reverse_checkpoints.empty() ? (long long)reverse_cursor : -1) + "," +
                json_bool("reverseHasForwardHistory", reverse_trace_enabled && !reverse_checkpoints.empty() && reverse_cursor + 1 < reverse_checkpoints.size()) + "," +
                json_num("watchpointCount", (long long)debug_watchpoint_count) + "," +
                json_bool("breakOnExit", break_on_exit);
        if (last_process_exit.valid) {
            extra += "," +
                json_num("lastExitCode", last_process_exit.exit_code) + "," +
                json_num("lastExitPsp",  last_process_exit.psp);
        }
        send_ok(extra);
        return;
    }

    if (cmd == "reset" || cmd == "machine_reset") {
        long long preserve_exit_policy = 0;
        json_get_int(json, "preserveExitPolicy", preserve_exit_policy);
        send_ok(json_str("msg", "Machine reset requested") + "," +
                json_str("method", "On_Software_CPU_Reset") + "," +
                json_bool("socketPreserved", true) + "," +
                json_bool("debugStateCleared", true));
        clear_socket_debug_session_state(preserve_exit_policy != 0);
        On_Software_CPU_Reset();
        return;
    }

    if (cmd == "clear_debug_state") {
        long long preserve_exit_policy = 0;
        json_get_int(json, "preserveExitPolicy", preserve_exit_policy);
        clear_socket_debug_session_state(preserve_exit_policy != 0);
        send_ok(json_str("msg", "Socket debug state cleared") + "," +
                json_bool("debugStateCleared", true) + "," +
                socket_state_fields());
        return;
    }

    if (cmd == "last_stop") {
        if (!last_stop_event_json.empty()) {
            send_response(last_stop_event_json);
        } else if (!last_fault_stop_event_json.empty()) {
            send_response(last_fault_stop_event_json);
        } else {
            send_error("No latched stop event");
        }
        return;
    }

    if (cmd == "get_load_info") {
        if (last_load_info.valid) {
            send_ok(json_bool("available", true) + "," + load_info_json_field());
        } else {
            send_ok(json_bool("available", false));
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
            last_fault_stop_event_json.clear();
            socket_freeze_wait = false;
            send_ok(json_str("msg", "Continuing"));
        } else if (!IsDebuggerRunwatch()) {
            last_stop_event_json.clear();
            last_fault_stop_event_json.clear();
            DEBUG_ResumeNormalFromSocket();
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

    if (cmd == "sym") {
        std::string name;
        if (!json_get_string(json, "name", name)) {
            send_error("Missing 'name'");
            return;
        }
        const DebugSymbol* symbol = DEBUG_Symbols().Resolve(name);
        if (symbol == NULL) {
            send_error("Unknown symbol");
            return;
        }
        send_ok(symbol_json(*symbol));
        return;
    }

    if (cmd == "sym_list") {
        std::string filter;
        json_get_string(json, "filter", filter);
        long long limit = 100;
        json_get_int(json, "limit", limit);
        if (limit < 0) limit = 0;

        const std::vector<DebugSymbol>& all = DEBUG_Symbols().All();
        long long matched = 0;
        std::string arr;
        for (size_t i = 0; i < all.size(); i++) {
            if (!name_contains(all[i].name, filter)) continue;
            matched++;
            if (matched > limit) continue;
            if (!arr.empty()) arr += ",";
            arr += "{" + symbol_json(all[i]) + "}";
        }
        send_ok(json_num("total", (long long)DEBUG_Symbols().Size()) + "," +
                json_num("matched", matched) + "," +
                json_num("lines", (long long)DEBUG_Symbols().LineCount()) + "," +
                "\"symbols\":[" + arr + "]");
        return;
    }

    // No address given means where the CPU is now.
    if (cmd == "where") {
        long long linear = 0, seg = 0, off = 0;
        std::string extra;
        uint32_t address;

        if (json_get_int(json, "linear", linear)) {
            address = (uint32_t)linear;
        } else if (json_get_int(json, "seg", seg) && json_get_int(json, "off", off)) {
            address = (uint32_t)GetAddress((uint16_t)seg, (uint32_t)off);
        } else {
            const uint16_t cs = SegValue(SegNames::cs);
            address = (uint32_t)GetAddress(cs, reg_eip);
            extra = "," + json_num("CS", cs) + "," + json_hex("EIP", reg_eip);
        }
        send_ok(address_json(address) + extra);
        return;
    }

    if (cmd == "sym_load") {
        std::string file;
        if (!json_get_string(json, "file", file)) {
            send_error("Missing 'file'");
            return;
        }

        long long load_linear = 0, load_seg = 0;
        if (!json_get_int(json, "loadLinear", load_linear)) {
            if (json_get_int(json, "loadSeg", load_seg)) load_linear = load_seg << 4;
            else load_linear = 0;
        }

        std::string program = file;
        json_get_string(json, "program", program);

        DebugSymbolStore& store = DEBUG_Symbols();
        const size_t before = store.Size();
        store.ClearProgram(program);

        // A .MAP is text and a program's own debug info is not, so which one
        // this file is does not have to be declared.
        LinkMapFile map;
        DebugInfo info;
        if (DEBUG_ReadLinkMapFile(file.c_str(), map) && !map.publics.empty()) {
            store.AddLinkMap(map, (uint32_t)load_linear, program);
            send_ok(json_str("format", "map") + "," +
                    json_num("added", (long long)store.Size() - (long long)before) + "," +
                    json_num("total", (long long)store.Size()));
        } else if (DEBUG_ParseDebugInfo(file.c_str(), (uint32_t)load_linear, info)) {
            store.AddDebugInfo(info, program);
            send_ok(json_str("format", info.version) + "," +
                    json_num("added", (long long)store.Size() - (long long)before) + "," +
                    json_num("lines", (long long)store.LineCount()) + "," +
                    json_num("total", (long long)store.Size()));
        } else {
            send_error("File carries no LINK map or debug info this build can read");
        }
        return;
    }

    if (cmd == "sym_clear") {
        std::string program;
        if (json_get_string(json, "program", program)) DEBUG_Symbols().ClearProgram(program);
        else DEBUG_Symbols().Clear();
        send_ok(json_num("total", (long long)DEBUG_Symbols().Size()));
        return;
    }

    if (cmd == "mem_read") {
        long long seg, off, len, addr_linear;
        if (!json_get_int(json, "len", len)) {
            send_error("Missing 'len'");
            return;
        }
        if (len > 65536) len = 65536; // Limit
        
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

    if (cmd == "alloc_trace") {
        std::string op, path;
        json_get_string(json, "op", op);
        if (op == "stop") {
            if (alloc_trace_fp) { fclose(alloc_trace_fp); alloc_trace_fp = NULL; }
            send_ok(json_num("events", alloc_trace_n));
            return;
        }
        if (!json_get_string(json, "file", path) || path.empty()) {
            send_error("Missing 'file'");
            return;
        }
        if (alloc_trace_fp) { fclose(alloc_trace_fp); alloc_trace_fp = NULL; }
        alloc_trace_fp = fopen(path.c_str(), "w");
        if (!alloc_trace_fp) { send_error("Could not open 'file' for writing"); return; }
        alloc_trace_n = 0;
        send_ok(json_str("file", path));
        return;
    }

    if (cmd == "mem_dump") {
        long long seg, off, len, addr_linear;
        std::string path;
        if (!json_get_int(json, "len", len) || len <= 0) {
            send_error("Missing or bad 'len'");
            return;
        }
        if (!json_get_string(json, "file", path) || path.empty()) {
            send_error("Missing 'file'");
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

        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            send_error("Could not open 'file' for writing");
            return;
        }

        /* Unreadable bytes are written as 0x00 and counted, rather than
           aborting: a dump straddling an unmapped page is still worth
           having, and the count says how much of it to distrust. */
        long long unreadable = 0;
        for (long long i = 0; i < len; i++) {
            uint8_t val;
            if (mem_readb_checked(addr + (PhysPt)i, &val)) { val = 0; unreadable++; }
            fputc(val, f);
        }
        fclose(f);

        send_ok(json_str("file", path) + "," +
                json_num("bytes", len) + "," +
                json_num("unreadable", unreadable) + "," +
                json_hex("linear", (uint32_t)addr));
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
        extern std::string capture_screenshot_override_path;
        extern std::string GetCaptureFilePath(const char * type,const char * ext);
        std::string path;
        if (json_get_string(json, "path", path)) {
            capture_screenshot_override_path = path;
        } else {
            path = GetCaptureFilePath("Screenshot", ".png");
        }
        CAPTURE_ScreenShotEvent(true);
        send_ok(json_str("msg", "Screenshot triggered") + "," +
                json_str("path", path));
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
        //   mask: optional hex string same length as pattern; "00" byte = wildcard
        //   align: optional alignment (1=any, 2=word, 4=dword, etc.)
        //   max_results: optional, default 100
        long long start_addr, search_len, max_results, align;
        std::string pattern, mask_str;
        
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
        if (!json_get_int(json, "align", align) || align < 1) align = 1;
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

        // Parse optional mask
        std::vector<uint8_t> mask_bytes;
        bool has_mask = json_get_string(json, "mask", mask_str) && !mask_str.empty();
        if (has_mask) {
            if (mask_str.length() != pattern.length()) {
                send_error("'mask' must be same length as 'pattern'");
                return;
            }
            for (size_t i = 0; i + 1 < mask_str.length(); i += 2) {
                char hex[3] = {mask_str[i], mask_str[i+1], 0};
                char* end;
                unsigned long val = strtoul(hex, &end, 16);
                if (*end != 0) {
                    send_error("Invalid hex mask");
                    return;
                }
                mask_bytes.push_back((uint8_t)val);
            }
        }
        
        // Search for pattern
        std::string results = "\"matches\":[";
        bool first = true;
        long long found_count = 0;
        PhysPt addr = (PhysPt)start_addr;
        // Align start address if requested
        if (align > 1) {
            addr = (addr + (PhysPt)(align - 1)) & ~(PhysPt)(align - 1);
        }
        PhysPt end_addr = (PhysPt)start_addr + (PhysPt)search_len;
        if (pattern_bytes.size() <= (size_t)search_len)
            end_addr = (PhysPt)start_addr + (PhysPt)search_len - pattern_bytes.size();
        else
            end_addr = addr - 1; // empty range

        PhysPt step = (align > 1) ? (PhysPt)align : 1;
        
        while (addr <= end_addr && found_count < max_results) {
            bool match = true;
            for (size_t i = 0; i < pattern_bytes.size() && match; i++) {
                uint8_t val;
                if (mem_readb_checked(addr + i, &val)) {
                    match = false;
                } else {
                    uint8_t effective_val     = val;
                    uint8_t effective_pattern = pattern_bytes[i];
                    if (has_mask && i < mask_bytes.size()) {
                        uint8_t m = mask_bytes[i];
                        effective_val     &= m;
                        effective_pattern &= m;
                    }
                    if (effective_val != effective_pattern) match = false;
                }
            }
            
            if (match) {
                if (!first) results += ",";
                first = false;
                
                char addr_str[16];
                snprintf(addr_str, sizeof(addr_str), "%08X", (uint32_t)addr);
                results += "\"" + std::string(addr_str) + "\"";
                found_count++;
                addr += (PhysPt)(pattern_bytes.size() > (size_t)step
                                  ? pattern_bytes.size() : (size_t)step);
            } else {
                addr += step;
            }
        }
        results += "]";
        
        send_ok(results + "," + 
                json_num("count", found_count) + "," +
                json_hex("start", (uint32_t)start_addr) + "," +
                json_num("searched", search_len) + "," +
                json_str("pattern", pattern) + "," +
                json_bool("has_mask", has_mask) + "," +
                json_num("align", align));
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

    if (cmd == "wait_for_shell") {
        long long timeout_ms = 0;
        json_get_int(json, "timeoutMs", timeout_ms);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 5000) timeout_ms = 5000;

        const long long sleep_us = 10000;
        long long waited_ms = 0;
        while (first_shell == nullptr && waited_ms < timeout_ms) {
            GFX_Events();
            usleep((useconds_t)sleep_us);
            waited_ms += sleep_us / 1000;
        }

        if (first_shell == nullptr) {
            send_error("Shell not initialized; retry wait_for_shell or poll status.shellReady before dos_cmd");
            return;
        }

        send_ok(json_bool("shellReady", true) + "," + json_num("waitedMs", waited_ms));
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
            send_error("Shell not initialized; call wait_for_shell or poll status.shellReady before dos_cmd");
            return;
        }

        if (!IsDebuggerRunwatch() && !IsDebuggerRunNormal()) {
            if (socket_freeze_wait || socket_freeze_loop_active ||
                latched_stop_blocks_dos_command()) {
                send_error("Cannot run DOS command while stopped at a debugger event; continue first");
                return;
            }

            // The socket-only debugger parks an otherwise idle DOS prompt in
            // DEBUG_Loop. Feeding the BIOS keyboard buffer from that state is
            // not reliable, and re-entering ParseLine from here is unsafe for
            // external EXE execution. Hand the line to the active InputCommand
            // call and resume the prompt so the shell executes it in-context.
            if (!DOS_Shell_QueueCommandFromDebugger(command.c_str())) {
                send_error("A queued DOS command is still pending");
                return;
            }
            BIOS_AddKeyToBuffer(0x1C0D);
            DEBUG_ResumeNormalFromSocket();
            send_ok(json_str("msg", "Command queued for shell execution") + "," +
                    json_str("command", command) + "," +
                    json_bool("queuedShellCommand", true));
            return;
        }

        execute_dos_command_and_respond(command);
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
                keycode = bios_keycode_for_ascii(key[0]);
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
        else if (key.length() == 1) {
            static const KBD_KEYS letter_keys[26] = {
                KBD_a, KBD_b, KBD_c, KBD_d, KBD_e, KBD_f, KBD_g, KBD_h, KBD_i,
                KBD_j, KBD_k, KBD_l, KBD_m, KBD_n, KBD_o, KBD_p, KBD_q, KBD_r,
                KBD_s, KBD_t, KBD_u, KBD_v, KBD_w, KBD_x, KBD_y, KBD_z
            };
            static const KBD_KEYS digit_keys[10] = {
                KBD_0, KBD_1, KBD_2, KBD_3, KBD_4, KBD_5, KBD_6, KBD_7, KBD_8, KBD_9
            };
            const char c = key[0];
            if (c >= 'a' && c <= 'z') kbd_key = letter_keys[c - 'a'];
            else if (c >= '0' && c <= '9') kbd_key = digit_keys[c - '0'];
        }
        if (kbd_key == KBD_NONE) {
            send_error("Unknown key name for key_hw; use enter, esc, space, a-z, or 0-9");
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
        if (len > 65536) len = 65536;

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

    if (cmd == "mem_write_linear") {
        // Write to a linear (virtual) address via page-walk + physdev_writeb.
        // Mirrors mem_read_linear. Raises an error if any page is not present.
        long long lin_addr;
        std::string data_hex;
        if (!json_get_int(json, "addr", lin_addr)) {
            send_error("Missing 'addr' (linear address)");
            return;
        }
        if (!json_get_string(json, "data", data_hex)) {
            send_error("Missing 'data' (hex string)");
            return;
        }
        if (data_hex.length() % 2 != 0) {
            send_error("'data' must be an even-length hex string");
            return;
        }
        long long byte_count = (long long)data_hex.length() / 2;
        if (byte_count > 65536) {
            send_error("'data' too long (max 65536 bytes)");
            return;
        }
        for (long long i = 0; i < byte_count; i++) {
            char hex[3] = {data_hex[i*2], data_hex[i*2+1], 0};
            char* endp;
            uint8_t bval = (uint8_t)strtoul(hex, &endp, 16);
            if (*endp != 0) {
                send_error("Invalid hex in 'data'");
                return;
            }
            uint32_t phys = 0;
            if (!LinearToPhysical((uint32_t)(lin_addr + i), phys)) {
                send_error("Page not present");
                return;
            }
            physdev_writeb((PhysPt64)phys, bval);
        }
        send_ok(json_hex("linear_addr", (uint32_t)lin_addr) + "," +
                json_num("bytes_written", byte_count) + "," +
                json_bool("paging", paging.enabled));
        return;
    }

    if (cmd == "wp_set") {
        // Set a write watchpoint on a linear address range.
        // {"cmd":"wp_set","linear":ADDR,"len":N}
        // Hits are delivered by the normal-core socket hooks; setting the
        // watchpoint must not depend on the current decoder pointer because
        // cycles=max can swap decoders after the CPU starts.
        long long linear, len;
        if (!json_get_int(json, "linear", linear)) {
            send_error("Missing 'linear'");
            return;
        }
        if (!json_get_int(json, "len", len) || len <= 0) {
            send_error("Missing or invalid 'len'");
            return;
        }
        int slot = -1;
        for (int i = 0; i < MAX_WATCHPOINTS; i++) {
            if (!watchpoints[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            send_error("No free watchpoint slots (max 16)");
            return;
        }
        watchpoints[slot].start     = (uint32_t)linear;
        watchpoints[slot].end       = (uint32_t)(linear + len);
        watchpoints[slot].active    = true;
        watchpoints[slot].hit_count = 0;
        rebuild_watchpoint_count();
        send_ok(json_num("slot", slot) + "," +
                json_hex("linear", (uint32_t)linear) + "," +
                json_num("len", len) + "," +
                json_bool("normalCoreHooksActive", debug_socket_normal_core_hooks_active != 0) + "," +
                json_str("note", "Watchpoint armed; hits require active core=normal socket hooks."));
        return;
    }

    if (cmd == "wp_clear") {
        // Clear watchpoints.  With "slot":N clears that slot; without, clears all.
        long long slot_val;
        if (json_get_int(json, "slot", slot_val)) {
            int slot = (int)slot_val;
            if (slot < 0 || slot >= MAX_WATCHPOINTS || !watchpoints[slot].active) {
                send_error("Invalid or inactive slot");
                return;
            }
            watchpoints[slot].active = false;
            rebuild_watchpoint_count();
            send_ok(json_num("slot", slot));
        } else {
            for (int i = 0; i < MAX_WATCHPOINTS; i++) watchpoints[i].active = false;
            rebuild_watchpoint_count();
            send_ok(json_str("msg", "All watchpoints cleared"));
        }
        return;
    }

    if (cmd == "wp_list") {
        std::string arr = "[";
        bool first = true;
        for (int i = 0; i < MAX_WATCHPOINTS; i++) {
            if (!watchpoints[i].active) continue;
            if (!first) arr += ",";
            first = false;
            arr += "{" +
                   json_num("slot",      i) + "," +
                   json_hex("linear",    watchpoints[i].start) + "," +
                   json_num("len",       (long long)(watchpoints[i].end - watchpoints[i].start)) + "," +
                   json_num("hit_count", (long long)watchpoints[i].hit_count) +
                   "}";
        }
        arr += "]";
        send_ok("\"watchpoints\":" + arr + "," + json_num("count", (long long)debug_watchpoint_count));
        return;
    }

    if (cmd == "catch_exceptions") {
        // Arm/disarm first-chance exception catching.
        // {"cmd":"catch_exceptions","vectors":[6,13,14],
        //  "skip":[{"vec":14,"count":1}],
        //  "cr2_ignore":[{"start":0,"end":65535}]}
        // Pass vectors:[] to disarm.
        std::string op;
        if (json_get_string(json, "op", op) && op == "status") {
            std::string vectors = "[";
            bool first = true;
            for (int i = 0; i < 32; i++) {
                if (!catch_exceptions_vectors[i]) continue;
                if (!first) vectors += ",";
                first = false;
                vectors += std::to_string(i);
            }
            vectors += "]";
            send_ok(json_bool("armed", catch_exceptions_armed) + "," +
                    "\"vectors\":" + vectors + "," +
                    json_num("skip_count", catch_exceptions_skip_count) + "," +
                    json_num("cr2_ignore_count", (long long)catch_exceptions_cr2_ignore.size()));
            return;
        }
        std::string vec_key_search = "\"vectors\":";
        if (json.find(vec_key_search) == std::string::npos) {
            send_error("Missing 'vectors' array");
            return;
        }
        // Parse vectors array (hand-rolled: look for numbers between [ and ])
        size_t vec_start = json.find('[', json.find(vec_key_search));
        size_t vec_end   = (vec_start != std::string::npos) ? json.find(']', vec_start) : std::string::npos;
        if (vec_start == std::string::npos || vec_end == std::string::npos) {
            send_error("Invalid 'vectors' array");
            return;
        }
        // Reset
        for (int i = 0; i < 32; i++) catch_exceptions_vectors[i] = false;
        catch_exceptions_skip_count = 0;
        catch_exceptions_cr2_ignore.clear();

        // Parse vector numbers
        std::string vec_str = json.substr(vec_start + 1, vec_end - vec_start - 1);
        size_t p = 0;
        while (p < vec_str.size()) {
            while (p < vec_str.size() && (vec_str[p] == ' ' || vec_str[p] == ',')) p++;
            if (p >= vec_str.size()) break;
            char* endp;
            long v = strtol(vec_str.c_str() + p, &endp, 0);
            if (endp == vec_str.c_str() + p) break;
            p = (size_t)(endp - vec_str.c_str());
            if (v >= 0 && v < 32) catch_exceptions_vectors[v] = true;
        }

        // Parse optional skip list: [{"vec":14,"count":1},...]
        size_t skip_key = json.find("\"skip\":");
        if (skip_key != std::string::npos) {
            size_t arr_s = json.find('[', skip_key);
            size_t arr_e = (arr_s != std::string::npos) ? json.find(']', arr_s) : std::string::npos;
            if (arr_s != std::string::npos && arr_e != std::string::npos) {
                std::string sub = json.substr(arr_s, arr_e - arr_s + 1);
                size_t obj = 0;
                while (catch_exceptions_skip_count < 32) {
                    obj = sub.find('{', obj);
                    if (obj == std::string::npos) break;
                    size_t obj_e = sub.find('}', obj);
                    if (obj_e == std::string::npos) break;
                    std::string entry = sub.substr(obj, obj_e - obj + 1);
                    long long vec_n = -1, cnt_n = 0;
                    json_get_int(entry, "vec",   vec_n);
                    json_get_int(entry, "count", cnt_n);
                    if (vec_n >= 0 && vec_n < 32) {
                        catch_exceptions_skip_list[catch_exceptions_skip_count].vec       = (uint8_t)vec_n;
                        catch_exceptions_skip_list[catch_exceptions_skip_count].remaining = (int)cnt_n;
                        catch_exceptions_skip_count++;
                    }
                    obj = obj_e + 1;
                }
            }
        }

        // Parse optional cr2_ignore list: [{"start":X,"end":Y},...]
        size_t cr2_key = json.find("\"cr2_ignore\":");
        if (cr2_key != std::string::npos) {
            size_t arr_s = json.find('[', cr2_key);
            size_t arr_e = (arr_s != std::string::npos) ? json.find(']', arr_s) : std::string::npos;
            if (arr_s != std::string::npos && arr_e != std::string::npos) {
                std::string sub = json.substr(arr_s, arr_e - arr_s + 1);
                size_t obj = 0;
                while (true) {
                    obj = sub.find('{', obj);
                    if (obj == std::string::npos) break;
                    size_t obj_e = sub.find('}', obj);
                    if (obj_e == std::string::npos) break;
                    std::string entry = sub.substr(obj, obj_e - obj + 1);
                    long long s_val = 0, e_val = 0;
                    json_get_int(entry, "start", s_val);
                    json_get_int(entry, "end",   e_val);
                    CR2IgnoreRange r;
                    r.start = (uint32_t)s_val;
                    r.end   = (uint32_t)e_val;
                    catch_exceptions_cr2_ignore.push_back(r);
                    obj = obj_e + 1;
                }
            }
        }

        // Arm if any vector is set
        catch_exceptions_armed = false;
        for (int i = 0; i < 32; i++) {
            if (catch_exceptions_vectors[i]) { catch_exceptions_armed = true; break; }
        }

        send_ok(json_bool("armed", catch_exceptions_armed) + "," +
                json_num("skip_count", catch_exceptions_skip_count) + "," +
                json_num("cr2_ignore_count", (long long)catch_exceptions_cr2_ignore.size()));
        return;
    }

    if (cmd == "break_on_exit") {
        // {"cmd":"break_on_exit","enable":true/false}
        long long en = 1;
        json_get_int(json, "enable", en);
        break_on_exit = (en != 0);
        send_ok(json_bool("break_on_exit", break_on_exit));
        return;
    }

    if (cmd == "get_last_exit") {
        if (!last_process_exit.valid) {
            send_ok(json_bool("available", false));
        } else {
            send_ok(json_bool("available", true) + "," +
                    json_num("exit_code", last_process_exit.exit_code) + "," +
                    json_num("psp",       last_process_exit.psp) + "," +
                    json_bool("tsr",      last_process_exit.tsr) + "," +
                    json_bool("abnormal", last_process_exit.abnormal));
        }
        return;
    }

    if (cmd == "reverse_trace") {
        // Bidirectional best-effort reverse checkpoints.
        // {"cmd":"reverse_trace","op":"start","max_checkpoints":512,"checkpoint":"branch"}
        // {"cmd":"reverse_trace","op":"stop"|"status"|"list"}
        // {"cmd":"reverse_trace","op":"back"|"forward","steps":N}
        // {"cmd":"reverse_trace","op":"goto","checkpoint_id":ID}
        std::string op;
        if (!json_get_string(json, "op", op)) {
            send_error("Missing 'op'");
            return;
        }

        if (op == "start") {
            std::string checkpoint_mode = "branch";
            std::string checkpoint_value;
            std::string mode_value;
            const bool has_checkpoint = json_get_string(json, "checkpoint", checkpoint_value);
            const bool has_mode = json_get_string(json, "mode", mode_value);
            if (has_checkpoint && has_mode && checkpoint_value != mode_value) {
                send_error("reverse_trace checkpoint and mode disagree");
                return;
            }
            if (has_checkpoint) {
                checkpoint_mode = checkpoint_value;
            } else if (has_mode) {
                checkpoint_mode = mode_value;
            }
            if (checkpoint_mode != "branch" && checkpoint_mode != "instruction") {
                send_error("reverse_trace checkpoint/mode must be 'branch' or 'instruction'");
                return;
            }

            long long max_val = (long long)REVERSE_DEFAULT_MAX_CHECKPOINTS;
            json_get_int(json, "max_checkpoints", max_val);
            if (max_val < (long long)REVERSE_MIN_CHECKPOINTS) max_val = (long long)REVERSE_MIN_CHECKPOINTS;
            if (max_val > (long long)REVERSE_MAX_CHECKPOINTS) max_val = (long long)REVERSE_MAX_CHECKPOINTS;

            reverse_reset((size_t)max_val);
            reverse_mode = checkpoint_mode == "instruction" ? REVERSE_CHECKPOINT_INSTRUCTION : REVERSE_CHECKPOINT_BRANCH;
            reverse_trace_enabled = true;
            debug_reverse_trace_active = 1;
            reverse_checkpoints.push_back(reverse_make_checkpoint());
            reverse_cursor = 0;
            send_ok(json_str("msg", "Reverse trace started") + "," +
                    json_bool("normalCoreHooksActive", debug_socket_normal_core_hooks_active != 0) + "," +
                    json_str("note", "Reverse checkpoints require active core=normal socket hooks.") + "," +
                    reverse_status_fields());
            return;
        }

        if (op == "stop") {
            const size_t old_count = reverse_checkpoints.size();
            reverse_reset(reverse_max_checkpoints);
            send_ok(json_str("msg", "Reverse trace stopped") + "," +
                    json_num("cleared_checkpoints", (long long)old_count));
            return;
        }

        if (op == "status") {
            send_ok(reverse_status_fields());
            return;
        }

        if (op == "list") {
            long long last_n = (long long)reverse_checkpoints.size();
            json_get_int(json, "last", last_n);
            if (last_n < 0) last_n = 0;
            if (last_n > (long long)reverse_checkpoints.size()) last_n = (long long)reverse_checkpoints.size();
            const size_t start = reverse_checkpoints.size() - (size_t)last_n;
            std::string arr = "[";
            bool first = true;
            for (size_t i = start; i < reverse_checkpoints.size(); i++) {
                if (!first) arr += ",";
                first = false;
                arr += reverse_checkpoint_json(reverse_checkpoints[i],
                                               i,
                                               reverse_trace_enabled && i == reverse_cursor,
                                               i + 1 == reverse_checkpoints.size());
            }
            arr += "]";
            send_ok(reverse_status_fields() + "," +
                    "\"checkpoints\":" + arr + "," +
                    json_num("returned", last_n));
            return;
        }

        if (op == "back" || op == "forward") {
            if (!reverse_trace_enabled || reverse_checkpoints.empty()) {
                send_error("reverse_trace is not enabled");
                return;
            }
            if (!reverse_require_navigation_stopped()) return;
            long long steps = 1;
            json_get_int(json, "steps", steps);
            if (steps < 1) {
                send_error("'steps' must be >= 1");
                return;
            }
            const size_t old_cursor = reverse_cursor;
            if (op == "back") {
                if (reverse_cursor == 0) {
                    send_ok(json_bool("noOp", true) + "," +
                            json_str("msg", "Already at oldest retained checkpoint") + "," +
                            reverse_navigation_fields("back", old_cursor));
                    return;
                }
                if ((uint64_t)steps > (uint64_t)reverse_cursor) {
                    send_error("Cannot move back that many checkpoints; request exceeds retained history");
                    return;
                }
                reverse_navigate_to(reverse_cursor - (size_t)steps, "back");
            } else {
                const size_t newest = reverse_checkpoints.size() - 1;
                if (reverse_cursor >= newest) {
                    send_ok(json_bool("noOp", true) + "," +
                            json_str("msg", "Already at newest checkpoint") + "," +
                            reverse_navigation_fields("forward", old_cursor));
                    return;
                }
                if ((uint64_t)steps > (uint64_t)(newest - reverse_cursor)) {
                    send_error("Cannot move forward that many checkpoints; request exceeds forward history");
                    return;
                }
                reverse_navigate_to(reverse_cursor + (size_t)steps, "forward");
            }
            return;
        }

        if (op == "goto") {
            if (!reverse_trace_enabled || reverse_checkpoints.empty()) {
                send_error("reverse_trace is not enabled");
                return;
            }
            if (!reverse_require_navigation_stopped()) return;
            long long checkpoint_id = -1;
            if (!json_get_int(json, "checkpoint_id", checkpoint_id) || checkpoint_id < 0) {
                send_error("Missing or invalid 'checkpoint_id'");
                return;
            }
            size_t target = reverse_checkpoints.size();
            for (size_t i = 0; i < reverse_checkpoints.size(); i++) {
                if (reverse_checkpoints[i].id == (uint64_t)checkpoint_id) {
                    target = i;
                    break;
                }
            }
            if (target >= reverse_checkpoints.size()) {
                send_error("checkpoint_id is not in retained history");
                return;
            }
            reverse_navigate_to(target, "goto");
            return;
        }

        send_error("Unknown reverse_trace op (start|stop|status|list|back|forward|goto)");
        return;
    }

    if (cmd == "trace") {
        // Branch trace ring control.
        // {"cmd":"trace","op":"start"|"stop"|"status"}
        // {"cmd":"trace","op":"dump","last":N}
        std::string op;
        if (!json_get_string(json, "op", op)) {
            send_error("Missing 'op'");
            return;
        }
        if (op == "start") {
            branch_trace_enabled = true;
            branch_ring_head  = 0;
            branch_ring_count = 0;
            send_ok(json_str("msg", "Branch trace started") + "," +
                    json_num("ring_size", BRANCH_RING_SIZE));
        } else if (op == "stop") {
            branch_trace_enabled = false;
            send_ok(json_str("msg", "Branch trace stopped") + "," +
                    json_num("entries", (long long)branch_ring_count));
        } else if (op == "status") {
            send_ok(json_bool("enabled",  branch_trace_enabled) + "," +
                    json_num("entries",   (long long)branch_ring_count) + "," +
                    json_num("ring_size", BRANCH_RING_SIZE));
        } else if (op == "dump") {
            long long last_n;
            if (!json_get_int(json, "last", last_n) || last_n <= 0)
                last_n = (long long)branch_ring_count;
            if (last_n > BRANCH_RING_SIZE) last_n = BRANCH_RING_SIZE;
            if (last_n > (long long)branch_ring_count) last_n = (long long)branch_ring_count;

            // Walk the ring backwards from most recent entry
            std::string arr = "[";
            bool first = true;
            for (long long k = last_n - 1; k >= 0; k--) {
                uint32_t idx = (uint32_t)((branch_ring_head - 1 - k + BRANCH_RING_SIZE * 2)
                                          % BRANCH_RING_SIZE);
                const BranchEntry& e = branch_ring[idx];
                if (!first) arr += ",";
                first = false;
                char fbuf[24], tbuf[24];
                snprintf(fbuf, sizeof(fbuf), "0x%08X", e.from_linear);
                snprintf(tbuf, sizeof(tbuf), "0x%08X", e.to_linear);
                arr += "{" +
                       json_num("from_cs",     (long long)e.from_cs) + "," +
                       json_str("from",        fbuf) + "," +
                       json_num("to_cs",       (long long)e.to_cs) + "," +
                       json_str("to",          tbuf) +
                       "}";
            }
            arr += "]";
            send_ok("\"entries\":" + arr + "," + json_num("count", last_n));
        } else {
            send_error("Unknown trace op (start|stop|status|dump)");
        }
        return;
    }

    if (cmd == "floppy_swap") {
        // Swap the active disk image on a mounted floppy/CD/HDD drive.
        // Works while a DOS program is running — no shell dependency.
        //
        // {"cmd":"floppy_swap","drive":"A"}
        //   Cycle to the next image in the pre-loaded swap list (same as GUI "Swap disk").
        //   Requires the drive to have been imgmounted with multiple images.
        //
        // {"cmd":"floppy_swap","drive":"A","image":"/host/path/Disk02.img"}
        //   Replace the currently-active image with a new file from the host filesystem.
        //   Equivalent to DriveManager::ChangeDisk — updates both the DOS filesystem
        //   layer (Drives[]) and the BIOS INT 13h layer (imageDiskList[]).
        std::string drive_str;
        if (!json_get_string(json, "drive", drive_str) || drive_str.empty()) {
            send_error("Missing 'drive'");
            return;
        }
        int driveIdx = toupper((unsigned char)drive_str[0]) - 'A';
        if (driveIdx < 0 || driveIdx >= DOS_DRIVES) {
            send_error("Invalid drive letter");
            return;
        }
        if (Drives[driveIdx] == nullptr) {
            send_error("No drive mounted at this letter");
            return;
        }

        std::string image_path;
        bool has_image = json_get_string(json, "image", image_path) && !image_path.empty();

        if (!has_image) {
            // Cycle to the next image in the existing DriveManager swap list.
            if (DriveManager::GetDisksSize(driveIdx) < 1) {
                send_error("No disk images in swap list; use imgmount with multiple images first");
                return;
            }
            swapInDrive(driveIdx, 0);
            char pos_buf[32];
            snprintf(pos_buf, sizeof(pos_buf), "%s", DriveManager::GetDrivePosition(driveIdx));
            send_ok(json_str("msg", "Disk swapped (cycled to next)") + "," +
                    json_str("drive", std::string(1, (char)('A' + driveIdx))) + "," +
                    json_str("position", pos_buf));
            return;
        }

        // Replace the currently-active image with a new file.
        // Create a fatDrive from the image path (auto-detects geometry when sizes are 0).
        if (DriveManager::GetDisksSize(driveIdx) < 1) {
            send_error("Drive has no managed disk list; use imgmount first then use floppy_swap");
            return;
        }
        std::vector<std::string> opts;
        fatDrive *newDrive = new fatDrive(image_path.c_str(), 0, 0, 0, 0, opts);
        if (!newDrive->created_successfully) {
            delete newDrive;
            send_error(("Failed to open image: " + image_path).c_str());
            return;
        }
        // ChangeDisk: replaces current slot in DriveManager, updates Drives[] and imageDiskList[].
        DriveManager::ChangeDisk(driveIdx, newDrive);
        send_ok(json_str("msg", "New image loaded") + "," +
                json_str("drive", std::string(1, (char)('A' + driveIdx))) + "," +
                json_str("image", image_path) + "," +
                json_str("position", DriveManager::GetDrivePosition(driveIdx)));
        return;
    }

    if (cmd == "floppy_load_list") {
        // Pre-load a list of disk images into a drive's swap list.
        // Replicates what imgmount does with multiple image paths, without going through
        // the DOS shell parser.  After this call, floppy_swap (no image) cycles through
        // the list using the existing swapInDrive / DriveManager::CycleDisks mechanism.
        //
        // {"cmd":"floppy_load_list","drive":"A","images":["/abs/Disk01.img","/abs/Disk02.img",...]}
        //   • Clears any existing swap list for the drive.
        //   • Creates a fatDrive for each path (same as imgmount -t floppy -fs fat).
        //   • Populates diskSwap[] (BIOS INT 13h layer) and DriveManager (DOS filesystem
        //     layer) with the new images in order.
        //   • Makes the first image active immediately.
        //   • Returns {"status":"ok","msg":"N images loaded for drive A","count":N}.
        std::string drive_str;
        if (!json_get_string(json, "drive", drive_str) || drive_str.empty()) {
            send_error("Missing 'drive'");
            return;
        }
        int driveIdx = toupper((unsigned char)drive_str[0]) - 'A';
        if (driveIdx < 0 || driveIdx >= DOS_DRIVES) {
            send_error("Invalid drive letter");
            return;
        }
        if (Drives[driveIdx] == nullptr) {
            send_error("No drive mounted at this letter");
            return;
        }

        std::vector<std::string> paths;
        if (!json_get_string_array(json, "images", paths) || paths.empty()) {
            send_error("Missing or empty 'images' array");
            return;
        }
        if ((int)paths.size() > MAX_SWAPPABLE_DISKS) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Too many images (max %d)", MAX_SWAPPABLE_DISKS);
            send_error(errbuf);
            return;
        }

        // Create fatDrive for every path (auto-detects geometry when sizes are 0,
        // matching what imgmount does for -t floppy -fs fat).
        std::vector<fatDrive*> newDrives;
        std::vector<std::string> opts;
        for (const auto& p : paths) {
            fatDrive* fd = new fatDrive(p.c_str(), 0, 0, 0, 0, opts);
            if (!fd->created_successfully) {
                delete fd;
                for (auto* d : newDrives) delete d;
                send_error(("Failed to open image: " + p).c_str());
                return;
            }
            newDrives.push_back(fd);
        }

        // Clear old diskSwap[] entries that belong to this drive (BIOS layer).
        if (swapInDisksSpecificDrive == driveIdx || swapInDisksSpecificDrive == -1) {
            for (size_t si = 0; si < MAX_SWAPPABLE_DISKS; si++) {
                if (diskSwap[si] != NULL) {
                    diskSwap[si]->Release();
                    diskSwap[si] = NULL;
                }
            }
            swapInDisksSpecificDrive = -1;
        }

        // Replace the DriveManager disk list for this drive (DOS filesystem layer).
        // ClearDrive unmounts (and deletes) all existing disks; AppendDisk + InitializeDrive
        // rebuilds the list and makes newDrives[0] the active drive in Drives[].
        DriveManager::ClearDrive(driveIdx);
        for (auto* fd : newDrives) {
            DriveManager::AppendDisk(driveIdx, fd);
        }
        DriveManager::InitializeDrive(driveIdx);

        // Populate diskSwap[] with the underlying imageDisk from each fatDrive
        // (same pattern as imgmount multi-image for floppy drives).
        for (size_t si = 0; si < newDrives.size() && si < MAX_SWAPPABLE_DISKS; si++) {
            imageDisk* img = newDrives[si]->loadedDisk;
            if (img != NULL) {
                diskSwap[si] = img;
                diskSwap[si]->Addref();
            }
        }
        swapPosition = 0;

        // Only floppy drives (A: = 0, B: = 1) use swapInDisksSpecificDrive and imageDiskList.
        if (driveIdx < 2) {
            swapInDisksSpecificDrive = driveIdx;
            imageDisk* firstImg = newDrives[0]->loadedDisk;
            if (firstImg != NULL) {
                if (imageDiskList[driveIdx] != NULL) {
                    imageDiskList[driveIdx]->Release();
                }
                imageDiskList[driveIdx] = firstImg;
                imageDiskList[driveIdx]->Addref();
                imageDiskChange[driveIdx] = true;
            }
        }

        char msg_buf[64];
        snprintf(msg_buf, sizeof(msg_buf), "%d image%s loaded for drive %c",
                 (int)newDrives.size(), newDrives.size() == 1 ? "" : "s",
                 'A' + driveIdx);
        send_ok(json_str("msg", msg_buf) + "," +
                json_str("drive", std::string(1, (char)('A' + driveIdx))) + "," +
                json_num("count", (long long)newDrives.size()));
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

    // Move to a high fd (>= 100) to avoid collision with disk-image file descriptors
    // that are opened in low-numbered slots by imgmount / floppy_load_list.
    server_socket = socket_bump_fd(server_socket);

    // Allow reuse
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

#if defined(__APPLE__) || defined(__MACH__)
    // On macOS, suppress SIGPIPE at the socket level (MSG_NOSIGNAL is unavailable).
    setsockopt(server_socket, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

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
            // Move to a high fd (>= 100) before doing anything else so that
            // subsequent fclose() calls on disk-image files cannot accidentally
            // recycle this fd number and silently break the socket.
            client_socket = socket_bump_fd(client_socket);

            // Set non-blocking
            int flags = fcntl(client_socket, F_GETFL, 0);
            fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);

#if defined(__APPLE__) || defined(__MACH__)
            int nosig = 1;
            setsockopt(client_socket, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
            LOG_MSG("DEBUG_Socket: Client connected (fd %d)", client_socket);
            
            // Reset protocol mode for new connection
            gdb_mode = false;
            
            // Send initial state (JSON format - will switch to GDB if first packet is GDB RSP)
            send_response("{" + json_str("event", "connected") + "," + socket_state_fields() + "}");
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
        std::string load_info;
        if (g_exec_breakpoint_pending &&
            g_exec_breakpoint_seg == seg &&
            g_exec_breakpoint_off == off &&
            load_info_matches_entry(seg, off)) {
            load_info = "," + load_info_json_field();
        }
        send_and_latch_stop("{" + json_str("event", "stopped") + "," +
                            stop_context_json("breakpoint", seg, off) + "," +
                            json_str("addr", addr) + "," + get_registers_json() +
                            load_info + "}");
    }
}

/* ---- allocation trace -------------------------------------------------
   Every DOS and EMS memory call, logged where it is made rather than
   where it lands. The guest cannot do this to itself usefully: a tracer
   inside the program costs the very conventional memory it is trying to
   account for, and misses whatever uGL and the C runtime do behind their
   own APIs. Here it costs the guest nothing at all.

   Requests only, not results -- the INT has not dispatched yet, so AX
   does not hold the returned segment. What a request records (who asked,
   for how much, in what order) is what an allocation profile needs;
   whether it succeeded is already visible in the program's own output. */

void DEBUG_Socket_TraceAlloc(uint8_t intNum) {
    if (!alloc_trace_fp) return;
    if (intNum != 0x21 && intNum != 0x67) return;

    const char* what = NULL;
    uint32_t    arg  = 0;

    if (intNum == 0x21) {
        switch (reg_ah) {
            case 0x48: what = "dos_alloc";  arg = reg_bx;        break; /* paragraphs */
            case 0x49: what = "dos_free";   arg = SegValue(es);  break;
            case 0x4A: what = "dos_resize"; arg = reg_bx;        break;
            default: return;
        }
    } else {
        switch (reg_ah) {
            case 0x43: what = "ems_alloc";  arg = reg_bx;        break; /* 16K pages */
            case 0x45: what = "ems_free";   arg = reg_dx;        break; /* handle */
            case 0x47: what = "ems_savemap";arg = reg_dx;        break;
            case 0x48: what = "ems_restmap";arg = reg_dx;        break;
            default: return;
        }
    }

    fprintf(alloc_trace_fp, "%u %s arg=%u bytes=%u caller=%04X:%04X\n",
            (unsigned)alloc_trace_n++, what, (unsigned)arg,
            (unsigned)(intNum == 0x21 ? arg * 16u : arg * 16384u),
            (unsigned)SegValue(cs), (unsigned)reg_eip);
    fflush(alloc_trace_fp);   /* a run that dies mid-load still leaves the trail */
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
        const std::string stop_json = "{" + json_str("event", "stopped") + "," +
                                      stop_context_json("interrupt", seg, off, intNum) + "," +
                                      json_str("addr", addr) + "," + get_registers_json() + "}";
        if (intNum == 0x06 || intNum == 0x0D || intNum == 0x0E) {
            send_and_latch_fault_stop(stop_json);
        } else {
            send_and_latch_stop(stop_json);
        }
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
    socket_freeze_loop_active = true;
    while (socket_freeze_wait) {
        DEBUG_Socket_CheckCommands();
        GFX_Events();
        usleep(1000);  // 1ms; keeps host responsive without busy-spinning
    }
    socket_freeze_loop_active = false;
}

bool DEBUG_Socket_IsActive(void) {
    return server_socket >= 0;
}

int DEBUG_Socket_GetPort(void) {
    return socket_port;
}

#endif // C_DEBUG

