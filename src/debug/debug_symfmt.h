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

#include <string>
#include <vector>

namespace dbgsym {

/* Reads past the end return 0 rather than trapping: this parses files the
 * emulator did not write, and a truncated record must degrade to a warning. */
class Bytes {
public:
	Bytes() : p(nullptr), n(0) {}
	Bytes(const uint8_t *data,size_t len) : p(data), n(len) {}

	size_t size() const { return n; }
	const uint8_t *data() const { return p; }

	uint8_t u8(size_t at) const { return at < n ? p[at] : (uint8_t)0; }
	uint16_t u16(size_t at) const;
	uint32_t u32(size_t at) const;

	std::string latin1(size_t from,size_t to) const;
	/* Length-prefixed name. next, when given, receives the offset one past
	 * the record as the format counts it, which can exceed size(). */
	std::string pstr(size_t at,size_t *next = nullptr) const;
	Bytes sub(size_t from,size_t len) const;

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

bool ParseMzHeader(const Bytes &data,MzHeader &out);
bool ParseMzImage(const Bytes &data,MzImage &out);

/* The header fields DOS EXEC reports back through loadInfo, joined. Matching
 * a host file to the running program by name alone picks the wrong one as
 * soon as two build directories hold the same basename. */
std::string MzFingerprint(const MzHeader &header);

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

bool IsCvSignature(const std::string &text);

/* Where the debug block starts, or false if the file carries none. */
bool FindCvBase(const Bytes &data,uint64_t &base,std::string &signature);

bool ParseCodeView(const Bytes &data,CvInfo &out);

/* A run of [length:u16][kind:u16][data] records. Exposed for the tests. */
std::vector<CvSymbol> ParseCvSymbolRun(const Bytes &body,uint16_t moduleIndex,size_t from,size_t to);

/* ---- host file helpers ---- */

bool ReadHostFile(const char *path,std::vector<uint8_t> &out);
bool ReadCodeViewFile(const char *path,CvInfo &out);

}

#endif
