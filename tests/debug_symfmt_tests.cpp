/*
 *  Unit tests for the appended-debug-info readers in src/debug/debug_symfmt*.
 *
 *  Every expected number here was produced by the TypeScript readers in
 *  mcp/src, which were checked against the .MAP files LINK wrote for the same
 *  fixtures. They are the port's oracle: a C++ reader that disagrees with one
 *  of them disagrees with the linker.
 */

#include <ctype.h>
#include <stdlib.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/debug/debug_image.h"
#include "../src/debug/debug_symfmt.h"

namespace {

/* The fixtures live in the source tree, and the test binary is the emulator,
 * started from wherever the user happens to be. */
std::string FixtureDir()
{
	const char *env = getenv("DOSBOX_TEST_FIXTURES");
	if (env != NULL && *env != 0) return std::string(env);

	std::string prefix = ".";
	for (int up = 0;up < 6;up++) {
		const std::string dir = prefix + "/mcp/test/fixtures";
		FILE *probe = fopen((dir + "/cvprobe.exe").c_str(),"rb");
		if (probe != NULL) {
			fclose(probe);
			return dir;
		}
		prefix += "/..";
	}
	return std::string();
}

std::string Fixture(const char *name)
{
	const std::string dir = FixtureDir();
	return dir.empty() ? std::string() : dir + "/" + name;
}

bool LoadFixture(const char *name,std::vector<uint8_t> &out)
{
	const std::string path = Fixture(name);
	return !path.empty() && DEBUG_ReadHostFile(path.c_str(),out);
}

size_t CountKind(const CvInfo &info,CvSymbolKind kind)
{
	size_t n = 0;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].kind == kind) n++;
	return n;
}

class DebugSymFmtTest : public ::testing::Test {
protected:
	void SetUp() override {
		if (FixtureDir().empty())
			GTEST_SKIP() << "mcp/test/fixtures not found; set DOSBOX_TEST_FIXTURES";
	}
};

TEST_F(DebugSymFmtTest, MzImagePutsAppendedBlockWhereLinkWroteTheCvSignature)
{
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",data));

	MzImage image;
	const DebugBytes bytes(data.data(),data.size());
	ASSERT_TRUE(DEBUG_ParseMzImage(bytes,image));
	EXPECT_EQ(484868u,image.appendedOffset);
	EXPECT_EQ("NB08",bytes.latin1((size_t)image.appendedOffset,(size_t)image.appendedOffset + 4));
}

TEST_F(DebugSymFmtTest, ParseMzHeaderRejectsAFileThatIsNotAnMzImage)
{
	const std::string text = "not an exe at all, but long enough";
	MzHeader header;
	EXPECT_FALSE(DEBUG_ParseMzHeader(DebugBytes((const uint8_t*)text.data(),text.size()),header));
}

TEST_F(DebugSymFmtTest, MzFingerprintSeparatesTwoProgramsOfTheSameName)
{
	std::vector<uint8_t> probeData,qrenderData;
	ASSERT_TRUE(LoadFixture("cvprobe.exe",probeData));
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",qrenderData));

	MzHeader probe,qrender;
	ASSERT_TRUE(DEBUG_ParseMzHeader(DebugBytes(probeData.data(),probeData.size()),probe));
	ASSERT_TRUE(DEBUG_ParseMzHeader(DebugBytes(qrenderData.data(),qrenderData.size()),qrender));
	EXPECT_NE(DEBUG_MzFingerprint(probe),DEBUG_MzFingerprint(qrender));
}

TEST_F(DebugSymFmtTest, AZeroCblpMeansAFullLastPageNotAnEmptyOne)
{
	/* Neither linker fixture leaves e_cblp at 0, so only a synthetic header
	 * exercises the branch: reading it literally shortens the image by a
	 * whole page and puts the appended block 512 bytes early. */
	std::vector<uint8_t> data(1100,0);
	data[0] = 'M'; data[1] = 'Z';
	data[4] = 2;			/* pages = 2, e_cblp left at 0 */
	data[8] = 2;			/* header paragraphs = 2 */

	MzImage image;
	ASSERT_TRUE(DEBUG_ParseMzImage(DebugBytes(data.data(),data.size()),image));
	EXPECT_EQ(32u,image.imageOffset);
	EXPECT_EQ(992u,image.imageSize);
	EXPECT_EQ(1024u,image.appendedOffset);
}

TEST_F(DebugSymFmtTest, ReadCodeViewFindsTheAppendedBlockThroughTheEofTrailer)
{
	CvInfo info;
	ASSERT_TRUE(DEBUG_ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));
	EXPECT_EQ("NB08",info.signature);
	EXPECT_EQ(19560u,info.base);
	EXPECT_EQ(61u,info.directory.size());
	EXPECT_TRUE(info.warnings.empty());
}

TEST_F(DebugSymFmtTest, ReadCodeViewReadsModulesPublicsSegmentsAndLineTables)
{
	CvInfo info;
	ASSERT_TRUE(DEBUG_ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));

	ASSERT_FALSE(info.modules.empty());
	EXPECT_EQ("cvprobe.obj",info.modules[0].name);
	EXPECT_EQ(51u,info.modules.size());
	EXPECT_EQ(72u,info.segments.size());
	EXPECT_EQ(720u,CountKind(info,CV_SYM_PUBLIC));

	const CvLineTable *table = NULL;
	for (size_t i = 0;i < info.lines.size();i++)
		if (info.lines[i].file == "cvprobe.bas") { table = &info.lines[i]; break; }
	ASSERT_TRUE(table != NULL);
	EXPECT_EQ(1u,table->segment);
	ASSERT_FALSE(table->lines.empty());
	EXPECT_EQ(48u,table->lines[0].offset);
	EXPECT_EQ(13u,table->lines[0].line);
}

TEST_F(DebugSymFmtTest, AlignSymCvOmfSigIsSkippedSoPerModuleSymbolsSurvive)
{
	/* Reading the 4-byte signature as a record length made the whole
	 * subsection parse as nothing: 0 procs and 0 module data out of 340 real
	 * bytes. */
	CvInfo info;
	ASSERT_TRUE(DEBUG_ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));

	std::vector<std::string> procs;
	const CvSymbol *add = NULL;
	for (size_t i = 0;i < info.symbols.size();i++) {
		if (info.symbols[i].kind != CV_SYM_PROC) continue;
		procs.push_back(info.symbols[i].name);
		if (info.symbols[i].name == "pr_add") add = &info.symbols[i];
	}
	std::sort(procs.begin(),procs.end());

	ASSERT_EQ(4u,procs.size());
	EXPECT_EQ("b$sd",procs[0]);
	EXPECT_EQ("pr_add",procs[1]);
	EXPECT_EQ("pr_fill",procs[2]);
	EXPECT_EQ("pr_show",procs[3]);

	ASSERT_TRUE(add != NULL);
	EXPECT_EQ(1u,add->segment);
	EXPECT_EQ(180u,add->offset);
	EXPECT_EQ(34u,add->size);
	EXPECT_EQ("cvprobe.obj",add->module);
}

TEST_F(DebugSymFmtTest, ParseSymbolRunKeepsARunThatHasNoLeadingSignature)
{
	uint8_t body[16] = {0};
	body[0] = 10; body[1] = 0;			/* length */
	body[2] = 0x03; body[3] = 0x01;			/* S_PUB16 */
	body[4] = 0x34; body[5] = 0x12;			/* offset */
	body[6] = 7; body[7] = 0;			/* segment */
	body[10] = 2; body[11] = 'a'; body[12] = 'b';	/* name */

	const std::vector<CvSymbol> parsed = DEBUG_ParseCvSymbolRun(DebugBytes(body,sizeof(body)),1,0,sizeof(body));
	ASSERT_EQ(1u,parsed.size());
	EXPECT_EQ("ab",parsed[0].name);
	EXPECT_EQ(7u,parsed[0].segment);
	EXPECT_EQ(0x1234u,parsed[0].offset);
}

TEST_F(DebugSymFmtTest, ParseSymbolRunReadsThe32BitPublicForm)
{
	uint8_t body[20] = {0};
	body[0] = 14; body[1] = 0;			/* length */
	body[2] = 0x03; body[3] = 0x02;			/* S_PUB32 */
	body[4] = 0x78; body[5] = 0x56; body[6] = 0x34; body[7] = 0x12;
	body[8] = 9; body[9] = 0;			/* segment */
	body[12] = 3; body[13] = 'w'; body[14] = 'i'; body[15] = 'd';

	const std::vector<CvSymbol> parsed = DEBUG_ParseCvSymbolRun(DebugBytes(body,sizeof(body)),1,0,sizeof(body));
	ASSERT_EQ(1u,parsed.size());
	EXPECT_EQ("wid",parsed[0].name);
	EXPECT_EQ(9u,parsed[0].segment);
	EXPECT_EQ(0x12345678u,parsed[0].offset);
	EXPECT_EQ(CV_SYM_PUBLIC,parsed[0].kind);
}

/* A CV4 record: length, kind, then the body. */
static void AddCvRecord(std::vector<uint8_t> &out,uint16_t kind,const std::vector<uint8_t> &body)
{
	const uint16_t length = (uint16_t)(2 + body.size());
	out.push_back((uint8_t)length); out.push_back((uint8_t)(length >> 8));
	out.push_back((uint8_t)kind); out.push_back((uint8_t)(kind >> 8));
	out.insert(out.end(),body.begin(),body.end());
}

static void AddU32(std::vector<uint8_t> &out,uint32_t value)
{
	for (int i = 0;i < 4;i++) out.push_back((uint8_t)(value >> (8 * i)));
}

static void AddU16(std::vector<uint8_t> &out,uint16_t value)
{
	out.push_back((uint8_t)value); out.push_back((uint8_t)(value >> 8));
}

/* A 32-bit program's functions were skipped whole: GORILLA's LE image has none today, but any
 * 32-bit CV4 image names its procs S_GPROC32, and `locals` answered nothing for all of them. */
