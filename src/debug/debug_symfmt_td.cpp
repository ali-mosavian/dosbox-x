/*
 * debug_symfmt_td.cpp - Borland TDINFO, as TLink leaves it in a 16-bit MZ EXE
 * or TDSTRIP moves it into a .TDS beside one.
 */

#include "debug_symfmt.h"

#include <stdio.h>

static const uint16_t TDINFO_MAGIC = 0x52fb;

struct TdHeader {
	uint32_t namesPoolSize;
	uint16_t namesCount;
	uint16_t typesCount;
	uint16_t membersCount;
	uint16_t symbolsCount;
	uint16_t globalsCount;
	uint16_t modulesCount;
	uint16_t localsCount;
	uint16_t scopesCount;
	uint16_t lineNumbersCount;
	uint16_t sourceFilesCount;
	uint16_t segmentsCount;
	uint16_t correlationsCount;
	uint16_t extensionSize;
	uint8_t minor;
	uint8_t major;
};

static bool ReadTdHeader(const DebugBytes &data,uint64_t at,TdHeader &out)
{
	if (at + 48 > data.size() || data.u16((size_t)at) != TDINFO_MAGIC) return false;

	out.minor = data.u8((size_t)at + 2);
	out.major = data.u8((size_t)at + 3);
	out.namesPoolSize = data.u32((size_t)at + 4);
	out.namesCount = data.u16((size_t)at + 8);
	out.typesCount = data.u16((size_t)at + 10);
	out.membersCount = data.u16((size_t)at + 12);
	out.symbolsCount = data.u16((size_t)at + 14);
	out.globalsCount = data.u16((size_t)at + 16);
	out.modulesCount = data.u16((size_t)at + 18);
	out.localsCount = data.u16((size_t)at + 20);
	out.scopesCount = data.u16((size_t)at + 22);
	out.lineNumbersCount = data.u16((size_t)at + 24);
	out.sourceFilesCount = data.u16((size_t)at + 26);
	out.segmentsCount = data.u16((size_t)at + 28);
	out.correlationsCount = data.u16((size_t)at + 30);
	out.extensionSize = data.u16((size_t)at + 46);
	return true;
}

/* Name-pool entries are NUL-terminated and indexed from 1. */
static std::vector<std::string> ReadNamePool(const DebugBytes &data,const TdHeader &header)
{
	std::vector<std::string> names;
	if (header.namesPoolSize > data.size()) return names;

	size_t at = data.size() - header.namesPoolSize;
	while (at < data.size() && names.size() < header.namesCount) {
		size_t end = at;
		while (end < data.size() && data.u8(end) != 0) end++;
		names.push_back(data.latin1(at,end));
		at = end + 1;
	}
	return names;
}

bool DEBUG_FindTdInfoBase(const DebugBytes &data,uint64_t &base)
{
	if (data.size() >= 48 && data.u16(0) == TDINFO_MAGIC) {
		base = 0;
		return true;
	}

	MzImage image;
	if (!DEBUG_ParseMzImage(data,image)) return false;

	const uint64_t at = image.appendedOffset;
	if (at + 48 > data.size() || data.u16((size_t)at) != TDINFO_MAGIC) return false;
	base = at;
	return true;
}

