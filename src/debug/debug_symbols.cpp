/*
 * debug_symbols.cpp - reading a program's own debug info as the DOS loader
 * hands it over, and registering it in the symbol store.
 */

#include "debug_symbols.h"

#if C_DEBUG

#include "logging.h"
#include "dos_inc.h"

static bool ReadGuestFile(const char *name,std::vector<uint8_t> &out,uint32_t maxBytes)
{
	uint16_t handle = 0;
	if (!DOS_OpenFile(name,OPEN_READ,&handle)) return false;

	uint32_t size = 0;
	if (!DOS_SeekFile(handle,&size,DOS_SEEK_END) || size == 0 || size > maxBytes) {
		DOS_CloseFile(handle);
		return false;
	}

	uint32_t pos = 0;
	DOS_SeekFile(handle,&pos,DOS_SEEK_SET);

	out.resize(size);
	uint32_t done = 0;
	while (done < size) {
		uint16_t want = (uint16_t)((size - done) > 0x8000u ? 0x8000u : (size - done));
		if (!DOS_ReadFile(handle,&out[done],&want) || want == 0) break;
		done += want;
	}
	DOS_CloseFile(handle);

	out.resize(done);
	return done == size;
}

/* A file beside the program with the same stem: the block TDSTRIP moved out
 * into a .TDS, or the .MAP its linker wrote. */
static bool ReadSidecar(const char *program,const char *extension,std::vector<uint8_t> &out)
{
	const std::string path = program;
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return false;
	if (path.find_first_of("/\\",dot) != std::string::npos) return false;

	return ReadGuestFile((path.substr(0,dot) + extension).c_str(),out,16u*1024u*1024u);
}

/* True when the file has bytes past the load image, which is where every
 * appended format puts its block. Reading 32 bytes settles it. */
static bool HasAppendedData(const char *program)
{
	uint16_t handle = 0;
	if (!DOS_OpenFile(program,OPEN_READ,&handle)) return false;

	uint8_t header[32];
	uint16_t got = sizeof(header);
	const bool read = DOS_ReadFile(handle,header,&got) && got == sizeof(header);

	uint32_t size = 0;
	const bool sized = DOS_SeekFile(handle,&size,DOS_SEEK_END);
	DOS_CloseFile(handle);
	if (!read || !sized) return false;

	MzHeader mz;
	if (!DEBUG_ParseMzHeader(DebugBytes(header,sizeof(header)),mz)) return false;
	return size > DEBUG_MzAppendedOffset(mz);
}

void DEBUG_SymbolsOnProgramLoad(const char *program,bool isCom,uint16_t loadSeg,uint16_t psp,uint32_t imageBytes)
{
	if (program == NULL) return;

	/* Opening files here must leave no trace: the guest reads the DOS error
	 * code after EXEC returns, and a missing sidecar would overwrite it. */
	const uint16_t saved_errorcode = dos.errorcode;
	const uint32_t loadLinear = (uint32_t)loadSeg << 4u;

	std::vector<uint8_t> data;
	DebugInfo info;
	bool found = false;

	/* A COM image has no header saying where its load image stops, so no
	 * appended format can be found in one. */
	if (!isCom) {
		if (HasAppendedData(program) && ReadGuestFile(program,data,64u*1024u*1024u))
			found = DEBUG_ParseDebugInfoBytes(DebugBytes(data.data(),data.size()),program,loadLinear,info);

		if (!found && ReadSidecar(program,".TDS",data))
			found = DEBUG_ParseDebugInfoBytes(DebugBytes(data.data(),data.size()),program,loadLinear,info);
	}

	/* Debug info that names nothing does not shadow a .MAP that names
	 * something: BC 4.5 and LINK 3.69 append NB00, which is read no further
	 * than its directory. */
	if (found && info.symbols.empty() && info.lines.empty()) {
		LOG_MSG("DEBUG: %s carries %s with no symbols read; trying its .MAP",program,info.version.c_str());
		found = false;
	}

	if (found) {
		DEBUG_Symbols().ClearProgram(program);
		DEBUG_Symbols().AddDebugInfo(info,program);
		/* The layout is the linker's: its .MAP lists every segment with its
		 * class, where debug info may list some -- CV3 one per module. */
		LinkMapFile map;
		std::vector<DebugSegment> layout = info.segments;
		if (ReadSidecar(program,".MAP",data)) {
			DEBUG_ParseLinkMap(std::string((const char*)data.data(),data.size()),program,map);
			if (!map.segments.empty()) layout = DEBUG_LinkMapSegments(map);
		}
		DEBUG_Symbols().AddSegments(layout,loadLinear,imageBytes,program,psp);
		LOG_MSG("DEBUG: %s carries %u %s symbols, loaded at segment %04X",
		        program,(unsigned int)info.symbols.size(),info.version.c_str(),loadSeg);
		dos.errorcode = saved_errorcode;
		return;
	}

	/* A .MAP names far less -- publics only, no modules, sizes or lines -- so
	 * it is what to fall back to, never what to prefer.
	 *
	 * Only for an EXE: a COM .MAP's addresses are written relative to
	 * whatever origin its toolchain chose, and no COM fixture was available
	 * to find out which. Guessing would put every symbol 0x100 out. */
	if (!isCom && ReadSidecar(program,".MAP",data)) {
		LinkMapFile map;
		DEBUG_ParseLinkMap(std::string((const char*)data.data(),data.size()),program,map);
		if (!map.publics.empty() || !map.segments.empty()) {
			DEBUG_Symbols().ClearProgram(program);
			const size_t before = DEBUG_Symbols().Size();
			DEBUG_Symbols().AddLinkMap(map,loadLinear,program);
			DEBUG_Symbols().AddSegments(DEBUG_LinkMapSegments(map),loadLinear,imageBytes,program,psp);
			LOG_MSG("DEBUG: %s has no appended debug info; its .MAP gives %u symbols at segment %04X",
			        program,(unsigned int)(DEBUG_Symbols().Size() - before),loadSeg);
		}
	}

	dos.errorcode = saved_errorcode;
}

#endif
