/*
 * debug_symfmt_le.cpp - the names an LE (linear executable) image exports:
 * its object table, resident and non-resident name tables and entry table.
 */

#include "debug_symfmt.h"
#include "debug_image.h"

#include <algorithm>

static const uint32_t LE_MAX_OBJECTS = 4096;

/* Where the LE header starts: at 0, or at the offset a DOS stub's MZ header
 * gives in e_lfanew. */
static bool FindLeHeader(const DebugBytes &data,uint32_t &at)
{
	at = (data.u16(0) == 0x5a4d || data.u16(0) == 0x4d5a) ? data.u32(0x3c) : 0;
	return data.u16(at) == 0x454c;
}

/* A name table: length-prefixed names, each followed by a 16-bit ordinal, ending at a zero length. */
static void ReadNames(const DebugBytes &data,size_t from,bool resident,std::vector<LeName> &out)
{
	size_t at = from;
	while (at < data.size() && data.u8(at) != 0) {
		size_t next = 0;
		LeName name;
		name.name = data.pstr(at,&next);
		name.ordinal = data.u16(next);
		name.resident = resident;
		out.push_back(name);
		at = next + 2;
	}
}

static void ReadEntries(const DebugBytes &data,size_t from,std::vector<LeEntry> &out)
{
	size_t at = from;
	uint16_t ordinal = 1;
	while (at < data.size()) {
		const uint8_t count = data.u8(at++);
		if (count == 0) return;
		const uint8_t type = data.u8(at++);
		if (type == 0) {
			ordinal += count;
			continue;
		}

		static const size_t record[5] = {0,3,5,5,7};
		if (type > 4) return;
		const uint16_t object = data.u16(at);
		at += 2;
		for (uint8_t i = 0;i < count;i++,ordinal++) {
			LeEntry entry;
			entry.ordinal = ordinal;
			entry.object = object;
			if (type == 1 || type == 2) entry.offset = data.u16(at + 1);
			else if (type == 3) entry.offset = data.u32(at + 1);
			/* A forwarder names another module's export: no address here. */
			if (type != 4) out.push_back(entry);
			at += record[type];
		}
	}
}

bool DEBUG_ParseLe(const DebugBytes &data,LeImage &out)
{
	uint32_t header = 0;
	if (!FindLeHeader(data,header)) return false;

	const uint32_t objectCount = data.u32(header + 0x44);
	if (objectCount > LE_MAX_OBJECTS) return false;

	out.eipObject = data.u32(header + 0x18);
	out.eip = data.u32(header + 0x1c);
	out.pageSize = data.u32(header + 0x28);

	const uint32_t objectTable = data.u32(header + 0x40);
	for (uint32_t i = 0;i < objectCount;i++) {
		const size_t at = (size_t)header + objectTable + (size_t)i * 24u;
		LeObject object;
		object.size = data.u32(at);
		object.base = data.u32(at + 4);
		object.flags = data.u32(at + 8);
		object.firstPage = data.u32(at + 12);
		object.pageCount = data.u32(at + 16);
		out.objects.push_back(object);
	}

	ReadNames(data,(size_t)header + data.u32(header + 0x58),true,out.names);
	/* Unlike every other table, this one is an offset from the start of the file. */
	if (data.u32(header + 0x88) != 0) ReadNames(data,data.u32(header + 0x88),false,out.names);
	ReadEntries(data,(size_t)header + data.u32(header + 0x5c),out.entries);

	return !out.objects.empty();
}

/* ---- what an LE loader put in memory ---- */

static const uint32_t LE_PAGE_LEGAL = 0;

/* How many bytes a fixup's source occupies, by its source type. */
static uint32_t FixupSourceBytes(uint8_t source)
{
	switch (source & 0xf) {
	case 0: return 1;
	case 1: case 5: return 2;
	case 2: case 7: case 8: return 4;
	case 6: return 6;
	}
	return 0;
}

/* Marks, in `fixed`, the bytes of one page the loader rewrites; `page` is its
 * index in the object so a source straddling a page edge lands in both. */
