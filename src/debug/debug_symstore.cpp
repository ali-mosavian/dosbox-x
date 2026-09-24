/*
 * debug_symstore.cpp - the symbol store.
 */

#include "debug_symstore.h"
#include "dos_inc.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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
	scopes.clear();
	segments.clear();
	pieces.clear();
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

	/* Scopes point at each other by index, so dropping some renumbers the
	 * rest; the parents are rewritten rather than left dangling. */
	std::vector<int32_t> mapping(scopes.size(),-1);
	std::vector<DebugScopeEntry> keptScopes;
	for (size_t i = 0;i < scopes.size();i++) {
		if (scopes[i].program == program) continue;
		mapping[i] = (int32_t)keptScopes.size();
		keptScopes.push_back(scopes[i]);
	}
	for (size_t i = 0;i < keptScopes.size();i++) {
		const int32_t parent = keptScopes[i].parent;
		keptScopes[i].parent = parent >= 0 && (size_t)parent < mapping.size() ? mapping[parent] : -1;
	}
	scopes.swap(keptScopes);

	std::vector<DebugProgramSegment> keptSegments;
	for (size_t i = 0;i < segments.size();i++)
		if (segments[i].program != program) keptSegments.push_back(segments[i]);
	segments.swap(keptSegments);
	Flatten();
}

void DebugSymbolStore::AddSegments(const std::vector<DebugSegment> &from,uint32_t loadLinear,uint32_t imageBytes,
                                   const std::string &program,uint16_t psp)
{
	if (from.empty()) return;
	DebugProgramSegment image;
	image.begin = loadLinear;
	image.end = loadLinear + imageBytes;
	image.program = program;
	image.psp = psp;
	if (imageBytes) segments.push_back(image);
	for (size_t i = 0;i < from.size();i++) {
		DebugProgramSegment segment = image;
		segment.begin = loadLinear + from[i].imageOffset;
		segment.end = loadLinear + from[i].endOffset;
		segment.code = from[i].code;
		segments.push_back(segment);
	}
	Flatten();
}

void DebugSymbolStore::Flatten()
{
	std::vector<uint32_t> bounds;
	for (size_t i = 0;i < segments.size();i++) {
		bounds.push_back(segments[i].begin);
		bounds.push_back(segments[i].end);
	}
	std::sort(bounds.begin(),bounds.end());
	bounds.erase(std::unique(bounds.begin(),bounds.end()),bounds.end());

	pieces.clear();
	for (size_t b = 0;b + 1 < bounds.size();b++) {
		const DebugProgramSegment *cover = NULL;
		for (size_t i = 0;i < segments.size();i++) {
			const DebugProgramSegment &segment = segments[i];
			if (segment.begin > bounds[b] || segment.end < bounds[b+1]) continue;
			if (cover == NULL || (segment.code && !cover->code)) cover = &segment;
		}
		if (cover == NULL) continue;
		DebugProgramSegment piece = *cover;
		piece.begin = bounds[b];
		piece.end = bounds[b+1];
		pieces.push_back(piece);
	}
}

static bool BeginsAfter(uint32_t linear,const DebugProgramSegment &piece) { return linear < piece.begin; }

const DebugProgramSegment *DebugSymbolStore::PieceAt(uint32_t linear) const
{
	std::vector<DebugProgramSegment>::const_iterator next =
		std::upper_bound(pieces.begin(),pieces.end(),linear,BeginsAfter);
	if (next == pieces.begin() || linear >= (next - 1)->end) return NULL;
	return &*(next - 1);
}

