/*
 * debug_symstore.cpp - the symbol store.
 */

#include "debug_symstore.h"

#include <ctype.h>

static DebugSymbolStore debug_symbols;

DebugSymbolStore &DEBUG_Symbols(void)
{
	return debug_symbols;
}

static std::string SymUpper(const std::string &text)
{
	std::string out = text;
	for (size_t i = 0;i < out.size();i++) out[i] = (char)toupper((unsigned char)out[i]);
	return out;
}

/* A module is named by a path in some formats and a bare name in others, so
 * "libc" has to match "libc.d32" and "\\src\\libc.obj" alike. */
static bool ModuleNameMatches(const std::string &candidate,const std::string &requested)
{
	if (candidate.empty()) return false;

	const std::string want = SymUpper(requested);
	const std::string have = SymUpper(candidate);
	if (have == want) return true;

	const size_t slash = have.find_last_of("/\\");
	std::string base = slash == std::string::npos ? have : have.substr(slash + 1);

	const size_t dot = base.find_last_of('.');
	if (dot != std::string::npos) base.erase(dot);

	std::string bare = want;
	const size_t wantDot = bare.find_last_of('.');
	if (wantDot != std::string::npos) bare.erase(wantDot);
	return base == bare;
}

void DebugSymbolStore::Clear()
{
	symbols.clear();
	lines.clear();
}

void DebugSymbolStore::ClearProgram(const std::string &program)
{
	std::vector<DebugSymbol> keptSymbols;
	for (size_t i = 0;i < symbols.size();i++)
		if (symbols[i].program != program) keptSymbols.push_back(symbols[i]);
	symbols.swap(keptSymbols);

	std::vector<DebugSourceLine> keptLines;
	for (size_t i = 0;i < lines.size();i++)
		if (lines[i].program != program) keptLines.push_back(lines[i]);
	lines.swap(keptLines);
}

void DebugSymbolStore::Add(const DebugSymbol &symbol)
{
	symbols.push_back(symbol);
}

void DebugSymbolStore::AddDebugInfo(const DebugInfo &info,const std::string &program)
{
	for (size_t i = 0;i < info.symbols.size();i++) {
		DebugSymbol symbol = info.symbols[i];
		symbol.program = program;
		symbols.push_back(symbol);
	}

	/* Line ranges arrive load-relative; the store answers in linear
	 * addresses, so the load base is applied once here rather than at every
	 * lookup. */
	for (size_t i = 0;i < info.lines.size();i++) {
		DebugSourceLine line;
		line.begin = info.loadLinear + info.lines[i].imageOffset;
		line.end = info.loadLinear + info.lines[i].endOffset;
		line.line = info.lines[i].line;
		line.file = info.lines[i].file;
		line.module = info.lines[i].module;
		line.program = program;
		lines.push_back(line);
	}
}

void DebugSymbolStore::AddLinkMap(const LinkMapFile &map,uint32_t loadLinear,const std::string &program)
{
	/* The map keys each public twice, as written and upper-cased, so adding
	 * every entry would double every symbol. */
	for (std::map<std::string,LinkMapPublic>::const_iterator it = map.publics.begin();it != map.publics.end();++it) {
		if (it->first != it->second.name) continue;

		DebugSymbol symbol;
		symbol.name = it->second.name;
		symbol.offset = it->second.address.offset;
		symbol.segment = it->second.address.segment;
		symbol.segmentBase = (uint32_t)it->second.address.segment << 4u;
		symbol.linear = loadLinear + it->second.address.mapOffset;
		symbol.source = DEBUG_FORMAT_MAP;
		symbol.program = program;
		symbols.push_back(symbol);
	}
}

const DebugSymbol *DebugSymbolStore::Resolve(const std::string &name) const
{
	const size_t bang = name.find('!');
	if (bang != std::string::npos) {
		const std::string module = name.substr(0,bang);
		const std::string wanted = SymUpper(name.substr(bang + 1));

		const DebugSymbol *best = NULL;
		for (size_t i = 0;i < symbols.size();i++) {
			if (SymUpper(symbols[i].name) != wanted) continue;
			if (!ModuleNameMatches(symbols[i].module,module)) continue;
			if (best == NULL || symbols[i].linear < best->linear) best = &symbols[i];
		}
		return best;
	}

	for (size_t i = 0;i < symbols.size();i++)
		if (symbols[i].name == name) return &symbols[i];

	const std::string upper = SymUpper(name);
	for (size_t i = 0;i < symbols.size();i++)
		if (SymUpper(symbols[i].name) == upper) return &symbols[i];
	return NULL;
}

const DebugSymbol *DebugSymbolStore::Nearest(uint32_t linear,uint32_t &delta) const
{
	const DebugSymbol *best = NULL;
	for (size_t i = 0;i < symbols.size();i++) {
		const DebugSymbol &symbol = symbols[i];
		if (symbol.linear > linear) continue;

		const uint32_t distance = linear - symbol.linear;
		if (symbol.hasSize && symbol.size > 0) {
			if (distance >= symbol.size) continue;
		} else if (distance >= 0x10000u) {
			/* Most formats give no size, and without a bound the lowest
			 * symbol in the store answers for the whole address space: at a
			 * BIOS address, cvprobe.exe's _end came back with +0xF06E6 on it.
			 * A 16-bit symbol can only be reached through its own segment, so
			 * nothing 64K past it is inside it. */
			continue;
		}
		if (best == NULL || distance < delta) {
			best = &symbol;
			delta = distance;
		}
	}
	return best;
}

const DebugSourceLine *DebugSymbolStore::LineAt(uint32_t linear) const
{
	const DebugSourceLine *best = NULL;
	for (size_t i = 0;i < lines.size();i++) {
		if (linear < lines[i].begin || linear >= lines[i].end) continue;
		if (best == NULL || lines[i].begin > best->begin) best = &lines[i];
	}
	return best;
}

std::string DebugSymbolStore::Describe(uint32_t linear) const
{
	uint32_t delta = 0;
	const DebugSymbol *found = Nearest(linear,delta);
	if (found == NULL) return std::string();

	std::string out;
	if (!found->module.empty()) out = found->module + ":";
	out += found->name;
	if (delta != 0) out += "+" + DEBUG_Hex(delta);
	return out;
}