TEST_F(DebugSymFmtTest, ParseSymbolRunReadsA32BitProcAndItsLocals)
{
	std::vector<uint8_t> proc;
	AddU32(proc,0); AddU32(proc,0); AddU32(proc,0);	/* parent, end, next */
	AddU32(proc,0x40); AddU32(proc,4); AddU32(proc,0x3c);	/* length, debug start, debug end */
	AddU32(proc,0x1000);				/* offset */
	AddU16(proc,2); AddU16(proc,0x100);		/* segment, type */
	proc.push_back(0);				/* flags */
	proc.push_back(4); proc.push_back('d'); proc.push_back('r'); proc.push_back('a'); proc.push_back('w');

	std::vector<uint8_t> local;
	AddU32(local,(uint32_t)-8);			/* BP-relative */
	AddU16(local,0x74);				/* type */
	local.push_back(1); local.push_back('x');

	std::vector<uint8_t> body;
	AddCvRecord(body,0x0205,proc);			/* S_GPROC32 */
	AddCvRecord(body,0x0200,local);			/* S_BPREL32 */
	AddCvRecord(body,0x0006,std::vector<uint8_t>());	/* S_ENDBLK */

	std::vector<CvScope> scopes;
	const std::vector<CvSymbol> parsed = DEBUG_ParseCvSymbolRun(DebugBytes(body.data(),body.size()),1,0,body.size(),&scopes);
	ASSERT_EQ(1u,parsed.size());
	EXPECT_EQ("draw",parsed[0].name);
	EXPECT_EQ(CV_SYM_PROC,parsed[0].kind);
	EXPECT_EQ(0x1000u,parsed[0].offset);
	EXPECT_EQ(0x40u,parsed[0].size);
	ASSERT_EQ(1u,scopes.size());
	ASSERT_EQ(1u,scopes[0].locals.size());
	EXPECT_EQ("x",scopes[0].locals[0].name);
	EXPECT_EQ(-8,scopes[0].locals[0].frameOffset);
}

/* A frameless -m32 function names its locals off ESP (S_REGREL32); the reader dropped every one of them, so
 * `locals` in such a function answered nothing. */
TEST_F(DebugSymFmtTest, ParseSymbolRunReadsALocalNamedOffTheStackPointer)
{
	std::vector<uint8_t> proc;
	AddU32(proc,0); AddU32(proc,0); AddU32(proc,0);
	AddU32(proc,0x40); AddU32(proc,4); AddU32(proc,0x3c);
	AddU32(proc,0x1000);
	AddU16(proc,1); AddU16(proc,0x100);
	proc.push_back(0);
	proc.push_back(3); proc.push_back('b'); proc.push_back('u'); proc.push_back('m');

	std::vector<uint8_t> local;
	AddU32(local,8);				/* ESP + 8 */
	AddU16(local,21);				/* CV4's ESP */
	AddU16(local,0x74);				/* type */
	local.push_back(1); local.push_back('n');

	std::vector<uint8_t> body;
	AddCvRecord(body,0x0205,proc);			/* S_GPROC32 */
	AddCvRecord(body,0x020c,local);			/* S_REGREL32 */
	AddCvRecord(body,0x0006,std::vector<uint8_t>());

	std::vector<CvScope> scopes;
	DEBUG_ParseCvSymbolRun(DebugBytes(body.data(),body.size()),1,0,body.size(),&scopes);
	ASSERT_EQ(1u,scopes.size());
	ASSERT_EQ(1u,scopes[0].locals.size());
	EXPECT_EQ("n",scopes[0].locals[0].name);
	EXPECT_EQ(CV_LOCAL_STACK,scopes[0].locals[0].storage);
	EXPECT_EQ(8,scopes[0].locals[0].frameOffset);
}

/* An LE image whose name tables and entry table were never read: `sym` for an exported function found nothing. */
TEST_F(DebugSymFmtTest, ParseLeReadsObjectsExportNamesAndEntries)
{
	std::vector<uint8_t> le(0x98,0);
	std::vector<uint8_t> objects,names,entries;
	AddU32(objects,0x300); AddU32(objects,0x10000); AddU32(objects,5); AddU32(objects,1); AddU32(objects,1); AddU32(objects,0);
	names.push_back(3); names.push_back('m'); names.push_back('o'); names.push_back('d'); AddU16(names,0);
	names.push_back(4); names.push_back('m'); names.push_back('a'); names.push_back('i'); names.push_back('n'); AddU16(names,1);
	names.push_back(0);
	entries.push_back(1); entries.push_back(3);		/* one 32-bit entry */
	AddU16(entries,1);					/* in object 1 */
	entries.push_back(1);					/* flags */
	AddU32(entries,0x44);
	entries.push_back(0);

	le[0] = 'L'; le[1] = 'E';
	le[0x29] = 0x10;					/* page size 4096 */
	le[0x40] = 0x98;					/* object table, then names, then entries */
	le[0x44] = 1;
	le[0x58] = (uint8_t)(0x98 + objects.size());
	le[0x5c] = (uint8_t)(0x98 + objects.size() + names.size());
	le.insert(le.end(),objects.begin(),objects.end());
	le.insert(le.end(),names.begin(),names.end());
	le.insert(le.end(),entries.begin(),entries.end());

	LeImage image;
	ASSERT_TRUE(DEBUG_ParseLe(DebugBytes(le.data(),le.size()),image));
	ASSERT_EQ(1u,image.objects.size());
	EXPECT_EQ(0x10000u,image.objects[0].base);
	ASSERT_EQ(2u,image.names.size());
	EXPECT_EQ("main",image.names[1].name);
	EXPECT_EQ(1u,image.names[1].ordinal);
	ASSERT_EQ(1u,image.entries.size());
	EXPECT_EQ(1u,image.entries[0].object);
	EXPECT_EQ(0x44u,image.entries[0].offset);
}

static DebugImageObject PatternObject(uint16_t index,size_t bytes,uint8_t seed)
{
	DebugImageObject object;
	object.index = index;
	object.size = (uint32_t)bytes;
	DebugImagePage page;
	uint32_t state = seed;
	for (size_t i = 0;i < bytes;i++) {
		state = state * 1103515245u + 12345u;
		page.bytes.push_back((uint8_t)(state >> 16));
	}
	page.fixed.assign(bytes,1);
	object.pages.push_back(page);
	return object;
}

/* DOS/32A puts objects at addresses nothing records (0x170010, 0x180500 in GORILLA); a map read as if
 * loaded at the stub's segment named every function 0x8250 + offset. */
TEST_F(DebugSymFmtTest, LocateObjectsFindsEachObjectWhereItsBytesAre)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	objects.push_back(PatternObject(2,512,91));

	std::vector<uint8_t> memory(0x10000,0);
	memcpy(&memory[0x3010],&objects[0].pages[0].bytes[0],512);
	memcpy(&memory[0x8500],&objects[1].pages[0].bytes[0],512);

	DebugPlacement placement;
	ASSERT_TRUE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
	EXPECT_EQ(0x3010u,placement[1]);
	EXPECT_EQ(0x8500u,placement[2]);
}

TEST_F(DebugSymFmtTest, LocateObjectsIgnoresTheBytesALoaderRelocates)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	objects[0].pages[0].fixed[8] = 0;			/* a fixup sits inside the first window */
	std::vector<uint8_t> memory(0x4000,0);
	memcpy(&memory[0x1000],&objects[0].pages[0].bytes[0],512);
	memory[0x1008] ^= 0xff;

	DebugPlacement placement;
	ASSERT_TRUE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
	EXPECT_EQ(0x1000u,placement[1]);
}

/* A loader reads through a low buffer, so a copy of a page can outlive the load: two matches are no answer. */
TEST_F(DebugSymFmtTest, LocateObjectsDeclinesWhenTwoPlacesMatchEqually)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	std::vector<uint8_t> memory(0x4000,0);
	memcpy(&memory[0x1000],&objects[0].pages[0].bytes[0],512);
	memcpy(&memory[0x2000],&objects[0].pages[0].bytes[0],512);

	DebugPlacement placement;
	EXPECT_FALSE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
}

/* DOS/32A reads through an 8 KB buffer that keeps the last pages it read, with the file's bytes in them. A tiny
 * image has nothing but that copy and the loaded one, and two equal matches declined the code object: its
 * symbols never appeared. The loaded copy is the one whose relocated bytes differ from the file's. */
TEST_F(DebugSymFmtTest, LocateObjectsPrefersTheCopyTheLoaderRelocated)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	objects[0].pages[0].fixed[300] = 0;			/* a fixup the first windows do not cover */
	objects[0].pages[0].fixed[301] = 0;

	std::vector<uint8_t> memory(0x4000,0);
	memcpy(&memory[0x1000],&objects[0].pages[0].bytes[0],512);	/* the loader's buffer: the file as read */
	memcpy(&memory[0x2000],&objects[0].pages[0].bytes[0],512);	/* the object, relocated */
	memory[0x2000+300] ^= 0x55;

	DebugPlacement placement;
	ASSERT_TRUE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
	EXPECT_EQ(0x2000u,placement[1]);
}

/* The loader's buffer is rewritten page after page: a copy of the code with the data page's bytes laid over its
 * start differed from the file in every masked byte too, and scored as the most relocated. The object is the
 * copy that is the file's but for the bytes the loader rewrites. */
TEST_F(DebugSymFmtTest, LocateObjectsDoesNotTakeABufferWithAnotherPageOverItsStart)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	for (int i = 8;i < 12;i++) objects[0].pages[0].fixed[i] = 0;		/* a fixup near the start */
	objects[0].pages[0].fixed[300] = 0;

	std::vector<uint8_t> memory(0x4000,0);
	memcpy(&memory[0x1000],&objects[0].pages[0].bytes[0],512);	/* the buffer, its start overwritten */
	for (int i = 0;i < 16;i++) memory[0x1000+i] = (uint8_t)(0xa0 + i);
	memcpy(&memory[0x2000],&objects[0].pages[0].bytes[0],512);	/* the object, relocated */
	memory[0x2000+8] ^= 0x55;				/* one byte of each value differs: the rest were equal */
	memory[0x2000+300] ^= 0x55;
	memcpy(&memory[0x3000],&objects[0].pages[0].bytes[0],512);	/* the file as read */

	DebugPlacement placement;
	ASSERT_TRUE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
	EXPECT_EQ(0x2000u,placement[1]);
}

/* An NE segment's relocations leave its bytes as the file has them, so the loader's buffer copy and the loaded
 * one matched equally and neither was placed. The CPU is running at the image's entry in the loaded one. */
TEST_F(DebugSymFmtTest, LocateObjectsTakesTheCopyTheCpuRunsTheEntryIn)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	std::vector<uint8_t> memory(0x4000,0);
	memcpy(&memory[0x1000],&objects[0].pages[0].bytes[0],512);
	memcpy(&memory[0x2000],&objects[0].pages[0].bytes[0],512);

	DebugPlacement placement;
	EXPECT_FALSE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));

	DebugEntryHint hint;
	hint.object = 1;
	hint.base = 0x2000;
	ASSERT_TRUE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement,&hint));
	EXPECT_EQ(0x2000u,placement[1]);
}

TEST_F(DebugSymFmtTest, LocateObjectsWaitsWhileTheImageIsNotLoadedYet)
{
	std::vector<DebugImageObject> objects;
	objects.push_back(PatternObject(1,512,3));
	std::vector<uint8_t> memory(0x4000,0);

	DebugPlacement placement;
	EXPECT_FALSE(DEBUG_LocateObjects(objects,memory.data(),memory.size(),placement));
}

