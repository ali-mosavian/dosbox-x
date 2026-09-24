/*
 * debug_symfmt_cv.cpp - Microsoft CodeView debug info as CVPACK/LINK appends
 * it to a linked MZ EXE.
 *
 * Not the same shape as the pre-link CodeView BC leaves in an OMF .OBJ, where
 * every record's length and kind are one byte each and there is no directory:
 * here records are [length:u16][kind:u16][data] and the whole program's
 * symbols hang off one subsection directory, addressed by logical segment
 * index.
 */

#include "debug_symfmt.h"

#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <set>

static const char * const CV_SIGNATURES[] = {
	"NB00","NB01","NB02","NB03","NB04","NB05","NB06","NB07","NB08","NB09","NB10","NB11"
};

/* CV4 (NB05/NB08/NB09) and CV5 (NB11) subsection kinds. */
enum {
	sstModule = 0x120,
	sstTypes = 0x121,
	sstPublicSym = 0x123,
	sstSymbols = 0x124,
	sstAlignSym = 0x125,
	sstSrcModule = 0x127,
	sstGlobalSym = 0x129,
	sstGlobalPub = 0x12a,
	sstSegMap = 0x12d,
	sstGlobalTypes = 0x12b,
	sstStaticSym = 0x134
};

/* Symbol record kinds. The 32-bit forms are here because a mixed-model image
 * can carry them; every other kind is stepped over by its own length. */
enum {
	S_LDATA16 = 0x0101,
	S_GDATA16 = 0x0102,
	S_PUB16   = 0x0103,
	S_LPROC16 = 0x0104,
	S_GPROC16 = 0x0105,
	S_LABEL16 = 0x0109,
	S_BPREL16 = 0x0100,
	S_BPREL32 = 0x0200,
	S_REGISTER = 0x0002,
	S_ENDBLK  = 0x0006,
	S_BLOCK16 = 0x0107,
	S_LDATA32 = 0x0201,
	S_GDATA32 = 0x0202,
	S_PUB32   = 0x0203
};

static std::string SymFormat(const char *fmt,...)
{
	char buf[256];
	va_list args;
	va_start(args,fmt);
	vsnprintf(buf,sizeof(buf),fmt,args);
	va_end(args);
	return std::string(buf);
}

static bool ReadCvSignature(const DebugBytes &data,int64_t at,std::string &out)
{
	if (at < 0 || (uint64_t)at + 8 > data.size()) return false;
	const std::string text = data.latin1((size_t)at,(size_t)at + 4);
	if (!DEBUG_IsCvSignature(text)) return false;
	out = text;
	return true;
}

static std::vector<CvDirEntry> ReadDirectory(const DebugBytes &data,uint64_t base,bool isCv3,std::vector<std::string> &warnings)
{
	std::vector<CvDirEntry> entries;

	const uint32_t lfoDirectory = data.u32((size_t)base + 4);
	const uint64_t at = base + lfoDirectory;
	/* The CV4 header is 8 bytes before cDir has been read; CV3's count is 2. */
	if (at + (isCv3 ? 2u : 8u) > data.size()) {
		warnings.push_back(SymFormat("directory offset %u past end of file",(unsigned int)lfoDirectory));
		return entries;
	}

	/* CV3 has no directory header: a bare u16 count followed by 10-byte
	 * entries whose length field is 16-bit (Open Watcom's cv3_dir_entry;
	 * measured on BC 4.5 + LINK 3.69). */
	if (isCv3) {
		const uint16_t count = data.u16((size_t)at);
		for (uint16_t i = 0;i < count;i++) {
			const uint64_t off = at + 2u + (uint64_t)i * 10u;
			if (off + 10u > data.size()) break;
			CvDirEntry entry;
			entry.subsection = data.u16((size_t)off);
			entry.moduleIndex = data.u16((size_t)off + 2);
			entry.offset = data.u32((size_t)off + 4);
			entry.length = data.u16((size_t)off + 8);
			entries.push_back(entry);
		}
		return entries;
	}

	const uint16_t cbDirHeader = data.u16((size_t)at);
	const uint16_t cbDirEntry = data.u16((size_t)at + 2);
	const uint32_t cDir = data.u32((size_t)at + 4);
	if (cbDirEntry < 12 || cbDirHeader < 8) {
		warnings.push_back(SymFormat("implausible directory header cbDirHeader=%u cbDirEntry=%u",
		                          (unsigned int)cbDirHeader,(unsigned int)cbDirEntry));
		return entries;
	}

	for (uint32_t i = 0;i < cDir;i++) {
		const uint64_t off = at + cbDirHeader + (uint64_t)i * cbDirEntry;
		if (off + cbDirEntry > data.size()) break;
		CvDirEntry entry;
		entry.subsection = data.u16((size_t)off);
		entry.moduleIndex = data.u16((size_t)off + 2);
		entry.offset = data.u32((size_t)off + 4);
		entry.length = data.u32((size_t)off + 8);
		entries.push_back(entry);
	}
	if (entries.size() != cDir) {
		warnings.push_back(SymFormat("directory claims %u entries, %u fit in the file",
		                          (unsigned int)cDir,(unsigned int)entries.size()));
	}
	return entries;
}

