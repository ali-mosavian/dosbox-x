/*
 * debug_symfmt_pe.cpp - a PE image: its sections, the bytes the loader
 * rewrites in them (base relocations, the import address table) and the
 * names it exports.
 */

#include "debug_image.h"

#include <algorithm>

static const uint32_t PE_SIGNATURE = 0x00004550;
static const uint16_t PE32_MAGIC = 0x10b;
static const uint16_t PE32_PLUS_MAGIC = 0x20b;
static const uint32_t PE_SECTION_CODE = 0x00000020;
static const uint32_t PE_SECTION_EXECUTE = 0x20000000;
static const uint32_t PE_HEADER_BYTES = 40;
static const size_t PE_DIR_EXPORT = 0;
static const size_t PE_DIR_BASERELOC = 5;
static const size_t PE_DIR_IAT = 12;

struct PeSection {
	uint32_t rva = 0;
	uint32_t virtualSize = 0;
	uint32_t rawSize = 0;
	uint32_t rawOffset = 0;
	uint32_t characteristics = 0;
};

struct PeDirectory {
	uint32_t rva = 0;
	uint32_t size = 0;
};

static const PeSection *SectionOf(const std::vector<PeSection> &sections,uint32_t rva,size_t *index = NULL)
{
	for (size_t i = 0;i < sections.size();i++) {
		const uint32_t extent = sections[i].virtualSize ? sections[i].virtualSize : sections[i].rawSize;
		if (rva >= sections[i].rva && rva - sections[i].rva < extent) {
			if (index != NULL) *index = i;
			return &sections[i];
		}
	}
	return NULL;
}

/* The file offset of an RVA that lies in a section's raw data; false for one that does not. */
static bool FileOffset(const std::vector<PeSection> &sections,uint32_t rva,size_t &out)
{
	const PeSection *section = SectionOf(sections,rva);
	if (section == NULL || rva - section->rva >= section->rawSize) return false;
	out = (size_t)section->rawOffset + (rva - section->rva);
	return true;
}

/* Zeroes in the section's `fixed` the `bytes` at `rva`, where they fall within it. */
static void MaskBytes(const std::vector<PeSection> &sections,size_t section,uint32_t rva,uint32_t bytes,
                      std::vector<uint8_t> &fixed)
{
	for (uint32_t b = 0;b < bytes;b++) {
		const uint32_t at = rva + b - sections[section].rva;
		if (rva + b >= sections[section].rva && at < fixed.size()) fixed[at] = 0;
	}
}

static uint32_t RelocationBytes(uint16_t type)
{
	switch (type) {
	case 1: case 2: return 2;	/* HIGH, LOW */
	case 3: return 4;		/* HIGHLOW */
	case 10: return 8;		/* DIR64 */
	}
	return 0;
}