/* WLINK's map writes object:offset; read as paragraphs it put 0001:00001dff at 0x1dff + 0x10. */
TEST_F(DebugSymFmtTest, PlaceLinkMapRebasesAddressesByObject)
{
	LinkMapFile map;
	DEBUG_ParseLinkMap(
		"Segment                Class          Group          Address         Size\n"
		"=======                =====          =====          =======         ====\n"
		"\n"
		"_TEXT                  CODE           AUTO           0001:00000000   00000593\n"
		"\n"
		"Address        Symbol\n"
		"=======        ======\n"
		"\n"
		"Module: a.obj(a.bas)\n"
		"0001:00001dff* DOSUN\n",
		"a.map",map);
	ASSERT_EQ(1u,map.publics.count("DOSUN"));

	DebugPlacement placement;
	placement[1] = 0x170010;
	DEBUG_PlaceLinkMap(map,placement);
	EXPECT_EQ(0x171e0fu,map.publics["DOSUN"].address.mapOffset);
}

/* An object the loader's placement does not cover has no address: its publics sat at (object << 4) + offset,
 * a few bytes above zero, and `var` read the guest's real-mode vectors for a global. */
TEST_F(DebugSymFmtTest, PlaceLinkMapDropsWhatItCouldNotPlace)
{
	LinkMapFile map;
	DEBUG_ParseLinkMap(
		"Segment                Class          Group          Address         Size\n"
		"=======                =====          =====          =======         ====\n"
		"\n"
		"_TEXT                  CODE           AUTO           0001:00000000   00000593\n"
		"_DATA                  DATA           AUTO           0002:00000000   00000010\n"
		"\n"
		"Address        Symbol\n"
		"=======        ======\n"
		"\n"
		"Module: a.obj(a.c)\n"
		"0001:00000010* code_name\n"
		"0002:00000008* data_name\n",
		"a.map",map);

	DebugPlacement placement;
	placement[1] = 0x118000;
	DEBUG_PlaceLinkMap(map,placement);
	EXPECT_EQ(1u,map.publics.count("code_name"));
	EXPECT_EQ(0u,map.publics.count("data_name"));
	ASSERT_EQ(1u,map.segments.size());
	EXPECT_EQ("_TEXT",map.segments[0].name);
}

/* An NE image's segments came out of the file whole, with the loader's relocation chains in them, so the
 * locator saw the patched bytes as content and never matched; its exports were never read at all. */
TEST_F(DebugSymFmtTest, NeImageGivesSegmentsWithRelocationChainsMaskedAndTheExports)
{
	const size_t header = 0x40;
	std::vector<uint8_t> ne(0x400,0);
	ne[0] = 'M'; ne[1] = 'Z'; ne[0x3c] = (uint8_t)header;
	ne[header] = 'N'; ne[header+1] = 'E';
	ne[header+0x04] = 0x70;					/* entry table, from the header */
	ne[header+0x1c] = 1;					/* one segment */
	ne[header+0x22] = 0x40;					/* segment table */
	ne[header+0x26] = 0x50;					/* resident names */
	ne[header+0x32] = 0;					/* sector shift defaults to 9 */
	const size_t segmentTable = header + 0x40;
	ne[segmentTable] = 1;					/* sector 1 = file offset 0x200 */
	ne[segmentTable+2] = 0x20;				/* 32 bytes */
	ne[segmentTable+4] = 0x00; ne[segmentTable+5] = 0x01;	/* code, has relocations */
	const size_t names = header + 0x50;
	ne[names] = 3; ne[names+1] = 'm'; ne[names+2] = 'o'; ne[names+3] = 'd';
	ne[names+6] = 4; ne[names+7] = 'm'; ne[names+8] = 'a'; ne[names+9] = 'i'; ne[names+10] = 'n'; ne[names+11] = 1;
	const size_t entries = header + 0x70;
	ne[entries] = 1; ne[entries+1] = 1;			/* one entry in fixed segment 1 */
	ne[entries+2] = 1; ne[entries+3] = 0x10;		/* flags, offset 0x0010 */
	for (int i = 0;i < 0x20;i++) ne[0x200+i] = (uint8_t)(0x10 + i);
	ne[0x204] = 0x0c; ne[0x205] = 0;			/* the chain: 4 -> 0xc -> end */
	ne[0x20c] = 0xff; ne[0x20d] = 0xff;
	ne[0x220] = 1; ne[0x221] = 0;				/* one relocation */
	ne[0x222] = 5;						/* 16-bit offset */
	ne[0x223] = 0;						/* internal reference, not additive */
	ne[0x224] = 4; ne[0x225] = 0;				/* chain head at 4 */

	DebugImage image;
	ASSERT_TRUE(DEBUG_NeImage(DebugBytes(ne.data(),ne.size()),image));
	ASSERT_EQ(1u,image.objects.size());
	ASSERT_EQ(1u,image.objects[0].pages.size());
	const std::vector<uint8_t> fixed = image.objects[0].pages[0].fixed;
	ASSERT_EQ(0x20u,fixed.size());
	EXPECT_EQ(0,fixed[4]); EXPECT_EQ(0,fixed[5]);
	EXPECT_EQ(0,fixed[0xc]); EXPECT_EQ(0,fixed[0xd]);
	EXPECT_EQ(1,fixed[6]);
	EXPECT_EQ(1,fixed[0xe]);
	ASSERT_EQ(1u,image.exports.size());
	EXPECT_EQ("main",image.exports[0].name);
	EXPECT_EQ(1u,image.exports[0].object);
	EXPECT_EQ(0x10u,image.exports[0].offset);
}

/* A PE's sections were never read: the base relocations (the loader's rewrite of every absolute address) sat in
 * the bytes the locator compared, and an exported name never reached the store. */
TEST_F(DebugSymFmtTest, PeImageGivesSectionsWithRelocationsMaskedAndTheExports)
{
	std::vector<uint8_t> pe(0x300,0);
	pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40;
	const size_t header = 0x40;
	pe[header] = 'P'; pe[header+1] = 'E';
	pe[header+6] = 1;					/* one section */
	pe[header+20] = 0xe0;					/* optional header size */
	const size_t optional = header + 24;
	pe[optional] = 0x0b; pe[optional+1] = 0x01;		/* PE32 */
	const size_t directories = optional + 96;
	pe[directories] = 0x20; pe[directories+1] = 0x10;	/* export directory at RVA 0x1020 */
	pe[directories+4] = 0x40;				/* 0x40 bytes */
	pe[directories+5*8] = 0x10; pe[directories+5*8+1] = 0x10;	/* relocations at 0x1010 */
	pe[directories+5*8+4] = 12;
	const size_t section = optional + 0xe0;
	pe[section+8] = 0x00; pe[section+9] = 0x01;		/* virtual size 0x100 */
	pe[section+12] = 0x00; pe[section+13] = 0x10;		/* RVA 0x1000 */
	pe[section+16] = 0x00; pe[section+17] = 0x01;		/* raw size 0x100 */
	pe[section+20] = 0x00; pe[section+21] = 0x02;		/* raw offset 0x200 */
	pe[section+36] = 0x20;					/* code */

	const size_t raw = 0x200;
	pe[raw+0x10] = 0x00; pe[raw+0x11] = 0x10;		/* relocation block: page 0x1000 */
	pe[raw+0x14] = 12;
	pe[raw+0x18] = 0x08; pe[raw+0x19] = 0x30;		/* HIGHLOW at page offset 8 */
	pe[raw+0x20+24] = 1;					/* one name */
	pe[raw+0x20+28] = 0x50; pe[raw+0x20+29] = 0x10;		/* functions */
	pe[raw+0x20+32] = 0x54; pe[raw+0x20+33] = 0x10;		/* names */
	pe[raw+0x20+36] = 0x58; pe[raw+0x20+37] = 0x10;		/* ordinals */
	pe[raw+0x50] = 0x04; pe[raw+0x51] = 0x10;		/* function RVA 0x1004 */
	pe[raw+0x54] = 0x60; pe[raw+0x55] = 0x10;		/* name RVA 0x1060 */
	memcpy(&pe[raw+0x60],"pe_main",8);

	DebugImage image;
	ASSERT_TRUE(DEBUG_PeImage(DebugBytes(pe.data(),pe.size()),image));
	ASSERT_EQ(1u,image.objects.size());
	EXPECT_TRUE(image.objects[0].code);
	ASSERT_EQ(1u,image.objects[0].pages.size());
	const std::vector<uint8_t> fixed = image.objects[0].pages[0].fixed;
	ASSERT_EQ(0x100u,fixed.size());
	EXPECT_EQ(1,fixed[7]);
	EXPECT_EQ(0,fixed[8]); EXPECT_EQ(0,fixed[9]); EXPECT_EQ(0,fixed[10]); EXPECT_EQ(0,fixed[11]);
	EXPECT_EQ(1,fixed[12]);
	ASSERT_EQ(1u,image.exports.size());
	EXPECT_EQ("pe_main",image.exports[0].name);
	EXPECT_EQ(1u,image.exports[0].object);
	EXPECT_EQ(4u,image.exports[0].offset);
}

/* The MCP found D32X modules by searching memory for a magic number and kept the answer to itself; the emulator
 * had no reader, so a module a program loads at run time had no symbols unless the MCP was there to register them. */
TEST_F(DebugSymFmtTest, D32ImageGivesCodeAndDataWithRelocationsMaskedAndTheExports)
{
	std::vector<uint8_t> module(220,0);
	const uint32_t fields[][2] = {{0,0x58323344},{8,180},{20,72},{24,16},{36,1},{40,88},{44,100},{48,100},
	                              {52,112},{56,16},{60,128},{64,4},{68,4},{100,8},{104,4}};
	for (size_t i = 0;i < sizeof(fields) / sizeof(fields[0]);i++)
		for (int b = 0;b < 4;b++) module[fields[i][0] + b] = (uint8_t)(fields[i][1] >> (8 * b));
	module[32] = 1;						/* one export */
	memcpy(&module[72],"add",4);
	module[92] = 4;						/* export value 4 */
	module[108] = 2;					/* a relocation of the code at offset 8 */
	for (int i = 0;i < 16;i++) module[112+i] = (uint8_t)(0x40 + i);

	DebugImage image;
	ASSERT_TRUE(DEBUG_D32Image(DebugBytes(module.data(),module.size()),image));
	ASSERT_EQ(3u,image.objects.size());
	EXPECT_TRUE(image.objects[0].code);
	ASSERT_EQ(1u,image.objects[0].pages.size());
	const std::vector<uint8_t> fixed = image.objects[0].pages[0].fixed;
	ASSERT_EQ(16u,fixed.size());
	EXPECT_EQ(1,fixed[7]);
	EXPECT_EQ(0,fixed[8]); EXPECT_EQ(0,fixed[11]);
	EXPECT_EQ(1,fixed[12]);
	EXPECT_EQ(4u,image.objects[1].size);
	EXPECT_TRUE(image.objects[2].pages.empty());
	ASSERT_EQ(1u,image.exports.size());
	EXPECT_EQ("add",image.exports[0].name);
	EXPECT_EQ(1u,image.exports[0].object);
	EXPECT_EQ(4u,image.exports[0].offset);
}