static bool ParseModule(const DebugBytes &body,uint16_t index,CvModule &out)
{
	if (body.size() < 8) return false;

	const uint16_t cSeg = body.u16(4);
	out.index = index;
	out.style = body.latin1(6,8);

	size_t at = 8;
	for (uint16_t i = 0;i < cSeg;i++) {
		if (at + 12 > body.size()) break;
		CvSegInfo seg;
		seg.segment = body.u16(at);
		seg.offset = body.u32(at + 4);
		seg.length = body.u32(at + 8);
		out.segments.push_back(seg);
		at += 12;
	}
	out.name = body.pstr(at);
	return true;
}

/* A numeric leaf: a value below 0x8000 is itself, anything else says which
 * kind of number follows it. */
static uint32_t ReadNumericLeaf(const DebugBytes &body,size_t at,size_t &next)
{
	const uint16_t lead = body.u16(at);
	if (lead < 0x8000u) {
		next = at + 2;
		return lead;
	}

	switch (lead) {
	case 0x8000: next = at + 3; return body.u8(at + 2);			/* LF_CHAR */
	case 0x8001: next = at + 4; return body.u16(at + 2);			/* LF_SHORT */
	case 0x8002: next = at + 4; return body.u16(at + 2);			/* LF_USHORT */
	case 0x8003: next = at + 6; return body.u32(at + 2);			/* LF_LONG */
	case 0x8004: next = at + 6; return body.u32(at + 2);			/* LF_ULONG */
	}
	next = at + 2;
	return 0;
}

/*
 * The members of one LF_FIELDLIST: each is a leaf, the member's type, an
 * attribute word, where it sits in the structure, and its name. Bytes 0xF0
 * and up are padding to the next four-byte boundary, not a leaf.
 */
static std::vector<CvMember> ParseFieldList(const DebugBytes &body,size_t at,size_t end)
{
	std::vector<CvMember> members;

	while (at + 2 <= end) {
		/* LF_PADn says how many bytes to skip, itself included: stepping
		 * over one byte lands in the middle of the padding and reads 0x0000
		 * as the next leaf, which ends the list after its first member. */
		if (body.u8(at) >= 0xf0) {
			at += body.u8(at) & 0x0fu;
			continue;
		}

		const uint16_t leaf = body.u16(at);
		if (leaf != 0x0406 && leaf != 0x0405) break;	/* LF_MEMBER, LF_STMEMBER */

		CvMember member;
		member.type = body.u16(at + 2);

		size_t next = 0;
		member.offset = ReadNumericLeaf(body,at + 6,next);
		member.name = body.pstr(next,&next);
		members.push_back(member);
		at = next;
	}
	return members;
}

/* One [length:u16][leaf:u16][data] record, whether it was reached through the
 * packed table's offset array or by walking a module's own run. */
