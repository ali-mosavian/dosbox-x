/*
 * debug_symfmt_info.cpp - one entry point for every debug format that can
 * ride along with a 16-bit DOS EXE, and the mapping from a format's own
 * addressing to load-relative offsets.
 */

#include "debug_symfmt.h"

#include <stdio.h>

#include <algorithm>
#include <set>
#include <string>

static std::string SegKey(uint16_t moduleIndex,uint16_t segment)
{
	char buf[32];
	snprintf(buf,sizeof(buf),"%u:%u",(unsigned int)moduleIndex,(unsigned int)segment);
	return std::string(buf);
}

static bool ByOffset(const CvLine &a,const CvLine &b) { return a.offset < b.offset; }

static void CodeViewSymbols(const CvInfo &info,uint32_t loadLinear,DebugInfo &out)
{
	const std::map<uint16_t,uint32_t> bases = DEBUG_CvSegmentBases(info);
	std::set<uint16_t> missing;

	for (size_t i = 0;i < info.symbols.size();i++) {
		const CvSymbol &symbol = info.symbols[i];
		const std::map<uint16_t,uint32_t>::const_iterator base = bases.find(symbol.segment);
		if (base == bases.end()) {
			missing.insert(symbol.segment);
			continue;
		}

		DebugSymbol out_symbol;
		out_symbol.name = symbol.name;
		out_symbol.offset = symbol.offset;
		out_symbol.segment = symbol.segment;
		out_symbol.segmentBase = base->second;
		out_symbol.linear = loadLinear + base->second + symbol.offset;
		out_symbol.size = symbol.size;
		out_symbol.hasSize = symbol.hasSize;
		out_symbol.module = symbol.module;
		out_symbol.source = DEBUG_FORMAT_CODEVIEW;
		out.symbols.push_back(out_symbol);
	}

	if (!missing.empty()) {
		std::string list;
		for (std::set<uint16_t>::const_iterator it = missing.begin();it != missing.end();++it) {
			char buf[16];
			snprintf(buf,sizeof(buf),"%u",(unsigned int)*it);
			if (!list.empty()) list += ", ";
			list += buf;
		}
		out.warnings.push_back("no sstSegMap entry for segment(s) " + list);
	}
}

static void CodeViewLines(const CvInfo &info,DebugInfo &out)
{
	const std::map<uint16_t,uint32_t> bases = DEBUG_CvSegmentBases(info);

	/* A module's own SegInfo is what bounds its last line; CV writes no end
	 * offset for it. */
	std::map<std::string,uint32_t> contributions;
	for (size_t m = 0;m < info.modules.size();m++) {
		const CvModule &module = info.modules[m];
		for (size_t s = 0;s < module.segments.size();s++) {
			const CvSegInfo &segment = module.segments[s];
			contributions[SegKey(module.index,segment.segment)] = segment.offset + segment.length;
		}
	}

	for (size_t t = 0;t < info.lines.size();t++) {
		const CvLineTable &table = info.lines[t];
		const std::map<uint16_t,uint32_t>::const_iterator base = bases.find(table.segment);
		if (base == bases.end()) continue;

		std::vector<CvLine> sorted = table.lines;
		std::stable_sort(sorted.begin(),sorted.end(),ByOffset);

		const std::map<std::string,uint32_t>::const_iterator last =
			contributions.find(SegKey(table.moduleIndex,table.segment));

		for (size_t i = 0;i < sorted.size();i++) {
			uint32_t next;
			if (i + 1 < sorted.size()) next = sorted[i+1].offset;
			else if (last != contributions.end()) next = last->second;
			else next = sorted[i].offset + 1;

			DebugLine line;
			line.module = table.module;
			line.file = table.file;
			line.line = sorted[i].line;
			line.imageOffset = base->second + sorted[i].offset;
			line.endOffset = base->second + std::max(next,sorted[i].offset + 1);
			out.lines.push_back(line);
		}
	}
}

