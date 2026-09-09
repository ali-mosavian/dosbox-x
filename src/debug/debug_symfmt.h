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

/* Where appended debug info would start, from the header alone: a caller that
 * has read only the header can tell whether the file carries any. */
uint64_t DEBUG_MzAppendedOffset(const MzHeader &header);

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

/* A proc's frame: what CodeView records between the proc and its S_ENDBLK. */
enum CvLocalStorage {
	CV_LOCAL_FRAME,
	CV_LOCAL_REGISTER
};

struct CvLocal {
	std::string name;
	CvLocalStorage storage = CV_LOCAL_FRAME;
	int32_t frameOffset = 0;	/* BP-relative */
	uint16_t reg = 0;		/* CodeView register number */
	uint16_t type = 0;
};

struct CvScope {
	uint16_t moduleIndex = 0;
	uint16_t segment = 0;
	uint32_t offset = 0;
	uint32_t length = 0;
	std::string function;
	std::vector<CvLocal> locals;
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

/*
 * One record of sstGlobalTypes, addressed from a symbol as 0x1000 + index.
 * Anything below 0x1000 is a primitive encoded in the index itself.
 *
 * Only what a debugger needs to read a variable is kept: what it is, how wide,
 * and what it is made of. LF_BARRAY is a BASIC array, which carries no element
 * count because the count lives in the runtime descriptor the symbol points at.
 */
struct CvMember {
	std::string name;
	uint16_t type = 0;
	uint32_t offset = 0;
};

struct CvType {
	uint16_t leaf = 0;
	uint16_t utype = 0;	/* element, pointed-to or return type */
	uint32_t size = 0;	/* bytes, for arrays and structures */
	uint16_t fieldList = 0;	/* LF_FIELDLIST holding the members, for a structure */
	std::string name;
	std::vector<CvMember> members;	/* on the LF_FIELDLIST record itself */
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
	std::vector<CvType> types;	/* index 0x1000 + position */
	/* Before CVPACK runs there is no one global table: each module carries
	 * its own, and every one of them starts over at 0x1000. Keyed by module
	 * index, and empty once the types have been packed. */
	std::map<uint16_t,std::vector<CvType> > moduleTypes;
	std::vector<CvScope> scopes;
	std::vector<std::string> warnings;
};

bool DEBUG_IsCvSignature(const std::string &text);

/* Where the debug block starts, or false if the file carries none. */
bool DEBUG_FindCvBase(const DebugBytes &data,uint64_t &base,std::string &signature);

bool DEBUG_ParseCodeView(const DebugBytes &data,CvInfo &out);

/* A run of [length:u16][kind:u16][data] records. Exposed for the tests.
 * scopes, when given, collects the frames the proc records open. */
std::vector<CvSymbol> DEBUG_ParseCvSymbolRun(const DebugBytes &body,uint16_t moduleIndex,size_t from,size_t to,
                                             std::vector<CvScope> *scopes = NULL);

/* What a BASIC array's symbol addresses: a descriptor the runtime fills in,
 * not the elements. Measured on cvprobe.exe (2-byte elements, 16 of them)
 * and udtbas.exe (an 8-byte TYPE, 4 of them). */
struct BasicArrayDescriptor {
	uint16_t offset = 0;
	uint16_t segment = 0;
	uint16_t elementSize = 0;
	uint16_t count = 0;
};

/* False unless the bytes read as a descriptor whose element width is the one
 * the program's own type table declares -- the guard against following
 * something that is not a descriptor. expectedElementSize 0 skips it. */
bool DEBUG_ParseBasicArrayDescriptor(const DebugBytes &data,uint32_t expectedElementSize,
                                     BasicArrayDescriptor &out);

/* ---- Borland TDINFO ---- */

/*
 * Borland TLink symbolic debug info as it rides in a 16-bit DOS MZ EXE.
 *
 * This is TDINFO, magic 0x52FB -- not the 32-bit "TDS" (FB09/FB0A) that C++
 * Builder writes, which is a different format under a similar name. There is
 * no trailer to search back from: the block starts exactly where the MZ header
 * says the load image ends, and the name pool sits at the very end of the file
 * rather than with the tables it serves.
 *
 * Layouts from ramikg/tdinfo-parser's tdinfo_structs.py, the only complete
 * open-source description of the 16-bit form. It skips the source-file and
 * line-number records as padding, so those two were derived here from
 * tdsprobe.tds instead: a line record is {u16 line, u16 offset}, and every one
 * of the fixture's 17 records lands on a statement line of tdsprobe.c with the
 * offset its .MAP gives that statement's function.
 */

enum TdSymbolClass {
	TD_SYM_STATIC,
	TD_SYM_ABSOLUTE,
	TD_SYM_AUTO,
	TD_SYM_PASCAL_VAR,
	TD_SYM_REGISTER,
	TD_SYM_CONSTANT,
	TD_SYM_TYPEDEF,
	TD_SYM_STRUCT_UNION_ENUM
};

struct TdSymbol {
	std::string name;
	uint16_t segment = 0;
	/* Signed: an AUTO symbol's offset is a BP-relative displacement, not an
	 * address, and read unsigned it reports 65488 where -48 belongs. */
	int32_t offset = 0;
	TdSymbolClass symbolClass = TD_SYM_STATIC;
	uint16_t type = 0;
};

/* A field of a structure or union. Members carry no offset: the fields sit
 * one after another in declaration order, which is where they land under
 * Borland's default byte alignment. info 0xC0 ends a member list. */
struct TdMember {
	std::string name;
	uint16_t type = 0;
	uint8_t info = 0;
};

struct TdModule {
	uint16_t index = 0;
	std::string name;
};

struct TdSourceFile {
	std::string name;
};

/* Offsets are within the code segment of the module the line belongs to, the
 * same space a segment record's codeOffset counts in. */
struct TdLine {
	uint16_t line = 0;
	uint16_t offset = 0;
};

/* A type is reached by 1-based index from a symbol. id is Borland's TypeId:
 * 4 SCHAR, 5 SINT, 6 SLONG, 8 UCHAR, 9 UINT, 10 ULONG, 12 PCHAR, 13 FLOAT,
 * 15 DOUBLE, 26 ARRAY, 30 STRUCT, 31 UNION, 34 ENUM, 35 FUNCTION, 40 BOOL. */
struct TdType {
	uint8_t id = 0;
	std::string name;
	uint16_t size = 0;
	uint8_t classType = 0;
	uint16_t memberType = 0;	/* element type for ARRAY, return type for FUNCTION */
};

struct TdSegment {
	uint16_t module = 0;
	uint16_t codeSegment = 0;
	uint16_t codeOffset = 0;
	uint16_t codeLength = 0;
	uint16_t scopeIndex = 0;	/* 1-based into the scope table; 0 = none */
	uint16_t scopeCount = 0;
};

/* One function or block: which symbols it holds and what code it covers. */
struct TdScope {
	uint16_t symbolIndex = 0;	/* 1-based into the symbol table */
	uint16_t symbolCount = 0;
	uint16_t parent = 0;		/* 1-based; 0 = none */
	uint16_t function = 0;
	uint16_t offset = 0;		/* within the module's code segment */
	uint16_t length = 0;
};

struct TdInfo {
	std::string version;		/* e.g. "TDINFO 3.16" */
	uint64_t base = 0;
	std::vector<TdSymbol> symbols;
	std::vector<TdModule> modules;
	std::vector<TdSegment> segments;
	std::vector<TdScope> scopes;
	std::vector<TdSourceFile> sourceFiles;
	std::vector<TdLine> lines;
	std::vector<TdType> types;
	std::vector<TdMember> members;
	std::vector<std::string> warnings;
};

/* Where the block starts: the MZ image end, or offset 0.
 *
 * TDSTRIP writes the block out byte-identical to what it removed, so a
 * standalone .TDS is the same bytes with no MZ header in front -- measured on
 * tdsprobe.tds, which matches tdsprobe.exe's appended 2545 bytes exactly. */
bool DEBUG_FindTdInfoBase(const DebugBytes &data,uint64_t &base);
bool DEBUG_ParseBorland(const DebugBytes &data,TdInfo &out);

/* ---- Watcom ---- */

/*
 * Watcom ("WAT") debug info, as wlink appends it to a DOS EXE.
 *
 * Found from the END of the file, not from the MZ image end: the master header
 * is the last 14 bytes, and other Watcom trailers (FOX/resource) may be stacked
 * on top of it, each skipped by its own size. The MZ header's declared length
 * deliberately excludes the block.
 *
 * Layouts from OpenWatcom's bld/watcom/h/wdbginfo.h and bld/dip/watcom/c/
 * watldsym.c/watgbl.c. UNVERIFIED against a real Watcom binary -- there is no
 * OpenWatcom toolchain in this tree to produce one -- so this reads the format
 * as documented and reports what it finds rather than being proven against
 * known addresses the way the CodeView reader is.
 */

struct WatSymbol {
	std::string name;
	uint16_t segment = 0;
	uint32_t offset = 0;
	int32_t moduleIndex = -1;
	uint8_t kind = 0;
};

struct WatModule {
	int32_t index = 0;
	std::string name;
};

struct WatInfo {
	std::string version;		/* e.g. "WAT 3.0" */
	uint64_t base = 0;
	std::vector<WatSymbol> symbols;
	std::vector<WatModule> modules;
	std::vector<std::string> warnings;
};

bool DEBUG_ParseWatcom(const DebugBytes &data,WatInfo &out);

/* ---- Microsoft LINK .MAP ---- */

struct LinkMapSegment {
	uint32_t start = 0;
	uint32_t stop = 0;
	uint32_t length = 0;
	std::string name;
	std::string className;
};

struct LinkMapAddress {
	uint16_t segment = 0;
	uint32_t offset = 0;
	/* (segment << 4) + offset: load-relative, the space the whole map uses. */
	uint32_t mapOffset = 0;
};

struct LinkMapGroup {
	std::string name;
	LinkMapAddress address;
};

struct LinkMapPublic {
	std::string name;
	LinkMapAddress address;
};

struct LinkMapFile {
	std::string sourceName;
	std::vector<LinkMapSegment> segments;
	std::vector<LinkMapGroup> groups;
	/* Keyed by the name as written and by its upper-case form, first wins. */
	std::map<std::string,LinkMapPublic> publics;
	bool hasEntryPoint = false;
	LinkMapAddress entryPoint;
};

struct LinkMapResolution {
	std::string requested;
	std::string name;
	uint32_t linear = 0;
	uint32_t offset = 0;
	uint32_t mapOffset = 0;
	std::string explanation;
};

void DEBUG_ParseLinkMap(const std::string &text,const char *sourceName,LinkMapFile &out);
bool DEBUG_ReadLinkMapFile(const char *path,LinkMapFile &out);

/* Accepts a public, a segment name, "SEG+off"/"SEG:off", a raw "seg:off" pair,
 * or "entry". Returns false with a reason in error. */
bool DEBUG_ResolveLinkMapSymbol(const LinkMapFile &map,const std::string &requested,uint32_t loadLinear,
                                LinkMapResolution &out,std::string &error);

/* The public at exactly this address, else "SEGMENT+0x...", else false. */
bool DEBUG_DescribeMapAddress(const LinkMapFile &map,uint32_t loadLinear,uint32_t linear,std::string &out);

/* ---- format-independent layer ---- */

enum DebugFormatId {
	DEBUG_FORMAT_CODEVIEW,
	DEBUG_FORMAT_TDINFO,
	DEBUG_FORMAT_WATCOM,
	DEBUG_FORMAT_MAP
};

struct DebugModule {
	std::string name;
	uint16_t index = 0;
};

enum DebugValueKind {
	DEBUG_VALUE_UNKNOWN,
	DEBUG_VALUE_SIGNED,
	DEBUG_VALUE_UNSIGNED,
	DEBUG_VALUE_FLOAT
};

/* One field of a structure, placed within it. */
struct DebugField {
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
	std::string typeName;
	DebugValueKind kind = DEBUG_VALUE_UNKNOWN;
	uint32_t elementSize = 0;	/* non-zero when the field is itself an array */
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
	std::string program;		/* the loaded program these came with */
	/* Printable type, when the format says one: "int", "int[8]", "function".
	 * Empty means the format carried no type this reader understands. */
	std::string typeName;
	/* Bytes to read to see the whole variable, 0 when unknown. A function
	 * carries its code length in size instead. */
	uint32_t valueSize = 0;
	/* How to read those bytes, and for an array the width of one element --
	 * 0 for anything that is not one. */
	DebugValueKind valueKind = DEBUG_VALUE_UNKNOWN;
	uint32_t elementSize = 0;
	/* Set when the type is a structure, or an array of one: elementSize is
	 * then the stride between elements. */
	std::vector<DebugField> fields;
	bool isFunction = false;
	/* A BASIC array: the symbol addresses a runtime descriptor, and the
	 * elements are wherever that points. elementSize is one element. */
	bool isBasicArray = false;
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

/* Where a local lives while its function runs. */
enum DebugStorage {
	DEBUG_STORAGE_FRAME,		/* at a displacement from BP */
	DEBUG_STORAGE_REGISTER
};

struct DebugLocal {
	std::string name;
	DebugStorage storage = DEBUG_STORAGE_FRAME;
	int32_t frameOffset = 0;	/* BP-relative; negative for a local, positive for a parameter */
	/* 0..7 = AX,CX,DX,BX,SP,BP,SI,DI, x86's own encoding order. Confirmed
	 * against tdsprobe.exe's code: total(1) accumulates in CX, i(2) counts
	 * in DX, r(3) lands in BX, s(6) in SI. Anything above 7 is left
	 * undecoded rather than guessed at. */
	uint16_t reg = 0;
	std::string typeName;
	uint32_t valueSize = 0;
	DebugValueKind valueKind = DEBUG_VALUE_UNKNOWN;
	uint32_t elementSize = 0;
	std::vector<DebugField> fields;
	bool isBasicArray = false;
};

/* A function body or a block inside one, and the variables it holds. */
struct DebugScope {
	uint32_t imageOffset = 0;	/* load-relative, like DebugLine */
	uint32_t endOffset = 0;
	int32_t parent = -1;		/* index into DebugInfo::scopes; -1 = none */
	std::string function;		/* the symbol the scope starts at, when there is one */
	std::vector<DebugLocal> locals;
};

struct DebugInfo {
	std::string file;
	DebugFormatId format = DEBUG_FORMAT_CODEVIEW;
	std::string version;		/* the format's own marker, e.g. "NB08" */
	uint32_t loadLinear = 0;
	std::vector<DebugModule> modules;
	std::vector<DebugSymbol> symbols;
	std::vector<DebugLine> lines;
	std::vector<DebugScope> scopes;
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
 * used to guess. A stripped Borland EXE is read through its .TDS sidecar, so
 * the path matters and not only the bytes. */
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
