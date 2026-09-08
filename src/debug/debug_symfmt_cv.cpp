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

static const char * const CV_SIGNATURES[] = {
	"NB00","NB01","NB02","NB03","NB04","NB05","NB06","NB07","NB08","NB09","NB10","NB11"
};

/* CV4 (NB05/NB08/NB09) and CV5 (NB11) subsection kinds. */
enum {
	sstModule = 0x120,
	sstPublicSym = 0x123,
	sstSymbols = 0x124,
	sstAlignSym = 0x125,
	sstSrcModule = 0x127,
	sstGlobalSym = 0x129,
	sstGlobalPub = 0x12a,
	sstSegMap = 0x12d,
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

	/* CV3 has no directory header: a bare u16 count followed by 12-byte
	 * entries whose length field is 16-bit. */
	if (isCv3) {
		const uint16_t count = data.u16((size_t)at);
		for (uint16_t i = 0;i < count;i++) {
			const uint64_t off = at + 2u + (uint64_t)i * 12u;
			if (off + 12u > data.size()) break;
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
std::vector<CvSymbol> DEBUG_ParseCvSymbolRun(const DebugBytes &body,uint16_t moduleIndex,size_t from,size_t to)
{
	std::vector<CvSymbol> out;
	if (to > body.size()) to = body.size();

	size_t at = from;
	if (at + 4 <= to && body.u16(at) < 2) at += 4;

	while (at + 4 <= to) {
		const uint16_t length = body.u16(at);
		if (length < 2) break;
		const uint16_t kindCode = body.u16(at + 2);
		CvSymbol symbol;
		if (ParseSymbol(body,at + 4,kindCode,moduleIndex,symbol) && !symbol.name.empty())
			out.push_back(symbol);
		at += 2u + length;
	}
	return out;
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
	if (isCv3) {
		/* The CV3 subsection numbers are known; their record layouts are not,
		 * and no CV3 binary was available to measure against. Say so rather
		 * than reporting whatever the CV4 readers make of the bytes. */
		out.warnings.push_back(signature + " is a pre-CV4 layout; only the subsection directory is read");
		return true;
	}

	for (size_t i = 0;i < out.directory.size();i++) {
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
			const std::vector<CvSymbol> run = DEBUG_ParseCvSymbolRun(body,entry.moduleIndex,0,body.size());
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
