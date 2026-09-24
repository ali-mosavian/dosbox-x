/*
 * debug_symstore.h - the emulator's symbol store.
 *
 * One place every symbol source feeds into -- debug info appended to the
 * program by its linker, or a .MAP the user supplies -- so that name and
 * address lookups work the same whatever produced them.
 */

#ifndef DOSBOX_DEBUG_SYMSTORE_H
#define DOSBOX_DEBUG_SYMSTORE_H

#include "debug_symfmt.h"

/* A program's segment, made linear, and the process whose memory it is. */
struct DebugProgramSegment {
	uint32_t begin = 0;
	uint32_t end = 0;
	bool code = false;
	std::string program;
	uint16_t psp = 0;
};

/* A source line, with the address range it covers already made linear. */
struct DebugSourceLine {
	uint32_t begin = 0;
	uint32_t end = 0;		/* one past the last byte it covers */
	uint16_t line = 0;
	std::string file;
	std::string module;
	std::string program;
};

/* A scope as the store keeps it: the code it covers, in linear addresses. */
struct DebugScopeEntry {
	uint32_t begin = 0;
	uint32_t end = 0;
	int32_t parent = -1;		/* index into the store's own list */
	std::string function;
	std::string program;
	std::vector<DebugLocal> locals;
};

/* Where a location spec landed, and what it went through to get there. */
struct DebugLocation {
	uint32_t linear = 0;
	std::string kind;		/* "address", "symbol" or "line" */
	std::string description;
	std::string symbol;
	uint32_t delta = 0;		/* offset past the symbol, for "symbol" */
	std::string file;		/* for "line" */
	uint16_t line = 0;
	bool exactLine = true;		/* false when the next line with code was used */
};

class DebugSymbolStore {
public:
	void Clear();
	/* Drops what an earlier load of the same program left behind, so
	 * re-running one does not leave two sets of stale addresses. */
	void ClearProgram(const std::string &program);

	void Add(const DebugSymbol &symbol);
	void AddDebugInfo(const DebugInfo &info,const std::string &program);
	void AddLinkMap(const LinkMapFile &map,uint32_t loadLinear,const std::string &program);

	size_t Size() const { return symbols.size(); }
	size_t LineCount() const { return lines.size(); }
	const std::vector<DebugSymbol> &All() const { return symbols; }

	/* Exact name, then case-insensitive, then "module!name". */
	const DebugSymbol *Resolve(const std::string &name) const;

	/* The closest symbol at or below linear, ignoring one whose own size says
	 * the address is past its end. */
	const DebugSymbol *Nearest(uint32_t linear,uint32_t &delta) const;

	/* "module:name+0x0000000C", or empty when nothing covers the address. */
	std::string Describe(uint32_t linear) const;

	/* The source line covering an address, if any line covers it. */
	const DebugSourceLine *LineAt(uint32_t linear) const;

	const std::vector<DebugSourceLine> &Lines() const { return lines; }

	/* A program's layout, from its debug info or map, for the process at
	 * `psp`. The load image [loadLinear, loadLinear + imageBytes) backs the
	 * segments: a format that lists only code, as TDINFO does, still says
	 * the rest of the image is not. */
	void AddSegments(const std::vector<DebugSegment> &segments,uint32_t loadLinear,uint32_t imageBytes,
	                 const std::string &program,uint16_t psp);

	/* The data segment covering an address, while the process that loaded it
	 * still owns that memory; NULL for code, where code and data overlap, and
	 * where no live layout covers it. Cheap for code: the core asks on every
	 * transfer. */
	const DebugProgramSegment *ProgramDataAt(uint32_t linear) const;

	/* Locals and parameters of the innermost scope covering an address,
	 * then of each scope around it. A name declared twice resolves to the
	 * innermost one, the way the language scopes it. */
	bool ResolveLocal(uint32_t pcLinear,const std::string &name,DebugLocal &out,std::string &function) const;

	/* Everything in scope at an address, innermost first. functions[i] names
	 * the scope out[i] came from. */
	void LocalsAt(uint32_t pcLinear,std::vector<DebugLocal> &out,std::vector<std::string> &functions) const;

	/* The lowest address of a source line. A line with no code of its own
	 * resolves forward to the next one that has some, the way a debugger
	 * moves a breakpoint set on a blank line or a declaration; used says
	 * which line that was. The file matches on its name or its basename,
	 * case-insensitively: a program says TDSPROBE.C where the user types
	 * tdsprobe.c. */
	const DebugSourceLine *LineFor(const std::string &file,uint16_t line,uint16_t &used) const;

	/*
	 * A gdb-style location:
	 *
	 *   file.c:29     the line, or the next line that has code
	 *   func          a symbol, "module!func" included
	 *   func+0x10     that many bytes past one
	 *   *0x9C7E       an address, spelled out
	 *   0x9C7E        an address; a bare number is never a line number
	 *
	 * A segment:offset pair is not handled here -- what it means depends on
	 * the CPU mode, which the store knows nothing about.
	 */
	bool ResolveLocation(const std::string &spec,DebugLocation &out,std::string &error) const;

private:
	/* The layout piece covering an address, of any program, live or not. */
	const DebugProgramSegment *PieceAt(uint32_t linear) const;

	/* The innermost scope covering an address, or -1. */
	int32_t InnermostScope(uint32_t pcLinear) const;

	std::vector<DebugSymbol> symbols;
	std::vector<DebugSourceLine> lines;
	std::vector<DebugScopeEntry> scopes;
	std::vector<DebugProgramSegment> segments;
	/* segments flattened: sorted, disjoint, code winning where they overlap */
	std::vector<DebugProgramSegment> pieces;
	void Flatten();
};

DebugSymbolStore &DEBUG_Symbols(void);

#endif