/* The same, read from the symbols tests' images: found by walking up to tests/symbols/fixtures. */
static bool LoadSymbolsFixture(const char *name,std::vector<uint8_t> &out)
{
	std::string prefix = ".";
	for (int up = 0;up < 6;up++) {
		if (DEBUG_ReadHostFile((prefix + "/tests/symbols/fixtures/" + name).c_str(),out)) return true;
		prefix += "/..";
	}
	return false;
}

/* A `debug dwarf` image carried its lines and publics in an ELF block no reader opened: `where` named no source line. */
TEST_F(DebugSymFmtTest, DwarfReadsTheLinesAndPublicsJwlinkAppendsToAnLeImage)
{
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadSymbolsFixture("SYMLED.EXE",data));

	DwarfInfo info;
	ASSERT_TRUE(DEBUG_ParseDwarf(DebugBytes(data.data(),data.size()),info));
	EXPECT_EQ(2u,info.version);
	EXPECT_TRUE(info.warnings.empty());

	const DwarfLine *bump = NULL;
	for (size_t i = 0;i < info.lines.size();i++)
		if (info.lines[i].file == "prog.c" && info.lines[i].line == 11) bump = &info.lines[i];
	ASSERT_TRUE(bump != NULL);
	EXPECT_EQ(0x28u,bump->address);

	const DwarfSymbol *symbol = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].name == "bump_") symbol = &info.symbols[i];
	ASSERT_TRUE(symbol != NULL);
	EXPECT_EQ(0x28u,symbol->address);
	EXPECT_TRUE(symbol->function);
}

/* llrm's DWARF 5 (from an OMF object, relocated by jwlink): locals by frame base and location list, call frame
 * information, and the types they name. */
TEST_F(DebugSymFmtTest, DwarfReadsLocalsLocationListsFrameRowsAndTypes)
{
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadSymbolsFixture("SYMLOC.EXE",data));

	DwarfInfo info;
	ASSERT_TRUE(DEBUG_ParseDwarf(DebugBytes(data.data(),data.size()),info));
	EXPECT_EQ(5u,info.version);

	const DwarfScope *add = NULL,*run = NULL;
	for (size_t i = 0;i < info.scopes.size();i++) {
		if (info.scopes[i].function == "add") add = &info.scopes[i];
		if (info.scopes[i].function == "run") run = &info.scopes[i];
	}
	ASSERT_TRUE(add != NULL && run != NULL);
	EXPECT_EQ(DWARF_FRAME_CFA,add->frame);

	/* a parameter arrives in a register, then is kept at a place off the frame */
	ASSERT_EQ(3u,add->variables.size());
	const DwarfVariable &first = add->variables[0];
	EXPECT_EQ("first",first.name);
	EXPECT_TRUE(first.parameter);
	ASSERT_EQ(2u,first.where.size());
	EXPECT_EQ(DWARF_LOCATION_REGISTER,first.where[0].kind);
	EXPECT_EQ(0u,first.where[0].reg);
	EXPECT_EQ(DWARF_LOCATION_FRAME,first.where[1].kind);
	EXPECT_EQ(-12,first.where[1].offset);
	EXPECT_EQ(first.where[0].end,first.where[1].begin);

	/* the frame address is ESP plus a depth that follows the prologue */
	ASSERT_FALSE(info.cfa.empty());
	EXPECT_EQ(4u,info.cfa[0].reg);
	EXPECT_EQ(4,info.cfa[0].offset);
	bool deeper = false;
	for (size_t i = 0;i < info.cfa.size();i++) deeper |= info.cfa[i].offset > 4;
	EXPECT_TRUE(deeper);

	const DwarfVariable *p = NULL;
	for (size_t i = 0;i < run->variables.size();i++)
		if (run->variables[i].name == "p") p = &run->variables[i];
	ASSERT_TRUE(p != NULL);
	const std::map<size_t,size_t>::const_iterator type = info.typeIndex.find(p->type);
	ASSERT_TRUE(type != info.typeIndex.end());
	EXPECT_EQ("pt",info.types[type->second].name);
	EXPECT_EQ(2u,info.types[type->second].members.size());
	EXPECT_EQ(8u,info.types[type->second].size);
}

TEST_F(DebugSymFmtTest, TheMzImageEndFindsTheBlockWhenTheTrailerIsUnusable)
{
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",data));
	ASSERT_GT(data.size(),4u);
	data[data.size()-4] = 0xef; data[data.size()-3] = 0xbe;
	data[data.size()-2] = 0xad; data[data.size()-1] = 0xde;

	uint64_t base = 0;
	std::string signature;
	ASSERT_TRUE(DEBUG_FindCvBase(DebugBytes(data.data(),data.size()),base,signature));
	EXPECT_EQ(484868u,base);
	EXPECT_EQ("NB08",signature);
}

TEST_F(DebugSymFmtTest, ADirectoryHeaderNearEofWarnsInsteadOfThrowing)
{
	/* The bounds check read 4 bytes and the header is 8: a truncated or
	 * mis-stamped lfoDirectory threw, which the caller turned into "carries
	 * no debug info" for the whole file. */
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadFixture("cvprobe.exe",data));

	const size_t base = 19560;
	const uint32_t lfo = (uint32_t)(data.size() - base - 5);
	data[base+4] = (uint8_t)(lfo & 0xff);
	data[base+5] = (uint8_t)((lfo >> 8) & 0xff);
	data[base+6] = (uint8_t)((lfo >> 16) & 0xff);
	data[base+7] = (uint8_t)((lfo >> 24) & 0xff);

	CvInfo info;
	ASSERT_TRUE(DEBUG_ParseCodeView(DebugBytes(data.data(),data.size()),info));
	EXPECT_TRUE(info.directory.empty());
	ASSERT_FALSE(info.warnings.empty());
	EXPECT_NE(std::string::npos,info.warnings[0].find("past end of file"));
}

TEST_F(DebugSymFmtTest, ReadCodeViewScalesToARealMediumModelProgram)
{
	CvInfo info;
	ASSERT_TRUE(DEBUG_ReadCodeViewFile(Fixture("qrender-cv.exe").c_str(),info));
	EXPECT_EQ("NB08",info.signature);
	EXPECT_EQ(304u,info.modules.size());
	EXPECT_EQ(282u,info.segments.size());
	EXPECT_EQ(2104u,CountKind(info,CV_SYM_PUBLIC));
	EXPECT_EQ(245u,CountKind(info,CV_SYM_PROC));
	EXPECT_EQ(6u,CountKind(info,CV_SYM_LABEL));
	EXPECT_TRUE(info.warnings.empty());
}


/*
 * The .MAP and the EXE come from the same LINK invocation, so an address that
 * differs from the map is the appended-info reader being wrong -- there is
 * nothing else it could be.
 */

std::string Upper(const std::string &text)
{
	std::string out = text;
	for (size_t i = 0;i < out.size();i++) out[i] = (char)toupper((unsigned char)out[i]);
	return out;
}

std::map<std::string,uint32_t> LoadMapPublics(const std::string &path)
{
	std::map<std::string,uint32_t> publics;
	LinkMapFile map;
	if (!DEBUG_ReadLinkMapFile(path.c_str(),map)) return publics;

	for (std::map<std::string,LinkMapPublic>::const_iterator it = map.publics.begin();it != map.publics.end();++it)
		publics[it->first] = it->second.address.mapOffset;
	return publics;
}

struct Agreement {
	size_t agree = 0;
	size_t disagree = 0;
	size_t absent = 0;
};

Agreement AgreementWithMap(const char *exe,const char *map)
{
	Agreement counts;
	CvInfo info;
	EXPECT_TRUE(DEBUG_ReadCodeViewFile(Fixture(exe).c_str(),info));

	const std::map<std::string,uint32_t> publics = LoadMapPublics(Fixture(map));
	const std::map<uint16_t,uint32_t> bases = DEBUG_CvSegmentBases(info);

	for (size_t i = 0;i < info.symbols.size();i++) {
		const CvSymbol &symbol = info.symbols[i];
		if (symbol.kind != CV_SYM_PUBLIC) continue;

		std::map<std::string,uint32_t>::const_iterator found = publics.find(symbol.name);
		if (found == publics.end()) found = publics.find(Upper(symbol.name));
		if (found == publics.end()) { counts.absent++; continue; }

		const std::map<uint16_t,uint32_t>::const_iterator base = bases.find(symbol.segment);
		if (base != bases.end() && base->second + symbol.offset == found->second) counts.agree++;
		else counts.disagree++;
	}
	return counts;
}

TEST_F(DebugSymFmtTest, CodeViewPublicsLandWhereCvprobeMapPutsThem)
{
	const Agreement counts = AgreementWithMap("cvprobe.exe","cvprobe.map");
	EXPECT_EQ(0u,counts.disagree);
	EXPECT_EQ(715u,counts.agree);
}

TEST_F(DebugSymFmtTest, CodeViewPublicsLandWhereQrenderMapPutsThem)
{
	/* segmap.frame alone -- the obvious reading, and wrong -- put 345 of 715
	 * cvprobe publics at the wrong address. Segments sharing a group share a
	 * frame and are told apart by segmap.offset. */
	const Agreement counts = AgreementWithMap("qrender-cv.exe","qrender-cv.map");
	EXPECT_EQ(0u,counts.disagree);
	EXPECT_EQ(2086u,counts.agree);
}