/* A symbol names a 1-based type record; ARRAY points at its element type. */
static std::string BorlandTypeName(const TdInfo &info,uint16_t typeIndex,DebugSymbol &out)
{
	out.valueSize = 0;
	out.valueKind = DEBUG_VALUE_UNKNOWN;
	out.elementSize = 0;
	out.isFunction = false;
	if (typeIndex < 1 || typeIndex > info.types.size()) return std::string();

	const TdType &type = info.types[typeIndex-1];
	out.valueSize = type.size;

	const char *scalar = NULL;
	switch (type.id) {
	case 4:  scalar = "signed char";   out.valueKind = DEBUG_VALUE_SIGNED; break;
	case 5:  scalar = "int";           out.valueKind = DEBUG_VALUE_SIGNED; break;
	case 6:  scalar = "long";          out.valueKind = DEBUG_VALUE_SIGNED; break;
	case 8:  scalar = "unsigned char"; out.valueKind = DEBUG_VALUE_UNSIGNED; break;
	case 9:  scalar = "unsigned int";  out.valueKind = DEBUG_VALUE_UNSIGNED; break;
	case 10: scalar = "unsigned long"; out.valueKind = DEBUG_VALUE_UNSIGNED; break;
	case 12: scalar = "char *";        out.valueKind = DEBUG_VALUE_UNSIGNED; break;
	case 13: scalar = "float";         out.valueKind = DEBUG_VALUE_FLOAT; break;
	case 15: scalar = "double";        out.valueKind = DEBUG_VALUE_FLOAT; break;
	case 40: scalar = "bool";          out.valueKind = DEBUG_VALUE_UNSIGNED; break;
	case 30: scalar = "struct"; break;
	case 31: scalar = "union"; break;
	case 34: scalar = "enum"; break;
	}
	if (scalar != NULL) return !type.name.empty() ? type.name : scalar;

	if (type.id == 35) {			/* FUNCTION */
		out.isFunction = true;
		out.valueSize = 0;
		return "function";
	}

	if (type.id == 26) {			/* ARRAY */
		DebugSymbol element;
		const std::string elementName = BorlandTypeName(info,type.memberType,element);

		out.valueSize = type.size;
		out.valueKind = element.valueKind;
		out.elementSize = element.valueSize;

		char count[32];
		snprintf(count,sizeof(count),"[%u]",
		         (unsigned int)(element.valueSize > 0 ? type.size / element.valueSize : 0));
		return (elementName.empty() ? std::string("?") : elementName) + count;
	}

	return std::string();
}

/* A scope with no parent is a function body, and its length is the only place
 * TDINFO says how far a function reaches. */
static void BorlandFunctionSizes(const TdInfo &info,DebugInfo &out)
{
	for (size_t s = 0;s < info.segments.size();s++) {
		const TdSegment &segment = info.segments[s];
		for (uint16_t i = 0;i < segment.scopeCount;i++) {
			const size_t index = (size_t)segment.scopeIndex + i;
			if (index < 1 || index > info.scopes.size()) continue;

			const TdScope &scope = info.scopes[index-1];
			if (scope.parent != 0 || scope.length == 0) continue;

			for (size_t k = 0;k < out.symbols.size();k++) {
				DebugSymbol &symbol = out.symbols[k];
				if (symbol.segment != segment.codeSegment) continue;
				if (symbol.offset != scope.offset) continue;
				symbol.size = scope.length;
				symbol.hasSize = true;
			}
		}
	}
}

/* A scope's symbols are the locals and parameters of the function or block it
 * covers; only the two classes that say where a variable lives are kept. */
