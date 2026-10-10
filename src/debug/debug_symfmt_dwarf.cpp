/*
 * debug_symfmt_dwarf.cpp - DWARF 2 to 5 in the ELF block a linker appends to
 * an executable: the line program, and the DIEs that name code and data.
 *
 * jwlink writes version 2 (lines, labels and variables of the publics); llrm
 * writes 4 or 5 with locations. Offsets are 32-bit and addresses 4 bytes: a
 * 64-bit unit is refused by name, never misread.
 */

#include "debug_symfmt.h"

#include <string.h>

#include <algorithm>

namespace {

/* ---- the ELF container ---- */

struct Sections {
	DebugBytes info,abbrev,line,str,lineStr;
};

DebugBytes ElfSection(const DebugBytes &elf,const char *wanted)
{
	const uint32_t table = elf.u32(0x20);
	const uint16_t entry = elf.u16(0x2e);
	const uint16_t count = elf.u16(0x30);
	const uint16_t names = elf.u16(0x32);
	if (entry < 40 || names >= count) return DebugBytes();

	const size_t nameTable = elf.u32(table + (size_t)names * entry + 16);
	for (uint16_t s = 0;s < count;s++) {
		const size_t at = (size_t)table + (size_t)s * entry;
		const size_t name = nameTable + elf.u32(at);
		size_t end = name;
		while (end < elf.size() && elf.u8(end) != 0) end++;
		if (elf.latin1(name,end) == wanted) return elf.sub(elf.u32(at + 16),elf.u32(at + 20));
	}
	return DebugBytes();
}

/* The ELF image itself, or the one a linker appended: its size is the file's last word. */
bool FindElf(const DebugBytes &data,DebugBytes &elf)
{
	static const uint8_t magic[4] = {0x7f,'E','L','F'};
	if (data.size() >= 4 && memcmp(data.data(),magic,4) == 0) {
		elf = data;
		return true;
	}
	if (data.size() < 16) return false;
	const size_t size = data.u32(data.size() - 4);
	if (size < 52 || size > data.size()) return false;
	elf = data.sub(data.size() - size,size);
	return elf.size() >= 4 && memcmp(elf.data(),magic,4) == 0 && elf.u8(4) == 1;	/* ELFCLASS32 */
}

/* ---- reading ---- */

struct Reader {
	DebugBytes data;
	size_t at;
	Reader(const DebugBytes &bytes,size_t from) : data(bytes),at(from) {}

