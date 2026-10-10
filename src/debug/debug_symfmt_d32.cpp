/*
 * debug_symfmt_d32.cpp - a D32X module: the relocatable image a program loads
 * at run time, with its exports and its debug symbols.
 */

#include "debug_image.h"

static const uint32_t D32X_MAGIC = 0x58323344;
static const uint32_t D32S_MAGIC = 0x53323344;
static const uint32_t D32T_MAGIC = 0x54323344;
static const size_t D32_HEADER_BYTES = 72;
static const uint8_t D32_SECTION_CODE = 0;
static const uint8_t D32_SECTION_DATA = 1;
static const uint8_t D32_SECTION_BSS = 2;
static const size_t D32_RELOCATION_BYTES = 4;

static std::string CString(const DebugBytes &data,size_t from)
{
	size_t end = from;
	while (end < data.size() && data.u8(end) != 0) end++;
	return data.latin1(from,end);
}

static void AddObject(const DebugBytes &data,uint16_t index,size_t offset,uint32_t size,bool code,
                      const std::vector<std::pair<uint8_t,uint32_t> > &patches,uint8_t section,DebugImage &out)
{
	DebugImageObject object;
	object.index = index;
	object.size = size;
	object.code = code;
	/* BSS is not in the file: nothing of it to find in memory. */
	if (size != 0 && section != D32_SECTION_BSS) {
		const DebugBytes bytes = data.sub(offset,size);
		DebugImagePage page;
		page.bytes.assign(bytes.data(),bytes.data() + bytes.size());
		page.fixed.assign(page.bytes.size(),1);
		for (size_t p = 0;p < patches.size();p++) {
			if (patches[p].first != section) continue;
			for (size_t b = 0;b < D32_RELOCATION_BYTES && patches[p].second + b < page.fixed.size();b++)
				page.fixed[patches[p].second + b] = 0;
		}
		object.pages.push_back(page);
	}
	out.objects.push_back(object);
}

static void AddExport(DebugImage &out,const std::string &name,uint8_t section,uint32_t offset)
{
	for (size_t i = 0;i < out.exports.size();i++)
		if (out.exports[i].name == name) return;
	DebugImageExport exported;
	exported.name = name;
	exported.object = (uint16_t)(section + 1);
	exported.offset = offset;
	out.exports.push_back(exported);
}

bool DEBUG_D32Image(const DebugBytes &data,DebugImage &out)
{
	if (data.size() < D32_HEADER_BYTES || data.u32(0) != D32X_MAGIC) return false;

	const uint32_t strtab = data.u32(20);
	const uint16_t exportCount = data.u16(32);
	const uint32_t relocCount = data.u32(36);
	const uint32_t exportsAt = data.u32(40);
	const uint32_t relocsAt = data.u32(48);
	const uint32_t codeAt = data.u32(52);
	const uint32_t codeSize = data.u32(56);
	const uint32_t dataAt = data.u32(60);
	const uint32_t dataSize = data.u32(64);
	const uint32_t bssSize = data.u32(68);

	/* The loader rewrites the places a relocation names. */
	std::vector<std::pair<uint8_t,uint32_t> > patches;
	for (uint32_t r = 0;r < relocCount;r++) {
		const size_t at = (size_t)relocsAt + (size_t)r * 12;
		patches.push_back(std::make_pair(data.u8(at + 9),data.u32(at)));
	}

	AddObject(data,1,codeAt,codeSize,true,patches,D32_SECTION_CODE,out);
	AddObject(data,2,dataAt,dataSize,false,patches,D32_SECTION_DATA,out);
	AddObject(data,3,0,bssSize,false,patches,D32_SECTION_BSS,out);

	for (uint16_t e = 0;e < exportCount;e++) {
		const size_t at = (size_t)exportsAt + (size_t)e * 12;
		AddExport(out,CString(data,(size_t)strtab + data.u32(at)),data.u8(at + 8),data.u32(at + 4));
	}

	/* The debug block the linker appends names the module's functions and data as well. */
	if (data.size() >= 12 && data.u32(data.size() - 12) == D32T_MAGIC) {
		const size_t debugAt = data.u32(data.size() - 8);
		const size_t debugSize = data.u32(data.size() - 4);
		if (debugAt + debugSize <= data.size() && debugSize >= 20 && data.u32(debugAt) == D32S_MAGIC) {
			const uint16_t count = data.u16(debugAt + 6);
			const size_t symbols = debugAt + data.u32(debugAt + 8);
			const size_t strings = debugAt + data.u32(debugAt + 12);
			for (uint16_t s = 0;s < count;s++) {
				const size_t at = symbols + (size_t)s * 16;
				AddExport(out,CString(data,strings + data.u32(at)),data.u8(at + 12),data.u32(at + 4));
			}
		}
	}
	return true;
}
