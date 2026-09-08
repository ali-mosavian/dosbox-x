/*
 * debug_symfmt_info.cpp - one entry point for every debug format that can
 * ride along with a 16-bit DOS EXE, and the mapping from a format's own
 * addressing to load-relative offsets.
 */

#include "debug_symfmt.h"

#include <stdio.h>

#include <algorithm>
#include <set>

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
	char seg[16];
	snprintf(seg,sizeof(seg),"%u",(unsigned int)symbol.segment);
	return symbol.name + " = loadLinear " + DEBUG_Hex(info.loadLinear) +
	       " + segment " + seg + " base " + DEBUG_Hex(symbol.segmentBase) +
	       " + " + DEBUG_Hex(symbol.offset) + " (" + info.file + ")";
}

bool DEBUG_ParseDebugInfoBytes(const DebugBytes &data,const char *file,uint32_t loadLinear,DebugInfo &out)
{
	CvInfo cv;
	if (!DEBUG_ParseCodeView(data,cv)) return false;

	out.file = file != NULL ? file : "";
	out.format = DEBUG_FORMAT_CODEVIEW;
	out.version = cv.signature;
	out.loadLinear = loadLinear;
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