	uint8_t u8() { return data.u8(at++); }
	uint16_t u16() { const uint16_t v = data.u16(at); at += 2; return v; }
	uint32_t u32() { const uint32_t v = data.u32(at); at += 4; return v; }
	uint64_t uleb()
	{
		uint64_t value = 0;
		for (unsigned shift = 0;at < data.size() + 1 && shift < 64;shift += 7) {
			const uint8_t b = u8();
			value |= (uint64_t)(b & 0x7f) << shift;
			if (!(b & 0x80)) break;
		}
		return value;
	}
	int64_t sleb()
	{
		int64_t value = 0;
		unsigned shift = 0;
		uint8_t b = 0;
		while (shift < 64) {
			b = u8();
			value |= (int64_t)(b & 0x7f) << shift;
			shift += 7;
			if (!(b & 0x80)) break;
		}
		if (shift < 64 && (b & 0x40)) value |= -((int64_t)1 << shift);
		return value;
	}
	std::string cstr()
	{
		size_t end = at;
		while (end < data.size() && data.u8(end) != 0) end++;
		const std::string out = data.latin1(at,end);
		at = end + 1;
		return out;
	}
	bool done(size_t end) const { return at >= end || at > data.size(); }
};

std::string StringAt(const DebugBytes &table,uint32_t offset)
{
	size_t end = offset;
	while (end < table.size() && table.u8(end) != 0) end++;
	return table.latin1(offset,end);
}

/* ---- attribute forms ---- */

enum {
	DW_FORM_addr = 0x01,DW_FORM_block2 = 0x03,DW_FORM_block4 = 0x04,DW_FORM_data2 = 0x05,DW_FORM_data4 = 0x06,
	DW_FORM_data8 = 0x07,DW_FORM_string = 0x08,DW_FORM_block = 0x09,DW_FORM_block1 = 0x0a,DW_FORM_data1 = 0x0b,
	DW_FORM_flag = 0x0c,DW_FORM_sdata = 0x0d,DW_FORM_strp = 0x0e,DW_FORM_udata = 0x0f,DW_FORM_ref_addr = 0x10,
	DW_FORM_ref1 = 0x11,DW_FORM_ref2 = 0x12,DW_FORM_ref4 = 0x13,DW_FORM_ref8 = 0x14,DW_FORM_ref_udata = 0x15,
	DW_FORM_indirect = 0x16,DW_FORM_sec_offset = 0x17,DW_FORM_exprloc = 0x18,DW_FORM_flag_present = 0x19,
	DW_FORM_data16 = 0x1e,DW_FORM_line_strp = 0x1f
};

struct Value {
	uint64_t number = 0;
	std::string text;
	DebugBytes block;
	bool known = true;
};

/* False for a form this reader cannot step over: the unit's remaining DIEs cannot be found. */
bool ReadForm(Reader &r,uint64_t form,uint8_t addressSize,uint16_t version,const Sections &sections,Value &out)
{
	switch (form) {
	case DW_FORM_addr: out.number = addressSize == 4 ? r.u32() : (addressSize == 2 ? r.u16() : r.u8()); return true;
	case DW_FORM_data1: case DW_FORM_flag: case DW_FORM_ref1: out.number = r.u8(); return true;
	case DW_FORM_data2: case DW_FORM_ref2: out.number = r.u16(); return true;
	case DW_FORM_data4: case DW_FORM_ref4: case DW_FORM_sec_offset: out.number = r.u32(); return true;
	case DW_FORM_data8: case DW_FORM_ref8: out.number = r.u32(); out.number |= (uint64_t)r.u32() << 32; return true;
	case DW_FORM_udata: case DW_FORM_ref_udata: out.number = r.uleb(); return true;
	case DW_FORM_sdata: out.number = (uint64_t)r.sleb(); return true;
	case DW_FORM_string: out.text = r.cstr(); return true;
	case DW_FORM_strp: out.text = StringAt(sections.str,r.u32()); return true;
	case DW_FORM_line_strp: out.text = StringAt(sections.lineStr,r.u32()); return true;
	case DW_FORM_ref_addr: if (version <= 2) out.number = r.u32(); else out.number = r.u32(); return true;
	case DW_FORM_flag_present: out.number = 1; return true;
	case DW_FORM_data16: r.at += 16; return true;
	case DW_FORM_block1: case DW_FORM_block: case DW_FORM_block2: case DW_FORM_block4: case DW_FORM_exprloc: {
		const size_t length = form == DW_FORM_block1 ? r.u8() : form == DW_FORM_block2 ? r.u16()
		                    : form == DW_FORM_block4 ? r.u32() : (size_t)r.uleb();
		out.block = r.data.sub(r.at,length);
		r.at += length;
		return true;
	}
	case DW_FORM_indirect: return ReadForm(r,r.uleb(),addressSize,version,sections,out);
	}
	out.known = false;
	return false;
}

/* ---- the line program ---- */

struct FileEntry {
	std::string name;
	uint64_t directory = 0;
};

bool ReadLineUnit(const DebugBytes &lines,size_t &at,const Sections &sections,DwarfInfo &out)
{
	Reader r(lines,at);
	const uint32_t length = r.u32();
	if (length == 0xffffffffu || length < 4 || (size_t)at + 4 + length > lines.size()) return false;
	const size_t end = at + 4 + (size_t)length;
	at = end;

	const uint16_t version = r.u16();
	if (version < 2 || version > 5) {
		out.warnings.push_back("line program version not read");
		return true;
	}
	uint8_t addressSize = 4;
	if (version >= 5) {
		addressSize = r.u8();
		r.u8();					/* segment selector size */
	}
	const uint32_t headerLength = r.u32();
	const size_t program = r.at + headerLength;
	const uint8_t minimum = r.u8();
	if (version >= 4) r.u8();			/* maximum operations per instruction: 1 outside VLIW */
	const bool isStmt = r.u8() != 0;
	const int8_t lineBase = (int8_t)r.u8();
	const uint8_t lineRange = r.u8();
	const uint8_t opcodeBase = r.u8();
	std::vector<uint8_t> operands(opcodeBase ? opcodeBase : 1,0);
	for (uint8_t i = 1;i < opcodeBase;i++) operands[i] = r.u8();
	if (lineRange == 0) return true;

	std::vector<FileEntry> files;
	if (version < 5) {
		files.push_back(FileEntry());			/* index 0 is the unit itself */
		while (!r.done(program) && r.data.u8(r.at) != 0) r.cstr();	/* include directories */
		r.at++;
		while (!r.done(program) && r.data.u8(r.at) != 0) {
			FileEntry file;
			file.name = r.cstr();
			file.directory = r.uleb();
			r.uleb(); r.uleb();
			files.push_back(file);
		}
	} else {
		for (int table = 0;table < 2;table++) {
			const uint8_t formatCount = r.u8();
			std::vector<std::pair<uint64_t,uint64_t> > format;
			for (uint8_t f = 0;f < formatCount;f++) {
				const uint64_t type = r.uleb();
				format.push_back(std::make_pair(type,r.uleb()));
			}
			const uint64_t count = r.uleb();
			for (uint64_t e = 0;e < count && !r.done(program);e++) {
				FileEntry file;
				for (size_t f = 0;f < format.size();f++) {
					Value value;
					if (!ReadForm(r,format[f].second,addressSize,version,sections,value)) return true;
					if (format[f].first == 1) file.name = value.text;	/* DW_LNCT_path */
					else if (format[f].first == 2) file.directory = value.number;
				}
				if (table == 1) files.push_back(file);
			}
		}
	}
	r.at = program;

	/* The file a row names: version 5 counts from 0, the others from 1 with 0 the unit. */
	const size_t firstFile = version >= 5 ? 0 : 1;
	std::string unitName = files.size() > firstFile ? files[firstFile].name : std::string();

	uint32_t address = 0;
	uint64_t file = 1,line = 1;
	bool statement = isStmt;
	const size_t firstRow = out.lines.size();
	while (!r.done(end)) {
		const uint8_t op = r.u8();
		bool row = false,endSequence = false;
		if (op >= opcodeBase) {
			const uint8_t adjusted = (uint8_t)(op - opcodeBase);
			address += (uint32_t)(adjusted / lineRange) * minimum;
			line = (uint64_t)((int64_t)line + lineBase + (adjusted % lineRange));
			row = true;
		} else if (op == 0) {
			const uint64_t extra = r.uleb();
			const size_t next = r.at + (size_t)extra;
			const uint8_t sub = r.u8();
			if (sub == 1) {
				row = true;
				endSequence = true;
			} else if (sub == 2) address = extra - 1 == 4 ? r.u32() : (extra - 1 == 2 ? r.u16() : r.u8());
			r.at = next;
		} else switch (op) {
		case 1: row = true; break;
		case 2: address += (uint32_t)(r.uleb() * minimum); break;
		case 3: line = (uint64_t)((int64_t)line + r.sleb()); break;
		case 4: file = r.uleb(); break;
		case 5: r.uleb(); break;
		case 6: statement = !statement; break;
		case 7: case 10: case 11: break;
		case 8: address += (uint32_t)(((255 - opcodeBase) / lineRange) * minimum); break;
		case 9: address += r.u16(); break;
		default:
			for (uint8_t a = 0;a < operands[op];a++) r.uleb();
			break;
		}

		if (!row) continue;
		DwarfLine entry;
		entry.address = address;
		entry.line = (uint32_t)line;
		entry.endSequence = endSequence;
		entry.statement = statement;
		entry.unit = unitName;
		entry.file = file < files.size() ? files[(size_t)file].name : unitName;
		out.lines.push_back(entry);
		if (endSequence) {
			address = 0;
			file = 1;
			line = 1;
			statement = isStmt;
		}
	}
	(void)firstRow;
	return true;
}

/* ---- the DIEs ---- */

enum {
	DW_TAG_compile_unit = 0x11,DW_TAG_label = 0x0a,DW_TAG_subprogram = 0x2e,DW_TAG_variable = 0x34,
	DW_AT_name = 0x03,DW_AT_low_pc = 0x11,DW_AT_high_pc = 0x12,DW_AT_segment = 0x46,DW_AT_location = 0x02,
	DW_OP_addr = 0x03
};

struct Abbreviation {
	uint64_t tag = 0;
	bool children = false;
	std::vector<std::pair<uint64_t,uint64_t> > attributes;
};

/* A table's end is a zero code, but a linker may stop without one: the section's end ends it too. */
std::map<uint64_t,Abbreviation> ReadAbbreviations(const DebugBytes &abbrev,size_t from)
{
	std::map<uint64_t,Abbreviation> table;
	Reader r(abbrev,from);
	while (r.at < abbrev.size()) {
		const uint64_t code = r.uleb();
		if (code == 0) break;
		Abbreviation entry;
		entry.tag = r.uleb();
		entry.children = r.u8() != 0;
		for (;;) {
			const uint64_t name = r.uleb();
			const uint64_t form = r.uleb();
			if (name == 0 && form == 0) break;
			entry.attributes.push_back(std::make_pair(name,form));
			if (r.at >= abbrev.size()) break;
		}
		table[code] = entry;
	}
	return table;
}

/* The segment a block names: a single byte, or a constant pushed by DW_OP_const/constu. */
uint16_t SegmentOf(const DebugBytes &block)
{
	if (block.size() == 0) return 0;
	if (block.size() == 1) return block.u8(0);
	if (block.size() == 2 && block.u8(0) == 0x08) return block.u8(1);		/* DW_OP_const1u */
	if (block.size() == 3 && (block.u8(0) == 0x0a || block.u8(0) == 0x0c)) return block.u16(1);
	if (block.size() == 2 && block.u8(0) == 0x10) return block.u8(1);		/* DW_OP_constu */
	return block.u16(0);
}

bool ReadInfoUnit(const Sections &sections,size_t &at,DwarfInfo &out)
{
	Reader r(sections.info,at);
	const uint32_t length = r.u32();
	if (length == 0xffffffffu || length < 7 || (size_t)at + 4 + length > sections.info.size()) return false;
	const size_t end = at + 4 + (size_t)length;
	at = end;

	const uint16_t version = r.u16();
	if (version < 2 || version > 5) {
		out.warnings.push_back("info unit version not read");
		return true;
	}
	uint32_t abbrevAt;
	uint8_t addressSize;
	if (version >= 5) {
		const uint8_t unitType = r.u8();
		addressSize = r.u8();
		abbrevAt = r.u32();
		if (unitType != 1) return true;			/* only compile units name the program's code and data */
	} else {
		abbrevAt = r.u32();
		addressSize = r.u8();
	}
	out.version = std::max(out.version,version);

	const std::map<uint64_t,Abbreviation> abbreviations = ReadAbbreviations(sections.abbrev,abbrevAt);
	std::string unit;
	while (!r.done(end)) {
		const uint64_t code = r.uleb();
		if (code == 0) continue;
		const std::map<uint64_t,Abbreviation>::const_iterator found = abbreviations.find(code);
		if (found == abbreviations.end()) {
			out.warnings.push_back("a DIE names an abbreviation the table lacks");
			return true;
		}

		const Abbreviation &abbreviation = found->second;
		std::string name;
		uint64_t low = 0,high = 0;
		bool hasLow = false,hasHigh = false,highIsSize = false,hasAddress = false;
		uint16_t segment = 0;
		for (size_t a = 0;a < abbreviation.attributes.size();a++) {
			Value value;
			if (!ReadForm(r,abbreviation.attributes[a].second,addressSize,version,sections,value)) {
				out.warnings.push_back("an attribute form is not read");
				return true;
			}
			switch (abbreviation.attributes[a].first) {
			case DW_AT_name: name = value.text; break;
			case DW_AT_low_pc: low = value.number; hasLow = true; break;
			case DW_AT_high_pc:
				high = value.number;
				hasHigh = true;
				/* From version 4 a high_pc in a data form is the length; an address form is where it ends. */
				highIsSize = abbreviation.attributes[a].second != DW_FORM_addr;
				break;
			case DW_AT_segment: segment = SegmentOf(value.block); break;
			case DW_AT_location:
				if (value.block.size() == 1 + (size_t)addressSize && value.block.u8(0) == DW_OP_addr) {
					low = value.block.u32(1);
					hasAddress = true;
				}
				break;
			}
		}

		if (abbreviation.tag == DW_TAG_compile_unit) unit = name;
		else if (!name.empty() && (hasLow || hasAddress) &&
		         (abbreviation.tag == DW_TAG_subprogram || abbreviation.tag == DW_TAG_label ||
		          abbreviation.tag == DW_TAG_variable)) {
			DwarfSymbol symbol;
			symbol.name = name;
			symbol.address = (uint32_t)low;
			symbol.segment = segment;
			symbol.function = abbreviation.tag != DW_TAG_variable;
			symbol.unit = unit;
			if (hasHigh && hasLow) {
				symbol.hasSize = true;
				symbol.size = (uint32_t)(highIsSize ? high : high - low);
			}
			out.symbols.push_back(symbol);
		}
	}
	return true;
}

} /* namespace */

bool DEBUG_ParseDwarf(const DebugBytes &data,DwarfInfo &out)
{
	DebugBytes elf;
	if (!FindElf(data,elf)) return false;

	Sections sections;
	sections.info = ElfSection(elf,".debug_info");
	sections.abbrev = ElfSection(elf,".debug_abbrev");
	sections.line = ElfSection(elf,".debug_line");
	sections.str = ElfSection(elf,".debug_str");
	sections.lineStr = ElfSection(elf,".debug_line_str");
	if (sections.info.size() == 0 && sections.line.size() == 0) return false;

	for (size_t at = 0;at < sections.line.size();)
		if (!ReadLineUnit(sections.line,at,sections,out)) {
			out.warnings.push_back("a line program is cut short");
			break;
		}
	for (size_t at = 0;at < sections.info.size();)
		if (!ReadInfoUnit(sections,at,out)) {
			out.warnings.push_back("an info unit is cut short");
			break;
		}
	return true;
}
