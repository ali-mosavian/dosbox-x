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
#include <map>

namespace {

/* ---- the ELF container ---- */

struct Sections {
	DebugBytes info,abbrev,line,str,lineStr,frame,loc,loclists;
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
	uint64_t form = 0;
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
	DW_TAG_array_type = 0x01,DW_TAG_enumeration_type = 0x04,DW_TAG_formal_parameter = 0x05,
	DW_TAG_label = 0x0a,DW_TAG_lexical_block = 0x0b,DW_TAG_member = 0x0d,DW_TAG_pointer_type = 0x0f,
	DW_TAG_compile_unit = 0x11,DW_TAG_structure_type = 0x13,DW_TAG_subroutine_type = 0x15,
	DW_TAG_typedef = 0x16,DW_TAG_union_type = 0x17,DW_TAG_subrange_type = 0x21,DW_TAG_base_type = 0x24,
	DW_TAG_const_type = 0x26,DW_TAG_subprogram = 0x2e,DW_TAG_variable = 0x34,DW_TAG_volatile_type = 0x35,
	DW_AT_location = 0x02,DW_AT_name = 0x03,DW_AT_byte_size = 0x0b,DW_AT_low_pc = 0x11,DW_AT_high_pc = 0x12,
	DW_AT_upper_bound = 0x2f,DW_AT_count = 0x37,DW_AT_data_member_location = 0x38,DW_AT_encoding = 0x3e,
	DW_AT_frame_base = 0x40,DW_AT_type = 0x49,DW_AT_segment = 0x46,
	DW_OP_addr = 0x03,DW_OP_call_frame_cfa = 0x9c,DW_OP_fbreg = 0x91,DW_OP_regx = 0x90
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

/* One DIE, with the attributes this reader uses. */
struct Die {
	uint64_t tag = 0;
	size_t offset = 0;		/* in .debug_info */
	int parent = -1;
	size_t unit = 0;		/* index of its unit */
	std::map<uint64_t,Value> attributes;
	bool has(uint64_t at) const { return attributes.count(at) != 0; }
	uint64_t number(uint64_t at) const
	{
		const std::map<uint64_t,Value>::const_iterator it = attributes.find(at);
		return it == attributes.end() ? 0 : it->second.number;
	}
	std::string name() const
	{
		const std::map<uint64_t,Value>::const_iterator it = attributes.find(DW_AT_name);
		return it == attributes.end() ? std::string() : it->second.text;
	}
};

struct Unit {
	uint16_t version = 0;
	uint8_t addressSize = 4;
	size_t start = 0;		/* of its header in .debug_info */
	uint32_t base = 0;		/* the compile unit's low_pc: where its location lists' pairs start from */
	std::string name;
};

/* Reads every unit's DIEs; false only when a unit's end cannot be found. */
void ReadUnits(const Sections &sections,std::vector<Unit> &units,std::vector<Die> &dies,DwarfInfo &out)
{
	for (size_t at = 0;at < sections.info.size();) {
		Reader r(sections.info,at);
		const uint32_t length = r.u32();
		if (length == 0xffffffffu || length < 7 || (size_t)at + 4 + length > sections.info.size()) {
			out.warnings.push_back("an info unit is cut short");
			return;
		}
		Unit unit;
		unit.start = at;
		const size_t end = at + 4 + (size_t)length;
		at = end;

		unit.version = r.u16();
		if (unit.version < 2 || unit.version > 5) {
			out.warnings.push_back("info unit version not read");
			continue;
		}
		uint32_t abbrevAt;
		if (unit.version >= 5) {
			const uint8_t unitType = r.u8();
			unit.addressSize = r.u8();
			abbrevAt = r.u32();
			if (unitType != 1) continue;		/* only compile units name the program's code and data */
		} else {
			abbrevAt = r.u32();
			unit.addressSize = r.u8();
		}
		out.version = std::max(out.version,unit.version);

		const std::map<uint64_t,Abbreviation> abbreviations = ReadAbbreviations(sections.abbrev,abbrevAt);
		const size_t unitIndex = units.size();
		std::vector<int> open;			/* the DIEs whose children are being read */
		bool ok = true;
		while (ok && !r.done(end)) {
			const size_t where = r.at;
			const uint64_t code = r.uleb();
			if (code == 0) {
				if (!open.empty()) open.pop_back();
				continue;
			}
			const std::map<uint64_t,Abbreviation>::const_iterator found = abbreviations.find(code);
			if (found == abbreviations.end()) {
				out.warnings.push_back("a DIE names an abbreviation the table lacks");
				break;
			}

			Die die;
			die.tag = found->second.tag;
			die.offset = where;
			die.unit = unitIndex;
			die.parent = open.empty() ? -1 : open.back();
			for (size_t a = 0;a < found->second.attributes.size();a++) {
				Value value;
				if (!ReadForm(r,found->second.attributes[a].second,unit.addressSize,unit.version,sections,value)) {
					out.warnings.push_back("an attribute form is not read");
					ok = false;
					break;
				}
				/* A reference is relative to its unit's header. */
				const uint64_t form = found->second.attributes[a].second;
				value.form = form;
				if (form == DW_FORM_ref1 || form == DW_FORM_ref2 || form == DW_FORM_ref4 || form == DW_FORM_ref_udata)
					value.number += unit.start;
				die.attributes[found->second.attributes[a].first] = value;
			}
			if (!ok) break;

			if (die.tag == DW_TAG_compile_unit) {
				unit.name = die.name();
				unit.base = (uint32_t)die.number(DW_AT_low_pc);
			}
			dies.push_back(die);
			if (found->second.children) open.push_back((int)dies.size() - 1);
		}
		units.push_back(unit);
	}
	/* a unit pushed after its DIEs: patch the ones whose names are only known now */
}

/* ---- locations ---- */

/* The place a one-operation DWARF expression names; false for one that is not simple enough to read. */
bool ReadLocation(const DebugBytes &expression,DwarfLocation &out)
{
	if (expression.size() == 0) return false;
	Reader r(expression,0);
	const uint8_t op = r.u8();
	if (op == DW_OP_addr) {
		out.kind = DWARF_LOCATION_ADDRESS;
		out.address = r.u32();
		return r.at == expression.size();
	}
	if (op == DW_OP_fbreg) {
		out.kind = DWARF_LOCATION_FRAME;
		out.offset = (int32_t)r.sleb();
		return r.at == expression.size();
	}
	if (op >= 0x50 && op <= 0x6f) {
		out.kind = DWARF_LOCATION_REGISTER;
		out.reg = (uint16_t)(op - 0x50);
		return r.at == expression.size();
	}
	if (op == DW_OP_regx) {
		out.kind = DWARF_LOCATION_REGISTER;
		out.reg = (uint16_t)r.uleb();
		return r.at == expression.size();
	}
	if (op >= 0x70 && op <= 0x8f) {
		out.kind = DWARF_LOCATION_REGISTER_OFFSET;
		out.reg = (uint16_t)(op - 0x70);
		out.offset = (int32_t)r.sleb();
		return r.at == expression.size();
	}
	return false;
}

/* A location list: each entry is a range of the code and where the value is over it. */
void ReadLocationList(const Sections &sections,uint16_t version,uint32_t unitBase,size_t at,uint8_t addressSize,
                      std::vector<DwarfLocation> &out)
{
	uint32_t base = unitBase;
	if (version >= 5) {
		Reader r(sections.loclists,at);
		while (!r.done(sections.loclists.size())) {
			const uint8_t kind = r.u8();
			uint32_t begin = 0,finish = 0;
			bool range = true;
			if (kind == 0) return;
			switch (kind) {
			case 4: begin = base + (uint32_t)r.uleb(); finish = base + (uint32_t)r.uleb(); break;
			case 5: begin = 0; finish = 0xffffffffu; break;				/* default_location */
			case 6: base = r.u32(); range = false; break;				/* base_address */
			case 7: begin = r.u32(); finish = r.u32(); break;			/* start_end */
			case 8: begin = r.u32(); finish = begin + (uint32_t)r.uleb(); break;	/* start_length */
			default: return;		/* indexed forms need .debug_addr: not written by anything here */
			}
			if (!range) continue;
			const size_t length = (size_t)r.uleb();
			DwarfLocation where;
			if (ReadLocation(r.data.sub(r.at,length),where)) {
				where.begin = begin;
				where.end = finish;
				out.push_back(where);
			}
			r.at += length;
		}
		return;
	}

	Reader r(sections.loc,at);
	(void)addressSize;
	while (!r.done(sections.loc.size())) {
		const uint32_t first = r.u32();
		const uint32_t second = r.u32();
		if (first == 0 && second == 0) return;
		if (first == 0xffffffffu) {
			base = second;			/* a base address selection */
			continue;
		}
		const size_t length = r.u16();
		DwarfLocation where;
		if (ReadLocation(r.data.sub(r.at,length),where)) {
			where.begin = base + first;
			where.end = base + second;
			out.push_back(where);
		}
		r.at += length;
	}
}

/* ---- call frame information ---- */

struct CfaState {
	uint16_t reg = 4;
	int32_t offset = 4;
};

/* The rows of .debug_frame: the register the canonical frame address is measured from, and how far, over each
 * range of the code. A rule that is not a plain register and offset ends the function's rows there. */
void ReadFrames(const DebugBytes &frame,DwarfInfo &out)
{
	struct Cie {
		uint32_t codeAlign;
		int32_t dataAlign;
		CfaState initial;
	};
	std::map<size_t,Cie> cies;

	for (size_t at = 0;at + 4 <= frame.size();) {
		Reader r(frame,at);
		const uint32_t length = r.u32();
		if (length == 0 || length == 0xffffffffu || at + 4 + length > frame.size()) return;
		const size_t end = at + 4 + (size_t)length;
		const size_t entry = at;
		at = end;

		const uint32_t id = r.u32();
		if (id == 0xffffffffu) {					/* a CIE */
			const uint8_t version = r.u8();
			const std::string augmentation = r.cstr();
			if (version >= 4) { r.u8(); r.u8(); }			/* address size, segment selector size */
			Cie cie;
			cie.codeAlign = (uint32_t)r.uleb();
			cie.dataAlign = (int32_t)r.sleb();
			if (version == 1) r.u8(); else r.uleb();		/* return address register */
			if (!augmentation.empty()) {
				out.warnings.push_back("a CIE with augmentation is not read");
				continue;
			}
			while (!r.done(end)) {					/* initial instructions */
				const uint8_t op = r.u8();
				if (op == 0x0c) { cie.initial.reg = (uint16_t)r.uleb(); cie.initial.offset = (int32_t)r.uleb(); }
				else if (op == 0x0d) cie.initial.reg = (uint16_t)r.uleb();
				else if (op == 0x0e) cie.initial.offset = (int32_t)r.uleb();
				else if ((op & 0xc0) == 0x80) r.uleb();
				else if (op != 0) break;
			}
			cies[entry] = cie;
			continue;
		}

		const std::map<size_t,Cie>::const_iterator parent = cies.find(id);
		if (parent == cies.end()) continue;
		const Cie &cie = parent->second;
		uint32_t location = r.u32();
		const uint32_t finish = location + r.u32();

		CfaState state = cie.initial;
		std::vector<CfaState> saved;
		uint32_t rowStart = location;
		bool known = true;
		const struct {
			DwarfInfo &out;
			void close(uint32_t &from,uint32_t to,const CfaState &state) const
			{
				if (to > from) {
					DwarfCfaRow row;
					row.begin = from;
					row.end = to;
					row.reg = state.reg;
					row.offset = state.offset;
					out.cfa.push_back(row);
				}
				from = to;
			}
		} rows = {out};

		while (!r.done(end) && known) {
			const uint8_t op = r.u8();
			uint32_t advance = 0;
			bool advances = false;
			if ((op >> 6) == 1) { advance = (uint32_t)(op & 0x3f) * cie.codeAlign; advances = true; }
			else if ((op >> 6) == 2) { r.uleb(); continue; }		/* DW_CFA_offset */
			else if ((op >> 6) == 3) continue;				/* DW_CFA_restore */
			else switch (op) {
			case 0x00: break;
			case 0x02: advance = r.u8() * cie.codeAlign; advances = true; break;
			case 0x03: advance = r.u16() * cie.codeAlign; advances = true; break;
			case 0x04: advance = r.u32() * cie.codeAlign; advances = true; break;
			case 0x05: r.uleb(); r.uleb(); break;
			case 0x06: case 0x07: case 0x08: r.uleb(); break;
			case 0x09: r.uleb(); r.uleb(); break;
			case 0x0a: saved.push_back(state); break;
			case 0x0b: if (!saved.empty()) { state = saved.back(); saved.pop_back(); } break;
			case 0x0c: state.reg = (uint16_t)r.uleb(); state.offset = (int32_t)r.uleb(); break;
			case 0x0d: state.reg = (uint16_t)r.uleb(); break;
			case 0x0e: state.offset = (int32_t)r.uleb(); break;
			case 0x11: r.uleb(); r.sleb(); break;
			case 0x12: state.reg = (uint16_t)r.uleb(); state.offset = (int32_t)(r.sleb() * cie.dataAlign); break;
			case 0x13: state.offset = (int32_t)(r.sleb() * cie.dataAlign); break;
			default: known = false; break;
			}
			if (advances) {
				rows.close(rowStart,location + advance,state);
				location += advance;
			}
		}
		if (known) rows.close(rowStart,finish,state);
	}
}

/* ---- types, scopes and symbols ---- */

DwarfType *TypeOf(DwarfInfo &out,size_t offset)
{
	const std::map<size_t,size_t>::const_iterator found = out.typeIndex.find(offset);
	return found == out.typeIndex.end() ? NULL : &out.types[found->second];
}

void CollectTypes(const std::vector<Die> &dies,DwarfInfo &out)
{
	for (size_t i = 0;i < dies.size();i++) {
		const Die &die = dies[i];
		switch (die.tag) {
		case DW_TAG_base_type: case DW_TAG_pointer_type: case DW_TAG_structure_type: case DW_TAG_union_type:
		case DW_TAG_array_type: case DW_TAG_typedef: case DW_TAG_const_type: case DW_TAG_volatile_type:
		case DW_TAG_enumeration_type: case DW_TAG_subroutine_type: {
			DwarfType type;
			type.tag = (uint16_t)die.tag;
			type.name = die.name();
			type.size = (uint32_t)die.number(DW_AT_byte_size);
			type.encoding = (uint8_t)die.number(DW_AT_encoding);
			type.target = die.has(DW_AT_type) ? die.number(DW_AT_type) : 0;
			out.typeIndex[die.offset] = out.types.size();
			out.types.push_back(type);
			break;
		}
		}
	}
	for (size_t i = 0;i < dies.size();i++) {
		const Die &die = dies[i];
		if (die.parent < 0) continue;
		DwarfType *owner = TypeOf(out,dies[(size_t)die.parent].offset);
		if (owner == NULL) continue;
		if (die.tag == DW_TAG_member) {
			DwarfField field;
			field.name = die.name();
			field.offset = (uint32_t)die.number(DW_AT_data_member_location);
			field.type = die.has(DW_AT_type) ? die.number(DW_AT_type) : 0;
			owner->members.push_back(field);
		} else if (die.tag == DW_TAG_subrange_type) {
			if (die.has(DW_AT_upper_bound)) owner->count = (uint32_t)die.number(DW_AT_upper_bound) + 1;
			else if (die.has(DW_AT_count)) owner->count = (uint32_t)die.number(DW_AT_count);
		}
	}
}

/* The variables and scopes, and the plain symbols (code and data) the publics of jwlink's DWARF are. */
void CollectScopes(const Sections &sections,const std::vector<Unit> &units,const std::vector<Die> &dies,DwarfInfo &out)
{
	std::map<int,int> scopeOf;			/* a DIE's index -> its scope's, for the DIEs that open one */
	for (size_t i = 0;i < dies.size();i++) {
		const Die &die = dies[i];
		const Unit &unit = units[die.unit];
		const bool opens = (die.tag == DW_TAG_subprogram || die.tag == DW_TAG_lexical_block) && die.has(DW_AT_low_pc) &&
		                   die.has(DW_AT_high_pc);
		if (opens) {
			DwarfScope scope;
			scope.function = die.tag == DW_TAG_subprogram ? die.name() : std::string();
			scope.begin = (uint32_t)die.number(DW_AT_low_pc);
			const Value &high = die.attributes.find(DW_AT_high_pc)->second;
			scope.end = high.form == DW_FORM_addr ? (uint32_t)high.number : scope.begin + (uint32_t)high.number;
			for (int up = die.parent;up >= 0 && scope.parent < 0;up = dies[(size_t)up].parent)
				if (scopeOf.count(up)) scope.parent = scopeOf[up];
			scope.frame = DWARF_FRAME_CFA;
			if (die.has(DW_AT_frame_base)) {
				const DebugBytes &base = die.attributes.find(DW_AT_frame_base)->second.block;
				if (base.size() == 1 && base.u8(0) == DW_OP_call_frame_cfa) scope.frame = DWARF_FRAME_CFA;
				else if (base.size() == 1 && base.u8(0) >= 0x50 && base.u8(0) <= 0x6f) {
					scope.frame = DWARF_FRAME_REGISTER;
					scope.frameReg = (uint16_t)(base.u8(0) - 0x50);
				}
			} else if (scope.parent >= 0) {
				scope.frame = out.scopes[(size_t)scope.parent].frame;
				scope.frameReg = out.scopes[(size_t)scope.parent].frameReg;
			}
			scopeOf[(int)i] = (int)out.scopes.size();
			out.scopes.push_back(scope);

			if (die.tag == DW_TAG_subprogram && !die.name().empty()) {
				DwarfSymbol symbol;
				symbol.name = die.name();
				symbol.address = scope.begin;
				symbol.size = scope.end - scope.begin;
				symbol.hasSize = true;
				symbol.function = true;
				symbol.unit = unit.name;
				out.symbols.push_back(symbol);
			}
			continue;
		}

		if (die.tag != DW_TAG_variable && die.tag != DW_TAG_formal_parameter && die.tag != DW_TAG_label &&
		    die.tag != DW_TAG_subprogram) continue;
		if (die.name().empty()) continue;

		/* jwlink's publics: a name and where it begins. */
		if (die.has(DW_AT_low_pc) && !die.has(DW_AT_location) && (die.tag == DW_TAG_label || die.tag == DW_TAG_variable ||
		                                                          die.tag == DW_TAG_subprogram)) {
			DwarfSymbol symbol;
			symbol.name = die.name();
			symbol.address = (uint32_t)die.number(DW_AT_low_pc);
			symbol.segment = die.has(DW_AT_segment) ? SegmentOf(die.attributes.find(DW_AT_segment)->second.block) : 0;
			symbol.function = die.tag != DW_TAG_variable;
			symbol.unit = unit.name;
			symbol.type = die.has(DW_AT_type) ? die.number(DW_AT_type) : 0;
			out.symbols.push_back(symbol);
			continue;
		}
		if (!die.has(DW_AT_location)) continue;

		const Value &where = die.attributes.find(DW_AT_location)->second;
		std::vector<DwarfLocation> locations;
		if (where.block.size() != 0) {
			DwarfLocation location;
			if (ReadLocation(where.block,location)) locations.push_back(location);
		} else if (where.form == DW_FORM_sec_offset || where.form == DW_FORM_data4) {
			ReadLocationList(sections,unit.version,unit.base,(size_t)where.number,unit.addressSize,locations);
		}
		if (locations.empty()) continue;

		const int owner = die.parent >= 0 && scopeOf.count(die.parent) ? scopeOf[die.parent] : -1;
		if (owner >= 0) {
			DwarfVariable variable;
			variable.name = die.name();
			variable.type = die.has(DW_AT_type) ? die.number(DW_AT_type) : 0;
			variable.parameter = die.tag == DW_TAG_formal_parameter;
			variable.where = locations;
			out.scopes[(size_t)owner].variables.push_back(variable);
		} else if (locations[0].kind == DWARF_LOCATION_ADDRESS) {
			DwarfSymbol symbol;
			symbol.name = die.name();
			symbol.address = locations[0].address;
			symbol.unit = unit.name;
			symbol.type = die.has(DW_AT_type) ? die.number(DW_AT_type) : 0;
			out.symbols.push_back(symbol);
		}
	}
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
	sections.frame = ElfSection(elf,".debug_frame");
	sections.loc = ElfSection(elf,".debug_loc");
	sections.loclists = ElfSection(elf,".debug_loclists");
	if (sections.info.size() == 0 && sections.line.size() == 0) return false;

	for (size_t at = 0;at < sections.line.size();)
		if (!ReadLineUnit(sections.line,at,sections,out)) {
			out.warnings.push_back("a line program is cut short");
			break;
		}

	std::vector<Unit> units;
	std::vector<Die> dies;
	ReadUnits(sections,units,dies,out);
	CollectTypes(dies,out);
	CollectScopes(sections,units,dies,out);
	ReadFrames(sections.frame,out);
	return true;
}