const DebugProgramSegment *DebugSymbolStore::ProgramDataAt(uint32_t linear) const
{
	const DebugProgramSegment *found = PieceAt(linear);
	if (found == NULL || found->code) return NULL;
	const DebugProgramSegment &piece = *found;
	/* Another program may have this memory now; its layout is not this one. */
	uint16_t owner = 0, start = 0, end = 0;
	if (!DOS_MemoryBlockAt((uint16_t)(linear >> 4),owner,start,end) || owner != piece.psp) return NULL;
	return &piece;
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

	const size_t scopeBase = scopes.size();
	for (size_t i = 0;i < info.scopes.size();i++) {
		DebugScopeEntry scope;
		scope.begin = info.loadLinear + info.scopes[i].imageOffset;
		scope.end = info.loadLinear + info.scopes[i].endOffset;
		scope.parent = info.scopes[i].parent < 0
			? -1
			: (int32_t)(scopeBase + (size_t)info.scopes[i].parent);
		scope.function = info.scopes[i].function;
		scope.locals = info.scopes[i].locals;
		scope.program = program;
		scopes.push_back(scope);
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
	/* Nor past the end of the segment it is in, when the layout says where
	 * that is: a return into the stack named the last variable before it.
	 * Pieces are disjoint, so no lower symbol can reach further. */
	if (best != NULL && !(best->hasSize && best->size > 0)) {
		const DebugProgramSegment *piece = PieceAt(best->linear);
		if (piece != NULL && linear >= piece->end) return NULL;
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

/* A file is named by its full path in some formats and its basename in
 * others, and the case is the toolchain's choice, not the user's. */
static bool FileNameMatches(const std::string &candidate,const std::string &requested)
{
	if (candidate.empty()) return false;

	const std::string have = SymUpper(candidate);
	const std::string want = SymUpper(requested);
	if (have == want) return true;

	const size_t slash = have.find_last_of("/\\");
	return slash != std::string::npos && have.substr(slash + 1) == want;
}

const DebugSourceLine *DebugSymbolStore::LineFor(const std::string &file,uint16_t line,uint16_t &used) const
{
	const DebugSourceLine *exact = NULL;
	const DebugSourceLine *next = NULL;

	for (size_t i = 0;i < lines.size();i++) {
		if (!file.empty() && !FileNameMatches(lines[i].file,file)) continue;

		if (lines[i].line == line) {
			if (exact == NULL || lines[i].begin < exact->begin) exact = &lines[i];
		} else if (lines[i].line > line) {
			if (next == NULL || lines[i].line < next->line ||
			    (lines[i].line == next->line && lines[i].begin < next->begin))
				next = &lines[i];
		}
	}

	const DebugSourceLine *found = exact != NULL ? exact : next;
	if (found != NULL) used = found->line;
	return found;
}

/* Accepts 0x-prefixed hex and plain decimal, and nothing else: "0010" as an
 * address is a real risk of being read as octal or hex by accident. */
static bool ParseNumber(const std::string &text,uint32_t &out)
{
	if (text.empty()) return false;

	char *end = NULL;
	const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
	const unsigned long value = strtoul(text.c_str(),&end,hex ? 16 : 10);
	if (end == NULL || *end != 0) return false;

	out = (uint32_t)value;
	return true;
}

static std::string TrimSpec(const std::string &text)
{
	size_t begin = 0,end = text.size();
	while (begin < end && isspace((unsigned char)text[begin])) begin++;
	while (end > begin && isspace((unsigned char)text[end-1])) end--;
	return text.substr(begin,end - begin);
}

static bool AllDigits(const std::string &text)
{
	if (text.empty()) return false;
	for (size_t i = 0;i < text.size();i++)
		if (!isdigit((unsigned char)text[i])) return false;
	return true;
}

bool DebugSymbolStore::ResolveLocation(const std::string &spec,DebugLocation &out,std::string &error) const
{
	const std::string text = TrimSpec(spec);
	if (text.empty()) {
		error = "Empty location";
		return false;
	}

	if (text[0] == '*') {
		if (!ParseNumber(TrimSpec(text.substr(1)),out.linear)) {
			error = "Not an address: " + text;
			return false;
		}
		out.kind = "address";
		out.description = DEBUG_Hex(out.linear);
		return true;
	}

	/* file:line, told from a symbol by the digits after the colon. */
	const size_t colon = text.find_last_of(':');
	if (colon != std::string::npos && AllDigits(text.substr(colon + 1))) {
		const std::string file = text.substr(0,colon);
		const uint32_t wanted = (uint32_t)atoi(text.c_str() + colon + 1);

		uint16_t used = 0;
		const DebugSourceLine *found = LineFor(file,(uint16_t)wanted,used);
		if (found == NULL) {
			error = "No line table entry for " + text;
			return false;
		}

		char buf[32];
		snprintf(buf,sizeof(buf),"%u",(unsigned int)used);
		out.linear = found->begin;
		out.kind = "line";
		out.file = found->file;
		out.line = used;
		out.exactLine = used == wanted;
		out.description = found->file + ":" + buf + " = " + DEBUG_Hex(found->begin);
		if (!out.exactLine) out.description += " (the next line with code)";
		return true;
	}

	/* symbol+offset, with the sign kept out of a name that contains one. */
	std::string name = text;
	int32_t adjust = 0;
	const size_t sign = text.find_last_of("+-");
	if (sign != std::string::npos && sign > 0) {
		uint32_t amount = 0;
		if (ParseNumber(TrimSpec(text.substr(sign + 1)),amount)) {
			name = TrimSpec(text.substr(0,sign));
			adjust = text[sign] == '-' ? -(int32_t)amount : (int32_t)amount;
		}
	}

	const DebugSymbol *symbol = Resolve(name);
	if (symbol != NULL) {
		out.linear = (uint32_t)((int64_t)symbol->linear + adjust);
		out.kind = "symbol";
		out.symbol = symbol->name;
		out.delta = (uint32_t)(adjust > 0 ? adjust : 0);
		out.description = symbol->name +
			(adjust != 0 ? (adjust > 0 ? "+" : "-") + DEBUG_Hex((uint32_t)(adjust > 0 ? adjust : -adjust)) : "") +
			" = " + DEBUG_Hex(out.linear);
		return true;
	}

	if (ParseNumber(text,out.linear)) {
		out.kind = "address";
		out.description = DEBUG_Hex(out.linear);
		return true;
	}

	error = "Unknown location: " + text +
		" (expected file:line, a symbol, symbol+offset, or an address)";
	return false;
}

int32_t DebugSymbolStore::InnermostScope(uint32_t pcLinear) const
{
	int32_t best = -1;
	for (size_t i = 0;i < scopes.size();i++) {
		if (pcLinear < scopes[i].begin || pcLinear >= scopes[i].end) continue;
		if (best < 0 || (scopes[i].end - scopes[i].begin) <
		    (scopes[(size_t)best].end - scopes[(size_t)best].begin))
			best = (int32_t)i;
	}
	return best;
}

bool DebugSymbolStore::ResolveLocal(uint32_t pcLinear,const std::string &name,DebugLocal &out,
                                    std::string &function) const
{
	const std::string upper = SymUpper(name);

	for (int32_t at = InnermostScope(pcLinear);at >= 0;at = scopes[(size_t)at].parent) {
		const DebugScopeEntry &scope = scopes[(size_t)at];
		for (size_t i = 0;i < scope.locals.size();i++) {
			if (scope.locals[i].name != name && SymUpper(scope.locals[i].name) != upper) continue;
			out = scope.locals[i];
			function = scope.function;
			return true;
		}
	}
	return false;
}

void DebugSymbolStore::LocalsAt(uint32_t pcLinear,std::vector<DebugLocal> &out,
                                std::vector<std::string> &functions) const
{
	for (int32_t at = InnermostScope(pcLinear);at >= 0;at = scopes[(size_t)at].parent) {
		const DebugScopeEntry &scope = scopes[(size_t)at];
		for (size_t i = 0;i < scope.locals.size();i++) {
			bool shadowed = false;
			for (size_t k = 0;k < out.size();k++)
				if (out[k].name == scope.locals[i].name) shadowed = true;
			if (shadowed) continue;

			out.push_back(scope.locals[i]);
			functions.push_back(scope.function);
		}
	}
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
