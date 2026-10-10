/*
 * debug_symfmt_ne.cpp - an NE (segmented executable) image: its segments,
 * the relocations a loader applies to them, and the names it exports.
 */

#include "debug_image.h"

static const uint16_t NE_SEGMENT_RELOCATIONS = 0x0100;
static const uint16_t NE_RELOCATION_ADDITIVE = 0x04;
static const uint16_t NE_CHAIN_END = 0xffff;

static bool FindNeHeader(const DebugBytes &data,uint32_t &at)
{
	at = (data.u16(0) == 0x5a4d || data.u16(0) == 0x4d5a) ? data.u32(0x3c) : 0;
	return data.u16(at) == 0x454e;
}

/* How many bytes a relocation's source occupies, by its source type. */
static uint32_t RelocationSourceBytes(uint8_t source)
{
	switch (source & 0xf) {
	case 0: return 1;
	case 2: case 5: return 2;
	case 3: case 13: return 4;
	case 11: return 6;
	}
	return 0;
}

/* Zeroes in `fixed` the bytes the loader overwrites in one segment. A
 * relocation that is not additive names the head of a chain through the
 * segment's own bytes: each patched place holds the offset of the next. */
static void MaskRelocations(const DebugBytes &data,size_t at,size_t count,const DebugBytes &segment,
                            std::vector<uint8_t> &fixed)
{
	for (size_t i = 0;i < count;i++,at += 8) {
		const uint8_t source = data.u8(at);
		const uint8_t flags = data.u8(at + 1);
		const uint32_t bytes = RelocationSourceBytes(source);
		uint16_t offset = data.u16(at + 2);

		/* A chain cannot be longer than the segment has places for it. */
		for (size_t step = 0;step <= segment.size();step++) {
			for (uint32_t b = 0;b < bytes && (size_t)offset + b < fixed.size();b++) fixed[(size_t)offset + b] = 0;
			if (flags & NE_RELOCATION_ADDITIVE) break;
			const uint16_t next = segment.u16(offset);
			if (next == NE_CHAIN_END || next <= offset) break;
			offset = next;
		}
	}
}

/* A name table: length-prefixed names each followed by a 16-bit ordinal, ending at a zero length. */
static void ReadNames(const DebugBytes &data,size_t from,std::vector<std::pair<std::string,uint16_t> > &out)
{
	size_t at = from;
	while (at < data.size() && data.u8(at) != 0) {
		size_t next = 0;
		const std::string name = data.pstr(at,&next);
		out.push_back(std::make_pair(name,data.u16(next)));
		at = next + 2;
	}
}

struct NeEntry {
	uint16_t ordinal = 0;
	uint16_t segment = 0;
	uint16_t offset = 0;
};

static void ReadEntryTable(const DebugBytes &data,size_t from,std::vector<NeEntry> &out)
{
	size_t at = from;
	uint16_t ordinal = 1;
	while (at < data.size()) {
		const uint8_t count = data.u8(at++);
		if (count == 0) return;
		const uint8_t indicator = data.u8(at++);
		if (indicator == 0) {
			ordinal += count;
			continue;
		}
		for (uint8_t i = 0;i < count;i++,ordinal++) {
			NeEntry entry;
			entry.ordinal = ordinal;
			if (indicator == 0xff) {		/* movable: flags, INT 3Fh, segment, offset */
				entry.segment = data.u8(at + 3);
				entry.offset = data.u16(at + 4);
				at += 6;
			} else {				/* fixed: flags, offset */
				entry.segment = indicator;
				entry.offset = data.u16(at + 1);
				at += 3;
			}
			out.push_back(entry);
		}
	}
}

bool DEBUG_NeImage(const DebugBytes &data,DebugImage &out)
{
	uint32_t header = 0;
	if (!FindNeHeader(data,header)) return false;

	const uint16_t segmentCount = data.u16(header + 0x1c);
	const size_t segmentTable = (size_t)header + data.u16(header + 0x22);
	const uint16_t shift = data.u16(header + 0x32) ? data.u16(header + 0x32) : 9;

	for (uint16_t s = 0;s < segmentCount;s++) {
		const size_t entry = segmentTable + (size_t)s * 8;
		const uint32_t sector = data.u16(entry);
		const uint32_t length = data.u16(entry + 2) ? data.u16(entry + 2) : 0x10000u;
		const uint16_t flags = data.u16(entry + 4);

		DebugImageObject object;
		object.index = (uint16_t)(s + 1);
		object.size = length;
		object.code = (flags & 1) == 0;
		if (sector != 0) {
			const size_t file = (size_t)sector << shift;
			const DebugBytes bytes = data.sub(file,length);
			DebugImagePage page;
			page.bytes.assign(bytes.data(),bytes.data() + bytes.size());
			page.fixed.assign(page.bytes.size(),1);
			if (flags & NE_SEGMENT_RELOCATIONS) {
				const size_t table = file + length;
				MaskRelocations(data,table + 2,data.u16(table),bytes,page.fixed);
			}
			object.pages.push_back(page);
		}
		out.objects.push_back(object);
	}

	std::vector<std::pair<std::string,uint16_t> > names;
	ReadNames(data,(size_t)header + data.u16(header + 0x26),names);
	/* Unlike the other tables, this one is an offset from the start of the file. */
	if (data.u32(header + 0x2c) != 0) ReadNames(data,data.u32(header + 0x2c),names);

	std::vector<NeEntry> entries;
	ReadEntryTable(data,(size_t)header + data.u16(header + 0x04),entries);
	for (size_t n = 0;n < names.size();n++) {
		for (size_t e = 0;e < entries.size();e++) {
			if (entries[e].ordinal != names[n].second || names[n].second == 0) continue;
			DebugImageExport exported;
			exported.name = names[n].first;
			exported.object = entries[e].segment;
			exported.offset = entries[e].offset;
			out.exports.push_back(exported);
		}
	}
	return true;
}