static void BorlandScopes(const TdInfo &info,DebugInfo &out)
{
	std::vector<int32_t> mapping(info.scopes.size(),-1);

	for (size_t s = 0;s < info.segments.size();s++) {
		const TdSegment &segment = info.segments[s];
		const uint32_t base = (uint32_t)segment.codeSegment << 4u;

		for (uint16_t i = 0;i < segment.scopeCount;i++) {
			const size_t index = (size_t)segment.scopeIndex + i;
			if (index < 1 || index > info.scopes.size()) continue;

			const TdScope &scope = info.scopes[index-1];
			DebugScope out_scope;
			out_scope.imageOffset = base + scope.offset;
			out_scope.endOffset = base + scope.offset + scope.length;
			out_scope.parent = (int32_t)scope.parent;	/* still the TDINFO index */

			for (uint16_t k = 0;k < scope.symbolCount;k++) {
				const size_t at = (size_t)scope.symbolIndex + k;
				if (at < 1 || at > info.symbols.size()) continue;

				const TdSymbol &symbol = info.symbols[at-1];
				if (symbol.symbolClass != TD_SYM_AUTO && symbol.symbolClass != TD_SYM_REGISTER) continue;

				DebugLocal local;
				local.name = symbol.name;
				if (symbol.symbolClass == TD_SYM_REGISTER) {
					local.storage = DEBUG_STORAGE_REGISTER;
					local.reg = (uint16_t)symbol.offset;
				} else {
					local.storage = DEBUG_STORAGE_FRAME;
					local.frameOffset = symbol.offset;
				}

				DebugSymbol typed;
				local.typeName = BorlandTypeName(info,symbol.type,typed);
				local.valueSize = typed.valueSize;
				local.valueKind = typed.valueKind;
				local.elementSize = typed.elementSize;
				out_scope.locals.push_back(local);
			}

			mapping[index-1] = (int32_t)out.scopes.size();
			out.scopes.push_back(out_scope);
		}
	}

	/* Parents were written as TDINFO indices; turn them into our own, and
	 * name each function scope after the symbol that starts it. */
	for (size_t i = 0;i < out.scopes.size();i++) {
		const int32_t parent = out.scopes[i].parent;
		out.scopes[i].parent = parent >= 1 && (size_t)parent <= mapping.size() ? mapping[parent-1] : -1;

		for (size_t k = 0;k < out.symbols.size();k++) {
			if (!out.symbols[k].isFunction) continue;
			const uint32_t begin = out.symbols[k].segmentBase + out.symbols[k].offset;
			if (begin == out.scopes[i].imageOffset) {
				out.scopes[i].function = out.symbols[k].name;
				break;
			}
		}
	}

	/* A block inside a function reports that function: "in _tp_sum" is what
	 * a stop inside its loop is, and only the outermost scope starts at the
	 * entry point that names it. */
	for (size_t i = 0;i < out.scopes.size();i++) {
		if (!out.scopes[i].function.empty()) continue;

		for (int32_t at = out.scopes[i].parent;at >= 0;at = out.scopes[(size_t)at].parent) {
			if (out.scopes[(size_t)at].function.empty()) continue;
			out.scopes[i].function = out.scopes[(size_t)at].function;
			break;
		}
	}
}

/* Line offsets are counted in the code segment of the module they belong to,
 * and a segment record is what says which module owns which stretch of code. */
static void BorlandLines(const TdInfo &info,DebugInfo &out)
{
	std::string file;
	if (info.sourceFiles.size() == 1) file = info.sourceFiles[0].name;
	else if (info.sourceFiles.size() > 1)
		out.warnings.push_back("TDINFO does not say which of its source files a line belongs to; "
		                       "lines are reported against their module");

	for (size_t s = 0;s < info.segments.size();s++) {
		const TdSegment &segment = info.segments[s];
		const uint32_t begin = segment.codeOffset;
		const uint32_t end = (uint32_t)segment.codeOffset + segment.codeLength;

		std::string module;
		if (segment.module >= 1 && segment.module <= info.modules.size())
			module = info.modules[segment.module-1].name;

		std::vector<uint32_t> offsets;
		std::vector<uint16_t> numbers;
		for (size_t i = 0;i < info.lines.size();i++) {
			if (info.lines[i].offset < begin || info.lines[i].offset >= end) continue;
			offsets.push_back(info.lines[i].offset);
			numbers.push_back(info.lines[i].line);
		}
		if (offsets.empty()) continue;

		/* Sorted so that a line's end is the next line's start, whatever
		 * order the table happens to be written in. */
		for (size_t i = 0;i + 1 < offsets.size();i++)
			for (size_t k = i + 1;k < offsets.size();k++)
				if (offsets[k] < offsets[i]) {
					std::swap(offsets[i],offsets[k]);
					std::swap(numbers[i],numbers[k]);
				}

		const uint32_t base = (uint32_t)segment.codeSegment << 4u;
		for (size_t i = 0;i < offsets.size();i++) {
			const uint32_t next = i + 1 < offsets.size() ? offsets[i+1] : end;

			DebugLine line;
			line.module = module;
			line.file = file.empty() ? module : file;
			line.line = numbers[i];
			line.imageOffset = base + offsets[i];
			line.endOffset = base + std::max(next,offsets[i] + 1);
			out.lines.push_back(line);
		}
	}
}