static void MaskFixups(const DebugBytes &data,size_t at,size_t end,size_t pageSize,size_t page,
                       std::vector<std::vector<uint8_t> > &fixed)
{
	while (at + 4 <= end) {
		const uint8_t source = data.u8(at);
		const uint8_t flags = data.u8(at + 1);
		at += 2;

		size_t listCount = 0;
		int32_t offset = 0;
		if (source & 0x20) listCount = data.u8(at++);
		else {
			offset = (int16_t)data.u16(at);
			at += 2;
		}

		const bool wideObject = (flags & 0x40) != 0;
		switch (flags & 3) {
		case 0:
			at += wideObject ? 2 : 1;
			if ((source & 0xf) != 2) at += (flags & 0x10) ? 4 : 2;
			break;
		case 1:
			at += wideObject ? 2 : 1;
			at += (flags & 0x80) ? 1 : (flags & 0x10) ? 4 : 2;
			break;
		case 2:
			at += wideObject ? 2 : 1;
			at += (flags & 0x10) ? 4 : 2;
			break;
		case 3:
			at += wideObject ? 2 : 1;
			break;
		}
		if (flags & 4) at += (flags & 0x20) ? 4 : 2;

		const size_t sources = (source & 0x20) ? listCount : 1;
		for (size_t i = 0;i < sources;i++) {
			if (source & 0x20) {
				offset = (int16_t)data.u16(at);
				at += 2;
			}
			const int64_t first = (int64_t)page * (int64_t)pageSize + offset;
			for (uint32_t b = 0;b < FixupSourceBytes(source);b++) {
				const int64_t position = first + b;
				if (position < 0) continue;
				const size_t into = (size_t)(position / (int64_t)pageSize);
				if (into < fixed.size()) fixed[into][(size_t)(position % (int64_t)pageSize)] = 0;
			}
		}
	}
}

bool DEBUG_LeImage(const DebugBytes &data,DebugImage &out)
{
	LeImage le;
	if (!DEBUG_ParseLe(data,le)) return false;

	uint32_t header = 0;
	FindLeHeader(data,header);
	const size_t pageSize = le.pageSize;
	if (pageSize == 0) return false;

	const size_t pageMap = (size_t)header + data.u32(header + 0x48);
	const size_t dataPages = data.u32(header + 0x80);
	const size_t fixupPages = (size_t)header + data.u32(header + 0x68);
	const size_t fixupRecords = (size_t)header + data.u32(header + 0x6c);

	for (size_t o = 0;o < le.objects.size();o++) {
		const LeObject &object = le.objects[o];
		DebugImageObject image;
		image.index = (uint16_t)(o + 1);
		image.size = object.size;
		image.code = (object.flags & 0x4) != 0;

		const size_t pageCount = object.pageCount;
		std::vector<std::vector<uint8_t> > fixed(pageCount,std::vector<uint8_t>(pageSize,1));
		for (size_t p = 0;p < pageCount;p++) {
			const size_t number = object.firstPage + p;	/* 1-based */
			const size_t entry = pageMap + (number - 1) * 4;
			const uint32_t file = ((uint32_t)data.u8(entry) << 16) | ((uint32_t)data.u8(entry + 1) << 8) | data.u8(entry + 2);
			if (file == 0 || data.u8(entry + 3) != LE_PAGE_LEGAL) continue;
			MaskFixups(data,fixupRecords + data.u32(fixupPages + (number - 1) * 4),
			           fixupRecords + data.u32(fixupPages + number * 4),pageSize,p,fixed);
		}
		for (size_t p = 0;p < pageCount;p++) {
			const size_t number = object.firstPage + p;
			const size_t entry = pageMap + (number - 1) * 4;
			const uint32_t file = ((uint32_t)data.u8(entry) << 16) | ((uint32_t)data.u8(entry + 1) << 8) | data.u8(entry + 2);
			if (file == 0 || data.u8(entry + 3) != LE_PAGE_LEGAL) continue;

			DebugImagePage page;
			page.offset = (uint32_t)(p * pageSize);
			if (page.offset >= object.size) break;
			const size_t length = std::min(pageSize,(size_t)object.size - page.offset);
			const DebugBytes bytes = data.sub(dataPages + (size_t)(file - 1) * pageSize,length);
			page.bytes.assign(bytes.data(),bytes.data() + bytes.size());
			page.fixed.assign(fixed[p].begin(),fixed[p].begin() + page.bytes.size());
			image.pages.push_back(page);
		}
		out.objects.push_back(image);
	}

	/* A name's ordinal picks its entry; ordinal 0 names the module. */
	for (size_t n = 0;n < le.names.size();n++) {
		for (size_t e = 0;e < le.entries.size();e++) {
			if (le.entries[e].ordinal != le.names[n].ordinal || le.names[n].ordinal == 0) continue;
			DebugImageExport exported;
			exported.name = le.names[n].name;
			exported.object = le.entries[e].object;
			exported.offset = le.entries[e].offset;
			out.exports.push_back(exported);
		}
	}
	return true;
}