TEST_F(DebugSymFmtTest, SourceLineAtAnswersOnlyInsideALinesOwnRange)
{
	/* "nearest entry at or before the address" put CVPROBE's entry -- which
	 * is in the runtime, a module with no line info -- at ..\rt\strdsp1.c:40. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("cvprobe.exe").c_str(),0,info));
	EXPECT_TRUE(DEBUG_SourceLineAt(info,0x2dbc) == NULL);

	const DebugLine *first = NULL;
	for (size_t i = 0;i < info.lines.size();i++)
		if (info.lines[i].file == "cvprobe.bas") { first = &info.lines[i]; break; }
	ASSERT_TRUE(first != NULL);

	const uint16_t line = first->line;
	const uint32_t begin = first->imageOffset;
	const uint32_t end = first->endOffset;

	const DebugLine *at_begin = DEBUG_SourceLineAt(info,begin);
	ASSERT_TRUE(at_begin != NULL);
	EXPECT_EQ(line,at_begin->line);

	const DebugLine *at_last = DEBUG_SourceLineAt(info,end - 1);
	ASSERT_TRUE(at_last != NULL);
	EXPECT_EQ(line,at_last->line);

	const DebugLine *past = DEBUG_SourceLineAt(info,end);
	EXPECT_TRUE(past == NULL || past->line != line);
}

TEST_F(DebugSymFmtTest, TheLastLineOfATableRunsToTheModulesOwnExtent)
{
	/* CV writes no end offset for a line, so every line but the last is
	 * bounded by the next one. The last is bounded by the module's own
	 * SegInfo; without that it covers a single byte, and an address anywhere
	 * in the last statement of a file resolves to no line at all. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("cvprobe.exe").c_str(),0,info));

	std::vector<const DebugLine*> bas;
	for (size_t i = 0;i < info.lines.size();i++)
		if (info.lines[i].file == "cvprobe.bas") bas.push_back(&info.lines[i]);
	ASSERT_EQ(20u,bas.size());

	const DebugLine *last = bas[0];
	for (size_t i = 1;i < bas.size();i++)
		if (bas[i]->imageOffset > last->imageOffset) last = bas[i];

	EXPECT_EQ(37u,last->line);
	EXPECT_EQ(301u,last->imageOffset);
	EXPECT_EQ(316u,last->endOffset);

	const DebugLine *covering = DEBUG_SourceLineAt(info,315);
	ASSERT_TRUE(covering != NULL);
	EXPECT_EQ(37u,covering->line);
	EXPECT_TRUE(DEBUG_SourceLineAt(info,316) == NULL);
}

TEST_F(DebugSymFmtTest, ParseDebugInfoAppliesTheLoadBaseAndReportsModulesAndLines)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("qrender-cv.exe").c_str(),0x12340,info));
	EXPECT_EQ(DEBUG_FORMAT_CODEVIEW,info.format);
	EXPECT_EQ("NB08",info.version);
	EXPECT_EQ(304u,info.modules.size());

	bool has_d_poly = false;
	for (size_t i = 0;i < info.modules.size();i++)
		if (info.modules[i].name == "d_poly.obj") has_d_poly = true;
	EXPECT_TRUE(has_d_poly);

	const DebugSymbol *symbol = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (Upper(info.symbols[i].name) == "R_DRAW_WORLD") { symbol = &info.symbols[i]; break; }
	ASSERT_TRUE(symbol != NULL);

	const std::map<std::string,uint32_t> publics = LoadMapPublics(Fixture("qrender-cv.map"));
	std::map<std::string,uint32_t>::const_iterator found = publics.find(symbol->name);
	if (found == publics.end()) found = publics.find(Upper(symbol->name));
	ASSERT_TRUE(found != publics.end());
	EXPECT_EQ(0x12340u + found->second,symbol->linear);

	bool has_bas_line = false;
	for (size_t i = 0;i < info.lines.size();i++) {
		const std::string file = Upper(info.lines[i].file);
		if (file.size() >= 4 && file.compare(file.size()-4,4,".BAS") == 0 && info.lines[i].line > 0)
			has_bas_line = true;
	}
	EXPECT_TRUE(has_bas_line);

	/* Segment 0 is CodeView's absolutes bucket and is never in sstSegMap, so
	 * this warning is expected. Pinning it means a NEW warning gets noticed
	 * instead of joining a list nobody reads. */
	ASSERT_EQ(1u,info.warnings.size());
	EXPECT_EQ("no sstSegMap entry for segment(s) 0",info.warnings[0]);
}


TEST_F(DebugSymFmtTest, CodeViewTypesComeFromTheGlobalTypesTable)
{
	std::vector<uint8_t> data;
	CvInfo info;
	ASSERT_TRUE(LoadFixture("cvprobe.exe",data));
	ASSERT_TRUE(DEBUG_ParseCodeView(DebugBytes(data.data(),data.size()),info));

	/* The table is a flags word, a count, that many offsets, then the
	 * records; the offsets are counted from the start of the subsection.
	 * Reading the count where the flags are gives 1 record instead of 12. */
	ASSERT_EQ(12u,info.types.size());
	EXPECT_EQ(0x0201u,info.types[0].leaf);		/* LF_ARGLIST */
	EXPECT_EQ(0x0008u,info.types[1].leaf);		/* LF_PROCEDURE returning short */
	EXPECT_EQ(0x0011u,info.types[1].utype);
	EXPECT_EQ(0x0002u,info.types[2].leaf);		/* LF_POINTER to short */
	EXPECT_EQ(0x0011u,info.types[2].utype);
	EXPECT_EQ(2u,info.types[2].size);
	EXPECT_EQ(0x000du,info.types[3].leaf);		/* LF_BARRAY of short: pr_tab */
	EXPECT_EQ(0x0011u,info.types[3].utype);
}

TEST_F(DebugSymFmtTest, CodeViewArraysAndStructuresCarryTheirSize)
{
	std::vector<uint8_t> data;
	CvInfo info;
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",data));
	ASSERT_TRUE(DEBUG_ParseCodeView(DebugBytes(data.data(),data.size()),info));
	ASSERT_EQ(471u,info.types.size());

	/* A size read from the wrong offset in the record comes out as a leaf
	 * code or zero, so every array's has to be a whole number of elements
	 * and every structure has to have one. */
	unsigned int arrays = 0,structures = 0;
	for (size_t i = 0;i < info.types.size();i++) {
		const CvType &type = info.types[i];
		if (type.leaf == 0x0003) {
			arrays++;
			EXPECT_EQ(0x0010u,type.utype);	/* every one of them is char[] */
			EXPECT_NE(0u,type.size);
			EXPECT_GT(0x8000u,type.size);
		}
		if (type.leaf == 0x0005) {
			structures++;
			EXPECT_NE(0u,type.size);
			EXPECT_FALSE(type.name.empty());
		}
	}
	EXPECT_EQ(11u,arrays);
	EXPECT_EQ(77u,structures);

	/* Sizes pinned individually: read one field early the size comes off
	 * idxtype instead, which is 0x0021 for every one of them -- non-zero,
	 * under 0x8000, and wrong. These are the fixed string widths in
	 * qrender's own TYPEs. */
	std::vector<uint32_t> sizes;
	for (size_t i = 0;i < info.types.size();i++)
		if (info.types[i].leaf == 0x0003) sizes.push_back(info.types[i].size);
	std::sort(sizes.begin(),sizes.end());
	ASSERT_EQ(11u,sizes.size());
	EXPECT_EQ(1u,sizes[0]);
	EXPECT_EQ(4u,sizes[1]);
	EXPECT_EQ(1024u,sizes[10]);

	/* One of qrender's own BASIC TYPEs, by name and declared size. */
	const CvType *bounds = NULL;
	for (size_t i = 0;i < info.types.size();i++)
		if (info.types[i].name == "Bounds") bounds = &info.types[i];
	ASSERT_TRUE(bounds != NULL);
	EXPECT_EQ(12u,bounds->size);
}

TEST_F(DebugSymFmtTest, CodeViewVariablesGetASizeAndAReadableType)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("cvprobe.exe").c_str(),0x8240,info));

	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++) byName[info.symbols[i].name] = &info.symbols[i];

	/* BASIC's integer is a direct signed 2-byte, type index 0x0011. */
	ASSERT_TRUE(byName.count("pr_sum") != 0);
	EXPECT_EQ("short",byName["pr_sum"]->typeName);
	EXPECT_EQ(2u,byName["pr_sum"]->valueSize);
	EXPECT_EQ(DEBUG_VALUE_SIGNED,byName["pr_sum"]->valueKind);
	EXPECT_EQ("short",byName["pr_count"]->typeName);

	/* A BASIC array's symbol addresses a runtime descriptor rather than the
	 * elements, so it gets a name and no width to read. */
	ASSERT_TRUE(byName.count("pr_tab") != 0);
	EXPECT_EQ("BASIC array of short",byName["pr_tab"]->typeName);
	EXPECT_EQ(0u,byName["pr_tab"]->valueSize);

	ASSERT_TRUE(byName.count("pr_add") != 0);
	EXPECT_TRUE(byName["pr_add"]->isFunction);
	EXPECT_EQ(0u,byName["pr_add"]->valueSize);
	/* A proc keeps its code length; that is not a value width. */
	EXPECT_TRUE(byName["pr_add"]->hasSize);
	EXPECT_EQ(34u,byName["pr_add"]->size);
}

static std::map<std::string,const DebugSymbol*> ByName(const DebugInfo &info)
{
	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (byName.count(info.symbols[i].name) == 0 || info.symbols[i].valueSize != 0)
			byName[info.symbols[i].name] = &info.symbols[i];
	return byName;
}

TEST_F(DebugSymFmtTest, AJWasmProgramTypesReadTheSameBeforeAndAfterCvpack)
{
	/* jwasm + LINK leave every module carrying its own type table, which
	 * starts over at 0x1000; only CVPACK gathers them into the one global
	 * table. Reading just the global table left jwprobe.exe with no types at
	 * all, so nothing it declares had a name, a width or a field. */
	DebugInfo unpacked,packed;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("jwprobe.exe").c_str(),0x8240,unpacked));
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("jwpack.exe").c_str(),0x8240,packed));
	EXPECT_EQ("NB05",unpacked.version);
	EXPECT_EQ("NB08",packed.version);

	const std::map<std::string,const DebugSymbol*> before = ByName(unpacked);
	const std::map<std::string,const DebugSymbol*> after = ByName(packed);

	static const char * const names[] = {"jw_home","jw_nodes","jw_name","jw_count","jw_total"};
	for (size_t i = 0;i < sizeof(names)/sizeof(names[0]);i++) {
		ASSERT_TRUE(before.count(names[i]) != 0) << names[i];
		ASSERT_TRUE(after.count(names[i]) != 0) << names[i];
		EXPECT_EQ(after.at(names[i])->typeName,before.at(names[i])->typeName) << names[i];
		EXPECT_EQ(after.at(names[i])->valueSize,before.at(names[i])->valueSize) << names[i];
		EXPECT_EQ(after.at(names[i])->fields.size(),before.at(names[i])->fields.size()) << names[i];
	}

	/* The source declares Vtx as two SWORDs and an SDWORD. */
	EXPECT_EQ("Vtx",before.at("jw_home")->typeName);
	EXPECT_EQ(8u,before.at("jw_home")->valueSize);
	ASSERT_EQ(3u,before.at("jw_home")->fields.size());
	EXPECT_EQ("vx",before.at("jw_home")->fields[0].name);
	EXPECT_EQ(0u,before.at("jw_home")->fields[0].offset);
	EXPECT_EQ("tag",before.at("jw_home")->fields[2].name);
	EXPECT_EQ(4u,before.at("jw_home")->fields[2].offset);
	EXPECT_EQ(4u,before.at("jw_home")->fields[2].size);

	EXPECT_EQ("Vtx[4]",before.at("jw_nodes")->typeName);
	EXPECT_EQ(32u,before.at("jw_nodes")->valueSize);
	EXPECT_EQ("unsigned char[8]",before.at("jw_name")->typeName);
	EXPECT_EQ("short",before.at("jw_count")->typeName);
	EXPECT_EQ("long",before.at("jw_total")->typeName);
}