bool DEBUG_ParseBorland(const DebugBytes &data,TdInfo &out)
{
	uint64_t base = 0;
	if (!DEBUG_FindTdInfoBase(data,base)) return false;

	TdHeader header;
	if (!ReadTdHeader(data,base,header)) return false;

	const std::vector<std::string> names = ReadNamePool(data,header);
	if (names.size() != header.namesCount) {
		char buf[128];
		snprintf(buf,sizeof(buf),"name pool holds %u names, header claims %u",
		         (unsigned int)names.size(),(unsigned int)header.namesCount);
		out.warnings.push_back(buf);
	}

	char version[32];
	snprintf(version,sizeof(version),"TDINFO %u.%u",(unsigned int)header.major,(unsigned int)header.minor);
	out.version = version;
	out.base = base;

	/* The tables are a flat run in a fixed order with no directory: every one
	 * must be stepped over at its own record size to reach the next. */
	const uint64_t tables = base + 48 + header.extensionSize;

	for (uint16_t i = 0;i < header.symbolsCount;i++) {
		const uint64_t at = tables + (uint64_t)i * 9;
		if (at + 9 > data.size()) break;

		const uint16_t nameIndex = data.u16((size_t)at);
		if (nameIndex < 1 || nameIndex > names.size() || names[nameIndex-1].empty()) continue;

		TdSymbol symbol;
		symbol.name = names[nameIndex-1];
		symbol.type = data.u16((size_t)at + 2);
		symbol.symbolClass = (TdSymbolClass)(data.u8((size_t)at + 8) & 0x07);
		symbol.offset = symbol.symbolClass == TD_SYM_AUTO
			? (int32_t)(int16_t)data.u16((size_t)at + 4)
			: (int32_t)data.u16((size_t)at + 4);
		symbol.segment = data.u16((size_t)at + 6);
		out.symbols.push_back(symbol);
	}

	uint64_t at = tables + (uint64_t)header.symbolsCount * 9;

	for (uint16_t i = 0;i < header.modulesCount;i++) {
		const uint64_t off = at + (uint64_t)i * 16;
		if (off + 16 > data.size()) break;

		TdModule module;
		module.index = (uint16_t)(i + 1);
		const uint16_t nameIndex = data.u16((size_t)off);
		if (nameIndex >= 1 && nameIndex <= names.size()) module.name = names[nameIndex-1];
		out.modules.push_back(module);
	}
	at += (uint64_t)header.modulesCount * 16;

	/* A source file record is a name and four bytes this reader does not use;
	 * only the name is needed to say which file a line belongs to. */
	for (uint16_t i = 0;i < header.sourceFilesCount;i++) {
		const uint64_t off = at + (uint64_t)i * 6;
		if (off + 6 > data.size()) break;

		TdSourceFile file;
		const uint16_t nameIndex = data.u16((size_t)off);
		if (nameIndex >= 1 && nameIndex <= names.size()) file.name = names[nameIndex-1];
		out.sourceFiles.push_back(file);
	}
	at += (uint64_t)header.sourceFilesCount * 6;

	for (uint16_t i = 0;i < header.lineNumbersCount;i++) {
		const uint64_t off = at + (uint64_t)i * 4;
		if (off + 4 > data.size()) break;

		TdLine line;
		line.line = data.u16((size_t)off);
		line.offset = data.u16((size_t)off + 2);
		out.lines.push_back(line);
	}
	at += (uint64_t)header.lineNumbersCount * 4;

	for (uint16_t i = 0;i < header.scopesCount;i++) {
		const uint64_t off = at + (uint64_t)i * 12;
		if (off + 12 > data.size()) break;

		TdScope scope;
		scope.symbolIndex = data.u16((size_t)off);
		scope.symbolCount = data.u16((size_t)off + 2);
		scope.parent = data.u16((size_t)off + 4);
		scope.function = data.u16((size_t)off + 6);
		scope.offset = data.u16((size_t)off + 8);
		scope.length = data.u16((size_t)off + 10);
		out.scopes.push_back(scope);
	}
	at += (uint64_t)header.scopesCount * 12;

	for (uint16_t i = 0;i < header.segmentsCount;i++) {
		const uint64_t off = at + (uint64_t)i * 16;
		if (off + 16 > data.size()) break;

		TdSegment segment;
		segment.module = data.u16((size_t)off);
		segment.codeSegment = data.u16((size_t)off + 2);
		segment.codeOffset = data.u16((size_t)off + 4);
		segment.codeLength = data.u16((size_t)off + 6);
		segment.scopeIndex = data.u16((size_t)off + 8);
		segment.scopeCount = data.u16((size_t)off + 10);
		out.segments.push_back(segment);
	}
	at += (uint64_t)header.segmentsCount * 16;

	at += (uint64_t)header.correlationsCount * 8;

	for (uint16_t i = 0;i < header.typesCount;i++) {
		const uint64_t off = at + (uint64_t)i * 8;
		if (off + 8 > data.size()) break;

		TdType type;
		type.id = data.u8((size_t)off);
		const uint16_t nameIndex = data.u16((size_t)off + 1);
		if (nameIndex >= 1 && nameIndex <= names.size()) type.name = names[nameIndex-1];
		type.size = data.u16((size_t)off + 3);
		type.classType = data.u8((size_t)off + 5);
		type.memberType = data.u16((size_t)off + 6);
		out.types.push_back(type);
	}
	return true;
}