static CvType ParseTypeRecord(const DebugBytes &body,size_t record,uint16_t length)
{
	CvType type;
	type.leaf = body.u16(record);

	switch (type.leaf) {
	case 0x0001:					/* LF_MODIFIER: const/volatile */
		type.utype = body.u16(record + 4);
		break;
	case 0x0002:					/* LF_POINTER */
		type.utype = body.u16(record + 4);
		type.size = (body.u16(record + 2) & 0x1fu) == 0 ? 2u : 4u;
		break;
	case 0x0003: {					/* LF_ARRAY */
		size_t next = 0;
		type.utype = body.u16(record + 2);
		type.size = ReadNumericLeaf(body,record + 6,next);
		type.name = body.pstr(next);
		break;
	}
	case 0x0004:					/* LF_CLASS */
	case 0x0005:					/* LF_STRUCTURE */
	case 0x0006: {					/* LF_UNION */
		size_t next = 0;
		const size_t at_size = type.leaf == 0x0006 ? record + 6 : record + 12;
		type.fieldList = body.u16(record + 4);
		type.size = ReadNumericLeaf(body,at_size,next);
		type.name = body.pstr(next);
		break;
	}
	case 0x0204:					/* LF_FIELDLIST */
		type.members = ParseFieldList(body,record + 2,record + length);
		break;
	case 0x0008:					/* LF_PROCEDURE */
		type.utype = body.u16(record + 2);	/* return type */
		break;
	case 0x000d:					/* LF_BARRAY: a BASIC array */
		type.utype = body.u16(record + 2);
		break;
	default:
		break;
	}
	return type;
}

/*
 * sstGlobalTypes: flags, a count, that many record offsets, then the records.
 * The offsets are counted from the start of the subsection -- the first one
 * lands exactly on the byte after the offset array, which is where the
 * records begin.
 */
static std::vector<CvType> ParseGlobalTypes(const DebugBytes &body)
{
	std::vector<CvType> types;
	const uint32_t count = body.u32(4);
	if (count == 0 || (uint64_t)8 + (uint64_t)count * 4 > body.size()) return types;

	for (uint32_t i = 0;i < count;i++) {
		const uint32_t at = body.u32(8 + (size_t)i * 4);
		if ((uint64_t)at + 4 > body.size()) break;

		const uint16_t length = body.u16(at);
		const size_t record = (size_t)at + 2;
		if (length < 2 || (uint64_t)record + length > body.size()) break;

		types.push_back(ParseTypeRecord(body,record,length));
	}
	return types;
}

/*
 * sstTypes: what a module carries before CVPACK gathers every module's types
 * into one table. No offset array -- the records run back to back, opening
 * with the same 4-byte version signature the symbol subsections use.
 */
static std::vector<CvType> ParseModuleTypes(const DebugBytes &body)
{
	std::vector<CvType> types;
	size_t at = 0;
	if (at + 4 <= body.size() && body.u16(at) < 2) at += 4;

	while (at + 4 <= body.size()) {
		const uint16_t length = body.u16(at);
		if (length < 2 || (uint64_t)at + 2 + length > body.size()) break;
		types.push_back(ParseTypeRecord(body,at + 2,length));
		at += 2u + length;
	}
	return types;
}

static std::vector<CvSegMapEntry> ParseSegMap(const DebugBytes &body)
{
	std::vector<CvSegMapEntry> entries;
	if (body.size() < 4) return entries;

	/* OpenWatcom's DIP allocates cSegLog descriptors, but VBDOS LINK writes
	 * cSeg of them and cSeg is the larger of the two on both fixtures (72 vs
	 * 70, 282 vs 280). Taking the smaller drops the last descriptors and any
	 * symbol in them resolves nowhere. */
	const uint16_t count = std::max(body.u16(0),body.u16(2));
	for (uint16_t i = 0;i < count;i++) {
		const size_t at = 4 + (size_t)i * 20;
		if (at + 20 > body.size()) break;
		CvSegMapEntry entry;
		entry.index = (uint16_t)(i + 1);
		entry.flags = body.u16(at);
		entry.ovl = body.u16(at + 2);
		entry.group = body.u16(at + 4);
		entry.frame = body.u16(at + 6);
		entry.segNameIndex = body.u16(at + 8);
		entry.classNameIndex = body.u16(at + 10);
		entry.offset = body.u32(at + 12);
		entry.length = body.u32(at + 16);
		entries.push_back(entry);
	}
	return entries;
}