bool DEBUG_PeImage(const DebugBytes &data,DebugImage &out)
{
	if (data.u16(0) != 0x5a4d && data.u16(0) != 0x4d5a) return false;
	const size_t header = data.u32(0x3c);
	if (data.u32(header) != PE_SIGNATURE) return false;

	const uint16_t sectionCount = data.u16(header + 6);
	const size_t optional = header + 24;
	const uint16_t magic = data.u16(optional);
	if (magic != PE32_MAGIC && magic != PE32_PLUS_MAGIC) return false;
	const size_t directories = optional + (magic == PE32_MAGIC ? 96 : 112);
	const size_t table = optional + data.u16(header + 20);

	PeDirectory directory[16];
	for (size_t i = 0;i < 16;i++) {
		directory[i].rva = data.u32(directories + i * 8);
		directory[i].size = data.u32(directories + i * 8 + 4);
	}

	std::vector<PeSection> sections;
	for (uint16_t s = 0;s < sectionCount;s++) {
		const size_t at = table + (size_t)s * PE_HEADER_BYTES;
		PeSection section;
		section.virtualSize = data.u32(at + 8);
		section.rva = data.u32(at + 12);
		section.rawSize = data.u32(at + 16);
		section.rawOffset = data.u32(at + 20);
		section.characteristics = data.u32(at + 36);
		sections.push_back(section);
	}

	std::vector<std::vector<uint8_t> > fixed;
	for (size_t s = 0;s < sections.size();s++) {
		const uint32_t extent = sections[s].virtualSize ? sections[s].virtualSize : sections[s].rawSize;
		fixed.push_back(std::vector<uint8_t>(std::min(extent,sections[s].rawSize),1));
	}

	/* The loader adds the load delta at every base relocation, and fills the import address table. */
	size_t at = 0;
	if (directory[PE_DIR_BASERELOC].size != 0 && FileOffset(sections,directory[PE_DIR_BASERELOC].rva,at)) {
		const size_t end = at + directory[PE_DIR_BASERELOC].size;
		while (at + 8 <= end) {
			const uint32_t page = data.u32(at);
			const uint32_t block = data.u32(at + 4);
			if (block < 8) break;
			size_t index = 0;
			for (size_t entry = at + 8;entry + 2 <= at + block;entry += 2) {
				const uint16_t value = data.u16(entry);
				const uint32_t bytes = RelocationBytes(value >> 12);
				if (bytes != 0 && SectionOf(sections,page + (value & 0xfff),&index))
					MaskBytes(sections,index,page + (value & 0xfff),bytes,fixed[index]);
			}
			at += block;
		}
	}
	size_t index = 0;
	if (directory[PE_DIR_IAT].size != 0 && SectionOf(sections,directory[PE_DIR_IAT].rva,&index))
		MaskBytes(sections,index,directory[PE_DIR_IAT].rva,directory[PE_DIR_IAT].size,fixed[index]);

	for (size_t s = 0;s < sections.size();s++) {
		DebugImageObject object;
		object.index = (uint16_t)(s + 1);
		object.size = sections[s].virtualSize ? sections[s].virtualSize : sections[s].rawSize;
		object.code = (sections[s].characteristics & (PE_SECTION_CODE | PE_SECTION_EXECUTE)) != 0;
		if (!fixed[s].empty()) {
			const DebugBytes bytes = data.sub(sections[s].rawOffset,fixed[s].size());
			DebugImagePage page;
			page.bytes.assign(bytes.data(),bytes.data() + bytes.size());
			page.fixed.assign(fixed[s].begin(),fixed[s].begin() + page.bytes.size());
			object.pages.push_back(page);
		}
		out.objects.push_back(object);
	}

	size_t exportAt = 0;
	if (directory[PE_DIR_EXPORT].size != 0 && FileOffset(sections,directory[PE_DIR_EXPORT].rva,exportAt)) {
		const uint32_t names = data.u32(exportAt + 24);
		size_t functions = 0,nameTable = 0,ordinals = 0;
		if (FileOffset(sections,data.u32(exportAt + 28),functions) && FileOffset(sections,data.u32(exportAt + 32),nameTable) &&
		    FileOffset(sections,data.u32(exportAt + 36),ordinals)) {
			for (uint32_t n = 0;n < names;n++) {
				const uint32_t rva = data.u32(functions + (size_t)data.u16(ordinals + (size_t)n * 2) * 4);
				size_t nameAt = 0,section = 0;
				/* A forwarder's "address" is the name of another module's export. */
				if (!SectionOf(sections,rva,&section) || !FileOffset(sections,data.u32(nameTable + (size_t)n * 4),nameAt)) continue;
				if (rva >= directory[PE_DIR_EXPORT].rva && rva < directory[PE_DIR_EXPORT].rva + directory[PE_DIR_EXPORT].size) continue;

				DebugImageExport exported;
				size_t end = nameAt;
				while (end < data.size() && data.u8(end) != 0) end++;
				exported.name = data.latin1(nameAt,end);
				exported.object = (uint16_t)(section + 1);
				exported.offset = rva - sections[section].rva;
				out.exports.push_back(exported);
			}
		}
	}
	return true;
}
