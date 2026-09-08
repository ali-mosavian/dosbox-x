/*
 *  Unit tests for the LINK .MAP reader and the symbol store.
 *
 *  Both test files are #included into one translation unit, so file-local
 *  helpers here must not share a name with those in debug_symfmt_tests.cpp.
 *
 *  The expected values come from the TypeScript readers in mcp/src, which
 *  were checked against real linker output.
 */

#include <ctype.h>
#include <stdlib.h>

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/debug/debug_symfmt.h"
#include "../src/debug/debug_symstore.h"

namespace {

const char * const SAMPLE_MAP =
	"\n"
	" Start  Stop   Length Name                   Class\n"
	" 00000H 0024BH 0024CH D32TEST_CODE           BC_CODE\n"
	" 00250H 00EA1H 00C52H D32WRAP                CODE\n"
	" 00EA2H 04461H 035C0H CODE                   CODE\n"
	" 04470H 0499FH 00530H _TEXT                  CODE\n"
	" 0517CH 0669FH 01524H _DATA                  DATA\n"
	"\n"
	" Origin   Group\n"
	" 050E:0   DGROUP\n"
	" 04A7:0   FMGROUP\n"
	"\n"
	"  Address         Publics by Value\n"
	"\n"
	" 0447:043C       _main\n"
	" 0000:0250       _wrap_start\n"
	"\n"
	"Program entry point at 0447:043C\n";

std::string MapFixtureDir()
{
	const char *env = getenv("DOSBOX_TEST_FIXTURES");
	if (env != NULL && *env != 0) return std::string(env);

	std::string prefix = ".";
	for (int up = 0;up < 6;up++) {
		const std::string dir = prefix + "/mcp/test/fixtures";
		FILE *probe = fopen((dir + "/qrender-cv.map").c_str(),"rb");
		if (probe != NULL) {
			fclose(probe);
			return dir;
		}
		prefix += "/..";
	}
	return std::string();
}

std::string MapFixture(const char *name)
{
	const std::string dir = MapFixtureDir();
	return dir.empty() ? std::string() : dir + "/" + name;
}

LinkMapFile SampleMap()
{
	LinkMapFile map;
	DEBUG_ParseLinkMap(SAMPLE_MAP,"sample.map",map);
	return map;
}

DebugSymbol MakeSymbol(const char *name,uint32_t linear,uint32_t size,const char *module)
{
	DebugSymbol symbol;
	symbol.name = name;
	symbol.linear = linear;
	if (size != 0) {
		symbol.size = size;
		symbol.hasSize = true;
	}
	if (module != NULL) symbol.module = module;
	return symbol;
}

class DebugSymbolsTest : public ::testing::Test {};

TEST_F(DebugSymbolsTest, ParseLinkMapReadsSegmentsGroupsPublicsAndEntryPoint)
{
	const LinkMapFile map = SampleMap();

	ASSERT_EQ(5u,map.segments.size());
	EXPECT_EQ("D32WRAP",map.segments[1].name);
	EXPECT_EQ(0x250u,map.segments[1].start);

	ASSERT_FALSE(map.groups.empty());
	EXPECT_EQ("DGROUP",map.groups[0].name);
	EXPECT_EQ(0x50e0u,map.groups[0].address.mapOffset);

	const std::map<std::string,LinkMapPublic>::const_iterator main = map.publics.find("_main");
	ASSERT_TRUE(main != map.publics.end());
	EXPECT_EQ(0x48acu,main->second.address.mapOffset);

	ASSERT_TRUE(map.hasEntryPoint);
	EXPECT_EQ(0x48acu,map.entryPoint.mapOffset);
}

TEST_F(DebugSymbolsTest, ACrLfMapParsesTheSameAsALfOne)
{
	/* LINK writes CRLF. With the \r left on, every public was keyed under a
	 * name ending in it and nothing resolved: 0 of 715 cvprobe publics. */
	std::string crlf;
	for (const char *p = SAMPLE_MAP;*p != 0;p++) {
		if (*p == '\n') crlf += '\r';
		crlf += *p;
	}

	LinkMapFile map;
	DEBUG_ParseLinkMap(crlf,"sample.map",map);

	ASSERT_EQ(5u,map.segments.size());
	EXPECT_EQ("_TEXT",map.segments[3].name);
	EXPECT_TRUE(map.publics.find("_wrap_start") != map.publics.end());
	EXPECT_TRUE(map.hasEntryPoint);
}

TEST_F(DebugSymbolsTest, ResolveLinkMapSymbolReadsSegmentPlusOffsetSpecs)
{
	const LinkMapFile map = SampleMap();
	LinkMapResolution resolved;
	std::string error;

	ASSERT_TRUE(DEBUG_ResolveLinkMapSymbol(map,"D32WRAP+0x250",0x12340,resolved,error)) << error;
	EXPECT_EQ(0x12340u + 0x250u + 0x250u,resolved.linear);
	EXPECT_EQ(0x4a0u,resolved.mapOffset);
}

TEST_F(DebugSymbolsTest, ResolveLinkMapSymbolReadsColonSpecsPublicsAndEntry)
{
	const LinkMapFile map = SampleMap();
	LinkMapResolution resolved;
	std::string error;

	ASSERT_TRUE(DEBUG_ResolveLinkMapSymbol(map,"D32WRAP:0",0x20000,resolved,error)) << error;
	EXPECT_EQ(0x20250u,resolved.linear);

	ASSERT_TRUE(DEBUG_ResolveLinkMapSymbol(map,"_main",0x20000,resolved,error)) << error;
	EXPECT_EQ(0x248acu,resolved.linear);

	ASSERT_TRUE(DEBUG_ResolveLinkMapSymbol(map,"entry",0x20000,resolved,error)) << error;
	EXPECT_EQ(0x248acu,resolved.linear);

	EXPECT_FALSE(DEBUG_ResolveLinkMapSymbol(map,"nosuchthing",0x20000,resolved,error));
	EXPECT_NE(std::string::npos,error.find("nosuchthing"));
}

TEST_F(DebugSymbolsTest, DescribeMapAddressNamesThePublicOrTheSegment)
{
	const LinkMapFile map = SampleMap();
	std::string text;

	ASSERT_TRUE(DEBUG_DescribeMapAddress(map,0x20000,0x20250,text));
	EXPECT_EQ("_wrap_start",text);

	ASSERT_TRUE(DEBUG_DescribeMapAddress(map,0x20000,0x204a0,text));
	EXPECT_EQ("D32WRAP+0x00000250",text);
}

TEST_F(DebugSymbolsTest, TheStoreResolvesExactNamesAndBoundsBySize)
{
	DebugSymbolStore store;
	store.Add(MakeSymbol("d32x_thunk16",0x123450,0x40,NULL));

	const DebugSymbol *found = store.Resolve("d32x_thunk16");
	ASSERT_TRUE(found != NULL);
	EXPECT_EQ(0x123450u,found->linear);

	EXPECT_EQ("d32x_thunk16+0x00000003",store.Describe(0x123453));
	/* 0x40 past a symbol of size 0x40 is not inside it. */
	EXPECT_EQ("",store.Describe(0x123490));
}

TEST_F(DebugSymbolsTest, ASizelessSymbolDoesNotReachOutsideItsOwnSegment)
{
	/* Every format but CodeView's procs gives no size, and unbounded the
	 * lowest symbol answers for the whole address space: asking where the CPU
	 * was in the BIOS returned cvprobe.exe's _end+0xF06E6. */
	DebugSymbolStore store;
	store.Add(MakeSymbol("_end",0x8000,0,NULL));

	EXPECT_EQ("_end+0x0000FFFF",store.Describe(0x8000 + 0xffff));
	EXPECT_EQ("",store.Describe(0x8000 + 0x10000));
	EXPECT_EQ("",store.Describe(0xfd186));
}

TEST_F(DebugSymbolsTest, TheStoreResolvesModuleQualifiedNamesCaseInsensitively)
{
	DebugSymbolStore store;
	store.Add(MakeSymbol("loader_body",0x810000,0,"libc.d32"));

	const DebugSymbol *bare = store.Resolve("libc!loader_body");
	ASSERT_TRUE(bare != NULL);
	EXPECT_EQ(0x810000u,bare->linear);

	const DebugSymbol *full = store.Resolve("LIBC.D32!loader_body");
	ASSERT_TRUE(full != NULL);
	EXPECT_EQ(0x810000u,full->linear);

	EXPECT_TRUE(store.Resolve("missing!loader_body") == NULL);
}

TEST_F(DebugSymbolsTest, ClearProgramDropsOnlyThatProgramsSymbols)
{
	DebugSymbolStore store;
	DebugSymbol first = MakeSymbol("stale",0x1000,0,NULL);
	first.program = "OLD.EXE";
	DebugSymbol second = MakeSymbol("kept",0x2000,0,NULL);
	second.program = "OTHER.EXE";
	store.Add(first);
	store.Add(second);

	store.ClearProgram("OLD.EXE");
	EXPECT_EQ(1u,store.Size());
	EXPECT_TRUE(store.Resolve("stale") == NULL);
	EXPECT_TRUE(store.Resolve("kept") != NULL);
}

TEST_F(DebugSymbolsTest, TheStoreAnswersSourceLinesInLinearAddresses)
{
	if (MapFixtureDir().empty()) GTEST_SKIP() << "mcp/test/fixtures not found; set DOSBOX_TEST_FIXTURES";

	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(MapFixture("cvprobe.exe").c_str(),0x12340,info));

