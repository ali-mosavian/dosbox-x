/*
 * debug_symfmt_wat.cpp - Watcom debug info, as wlink appends it to a DOS EXE.
 */

#include "debug_symfmt.h"

#include <stdio.h>

#include <map>

static const uint16_t WAT_DBG_SIGNATURE = 0x8386;

struct WatMaster {
	uint8_t exeMajor;
	uint8_t exeMinor;
	uint8_t objMajor;
	uint8_t objMinor;
	uint16_t langSize;
	uint16_t segmentSize;
	uint32_t debugSize;
	uint64_t at;			/* file offset of this header */
};

static bool ReadWatMaster(const DebugBytes &data,int64_t at,uint16_t &signature,WatMaster &out)
{
	if (at < 0 || (uint64_t)at + 14 > data.size()) return false;

	signature = data.u16((size_t)at);
	out.exeMajor = data.u8((size_t)at + 2);
	out.exeMinor = data.u8((size_t)at + 3);
	out.objMajor = data.u8((size_t)at + 4);
	out.objMinor = data.u8((size_t)at + 5);
	out.langSize = data.u16((size_t)at + 6);
	out.segmentSize = data.u16((size_t)at + 8);
	out.debugSize = data.u32((size_t)at + 10);
	out.at = (uint64_t)at;
	return true;
}

/* Other Watcom trailers that can sit after the debug block; skipped by size. */
static bool IsStackedSignature(uint16_t signature)
{
	return signature == 0x8300 || signature == 0x8301 || signature == 0x8302;
}

static bool FindWatMaster(const DebugBytes &data,WatMaster &out)
{
	int64_t end = (int64_t)data.size() - 14;
	for (int guard = 0;guard < 16;guard++) {
		uint16_t signature = 0;
		if (!ReadWatMaster(data,end,signature,out)) return false;
		if (signature == WAT_DBG_SIGNATURE) return true;
		if (!IsStackedSignature(signature)) return false;
		if (out.debugSize == 0 || (int64_t)out.debugSize > end) return false;
		end -= (int64_t)out.debugSize;
	}
	return false;
}

/*
 * A section's modules, plus where each one started.
 *
 * A V2 symbol names its module by BYTE OFFSET from the section's module area,
 * not by index, so the offsets have to be kept to resolve one.
 */
static void ReadWatModules(const DebugBytes &data,uint64_t from,uint64_t to,int32_t firstIndex,
                           std::vector<WatModule> &modules,std::map<uint32_t,int32_t> &byOffset)
{
	uint64_t at = from;
	while (at + 21 <= to) {
		const uint8_t nameLength = data.u8((size_t)at + 20);

		WatModule module;
		module.index = firstIndex + (int32_t)modules.size();
		module.name = data.latin1((size_t)at + 21,(size_t)at + 21 + nameLength);
		byOffset[(uint32_t)(at - from)] = module.index;
		modules.push_back(module);

		at += 21 + nameLength;
	}
}

/* V2 has no kind byte; the name length sits where V3 puts the kind. */
static void ReadWatGlobals(const DebugBytes &data,uint64_t from,uint64_t to,bool v2,int32_t firstIndex,
                           const std::map<uint32_t,int32_t> &byOffset,std::vector<WatSymbol> &symbols)
{
	uint64_t at = from;
	while (at + (v2 ? 9u : 10u) <= to) {
		const uint8_t nameLength = v2 ? data.u8((size_t)at + 8) : data.u8((size_t)at + 9);
		const uint64_t nameAt = at + (v2 ? 9u : 10u);
		if (nameAt + nameLength > to) break;

		const uint16_t mod = data.u16((size_t)at + 6);

		WatSymbol symbol;
		symbol.name = data.latin1((size_t)nameAt,(size_t)nameAt + nameLength);
		symbol.offset = data.u32((size_t)at);
		symbol.segment = data.u16((size_t)at + 4);
		symbol.kind = v2 ? (uint8_t)0 : data.u8((size_t)at + 8);
		if (v2) {
			const std::map<uint32_t,int32_t>::const_iterator found = byOffset.find(mod);
			symbol.moduleIndex = found != byOffset.end() ? found->second : -1;
		} else {
			/* V3's mod is a per-section index, so it needs the section's base. */
			symbol.moduleIndex = firstIndex + (int32_t)mod;
		}
		symbols.push_back(symbol);

		at = nameAt + nameLength;
	}
}

bool DEBUG_ParseWatcom(const DebugBytes &data,WatInfo &out)
{
	WatMaster master;
	if (!FindWatMaster(data,master)) return false;

	char version[32];
	snprintf(version,sizeof(version),"WAT %u.%u",(unsigned int)master.exeMajor,(unsigned int)master.exeMinor);
	out.version = version;
	out.base = master.at;

	if (master.exeMajor != 2 && master.exeMajor != 3) {
		char buf[64];
		snprintf(buf,sizeof(buf),"unsupported Watcom exe_major_ver %u",(unsigned int)master.exeMajor);
		out.warnings.push_back(buf);
		return true;
	}

	const bool v2 = master.exeMajor == 2;
	if (master.debugSize > master.at + 14) {
		char buf[80];
		snprintf(buf,sizeof(buf),"debug_size %u runs past the start of the file",(unsigned int)master.debugSize);
		out.warnings.push_back(buf);
		return true;
	}

	const uint64_t base = master.at + 14 - master.debugSize;
	out.base = base;

	/* Section count is not stored: walk section_size forward until the master. */
	uint64_t at = base + master.langSize + master.segmentSize;
	while (at + 18 <= master.at) {
		const uint32_t modOffset = data.u32((size_t)at);
		const uint32_t gblOffset = data.u32((size_t)at + 4);
		const uint32_t addrOffset = data.u32((size_t)at + 8);
		const uint32_t sectionSize = data.u32((size_t)at + 12);

		/* Sections end where the master header begins. */
		if (sectionSize < 18 || at + sectionSize > master.at) break;
		if (!(modOffset <= gblOffset && gblOffset <= addrOffset && addrOffset < sectionSize)) break;

		/* mod_offset == gbl_offset marks an empty overlay placeholder. */
		if (modOffset != gblOffset) {
			std::vector<WatModule> modules;
			std::map<uint32_t,int32_t> byOffset;
			const int32_t firstIndex = (int32_t)out.modules.size();

			ReadWatModules(data,at + modOffset,at + gblOffset,firstIndex,modules,byOffset);
			ReadWatGlobals(data,at + gblOffset,at + addrOffset,v2,firstIndex,byOffset,out.symbols);
			out.modules.insert(out.modules.end(),modules.begin(),modules.end());
		}
		at += sectionSize;
	}
	return true;
}