TEST_F(DebugSymFmtTest, ACodeViewProcCarriesTheLocalsBetweenItAndItsEndBlock)
{
	/* No CodeView program reported a local before this: only Borland built
	 * scopes, so locals and locals-by-name answered nothing for jwasm, for
	 * C and for BASIC alike. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("jwprobe.exe").c_str(),0x8240,info));

	std::map<std::string,const DebugScope*> byFunction;
	for (size_t i = 0;i < info.scopes.size();i++) byFunction[info.scopes[i].function] = &info.scopes[i];

	ASSERT_TRUE(byFunction.count("jw_fill") != 0);
	ASSERT_EQ(1u,byFunction["jw_fill"]->locals.size());
	EXPECT_EQ("i",byFunction["jw_fill"]->locals[0].name);
	EXPECT_EQ(DEBUG_STORAGE_FRAME,byFunction["jw_fill"]->locals[0].storage);
	EXPECT_EQ(-2,byFunction["jw_fill"]->locals[0].frameOffset);
	EXPECT_EQ("short",byFunction["jw_fill"]->locals[0].typeName);

	ASSERT_TRUE(byFunction.count("jw_sum") != 0);
	ASSERT_EQ(2u,byFunction["jw_sum"]->locals.size());
	EXPECT_EQ("acc",byFunction["jw_sum"]->locals[0].name);
	EXPECT_EQ(-4,byFunction["jw_sum"]->locals[0].frameOffset);
	EXPECT_EQ("long",byFunction["jw_sum"]->locals[0].typeName);
	EXPECT_EQ("j",byFunction["jw_sum"]->locals[1].name);
	EXPECT_EQ(-6,byFunction["jw_sum"]->locals[1].frameOffset);

	/* The scope covers the proc, so an address inside it finds the frame. */
	EXPECT_LT(byFunction["jw_sum"]->imageOffset,byFunction["jw_sum"]->endOffset);

	/* FASTCALL puts k in DX and m in AX -- jwasm assembled "mov ax, k" as
	 * "mov ax, dx". CodeView numbers those 11 and 9. */
	ASSERT_TRUE(byFunction.count("jw_scale") != 0);
	ASSERT_EQ(2u,byFunction["jw_scale"]->locals.size());
	EXPECT_EQ("m",byFunction["jw_scale"]->locals[0].name);
	EXPECT_EQ(DEBUG_STORAGE_REGISTER,byFunction["jw_scale"]->locals[0].storage);
	EXPECT_EQ(0u,byFunction["jw_scale"]->locals[0].reg);		/* AX */
	EXPECT_EQ("k",byFunction["jw_scale"]->locals[1].name);
	EXPECT_EQ(2u,byFunction["jw_scale"]->locals[1].reg);		/* DX */
}

TEST_F(DebugSymFmtTest, ABasicTypeDecodesFieldByFieldAndAsAnArrayOfItself)
{
	/* udtbas.bas declares TYPE Vtx as two integers and a long, then one of
	 * them and an array of four. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("udtbas.exe").c_str(),0x8240,info));

	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (byName.count(info.symbols[i].name) == 0) byName[info.symbols[i].name] = &info.symbols[i];

	ASSERT_TRUE(byName.count("home") != 0);
	EXPECT_EQ("Vtx",byName["home"]->typeName);
	EXPECT_EQ(8u,byName["home"]->valueSize);
	ASSERT_EQ(3u,byName["home"]->fields.size());
	EXPECT_EQ("x",byName["home"]->fields[0].name);
	EXPECT_EQ(0u,byName["home"]->fields[0].offset);
	EXPECT_EQ(2u,byName["home"]->fields[0].size);
	EXPECT_EQ("y",byName["home"]->fields[1].name);
	EXPECT_EQ(2u,byName["home"]->fields[1].offset);
	EXPECT_EQ("tag",byName["home"]->fields[2].name);
	EXPECT_EQ(4u,byName["home"]->fields[2].offset);
	EXPECT_EQ(4u,byName["home"]->fields[2].size);

	/* The array's width comes from the element, since a BASIC array record
	 * carries no count -- the descriptor holds that at runtime. */
	ASSERT_TRUE(byName.count("nodes") != 0);
	EXPECT_TRUE(byName["nodes"]->isBasicArray);
	EXPECT_EQ("BASIC array of Vtx",byName["nodes"]->typeName);
	EXPECT_EQ(8u,byName["nodes"]->elementSize);
	EXPECT_EQ(0u,byName["nodes"]->valueSize);
	ASSERT_EQ(3u,byName["nodes"]->fields.size());
	EXPECT_EQ("tag",byName["nodes"]->fields[2].name);
}

TEST_F(DebugSymFmtTest, ABasicArrayDescriptorLeadsToTheElements)
{
	/* udtbas.exe stopped at line 29: nodes() is four 8-byte Vtx. Reading a
	 * field early gave the descriptor's own flags instead of the count and
	 * printed one element. */
	static const uint8_t descriptor[16] = {
		0x5E,0x00, 0x3B,0x0C,       /* far pointer 0C3B:005E to the elements */
		0x00,0x00, 0x00,0x00,
		0x01,0x40,                  /* flags */
		0x5E,0x00,                  /* the offset again */
		0x08,0x00,                  /* one element is eight bytes */
		0x04,0x00 };                /* there are four of them */

	BasicArrayDescriptor parsed;
	ASSERT_TRUE(DEBUG_ParseBasicArrayDescriptor(DebugBytes(descriptor,sizeof(descriptor)),8,parsed));
	EXPECT_EQ(0x005Eu,parsed.offset);
	EXPECT_EQ(0x0C3Bu,parsed.segment);
	EXPECT_EQ(8u,parsed.elementSize);
	EXPECT_EQ(4u,parsed.count);

	/* cvprobe.exe's pr_tab: sixteen 2-byte integers. */
	static const uint8_t shorts[16] = {
		0xBE,0x42, 0x20,0x09, 0x00,0x00, 0x00,0x00,
		0x01,0x40, 0xBE,0x42, 0x02,0x00, 0x10,0x00 };

	ASSERT_TRUE(DEBUG_ParseBasicArrayDescriptor(DebugBytes(shorts,sizeof(shorts)),2,parsed));
	EXPECT_EQ(2u,parsed.elementSize);
	EXPECT_EQ(16u,parsed.count);
}

TEST_F(DebugSymFmtTest, WhatIsNotABasicArrayDescriptorIsNotFollowed)
{
	/* The width the type table declares is the guard: without it any bytes
	 * read as a pointer and led the reader somewhere arbitrary. */
	static const uint8_t descriptor[16] = {
		0x5E,0x00, 0x3B,0x0C, 0x00,0x00, 0x00,0x00,
		0x01,0x40, 0x5E,0x00, 0x08,0x00, 0x04,0x00 };

	BasicArrayDescriptor parsed;
	EXPECT_FALSE(DEBUG_ParseBasicArrayDescriptor(DebugBytes(descriptor,sizeof(descriptor)),12,parsed));

	static const uint8_t empty[16] = {
		0x5E,0x00, 0x3B,0x0C, 0x00,0x00, 0x00,0x00,
		0x01,0x40, 0x5E,0x00, 0x08,0x00, 0x00,0x00 };
	EXPECT_FALSE(DEBUG_ParseBasicArrayDescriptor(DebugBytes(empty,sizeof(empty)),8,parsed));

	/* Short of a whole descriptor, there is nothing to read. */
	EXPECT_FALSE(DEBUG_ParseBasicArrayDescriptor(DebugBytes(descriptor,15),8,parsed));
}

TEST_F(DebugSymFmtTest, CodeViewDecodesRealsAndStructuresInALargerProgram)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("qrender-cv.exe").c_str(),0x8240,info));

	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (byName.count(info.symbols[i].name) == 0) byName[info.symbols[i].name] = &info.symbols[i];

	/* BASIC's SINGLE and LONG, and one of qrender's own TYPEs: three
	 * singles, so twelve bytes. */
	ASSERT_TRUE(byName.count("host_accum") != 0);
	EXPECT_EQ("float",byName["host_accum"]->typeName);
	EXPECT_EQ(4u,byName["host_accum"]->valueSize);
	EXPECT_EQ(DEBUG_VALUE_FLOAT,byName["host_accum"]->valueKind);

	ASSERT_TRUE(byName.count("host_ticks") != 0);
	EXPECT_EQ("long",byName["host_ticks"]->typeName);
	EXPECT_EQ(4u,byName["host_ticks"]->valueSize);
	EXPECT_EQ(DEBUG_VALUE_SIGNED,byName["host_ticks"]->valueKind);

	ASSERT_TRUE(byName.count("cam_up") != 0);
	EXPECT_EQ("u3dVector3f",byName["cam_up"]->typeName);
	EXPECT_EQ(12u,byName["cam_up"]->valueSize);

	/* An array reached through a BASIC array, with its width in the name. */
	ASSERT_TRUE(byName.count("mem_tag") != 0);
	EXPECT_EQ("BASIC array of char[12]",byName["mem_tag"]->typeName);
}

