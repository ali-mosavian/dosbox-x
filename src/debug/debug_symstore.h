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

/* A source line, with the address range it covers already made linear. */
struct DebugSourceLine {
	uint32_t begin = 0;
	uint32_t end = 0;		/* one past the last byte it covers */
	uint16_t line = 0;
	std::string file;
	std::string module;
	std::string program;
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

private:
	std::vector<DebugSymbol> symbols;
	std::vector<DebugSourceLine> lines;
};

DebugSymbolStore &DEBUG_Symbols(void);

#endif