static bool ParseSymbol(const DebugBytes &body,size_t at,uint16_t kindCode,uint16_t moduleIndex,CvSymbol &out)
{
	out.moduleIndex = moduleIndex;

	switch (kindCode) {
	case S_PUB16:
	case S_LDATA16:
	case S_GDATA16:
		if (at + 6 > body.size()) return false;
		out.offset = body.u16(at);
		out.segment = body.u16(at + 2);
		out.type = body.u16(at + 4);
		out.name = body.pstr(at + 6);
		out.kind = kindCode == S_PUB16 ? CV_SYM_PUBLIC : (kindCode == S_GDATA16 ? CV_SYM_GLOBAL_DATA : CV_SYM_LOCAL_DATA);
		return true;
	case S_PUB32:
	case S_LDATA32:
	case S_GDATA32:
		if (at + 8 > body.size()) return false;
		out.offset = body.u32(at);
		out.segment = body.u16(at + 4);
		out.type = body.u16(at + 6);
		out.name = body.pstr(at + 8);
		out.kind = kindCode == S_PUB32 ? CV_SYM_PUBLIC : (kindCode == S_GDATA32 ? CV_SYM_GLOBAL_DATA : CV_SYM_LOCAL_DATA);
		return true;
	case S_LPROC16:
	case S_GPROC16:
		/* pParent/pEnd/pNext u32 each, then procLength, debugStart, debugEnd,
		 * offset, segment, procType, flags u8, name. */
		if (at + 25 > body.size()) return false;
		out.size = body.u16(at + 12);
		out.hasSize = true;
		out.offset = body.u16(at + 18);
		out.segment = body.u16(at + 20);
		out.type = body.u16(at + 22);
		out.name = body.pstr(at + 25);
		out.kind = CV_SYM_PROC;
		return true;
	case S_LABEL16:
		if (at + 5 > body.size()) return false;
		out.offset = body.u16(at);
		out.segment = body.u16(at + 2);
		out.name = body.pstr(at + 5);
		out.kind = CV_SYM_LABEL;
		return true;
	default:
		return false;
	}
}

/* sstGlobalPub / sstGlobalSym: a 16-byte hash header, then the symbol area.
 *
 * cbSymbol bounds the symbols; what follows is the name and address hash
 * tables, which are not records. Both fixtures happen to yield the same
 * symbols without the bound -- the extra records decode to kind 0 and are
 * dropped -- so this is right by the format, not by measurement. */
static std::vector<CvSymbol> ParseGlobalSymbols(const DebugBytes &body,uint16_t moduleIndex)
{
	if (body.size() < 16) return std::vector<CvSymbol>();
	const uint32_t cbSymbol = body.u32(4);
	const size_t end = (size_t)std::min((uint64_t)16u + cbSymbol,(uint64_t)body.size());
	return DEBUG_ParseCvSymbolRun(body,moduleIndex,16,end);
}

static std::vector<CvLineTable> ParseSrcModule(const DebugBytes &body,uint16_t moduleIndex)
{
	std::vector<CvLineTable> tables;
	if (body.size() < 4) return tables;

	const uint16_t cFile = body.u16(0);
	for (uint16_t f = 0;f < cFile;f++) {
		const size_t ptrAt = 4 + (size_t)f * 4;
		if (ptrAt + 4 > body.size()) break;
		const uint64_t fileAt = body.u32(ptrAt);
		if (fileAt + 4 > body.size()) continue;

		const uint16_t fileSegs = body.u16((size_t)fileAt);
		const uint64_t nameAt = fileAt + 4u + (uint64_t)fileSegs * 4u + (uint64_t)fileSegs * 8u;
		const std::string file = nameAt < body.size() ? body.pstr((size_t)nameAt) : std::string();

		for (uint16_t s = 0;s < fileSegs;s++) {
			const uint64_t lnPtrAt = fileAt + 4u + (uint64_t)s * 4u;
			if (lnPtrAt + 4 > body.size()) break;
			const uint64_t lnAt = body.u32((size_t)lnPtrAt);
			if (lnAt + 4 > body.size()) continue;

			CvLineTable table;
			table.moduleIndex = moduleIndex;
			table.file = file;
			table.segment = body.u16((size_t)lnAt);

			const uint16_t cPair = body.u16((size_t)lnAt + 2);
			const uint64_t offsetsAt = lnAt + 4u;
			const uint64_t linesAt = offsetsAt + (uint64_t)cPair * 4u;
			if (linesAt + (uint64_t)cPair * 2u > body.size()) continue;

			for (uint16_t p = 0;p < cPair;p++) {
				CvLine line;
				line.offset = body.u32((size_t)(offsetsAt + (uint64_t)p * 4u));
				line.line = body.u16((size_t)(linesAt + (uint64_t)p * 2u));
				table.lines.push_back(line);
			}
			tables.push_back(table);
		}
	}
	return tables;
}

bool DEBUG_IsCvSignature(const std::string &text)
{
	for (size_t i = 0;i < sizeof(CV_SIGNATURES)/sizeof(CV_SIGNATURES[0]);i++)
		if (text == CV_SIGNATURES[i]) return true;
	return false;
}