static void BorlandSymbols(const TdInfo &info,uint32_t loadLinear,DebugInfo &out)
{
	/* A TDINFO segment is already a load-relative paragraph, unlike CodeView's
	 * logical index, so there is no segment map to go through. A symbol record
	 * carries no module index either -- modules are reached the other way,
	 * from the segment table -- so these are not module-qualified. */
	for (size_t i = 0;i < info.symbols.size();i++) {
		const TdSymbol &symbol = info.symbols[i];
		if (symbol.symbolClass != TD_SYM_STATIC && symbol.symbolClass != TD_SYM_ABSOLUTE) continue;

		DebugSymbol out_symbol;
		out_symbol.name = symbol.name;
		out_symbol.offset = (uint32_t)symbol.offset;
		out_symbol.segment = symbol.segment;
		out_symbol.segmentBase = (uint32_t)symbol.segment << 4u;
		out_symbol.linear = loadLinear + out_symbol.segmentBase + out_symbol.offset;
		out_symbol.source = DEBUG_FORMAT_TDINFO;
		out_symbol.typeName = BorlandTypeName(info,symbol.type,out_symbol);
		out.symbols.push_back(out_symbol);
	}

	BorlandFunctionSizes(info,out);
}

static void WatcomSymbols(const WatInfo &info,uint32_t loadLinear,DebugInfo &out)
{
	std::map<int32_t,std::string> moduleNames;
	for (size_t i = 0;i < info.modules.size();i++)
		moduleNames[info.modules[i].index] = info.modules[i].name;

	for (size_t i = 0;i < info.symbols.size();i++) {
		const WatSymbol &symbol = info.symbols[i];

		DebugSymbol out_symbol;
		out_symbol.name = symbol.name;
		out_symbol.offset = symbol.offset;
		out_symbol.segment = symbol.segment;
		out_symbol.segmentBase = (uint32_t)symbol.segment << 4u;
		out_symbol.linear = loadLinear + out_symbol.segmentBase + symbol.offset;
		out_symbol.source = DEBUG_FORMAT_WATCOM;

		const std::map<int32_t,std::string>::const_iterator found = moduleNames.find(symbol.moduleIndex);
		if (found != moduleNames.end()) out_symbol.module = found->second;
		out.symbols.push_back(out_symbol);
	}
}

/* TDSTRIP moves the block into a .TDS beside the stripped EXE. */
static bool ParseBorlandSidecar(const char *file,std::vector<uint8_t> &data,TdInfo &out)
{
	if (file == NULL) return false;

	const std::string path = file;
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return false;
	if (path.find_first_of("/\\",dot) != std::string::npos) return false;

	const char * const extensions[2] = {".tds",".TDS"};
	for (int i = 0;i < 2;i++) {
		const std::string candidate = path.substr(0,dot) + extensions[i];
		if (candidate == path || !DEBUG_ReadHostFile(candidate.c_str(),data)) continue;
		if (DEBUG_ParseBorland(DebugBytes(data.data(),data.size()),out)) return true;
	}
	return false;
}

std::map<uint16_t,uint32_t> DEBUG_CvSegmentBases(const CvInfo &info)
{
	std::map<uint16_t,uint32_t> bases;
	for (size_t i = 0;i < info.segments.size();i++) {
		const CvSegMapEntry &segment = info.segments[i];
		bases[segment.index] = ((uint32_t)segment.frame << 4u) + segment.offset;
	}
	return bases;
}

