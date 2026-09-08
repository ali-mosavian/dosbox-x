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

/* The block TDSTRIP moved out of the EXE sits in a .TDS beside it. */
static bool ReadTdsSidecar(const char *program,std::vector<uint8_t> &out)
{
	const std::string path = program;
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return false;
	if (path.find_first_of("/\\",dot) != std::string::npos) return false;

	return ReadGuestFile((path.substr(0,dot) + ".TDS").c_str(),out,16u*1024u*1024u);
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

void DEBUG_SymbolsOnProgramLoad(const char *program,bool isCom,uint16_t loadSeg)
{
	/* No appended debug format exists for a COM image: it has no header to
	 * say where the load image stops. */
	if (program == NULL || isCom) return;

	/* Opening files here must leave no trace: the guest reads the DOS error
	 * code after EXEC returns, and a missing .TDS would overwrite it. */
	const uint16_t saved_errorcode = dos.errorcode;

	std::vector<uint8_t> data;
	DebugInfo info;
	bool found = false;

	if (HasAppendedData(program) && ReadGuestFile(program,data,64u*1024u*1024u))
		found = DEBUG_ParseDebugInfoBytes(DebugBytes(data.data(),data.size()),program,
		                                 (uint32_t)loadSeg << 4u,info);

	if (!found && ReadTdsSidecar(program,data))
		found = DEBUG_ParseDebugInfoBytes(DebugBytes(data.data(),data.size()),program,
		                                 (uint32_t)loadSeg << 4u,info);

	dos.errorcode = saved_errorcode;
	if (!found) return;

	DEBUG_Symbols().ClearProgram(program);
	DEBUG_Symbols().AddDebugInfo(info,program);
	LOG_MSG("DEBUG: %s carries %u %s symbols, loaded at segment %04X",
	        program,(unsigned int)info.symbols.size(),info.version.c_str(),loadSeg);
}

#endif