TEST_F(DebugSymFmtTest, ACodeViewStructuresFieldsAreAllOfThem)
{
	/* A member record is padded to a four-byte boundary with LF_PADn, which
	 * says how many bytes to skip including itself. Stepping over one byte
	 * lands inside the padding, reads 0x0000 as the next leaf and ends the
	 * list: u3dVector3f came back with x and neither y nor z. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("qrender-cv.exe").c_str(),0x8240,info));

	const DebugSymbol *camera = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].name == "cam_up") camera = &info.symbols[i];
	ASSERT_TRUE(camera != NULL);

	EXPECT_EQ("u3dVector3f",camera->typeName);
	ASSERT_EQ(3u,camera->fields.size());
	EXPECT_EQ("x",camera->fields[0].name);
	EXPECT_EQ(0u,camera->fields[0].offset);
	EXPECT_EQ("y",camera->fields[1].name);
	EXPECT_EQ(4u,camera->fields[1].offset);
	EXPECT_EQ("z",camera->fields[2].name);
	EXPECT_EQ(8u,camera->fields[2].offset);
	for (size_t i = 0;i < 3;i++) {
		EXPECT_EQ("float",camera->fields[i].typeName);
		EXPECT_EQ(4u,camera->fields[i].size);
		EXPECT_EQ(DEBUG_VALUE_FLOAT,camera->fields[i].kind);
	}
}

/* ---- Borland TDINFO ---- */

bool LoadTdInfo(const char *name,std::vector<uint8_t> &data,TdInfo &info)
{
	return LoadFixture(name,data) && DEBUG_ParseBorland(DebugBytes(data.data(),data.size()),info);
}

TEST_F(DebugSymFmtTest, BorlandFindsTdInfoAtTheMzImageEnd)
{
	std::vector<uint8_t> data;
	TdInfo info;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",data,info));

	EXPECT_EQ(8512u,info.base);
	EXPECT_EQ("TDINFO 3.16",info.version);
	ASSERT_EQ(1u,info.modules.size());
	EXPECT_EQ(1u,info.modules[0].index);
	EXPECT_EQ("TDSPROBE",info.modules[0].name);
	EXPECT_TRUE(info.warnings.empty());
}

TEST_F(DebugSymFmtTest, TdInfoSymbolsLandWhereTdsprobeMapPutsThem)
{
	/* The EXE and the MAP come from the same TLINK run, so a differing address
	 * can only be this parser. */
	std::vector<uint8_t> data;
	TdInfo info;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",data,info));

	const std::map<std::string,uint32_t> publics = LoadMapPublics(Fixture("tdsprobe.map"));
	size_t agree = 0;
	size_t disagree = 0;
	for (size_t i = 0;i < info.symbols.size();i++) {
		const TdSymbol &symbol = info.symbols[i];
		const std::map<std::string,uint32_t>::const_iterator found = publics.find(symbol.name);
		if (found == publics.end()) continue;
		if (((uint32_t)symbol.segment << 4) + (uint32_t)symbol.offset == found->second) agree++;
		else disagree++;
	}
	EXPECT_EQ(0u,disagree);
	EXPECT_EQ(46u,agree);
}

TEST_F(DebugSymFmtTest, TheSegmentTableIsReachedByStridingTheTablesBeforeIt)
{
	/* There is no directory: the segment table is found only by stepping over
	 * the symbol, module, source-file, line-number and scope tables at their
	 * own record sizes. A wrong stride decodes garbage here and nowhere else,
	 * which is why the counts either side of it are pinned too. */
	std::vector<uint8_t> data;
	TdInfo info;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",data,info));

	EXPECT_EQ(113u,info.symbols.size());
	EXPECT_EQ(17u,info.lines.size());

	ASSERT_EQ(1u,info.segments.size());
	EXPECT_EQ(1u,info.segments[0].module);
	EXPECT_EQ(419u,info.segments[0].codeSegment);
	EXPECT_EQ(14u,info.segments[0].codeOffset);
	EXPECT_EQ(112u,info.segments[0].codeLength);
}

TEST_F(DebugSymFmtTest, AnAutoSymbolsBpDisplacementIsSigned)
{
	/* Read unsigned, tdsprobe's local `t` comes back as 65534 instead of -2. */
	std::vector<uint8_t> data;
	TdInfo info;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",data,info));

	const TdSymbol *local = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].symbolClass == TD_SYM_AUTO && info.symbols[i].name == "t")
			local = &info.symbols[i];
	ASSERT_TRUE(local != NULL);
	EXPECT_EQ(-2,local->offset);
}

TEST_F(DebugSymFmtTest, BorlandLineRecordsAreLineAndOffsetPairs)
{
	/* tdinfo-parser skips this table as padding, so the layout was derived
	 * from the fixture: every record has to name a statement line of
	 * tdsprobe.c and an offset inside the module's 112 bytes of code. Read
	 * with the fields the other way round, record 0 claims line 14 at offset
	 * 13 -- a plausible-looking pair that is wrong for every record. */
	std::vector<uint8_t> data;
	TdInfo info;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",data,info));

	ASSERT_EQ(17u,info.lines.size());
	EXPECT_EQ(13u,info.lines[0].line);
	EXPECT_EQ(0x0eu,info.lines[0].offset);
	EXPECT_EQ(43u,info.lines[16].line);
	EXPECT_EQ(0x79u,info.lines[16].offset);

	/* The three function entries the .MAP places: tp_sum 0x0E, tp_scale
	 * 0x2B, main 0x44, at the lines they are declared on. */
	std::map<uint16_t,uint16_t> byLine;
	for (size_t i = 0;i < info.lines.size();i++) byLine[info.lines[i].line] = info.lines[i].offset;
	EXPECT_EQ(0x0eu,byLine[13]);
	EXPECT_EQ(0x2bu,byLine[25]);
	EXPECT_EQ(0x44u,byLine[34]);

	ASSERT_EQ(1u,info.sourceFiles.size());
	EXPECT_EQ("TDSPROBE.C",info.sourceFiles[0].name);
}

TEST_F(DebugSymFmtTest, ABorlandAddressReportsTheSourceLineCoveringIt)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("tdsprobe-stripped.exe").c_str(),0x1000,info));
	ASSERT_EQ(17u,info.lines.size());

	const uint32_t code = 0x1a3u << 4;
	const DebugLine *entry = DEBUG_SourceLineAt(info,code + 0x0e);
	ASSERT_TRUE(entry != NULL);
	EXPECT_EQ(13u,entry->line);
	EXPECT_EQ("TDSPROBE.C",entry->file);

	/* One byte before the next record still belongs to the line before it. */
	entry = DEBUG_SourceLineAt(info,code + 0x10);
	ASSERT_TRUE(entry != NULL);
	EXPECT_EQ(13u,entry->line);

	entry = DEBUG_SourceLineAt(info,code + 0x44);
	ASSERT_TRUE(entry != NULL);
	EXPECT_EQ(34u,entry->line);

	/* Past the module's 112 bytes of code nothing covers the address. */
	EXPECT_TRUE(DEBUG_SourceLineAt(info,code + 0x100) == NULL);

	/* A line ends where the next one starts, and the last runs to the end of
	 * the module's code. Stretching every line to that end still answers
	 * lookups correctly, so the ranges are pinned here directly. */
	const DebugLine *first = NULL,*last = NULL;
	for (size_t i = 0;i < info.lines.size();i++) {
		if (info.lines[i].line == 13) first = &info.lines[i];
		if (info.lines[i].line == 43) last = &info.lines[i];
	}
	ASSERT_TRUE(first != NULL && last != NULL);
	EXPECT_EQ(code + 0x0eu,first->imageOffset);
	EXPECT_EQ(code + 0x11u,first->endOffset);
	EXPECT_EQ(code + 14u + 112u,last->endOffset);
}

TEST_F(DebugSymFmtTest, BorlandTypesGiveAVariableItsSizeAndAFunctionItsExtent)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("tdsprobe-stripped.exe").c_str(),0x1000,info));

	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++) byName[info.symbols[i].name] = &info.symbols[i];

	ASSERT_TRUE(byName.count("_g_counter") != 0);
	EXPECT_EQ("int",byName["_g_counter"]->typeName);
	EXPECT_EQ(2u,byName["_g_counter"]->valueSize);

	EXPECT_EQ(DEBUG_VALUE_SIGNED,byName["_g_counter"]->valueKind);
	EXPECT_EQ(0u,byName["_g_counter"]->elementSize);

	ASSERT_TRUE(byName.count("_g_table") != 0);
	EXPECT_EQ("int[8]",byName["_g_table"]->typeName);
	EXPECT_EQ(16u,byName["_g_table"]->valueSize);
	/* An array reads as its element type, eight times over. */
	EXPECT_EQ(DEBUG_VALUE_SIGNED,byName["_g_table"]->valueKind);
	EXPECT_EQ(2u,byName["_g_table"]->elementSize);

	/* A function's extent comes from its scope record, not its type. */
	ASSERT_TRUE(byName.count("_tp_sum") != 0);
	EXPECT_TRUE(byName["_tp_sum"]->isFunction);
	EXPECT_TRUE(byName["_tp_sum"]->hasSize);
	EXPECT_EQ(29u,byName["_tp_sum"]->size);
	EXPECT_EQ(58u,byName["_main"]->size);
}

TEST_F(DebugSymFmtTest, BorlandScopesCarryParametersAndLocals)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("tdsprobe-stripped.exe").c_str(),0x8240,info));

	/* Three functions, each with a block scope inside it. */
	ASSERT_EQ(6u,info.scopes.size());

	const uint32_t code = 0x1a3u << 4;
	const DebugScope *sum = NULL,*sumBlock = NULL,*mainBlock = NULL;
	for (size_t i = 0;i < info.scopes.size();i++) {
		if (info.scopes[i].imageOffset == code + 14) sum = &info.scopes[i];
		if (info.scopes[i].imageOffset == code + 17) sumBlock = &info.scopes[i];
		if (info.scopes[i].imageOffset == code + 75) mainBlock = &info.scopes[i];
	}
	ASSERT_TRUE(sum != NULL && sumBlock != NULL && mainBlock != NULL);

	/* tp_sum's parameter, at [bp+06] the way the far call passes it. */
	EXPECT_EQ("_tp_sum",sum->function);
	ASSERT_EQ(1u,sum->locals.size());
	EXPECT_EQ("n",sum->locals[0].name);
	EXPECT_EQ(DEBUG_STORAGE_FRAME,sum->locals[0].storage);
	EXPECT_EQ(6,sum->locals[0].frameOffset);
	EXPECT_EQ("int",sum->locals[0].typeName);

	/* The block inside it nests, and its two counters live in registers:
	 * total accumulates in CX and i counts in DX. */
	EXPECT_TRUE(sumBlock->parent >= 0);
	EXPECT_EQ(sum,&info.scopes[(size_t)sumBlock->parent]);
	ASSERT_EQ(3u,sumBlock->locals.size());
	EXPECT_EQ("total",sumBlock->locals[0].name);
	EXPECT_EQ(DEBUG_STORAGE_REGISTER,sumBlock->locals[0].storage);
	EXPECT_EQ(1u,sumBlock->locals[0].reg);
	EXPECT_EQ("i",sumBlock->locals[1].name);
	EXPECT_EQ(2u,sumBlock->locals[1].reg);

	/* main keeps t on the stack at [bp-02] and s in SI. */
	ASSERT_EQ(2u,mainBlock->locals.size());
	EXPECT_EQ("t",mainBlock->locals[0].name);
	EXPECT_EQ(-2,mainBlock->locals[0].frameOffset);
	EXPECT_EQ("s",mainBlock->locals[1].name);
	EXPECT_EQ(DEBUG_STORAGE_REGISTER,mainBlock->locals[1].storage);
	EXPECT_EQ(6u,mainBlock->locals[1].reg);
}

TEST_F(DebugSymFmtTest, AStandaloneTdsParsesToTheBlockTdstripRemoved)
{
	std::vector<uint8_t> embeddedData,sidecarData;
	TdInfo embedded,sidecar;
	ASSERT_TRUE(LoadTdInfo("tdsprobe.exe",embeddedData,embedded));
	ASSERT_TRUE(LoadTdInfo("tdsprobe.tds",sidecarData,sidecar));

	EXPECT_EQ(0u,sidecar.base);
	ASSERT_EQ(embedded.symbols.size(),sidecar.symbols.size());
	for (size_t i = 0;i < embedded.symbols.size();i++) {
		EXPECT_EQ(embedded.symbols[i].name,sidecar.symbols[i].name);
		EXPECT_EQ(embedded.symbols[i].segment,sidecar.symbols[i].segment);
		EXPECT_EQ(embedded.symbols[i].offset,sidecar.symbols[i].offset);
		EXPECT_EQ(embedded.symbols[i].symbolClass,sidecar.symbols[i].symbolClass);
		EXPECT_EQ(embedded.symbols[i].type,sidecar.symbols[i].type);
	}
}