const DebugLine *DEBUG_SourceLineAt(const DebugInfo &info,uint32_t imageOffset)
{
	const DebugLine *best = NULL;
	for (size_t i = 0;i < info.lines.size();i++) {
		const DebugLine &entry = info.lines[i];
		if (imageOffset < entry.imageOffset || imageOffset >= entry.endOffset) continue;
		if (best == NULL || entry.imageOffset > best->imageOffset) best = &entry;
	}
	return best;
}

std::string DEBUG_Hex(uint32_t value)
{
	char buf[16];
	snprintf(buf,sizeof(buf),"0x%08X",(unsigned int)value);
	return std::string(buf);
}

std::string DEBUG_SymbolExplanation(const DebugInfo &info,const DebugSymbol &symbol)
{
	/* A CodeView segment index says nothing on its own, so its base is spelled
	 * out; a TDINFO or Watcom segment is already the paragraph it loads at. */
	if (symbol.source != DEBUG_FORMAT_CODEVIEW) {
		char seg[24];
		snprintf(seg,sizeof(seg),"0x%04X",(unsigned int)symbol.segment);
		return symbol.name + " = loadLinear " + DEBUG_Hex(info.loadLinear) +
		       " + " + seg + ":" + DEBUG_Hex(symbol.offset) + " (" + info.file + ")";
	}

	char seg[16];
	snprintf(seg,sizeof(seg),"%u",(unsigned int)symbol.segment);
	return symbol.name + " = loadLinear " + DEBUG_Hex(info.loadLinear) +
	       " + segment " + seg + " base " + DEBUG_Hex(symbol.segmentBase) +
	       " + " + DEBUG_Hex(symbol.offset) + " (" + info.file + ")";
}

bool DEBUG_ParseDebugInfoBytes(const DebugBytes &data,const char *file,uint32_t loadLinear,DebugInfo &out)
{
	out.file = file != NULL ? file : "";
	out.loadLinear = loadLinear;

	TdInfo td;
	std::vector<uint8_t> sidecar;
	if (DEBUG_ParseBorland(data,td) || ParseBorlandSidecar(file,sidecar,td)) {
		out.format = DEBUG_FORMAT_TDINFO;
		out.version = td.version;
		out.warnings = td.warnings;
		for (size_t i = 0;i < td.modules.size();i++) {
			DebugModule module;
			module.name = td.modules[i].name;
			module.index = td.modules[i].index;
			out.modules.push_back(module);
		}
		BorlandSymbols(td,loadLinear,out);
		BorlandLines(td,out);
		BorlandScopes(td,out);
		return true;
	}

	WatInfo wat;
	if (DEBUG_ParseWatcom(data,wat)) {
		out.format = DEBUG_FORMAT_WATCOM;
		out.version = wat.version;
		out.warnings = wat.warnings;
		for (size_t i = 0;i < wat.modules.size();i++) {
			DebugModule module;
			module.name = wat.modules[i].name;
			module.index = (uint16_t)wat.modules[i].index;
			out.modules.push_back(module);
		}
		WatcomSymbols(wat,loadLinear,out);
		return true;
	}

	CvInfo cv;
	if (!DEBUG_ParseCodeView(data,cv)) return false;

	out.format = DEBUG_FORMAT_CODEVIEW;
	out.version = cv.signature;
	out.warnings = cv.warnings;

	for (size_t i = 0;i < cv.modules.size();i++) {
		DebugModule module;
		module.name = cv.modules[i].name;
		module.index = cv.modules[i].index;
		out.modules.push_back(module);
	}

	CodeViewSymbols(cv,loadLinear,out);
	CodeViewLines(cv,out);
	return true;
}

bool DEBUG_ParseDebugInfo(const char *file,uint32_t loadLinear,DebugInfo &out)
{
	std::vector<uint8_t> data;
	if (!DEBUG_ReadHostFile(file,data)) return false;
	return DEBUG_ParseDebugInfoBytes(DebugBytes(data.data(),data.size()),file,loadLinear,out);
}
