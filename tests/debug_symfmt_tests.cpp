/*
 *  Unit tests for the appended-debug-info readers in src/debug/debug_symfmt*.
 *
 *  Every expected number here was produced by the TypeScript readers in
 *  mcp/src, which were checked against the .MAP files LINK wrote for the same
 *  fixtures. They are the port's oracle: a C++ reader that disagrees with one
 *  of them disagrees with the linker.
 */

#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/debug/debug_symfmt.h"

namespace {

using namespace dbgsym;

/* The fixtures live in the source tree, and the test binary is the emulator,
 * started from wherever the user happens to be. */
std::string FixtureDir() {
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

std::string Fixture(const char *name) {
	const std::string dir = FixtureDir();
	return dir.empty() ? std::string() : dir + "/" + name;
}

bool LoadFixture(const char *name,std::vector<uint8_t> &out) {
	const std::string path = Fixture(name);
	return !path.empty() && ReadHostFile(path.c_str(),out);
}

size_t CountKind(const CvInfo &info,CvSymbolKind kind) {
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
	const Bytes bytes(data.data(),data.size());
	ASSERT_TRUE(ParseMzImage(bytes,image));
	EXPECT_EQ(484868u,image.appendedOffset);
	EXPECT_EQ("NB08",bytes.latin1((size_t)image.appendedOffset,(size_t)image.appendedOffset + 4));
}

TEST_F(DebugSymFmtTest, ParseMzHeaderRejectsAFileThatIsNotAnMzImage)
{
	const std::string text = "not an exe at all, but long enough";
	MzHeader header;
	EXPECT_FALSE(ParseMzHeader(Bytes((const uint8_t*)text.data(),text.size()),header));
}

TEST_F(DebugSymFmtTest, MzFingerprintSeparatesTwoProgramsOfTheSameName)
{
	std::vector<uint8_t> probeData,qrenderData;
	ASSERT_TRUE(LoadFixture("cvprobe.exe",probeData));
	ASSERT_TRUE(LoadFixture("qrender-cv.exe",qrenderData));

	MzHeader probe,qrender;
	ASSERT_TRUE(ParseMzHeader(Bytes(probeData.data(),probeData.size()),probe));
	ASSERT_TRUE(ParseMzHeader(Bytes(qrenderData.data(),qrenderData.size()),qrender));
	EXPECT_NE(MzFingerprint(probe),MzFingerprint(qrender));
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
	ASSERT_TRUE(ParseMzImage(Bytes(data.data(),data.size()),image));
	EXPECT_EQ(32u,image.imageOffset);
	EXPECT_EQ(992u,image.imageSize);
	EXPECT_EQ(1024u,image.appendedOffset);
}

TEST_F(DebugSymFmtTest, ReadCodeViewFindsTheAppendedBlockThroughTheEofTrailer)
{
	CvInfo info;
	ASSERT_TRUE(ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));
	EXPECT_EQ("NB08",info.signature);
	EXPECT_EQ(19560u,info.base);
	EXPECT_EQ(61u,info.directory.size());
	EXPECT_TRUE(info.warnings.empty());
}

TEST_F(DebugSymFmtTest, ReadCodeViewReadsModulesPublicsSegmentsAndLineTables)
{
	CvInfo info;
	ASSERT_TRUE(ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));

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
	ASSERT_TRUE(ReadCodeViewFile(Fixture("cvprobe.exe").c_str(),info));

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

	const std::vector<CvSymbol> parsed = ParseCvSymbolRun(Bytes(body,sizeof(body)),1,0,sizeof(body));
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

	const std::vector<CvSymbol> parsed = ParseCvSymbolRun(Bytes(body,sizeof(body)),1,0,sizeof(body));
	ASSERT_EQ(1u,parsed.size());
	EXPECT_EQ("wid",parsed[0].name);
	EXPECT_EQ(9u,parsed[0].segment);
	EXPECT_EQ(0x12345678u,parsed[0].offset);
	EXPECT_EQ(CV_SYM_PUBLIC,parsed[0].kind);
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
	ASSERT_TRUE(FindCvBase(Bytes(data.data(),data.size()),base,signature));
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
	ASSERT_TRUE(ParseCodeView(Bytes(data.data(),data.size()),info));
	EXPECT_TRUE(info.directory.empty());
	ASSERT_FALSE(info.warnings.empty());
	EXPECT_NE(std::string::npos,info.warnings[0].find("past end of file"));
}

TEST_F(DebugSymFmtTest, ReadCodeViewScalesToARealMediumModelProgram)
{
	CvInfo info;
	ASSERT_TRUE(ReadCodeViewFile(Fixture("qrender-cv.exe").c_str(),info));
	EXPECT_EQ("NB08",info.signature);
	EXPECT_EQ(304u,info.modules.size());
	EXPECT_EQ(282u,info.segments.size());
	EXPECT_EQ(2104u,CountKind(info,CV_SYM_PUBLIC));
	EXPECT_EQ(245u,CountKind(info,CV_SYM_PROC));
	EXPECT_EQ(6u,CountKind(info,CV_SYM_LABEL));
	EXPECT_TRUE(info.warnings.empty());
}

}