	DebugSymbolStore store;
	store.AddDebugInfo(info,"CVPROBE.EXE");
	EXPECT_EQ(info.lines.size(),store.LineCount());

	/* cvprobe.bas ends on line 37, covering 301..316 in load-relative bytes. */
	const DebugSourceLine *last = store.LineAt(0x12340 + 315);
	ASSERT_TRUE(last != NULL);
	EXPECT_EQ("cvprobe.bas",last->file);
	EXPECT_EQ(37u,last->line);
	EXPECT_EQ(0x12340u + 301u,last->begin);

	EXPECT_TRUE(store.LineAt(0x12340 + 316) == NULL);
	/* The entry point is in the runtime, a module with no line info. */
	EXPECT_TRUE(store.LineAt(0x12340 + 0x2dbc) == NULL);

	store.ClearProgram("CVPROBE.EXE");
	EXPECT_EQ(0u,store.LineCount());
}

TEST_F(DebugSymbolsTest, AddLinkMapRegistersEachPublicOnce)
{
	if (MapFixtureDir().empty()) GTEST_SKIP() << "mcp/test/fixtures not found; set DOSBOX_TEST_FIXTURES";

	LinkMapFile map;
	ASSERT_TRUE(DEBUG_ReadLinkMapFile(MapFixture("qrender-cv.map").c_str(),map));

	DebugSymbolStore store;
	store.AddLinkMap(map,0x12340,"QRENDER.EXE");

	/* The map keys every public twice, as written and upper-cased, so a store
	 * that took both would hold 4174 symbols and report each one twice. */
	EXPECT_EQ(2087u,store.Size());

	const DebugSymbol *found = store.Resolve("R_DRAW_WORLD");
	ASSERT_TRUE(found != NULL);
	EXPECT_EQ(0x12340u + 0x1041eu,found->linear);
}

TEST_F(DebugSymbolsTest, TheStoreDescribesAnAddressInsideARealProgram)
{
	if (MapFixtureDir().empty()) GTEST_SKIP() << "mcp/test/fixtures not found; set DOSBOX_TEST_FIXTURES";

	DebugInfo info;
	ASSERT_TRUE(DEBUG_ParseDebugInfo(MapFixture("cvprobe.exe").c_str(),0x12340,info));

	DebugSymbolStore store;
	store.AddDebugInfo(info,"CVPROBE.EXE");
	/* The count the emulator reports when it loads this program for real. */
	EXPECT_EQ(725u,store.Size());
	EXPECT_EQ(info.symbols.size(),store.Size());

	const DebugSymbol *add = store.Resolve("pr_add");
	ASSERT_TRUE(add != NULL);
	EXPECT_EQ("cvprobe.obj:pr_add+0x00000003",store.Describe(add->linear + 3));
}

}
