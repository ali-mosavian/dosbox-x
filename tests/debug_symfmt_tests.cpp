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
 * A LINK .MAP publics reader, for this test only.
 *
 * The .MAP and the EXE come from the same LINK invocation, so a CodeView
 * address that differs from it is the CodeView reader being wrong -- there is
 * nothing else it could be. This mirrors mcp/src/linkmap.ts, including its
 * reading of the "Abs" class column as the symbol name: absolute symbols
 * therefore never match by name, which suits this comparison, since CodeView
 * keeps them in segment 0 and sstSegMap never describes it.
 */

std::string Upper(const std::string &text)
{
	std::string out = text;
	for (size_t i = 0;i < out.size();i++) out[i] = (char)toupper((unsigned char)out[i]);
	return out;
}

bool ParseMapAddressRow(const std::string &line,uint32_t &mapOffset,std::string &name)
{
	size_t at = line.find_first_not_of(" \t");
	if (at == std::string::npos) return false;

	uint32_t value[2] = {0,0};
	for (int part = 0;part < 2;part++) {
		size_t digits = 0;
		while (at < line.size() && isxdigit((unsigned char)line[at])) {
			value[part] = (value[part] << 4) + (uint32_t)(isdigit((unsigned char)line[at])
				? line[at] - '0'
				: (toupper((unsigned char)line[at]) - 'A' + 10));
			at++;
			digits++;
		}
		if (digits == 0) return false;
		if (part == 0) {
			if (at >= line.size() || line[at] != ':') return false;
			at++;
		}
	}

	if (at >= line.size() || !isspace((unsigned char)line[at])) return false;
	at = line.find_first_not_of(" \t",at);
	if (at == std::string::npos) return false;

	const size_t end = line.find_first_of(" \t",at);
	name = line.substr(at,end == std::string::npos ? std::string::npos : end - at);
	mapOffset = (value[0] << 4) + value[1];
	return true;
}

std::map<std::string,uint32_t> LoadMapPublics(const std::string &path)
{
	std::map<std::string,uint32_t> publics;
	std::vector<uint8_t> raw;
	if (!DEBUG_ReadHostFile(path.c_str(),raw)) return publics;

	std::string text((const char*)raw.data(),raw.size());
	bool inPublics = false;
	size_t at = 0;
	while (at <= text.size()) {
		const size_t eol = text.find('\n',at);
		std::string line = text.substr(at,eol == std::string::npos ? std::string::npos : eol - at);
		at = eol == std::string::npos ? text.size() + 1 : eol + 1;
		while (!line.empty() && (line[line.size()-1] == '\r' || line[line.size()-1] == ' ')) line.erase(line.size()-1);

		const std::string upper = Upper(line);
		if (upper.find("PUBLICS BY VALUE") != std::string::npos) { inPublics = true; continue; }
		if (upper.find("PROGRAM ENTRY POINT AT") != std::string::npos) { inPublics = false; continue; }

		uint32_t mapOffset = 0;
		std::string name;
		if (!inPublics || !ParseMapAddressRow(line,mapOffset,name)) continue;

		if (publics.find(name) == publics.end()) publics[name] = mapOffset;
		const std::string key = Upper(name);
		if (publics.find(key) == publics.end()) publics[key] = mapOffset;
	}
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

}