/*
 * The trailer's u32 is a distance measured backwards from the END of the
 * file, not an absolute offset -- so a byte appended after the trailer would
 * break it. The MZ image end is a second candidate for that case, and it is
 * only approximately where the block starts: qrender-cv.exe's lands exactly
 * there, cvprobe.exe's two bytes later, so it is a fallback and not the
 * primary.
 */
bool DEBUG_FindCvBase(const DebugBytes &data,uint64_t &base,std::string &signature)
{
	std::vector<int64_t> candidates;

	if (data.size() >= 8) {
		const std::string trailer = data.latin1(data.size() - 8,data.size() - 4);
		if (DEBUG_IsCvSignature(trailer))
			candidates.push_back((int64_t)data.size() - (int64_t)data.u32(data.size() - 4));
	}

	MzImage image;
	if (DEBUG_ParseMzImage(data,image) && image.appendedOffset < data.size())
		candidates.push_back((int64_t)image.appendedOffset);

	for (size_t i = 0;i < candidates.size();i++) {
		std::string found;
		if (ReadCvSignature(data,candidates[i],found)) {
			base = (uint64_t)candidates[i];
			signature = found;
			return true;
		}
	}
	return false;
}

/*
 * sstSymbols/sstAlignSym/sstStaticSym open with a 4-byte version signature
 * (1 for CV4) that is not a record. It is recognised rather than assumed per
 * subsection: a record length below 2 cannot be one, since the length always
 * covers at least the kind. Reading past it costs the whole subsection --
 * cvprobe.exe's two sstAlignSym blocks yielded 0 symbols before this.
 */
std::vector<CvSymbol> DEBUG_ParseCvSymbolRun(const DebugBytes &body,uint16_t moduleIndex,size_t from,size_t to,
                                             std::vector<CvScope> *scopes)
{
	std::vector<CvSymbol> out;
	if (to > body.size()) to = body.size();

	size_t at = from;
	if (at + 4 <= to && body.u16(at) < 2) at += 4;

	/* A proc's locals sit between it and its S_ENDBLK. Blocks inside it are
	 * only counted, not opened: their locals join the proc's frame, which is
	 * wider than the block but never points somewhere else. */
	CvScope open;
	unsigned int depth = 0;

	while (at + 4 <= to) {
		const uint16_t length = body.u16(at);
		if (length < 2) break;
		const uint16_t kindCode = body.u16(at + 2);
		const size_t data = at + 4;

		CvSymbol symbol;
		if (ParseSymbol(body,data,kindCode,moduleIndex,symbol) && !symbol.name.empty())
			out.push_back(symbol);

		if (scopes != NULL) {
			CvLocal local;
			switch (kindCode) {
			case S_LPROC16:
			case S_GPROC16:
				if (depth == 0 && symbol.kind == CV_SYM_PROC) {
					open = CvScope();
					open.moduleIndex = moduleIndex;
					open.segment = symbol.segment;
					open.offset = symbol.offset;
					open.length = symbol.size;
					open.function = symbol.name;
				}
				depth++;
				break;
			case S_BLOCK16:
				if (depth > 0) depth++;
				break;
			case S_ENDBLK:
				if (depth > 0 && --depth == 0 && !open.function.empty()) scopes->push_back(open);
				break;
			case S_BPREL16:
				local.frameOffset = (int32_t)(int16_t)body.u16(data);
				local.type = body.u16(data + 2);
				local.name = body.pstr(data + 4);
				if (depth > 0 && !local.name.empty()) open.locals.push_back(local);
				break;
			case S_BPREL32:
				local.frameOffset = (int32_t)body.u32(data);
				local.type = body.u16(data + 4);
				local.name = body.pstr(data + 6);
				if (depth > 0 && !local.name.empty()) open.locals.push_back(local);
				break;
			case S_REGISTER:
				local.storage = CV_LOCAL_REGISTER;
				local.type = body.u16(data);
				local.reg = body.u16(data + 2);
				local.name = body.pstr(data + 4);
				if (depth > 0 && !local.name.empty()) open.locals.push_back(local);
				break;
			default:
				break;
			}
		}
		at += 2u + length;
	}
	return out;
}

