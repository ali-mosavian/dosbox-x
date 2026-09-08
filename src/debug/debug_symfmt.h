/*
 * debug_symfmt.h - debug info appended to a DOS executable by the linker.
 *
 * Byte-buffer parsers with no emulator dependencies, so they can be unit
 * tested against real linker output (see tests/debug_symfmt_tests.cpp).
 * Ported from the TypeScript readers in mcp/src, which were validated
 * against the .MAP files produced by the same links.
 */

#ifndef DOSBOX_DEBUG_SYMFMT_H
#define DOSBOX_DEBUG_SYMFMT_H

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

/* Reads past the end return 0 rather than trapping: this parses files the
 * emulator did not write, and a truncated record must degrade to a warning. */
class DebugBytes {
public:
	DebugBytes() : p(NULL), n(0) {}
	DebugBytes(const uint8_t *data,size_t len) : p(data), n(len) {}

	size_t size() const { return n; }
	const uint8_t *data() const { return p; }

	uint8_t u8(size_t at) const { return at < n ? p[at] : (uint8_t)0; }
	uint16_t u16(size_t at) const;
	uint32_t u32(size_t at) const;

	std::string latin1(size_t from,size_t to) const;
	/* Length-prefixed name. next, when given, receives the offset one past
	 * the record as the format counts it, which can exceed size(). */
	std::string pstr(size_t at,size_t *next = NULL) const;
	DebugBytes sub(size_t from,size_t len) const;

private:
	const uint8_t *p;
	size_t n;
};

/* ---- MZ ---- */

struct MzHeader {
	uint16_t extraBytes = 0;
	uint16_t pages = 0;
	uint16_t relocationCount = 0;
	uint16_t headerParagraphs = 0;
	uint16_t minAlloc = 0;
	uint16_t maxAlloc = 0;
	uint16_t initSS = 0;
	uint16_t initSP = 0;
	uint16_t checksum = 0;
	uint16_t initIP = 0;
	uint16_t initCS = 0;
	uint16_t relocationTableOffset = 0;
	uint16_t overlay = 0;
};

struct MzImage {
	MzHeader header;
	uint64_t imageOffset = 0;	/* file offset of the load image */
	uint64_t imageSize = 0;		/* load image length, as DOS computes it */
	uint64_t appendedOffset = 0;	/* where appended debug info starts */
	uint64_t fileSize = 0;
};

bool DEBUG_ParseMzHeader(const DebugBytes &data,MzHeader &out);
bool DEBUG_ParseMzImage(const DebugBytes &data,MzImage &out);

/* The header fields DOS EXEC reports back through loadInfo, joined. Matching
 * a host file to the running program by name alone picks the wrong one as
 * soon as two build directories hold the same basename. */
std::string DEBUG_MzFingerprint(const MzHeader &header);

/* ---- CodeView ---- */

enum CvSymbolKind {
	CV_SYM_PUBLIC,
	CV_SYM_GLOBAL_DATA,
	CV_SYM_LOCAL_DATA,
	CV_SYM_PROC,
	CV_SYM_LABEL
};

struct CvSymbol {
	std::string name;
	uint16_t segment = 0;
	uint32_t offset = 0;
	CvSymbolKind kind = CV_SYM_PUBLIC;
	uint32_t size = 0;	/* proc records only */
	bool hasSize = false;
	uint16_t type = 0;
	uint16_t moduleIndex = 0;
	std::string module;
};

struct CvSegInfo {
	uint16_t segment = 0;
	uint32_t offset = 0;
	uint32_t length = 0;
};

struct CvModule {
	uint16_t index = 0;
	std::string name;
	std::string style;
	std::vector<CvSegInfo> segments;
};

struct CvSegMapEntry {
	uint16_t index = 0;
	uint16_t flags = 0;
	uint16_t ovl = 0;
	uint16_t group = 0;
	uint16_t frame = 0;
	uint16_t segNameIndex = 0;
	uint16_t classNameIndex = 0;
	uint32_t offset = 0;
	uint32_t length = 0;
};

struct CvLine {
	uint32_t offset = 0;
	uint16_t line = 0;
};

struct CvLineTable {
	uint16_t moduleIndex = 0;
	std::string module;
	std::string file;
	uint16_t segment = 0;
	std::vector<CvLine> lines;
};