TEST_F(DebugSymFmtTest, ParseDebugInfoReadsAStrippedExeThroughItsTdsSidecar)
{
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("tdsprobe-stripped.exe").c_str(),0x1000,info));
	EXPECT_EQ(DEBUG_FORMAT_TDINFO,info.format);
	/* Only static and absolute symbols carry an address worth registering;
	 * this is the count the emulator reports when it loads tdsprobe.exe. */
	EXPECT_EQ(97u,info.symbols.size());

	const DebugSymbol *symbol = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].name == "_main") { symbol = &info.symbols[i]; break; }
	ASSERT_TRUE(symbol != NULL);
	EXPECT_EQ(0x1000u + (0x1a3u << 4) + 0x44u,symbol->linear);
}

TEST_F(DebugSymFmtTest, ACodeViewExeIsNotMistakenForBorlandTdInfo)
{
	/* Both formats put their block at the end of the MZ image, so detection
	 * has to be by magic and nothing else. */
	std::vector<uint8_t> data;
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",data));

	uint64_t base = 0;
	TdInfo info;
	EXPECT_FALSE(DEBUG_FindTdInfoBase(DebugBytes(data.data(),data.size()),base));
	EXPECT_FALSE(DEBUG_ParseBorland(DebugBytes(data.data(),data.size()),info));
}

/* ---- Watcom ---- */

void PutU16(std::vector<uint8_t> &out,size_t at,uint16_t value)
{
	out[at] = (uint8_t)(value & 0xff);
	out[at+1] = (uint8_t)(value >> 8);
}

void PutU32(std::vector<uint8_t> &out,size_t at,uint32_t value)
{
	for (int i = 0;i < 4;i++) out[at+(size_t)i] = (uint8_t)((value >> (8*i)) & 0xff);
}

std::vector<uint8_t> WatcomModuleRecord(const std::string &name)
{
	std::vector<uint8_t> out(21 + name.size(),0);
	out[20] = (uint8_t)name.size();
	for (size_t i = 0;i < name.size();i++) out[21+i] = (uint8_t)name[i];
	return out;
}

/*
 * A hand-built block, not a fixture: there is no OpenWatcom toolchain in this
 * tree to link a real one with. It pins this reader's own arithmetic -- the
 * table order and the V2/V3 mod difference -- and claims nothing about what
 * wlink actually writes.
 */
std::vector<uint8_t> WatcomBlock(bool v2)
{
	const std::string symbolName = "_main";
	const std::vector<uint8_t> first = WatcomModuleRecord("startup.c");
	std::vector<uint8_t> module = first;
	const std::vector<uint8_t> second = WatcomModuleRecord("hello.c");
	module.insert(module.end(),second.begin(),second.end());

	std::vector<uint8_t> symbol((v2 ? 9u : 10u) + symbolName.size(),0);
	PutU32(symbol,0,0x1234);
	PutU16(symbol,4,0x0abc);
	/* The second module: V2 names it by byte offset into the module area, V3
	 * by index. Two modules is what tells the two readings apart -- with one,
	 * offset and index are both 0 and any reading passes. */
	PutU16(symbol,6,v2 ? (uint16_t)first.size() : (uint16_t)1);
	if (v2) {
		symbol[8] = (uint8_t)symbolName.size();
	} else {
		symbol[8] = 0x04;
		symbol[9] = (uint8_t)symbolName.size();
	}
	for (size_t i = 0;i < symbolName.size();i++) symbol[(v2 ? 9u : 10u)+i] = (uint8_t)symbolName[i];

	std::vector<uint8_t> section(18,0);
	section.insert(section.end(),module.begin(),module.end());
	section.insert(section.end(),symbol.begin(),symbol.end());
	section.insert(section.end(),4,0);
	PutU32(section,0,18);
	PutU32(section,4,(uint32_t)(18 + module.size()));
	PutU32(section,8,(uint32_t)(18 + module.size() + symbol.size()));
	PutU32(section,12,(uint32_t)section.size());

	std::vector<uint8_t> master(14,0);
	PutU16(master,0,0x8386);
	master[2] = v2 ? 2 : 3;
	master[4] = 1;
	PutU16(master,6,2);	/* lang_size */
	PutU16(master,8,2);	/* segment_size */
	PutU32(master,10,(uint32_t)(4 + section.size() + 14));

	std::vector<uint8_t> block;
	block.push_back('C');
	block.push_back(0);
	block.insert(block.end(),2,0);
	block.insert(block.end(),section.begin(),section.end());
	block.insert(block.end(),master.begin(),master.end());
	return block;
}

void ExpectWatcomBlockReadsBack(bool v2)
{
	const std::vector<uint8_t> block = WatcomBlock(v2);
	WatInfo info;
	ASSERT_TRUE(DEBUG_ParseWatcom(DebugBytes(block.data(),block.size()),info));

	EXPECT_EQ(v2 ? "WAT 2.0" : "WAT 3.0",info.version);
	ASSERT_EQ(2u,info.modules.size());
	EXPECT_EQ(0,info.modules[0].index);
	EXPECT_EQ("startup.c",info.modules[0].name);
	EXPECT_EQ(1,info.modules[1].index);
	EXPECT_EQ("hello.c",info.modules[1].name);

	ASSERT_EQ(1u,info.symbols.size());
	EXPECT_EQ("_main",info.symbols[0].name);
	EXPECT_EQ(0x1234u,info.symbols[0].offset);
	EXPECT_EQ(0x0abcu,info.symbols[0].segment);
	EXPECT_EQ(1,info.symbols[0].moduleIndex);
	EXPECT_EQ(v2 ? 0 : 0x04,info.symbols[0].kind);
}

TEST_F(DebugSymFmtTest, WatcomV3GlobalsResolveTheirModule)
{
	ExpectWatcomBlockReadsBack(false);
}

TEST_F(DebugSymFmtTest, WatcomV2GlobalsResolveTheirModule)
{
	ExpectWatcomBlockReadsBack(true);
}

TEST_F(DebugSymFmtTest, ACodeViewExeIsNotMistakenForWatcomDebugInfo)
{
	/* Watcom's master header is the last 14 bytes; CodeView's trailer is the
	 * last 8, so both readers look at overlapping bytes and only the signature
	 * separates them. */
	const char * const files[2] = {"qrender-cv.exe","cvprobe.exe"};
	for (int i = 0;i < 2;i++) {
		std::vector<uint8_t> data;
		ASSERT_TRUE(LoadFixture(files[i],data));
		WatInfo info;
		EXPECT_FALSE(DEBUG_ParseWatcom(DebugBytes(data.data(),data.size()),info));
	}
}

}

TEST_F(DebugSymFmtTest, ABorlandStructIsPlacedFieldByField)
{
	/* udtprobe.c's `struct Point { int x; int y; long tag; }` and its
	 * `path[4]`. A member record carries no offset, so the fields are laid
	 * out in declaration order and only reported when they add up to the
	 * size the type itself declares. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("udtprobe.exe").c_str(),0x8240,info));

	std::map<std::string,const DebugSymbol*> byName;
	for (size_t i = 0;i < info.symbols.size();i++) byName[info.symbols[i].name] = &info.symbols[i];

	ASSERT_TRUE(byName.count("_origin") != 0);
	const DebugSymbol *origin = byName["_origin"];
	EXPECT_EQ("Point",origin->typeName);
	EXPECT_EQ(8u,origin->valueSize);
	ASSERT_EQ(3u,origin->fields.size());
	EXPECT_EQ("x",origin->fields[0].name);
	EXPECT_EQ(0u,origin->fields[0].offset);
	EXPECT_EQ(2u,origin->fields[0].size);
	EXPECT_EQ("y",origin->fields[1].name);
	EXPECT_EQ(2u,origin->fields[1].offset);
	EXPECT_EQ("tag",origin->fields[2].name);
	EXPECT_EQ(4u,origin->fields[2].offset);
	EXPECT_EQ("long",origin->fields[2].typeName);
	EXPECT_EQ(4u,origin->fields[2].size);

	/* An array of them repeats those fields at its own stride. */
	ASSERT_TRUE(byName.count("_path") != 0);
	const DebugSymbol *path = byName["_path"];
	EXPECT_EQ("Point[4]",path->typeName);
	EXPECT_EQ(32u,path->valueSize);
	EXPECT_EQ(8u,path->elementSize);
	ASSERT_EQ(3u,path->fields.size());
	EXPECT_EQ("tag",path->fields[2].name);

	/* sum_x takes its array as a far pointer, the large model's default. */
	const DebugLocal *parameter = NULL;
	for (size_t i = 0;i < info.scopes.size();i++)
		for (size_t k = 0;k < info.scopes[i].locals.size();k++)
			if (info.scopes[i].locals[k].name == "p") parameter = &info.scopes[i].locals[k];
	ASSERT_TRUE(parameter != NULL);
	EXPECT_EQ("far ptr to Point",parameter->typeName);
	EXPECT_EQ(4u,parameter->valueSize);

	/* main's own `struct Point local` is a frame variable with the same
	 * fields, reached through its scope rather than the symbol table. */
	const DebugLocal *local = NULL;
	for (size_t i = 0;i < info.scopes.size();i++)
		for (size_t k = 0;k < info.scopes[i].locals.size();k++)
			if (info.scopes[i].locals[k].name == "local") local = &info.scopes[i].locals[k];
	ASSERT_TRUE(local != NULL);
	EXPECT_EQ("Point",local->typeName);
	EXPECT_EQ(DEBUG_STORAGE_FRAME,local->storage);
	ASSERT_EQ(3u,local->fields.size());
	EXPECT_EQ("tag",local->fields[2].name);
}

TEST_F(DebugSymFmtTest, ABorlandStructWithAHoleReportsNoFieldPlaces)
{
	/* udtalign.c is the same shape compiled with word alignment: `struct
	 * Padded { char c; long v; }` is 6 bytes, and its member records account
	 * for 5 -- the hole after c is a member record with info 0x40 and no
	 * type, which says nothing about how wide it is. Laying the fields out
	 * in declaration order would put v at 1 instead of 2, so the type is
	 * named and its places are left out. */
	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(Fixture("udtalign.exe").c_str(),0x8240,info));

	const DebugSymbol *padded = NULL;
	for (size_t i = 0;i < info.symbols.size();i++)
		if (info.symbols[i].name == "_padded") padded = &info.symbols[i];
	ASSERT_TRUE(padded != NULL);

	EXPECT_EQ("Padded",padded->typeName);
	EXPECT_EQ(6u,padded->valueSize);
	EXPECT_TRUE(padded->fields.empty());
}