/*
 * CV3 (NB00-NB02), in the layouts of Open Watcom's hll.h (cv3_*), measured
 * against BC 4.5 + LINK 3.69. A segment here is a paragraph of the load
 * image, not an index into a segment map, and records are relative to their
 * module's code segment. The reader builds the segment map CV3 lacks, so the
 * rest of the pipeline addresses CV3 and CV4 alike.
 */
enum {
	sst3Modules = 0x101,
	sst3Publics = 0x102,
	sst3Symbols = 0x104,
	sst3SrcLines = 0x105,
	sst3SrcLnSeg = 0x109
};

enum {
	S3_BLOCK = 0x00,
	S3_PROC = 0x01,
	S3_END = 0x02,
	S3_BPREL = 0x04,
	S3_STATIC = 0x05,
	S3_LABEL = 0x0B,
	S3_REGISTER = 0x0D,
	S3_CHANGESEG = 0x11
};

/* SegInfo {seg, offset, cb}, ovl, iLib, cSeg, reserved, name, then the
 * other cSeg-1 SegInfos. LINK 3.69 writes cSeg 0 with SegInfo filled in. */
static bool ParseModule3(const DebugBytes &body,uint16_t index,CvModule &out)
{
	if (body.size() < 13) return false;
	out.index = index;
	CvSegInfo first;
	first.segment = body.u16(0);
	first.offset = body.u16(2);
	first.length = body.u16(4);
	if (first.length != 0) out.segments.push_back(first);
	const uint8_t cSeg = body.u8(10);
	size_t at = 0;
	out.name = body.pstr(12,&at);
	for (uint8_t i = 1;i < cSeg && at + 6 <= body.size();i++,at += 6) {
		CvSegInfo more;
		more.segment = body.u16(at);
		more.offset = body.u16(at + 2);
		more.length = body.u16(at + 4);
		out.segments.push_back(more);
	}
	return true;
}

/* LINK 3.69 gives an absolute public type 1 -- every row its .MAP marks
 * Abs, and nothing else (CV3's own types start at 0x80). Its value is a
 * constant, not an address: kept, B$LENDRW named BC's main module code. */
static const uint16_t CV3_ABSOLUTE_PUBLIC = 0x0001;

static void ParsePublics3(const DebugBytes &body,uint16_t moduleIndex,std::vector<CvSymbol> &out)
{
	for (size_t at = 0;at + 7 <= body.size();) {
		CvSymbol symbol;
		symbol.offset = body.u16(at);
		symbol.segment = body.u16(at + 2);
		const uint16_t type = body.u16(at + 4);
		symbol.kind = CV_SYM_PUBLIC;
		symbol.moduleIndex = moduleIndex;
		symbol.name = body.pstr(at + 6,&at);
		if (!symbol.name.empty() && type != CV3_ABSOLUTE_PUBLIC) out.push_back(symbol);
	}
}

/* Records are {length, code, fields}, the length counting from the code.
 * A proc's locals sit between it and its end; blocks around and inside it
 * are only counted, as in the CV4 reader. */
static void ParseSymbols3(const DebugBytes &body,uint16_t moduleIndex,uint16_t codeSegment,CvInfo &out)
{
	uint16_t segment = codeSegment;
	std::vector<bool> nesting;	/* true for a proc */
	CvScope open;
	bool inProc = false;

	for (size_t at = 0;at + 2 <= body.size();) {
		const uint8_t length = body.u8(at);
		if (length == 0) break;
		const uint8_t code = body.u8(at + 1);
		const size_t data = at + 2;
		CvSymbol symbol;
		symbol.moduleIndex = moduleIndex;
		CvLocal local;

		switch (code) {
		case S3_BLOCK:
			nesting.push_back(false);
			break;
		case S3_PROC:
			symbol.kind = CV_SYM_PROC;
			symbol.segment = segment;
			symbol.offset = body.u16(data);
			symbol.size = body.u16(data + 4);
			symbol.hasSize = true;
			symbol.name = body.pstr(data + 13);
			out.symbols.push_back(symbol);
			if (!inProc) {
				open = CvScope();
				open.moduleIndex = moduleIndex;
				open.segment = segment;
				open.offset = symbol.offset;
				open.length = symbol.size;
				open.function = symbol.name;
				inProc = true;
			}
			nesting.push_back(true);
			break;
		case S3_END:
			if (nesting.empty()) break;
			if (nesting.back() && inProc &&
			    std::find(nesting.begin(),nesting.end() - 1,true) == nesting.end() - 1) {
				out.scopes.push_back(open);
				inProc = false;
			}
			nesting.pop_back();
			break;
		case S3_BPREL:
			local.frameOffset = (int32_t)(int16_t)body.u16(data);
			local.name = body.pstr(data + 4);
			if (inProc && !local.name.empty()) open.locals.push_back(local);
			break;
		case S3_REGISTER:
			local.storage = CV_LOCAL_REGISTER;
			local.reg = body.u8(data + 2);
			local.name = body.pstr(data + 3);
			if (inProc && !local.name.empty()) open.locals.push_back(local);
			break;
		case S3_STATIC:
			symbol.kind = inProc ? CV_SYM_LOCAL_DATA : CV_SYM_GLOBAL_DATA;
			symbol.offset = body.u16(data);
			symbol.segment = body.u16(data + 2);
			symbol.name = body.pstr(data + 6);
			if (!symbol.name.empty()) out.symbols.push_back(symbol);
			break;
		case S3_LABEL:
			symbol.kind = CV_SYM_LABEL;
			symbol.segment = segment;
			symbol.offset = body.u16(data);
			symbol.name = body.pstr(data + 3);
			if (!symbol.name.empty()) out.symbols.push_back(symbol);
			break;
		case S3_CHANGESEG:
			segment = body.u16(data);
			break;
		default:
			break;
		}
		at += 1u + length;
	}
}