struct CvDirEntry {
	uint16_t subsection = 0;
	uint16_t moduleIndex = 0;
	uint32_t offset = 0;
	uint32_t length = 0;
};

struct CvInfo {
	std::string signature;
	uint64_t base = 0;	/* file offset of the signature starting the block */
	std::vector<CvDirEntry> directory;
	std::vector<CvModule> modules;
	std::vector<CvSymbol> symbols;
	std::vector<CvSegMapEntry> segments;
	std::vector<CvLineTable> lines;
	std::vector<std::string> warnings;
};

bool DEBUG_IsCvSignature(const std::string &text);

/* Where the debug block starts, or false if the file carries none. */
bool DEBUG_FindCvBase(const DebugBytes &data,uint64_t &base,std::string &signature);

bool DEBUG_ParseCodeView(const DebugBytes &data,CvInfo &out);

/* A run of [length:u16][kind:u16][data] records. Exposed for the tests. */
std::vector<CvSymbol> DEBUG_ParseCvSymbolRun(const DebugBytes &body,uint16_t moduleIndex,size_t from,size_t to);

/* ---- format-independent layer ---- */

enum DebugFormatId {
	DEBUG_FORMAT_CODEVIEW,
	DEBUG_FORMAT_TDINFO,
	DEBUG_FORMAT_WATCOM
};

struct DebugModule {
	std::string name;
	uint16_t index = 0;
};

struct DebugSymbol {
	std::string name;
	uint32_t linear = 0;
	uint32_t offset = 0;		/* within its segment */
	uint16_t segment = 0;		/* CodeView logical index; a paragraph elsewhere */
	uint32_t segmentBase = 0;	/* load-relative byte base of that segment */
	uint32_t size = 0;
	bool hasSize = false;
	std::string module;
	DebugFormatId source = DEBUG_FORMAT_CODEVIEW;
};

struct DebugLine {
	std::string module;
	std::string file;
	uint16_t line = 0;
	uint32_t imageOffset = 0;	/* load-relative, the space a LINK .MAP uses */
	/* One past the last byte this line covers. Without it "nearest entry at
	 * or before the address" reaches across segments: at CVPROBE's entry it
	 * answered ..\rt\strdsp1.c:40, a line from another module entirely. */
	uint32_t endOffset = 0;
};

struct DebugInfo {
	std::string file;
	DebugFormatId format = DEBUG_FORMAT_CODEVIEW;
	std::string version;		/* the format's own marker, e.g. "NB08" */
	uint32_t loadLinear = 0;
	std::vector<DebugModule> modules;
	std::vector<DebugSymbol> symbols;
	std::vector<DebugLine> lines;
	std::vector<std::string> warnings;
};

/* Logical segment index -> load-relative BYTE offset.
 *
 * A CodeView symbol names a LINK segment index, not an address, and sstSegMap
 * is the only table that says where each one landed. frame alone is not the
 * answer: segments sharing a group share a frame and are told apart by
 * offset, the byte position within it. frame*16 alone puts 345 of 715 publics
 * in the wrong place on cvprobe.exe; frame*16 + offset puts all 715 exactly
 * where the .MAP from the same link says they are. */
std::map<uint16_t,uint32_t> DEBUG_CvSegmentBases(const CvInfo &info);

/* Detection is by signature; the toolchain that produced the file is never
 * used to guess. TDINFO and Watcom are not ported yet. */
bool DEBUG_ParseDebugInfo(const char *file,uint32_t loadLinear,DebugInfo &out);
bool DEBUG_ParseDebugInfoBytes(const DebugBytes &data,const char *file,uint32_t loadLinear,DebugInfo &out);

/* The source line covering a load-relative offset, if any line covers it. */
const DebugLine *DEBUG_SourceLineAt(const DebugInfo &info,uint32_t imageOffset);

std::string DEBUG_Hex(uint32_t value);
std::string DEBUG_SymbolExplanation(const DebugInfo &info,const DebugSymbol &symbol);

/* ---- host file helpers ---- */

bool DEBUG_ReadHostFile(const char *path,std::vector<uint8_t> &out);
bool DEBUG_ReadCodeViewFile(const char *path,CvInfo &out);

#endif