/* sstSrcLines: {file, count, {line, offset}...} repeated, in the module's
 * code segment. sstSrcLnSeg names the segment after the file. */
static void ParseLines3(const DebugBytes &body,uint16_t moduleIndex,uint16_t codeSegment,bool withSegment,
                        std::vector<CvLineTable> &out)
{
	for (size_t at = 0;at < body.size();) {
		CvLineTable table;
		table.moduleIndex = moduleIndex;
		table.file = body.pstr(at,&at);
		table.segment = codeSegment;
		if (withSegment) {
			table.segment = body.u16(at);
			at += 2;
		}
		const uint16_t count = body.u16(at);
		at += 2;
		if (at + (size_t)count * 4u > body.size()) break;
		for (uint16_t i = 0;i < count;i++,at += 4) {
			CvLine line;
			line.line = body.u16(at);
			line.offset = body.u16(at + 2);
			table.lines.push_back(line);
		}
		if (!table.lines.empty()) out.push_back(table);
	}
}

static void ParseCodeView3(const DebugBytes &data,uint64_t base,CvInfo &out)
{
	std::map<uint16_t,uint16_t> codeSegment;	/* module -> paragraph */
	std::set<uint16_t> paragraphs;
	std::vector<std::pair<const CvDirEntry *,DebugBytes> > rest;

	for (size_t i = 0;i < out.directory.size();i++) {
		const CvDirEntry &entry = out.directory[i];
		const uint64_t at = base + entry.offset;
		if (at + entry.length > data.size()) {
			out.warnings.push_back(SymFormat("subsection %x runs past end of file",(unsigned int)entry.subsection));
			continue;
		}
		const DebugBytes body = data.sub((size_t)at,entry.length);
		CvModule module;
		if (entry.subsection == sst3Modules && ParseModule3(body,entry.moduleIndex,module)) {
			for (size_t s = 0;s < module.segments.size();s++) {
				if (s == 0) codeSegment[module.index] = module.segments[s].segment;
				paragraphs.insert(module.segments[s].segment);
			}
			out.modules.push_back(module);
		} else {
			rest.push_back(std::make_pair(&entry,body));
		}
	}

	for (size_t i = 0;i < rest.size();i++) {
		const CvDirEntry &entry = *rest[i].first;
		const DebugBytes &body = rest[i].second;
		const uint16_t code = codeSegment[entry.moduleIndex];
		switch (entry.subsection) {
		case sst3Publics: ParsePublics3(body,entry.moduleIndex,out.symbols); break;
		case sst3Symbols: ParseSymbols3(body,entry.moduleIndex,code,out); break;
		case sst3SrcLines: ParseLines3(body,entry.moduleIndex,code,false,out.lines); break;
		case sst3SrcLnSeg: ParseLines3(body,entry.moduleIndex,code,true,out.lines); break;
		default: break;
		}
	}

	/* The segment map CV3 lacks, for addressing only: every paragraph
	 * anything is addressed in. No extents, so no layout: a module lists one
	 * segment, and rtinit.asm's code in INIT_CODE went unlisted, so what CV3
	 * calls code is not all the code. */
	for (size_t i = 0;i < out.symbols.size();i++) paragraphs.insert(out.symbols[i].segment);
	for (size_t i = 0;i < out.lines.size();i++) paragraphs.insert(out.lines[i].segment);
	for (std::set<uint16_t>::const_iterator it = paragraphs.begin();it != paragraphs.end();++it) {
		CvSegMapEntry entry;
		entry.index = *it;
		entry.frame = *it;
		out.segments.push_back(entry);
	}
	out.warnings.push_back(out.signature + " types are not read");
}

bool DEBUG_ParseCodeView(const DebugBytes &data,CvInfo &out)
{
	uint64_t base = 0;
	std::string signature;
	if (!DEBUG_FindCvBase(data,base,signature)) return false;

	out.signature = signature;
	out.base = base;

	const bool isCv3 = signature == "NB00" || signature == "NB01" || signature == "NB02";
	out.directory = ReadDirectory(data,base,isCv3,out.warnings);
	if (isCv3) ParseCodeView3(data,base,out);

	for (size_t i = 0;i < out.directory.size() && !isCv3;i++) {
		const CvDirEntry &entry = out.directory[i];
		const uint64_t at = base + entry.offset;
		if (at + entry.length > data.size()) {
			out.warnings.push_back(SymFormat("subsection %x runs past end of file",(unsigned int)entry.subsection));
			continue;
		}
		const DebugBytes body = data.sub((size_t)at,entry.length);

		switch (entry.subsection) {
		case sstModule: {
			CvModule module;
			if (ParseModule(body,entry.moduleIndex,module)) out.modules.push_back(module);
			break;
		}
		case sstPublicSym:
		case sstSymbols:
		case sstAlignSym:
		case sstStaticSym: {
			const std::vector<CvSymbol> run =
				DEBUG_ParseCvSymbolRun(body,entry.moduleIndex,0,body.size(),&out.scopes);
			out.symbols.insert(out.symbols.end(),run.begin(),run.end());
			break;
		}
		case sstGlobalPub:
		case sstGlobalSym: {
			const std::vector<CvSymbol> run = ParseGlobalSymbols(body,entry.moduleIndex);
			out.symbols.insert(out.symbols.end(),run.begin(),run.end());
			break;
		}
		case sstSegMap:
			out.segments = ParseSegMap(body);
			break;
		case sstTypes:
			out.moduleTypes[entry.moduleIndex] = ParseModuleTypes(body);
			break;
		case sstGlobalTypes:
			out.types = ParseGlobalTypes(body);
			break;
		case sstSrcModule: {
			const std::vector<CvLineTable> tables = ParseSrcModule(body,entry.moduleIndex);
			out.lines.insert(out.lines.end(),tables.begin(),tables.end());
			break;
		}
		default:
			break;
		}
	}

	std::map<uint16_t,std::string> moduleNames;
	for (size_t i = 0;i < out.modules.size();i++)
		moduleNames[out.modules[i].index] = out.modules[i].name;

	for (size_t i = 0;i < out.symbols.size();i++) {
		const std::map<uint16_t,std::string>::const_iterator it = moduleNames.find(out.symbols[i].moduleIndex);
		if (it != moduleNames.end()) out.symbols[i].module = it->second;
	}
	for (size_t i = 0;i < out.lines.size();i++) {
		const std::map<uint16_t,std::string>::const_iterator it = moduleNames.find(out.lines[i].moduleIndex);
		if (it != moduleNames.end()) out.lines[i].module = it->second;
	}
	return true;
}

bool DEBUG_ReadCodeViewFile(const char *path,CvInfo &out)
{
	std::vector<uint8_t> data;
	if (!DEBUG_ReadHostFile(path,data)) return false;
	return DEBUG_ParseCodeView(DebugBytes(data.data(),data.size()),out);
}

bool DEBUG_ParseBasicArrayDescriptor(const DebugBytes &data,uint32_t expectedElementSize,
                                     BasicArrayDescriptor &out)
{
	out.offset = data.u16(0);
	out.segment = data.u16(2);
	out.elementSize = data.u16(12);
	out.count = data.u16(14);

	if (out.elementSize == 0 || out.count == 0) return false;
	if (expectedElementSize != 0 && out.elementSize != expectedElementSize) return false;
	return true;
}
