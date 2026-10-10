/*
 * debug_symbols.cpp - reading a program's own debug info as the DOS loader
 * hands it over, and registering it in the symbol store.
 */

#include "debug_symbols.h"

#if C_DEBUG

#include "logging.h"
#include "dos_inc.h"
#include "mem.h"
#include "paging.h"
#include "cpu.h"
#include "debug_image.h"

#include <set>
#include <string.h>

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

/* What a program brought: its own bytes and the .MAP its linker wrote. */
struct ProgramFiles {
	std::vector<uint8_t> image;
	std::vector<uint8_t> map;
	bool hasMap = false;
};

/* Registers a program's debug info, or failing that its .MAP, at the address
 * it was loaded to. A real-mode image has one load address and no placement;
 * a protected-mode one has loadLinear 0 and says where each object went. */
static void RegisterSymbols(const char *program,bool isCom,const ProgramFiles &files,uint32_t loadLinear,
                            const DebugPlacement *placement,uint16_t psp,uint32_t imageBytes)
{
	DebugInfo info;
	bool found = false;

	/* A COM image has no header saying where its load image stops, so no
	 * appended format can be found in one. */
	if (!isCom) {
		if (!files.image.empty())
			found = DEBUG_ParseDebugInfoBytes(DebugBytes(files.image.data(),files.image.size()),program,loadLinear,info,placement);

		if (!found) {
			std::vector<uint8_t> sidecar;
			if (ReadSidecar(program,".TDS",sidecar))
				found = DEBUG_ParseDebugInfoBytes(DebugBytes(sidecar.data(),sidecar.size()),program,loadLinear,info,placement);
		}
	}

	/* Debug info that names nothing does not shadow a .MAP that names
	 * something: BC 4.5 and LINK 3.69 append NB00, which is read no further
	 * than its directory. */
	if (found && info.symbols.empty() && info.lines.empty()) {
		LOG_MSG("DEBUG: %s carries %s with no symbols read; trying its .MAP",program,info.version.c_str());
		found = false;
	}

	LinkMapFile map;
	if (files.hasMap && !isCom) {
		DEBUG_ParseLinkMap(std::string((const char*)files.map.data(),files.map.size()),program,map);
		if (placement != NULL) DEBUG_PlaceLinkMap(map,*placement);
	}

	if (found) {
		DEBUG_Symbols().ClearProgram(program);
		DEBUG_Symbols().AddDebugInfo(info,program);
		/* The layout is the linker's: its .MAP lists every segment with its
		 * class, where debug info may list some -- CV3 one per module. */
		std::vector<DebugSegment> layout = info.segments;
		if (!map.segments.empty()) layout = DEBUG_LinkMapSegments(map);
		DEBUG_Symbols().AddSegments(layout,loadLinear,imageBytes,program,psp);
		LOG_MSG("DEBUG: %s carries %u %s symbols, loaded at %05X",
		        program,(unsigned int)info.symbols.size(),info.version.c_str(),(unsigned int)loadLinear);
		return;
	}

	/* A .MAP names far less -- publics only, no modules, sizes or lines -- so
	 * it is what to fall back to, never what to prefer.
	 *
	 * Only for an EXE: a COM .MAP's addresses are written relative to
	 * whatever origin its toolchain chose, and no COM fixture was available
	 * to find out which. Guessing would put every symbol 0x100 out. */
	if (!isCom && (!map.publics.empty() || !map.segments.empty())) {
		DEBUG_Symbols().ClearProgram(program);
		const size_t before = DEBUG_Symbols().Size();
		DEBUG_Symbols().AddLinkMap(map,loadLinear,program);
		DEBUG_Symbols().AddSegments(DEBUG_LinkMapSegments(map),loadLinear,imageBytes,program,psp);
		LOG_MSG("DEBUG: %s has no appended debug info; its .MAP gives %u symbols at %05X",
		        program,(unsigned int)(DEBUG_Symbols().Size() - before),(unsigned int)loadLinear);
	}
}

/* A protected-mode program the loader has not placed yet. */
struct PendingImage {
	std::string program;
	bool isCom = false;
	uint16_t psp = 0;
	ProgramFiles files;
	DebugImage image;
	DebugPlacement registered;	/* what its symbols were last registered with */
	bool complete = false;		/* every object with content is placed: nothing left to look for */
};

static std::vector<PendingImage> pendingImages;

static void RegisterPlaced(const PendingImage &pending,const DebugPlacement &placement)
{
	/* Linear addresses throughout: nothing to add to them. No segment layout is
	 * registered, as that is a real-mode process's memory. */
	RegisterSymbols(pending.program.c_str(),false,pending.files,0,&placement,pending.psp,0);
	DEBUG_Symbols().AddExports(pending.image.exports,placement,pending.program);
}

void DEBUG_ImageLoadedAt(const std::string &program,const DebugPlacement &placement)
{
	for (size_t i = 0;i < pendingImages.size();i++) {
		if (pendingImages[i].program != program) continue;
		RegisterPlaced(pendingImages[i],placement);
		pendingImages[i].registered = placement;
		pendingImages[i].complete = true;
		return;
	}
}

/* The memory as a client of a paging DPMI host sees it: the pages the CPU's last page
 * directory maps, gathered into runs of consecutive linear pages. The directory
 * outlives the client's trips through real mode, which is when a query can come. */
static bool MappedMemory(std::vector<std::vector<uint8_t> > &runs,std::vector<DebugMemoryRegion> &regions)
{
	const uint8_t *ram = (const uint8_t *)GetMemBase();
	const size_t ramBytes = (size_t)MEM_TotalPages() * 4096u;
	const uint32_t directory = paging.cr3 & ~0xfffu;
	if (ram == NULL || directory == 0 || (size_t)directory + 4096u > ramBytes) return false;

	const bool largePages = (cpu.cr4 & 0x10u) != 0;
	std::vector<uint32_t> starts;
	for (uint32_t d = 0;d < 1024;d++) {
		const uint32_t entry = host_readd(ram + directory + d * 4u);
		if (!(entry & 1u)) continue;

		for (uint32_t t = 0;t < 1024;t++) {
			uint32_t physical;
			if ((entry & 0x80u) && largePages) {
				physical = (entry & 0xffc00000u) + t * 4096u;
			} else {
				const uint32_t table = entry & ~0xfffu;
				if ((size_t)table + 4096u > ramBytes) break;
				const uint32_t page = host_readd(ram + table + t * 4u);
				if (!(page & 1u)) continue;
				physical = page & ~0xfffu;
			}
			if ((size_t)physical + 4096u > ramBytes) continue;

			const uint32_t linear = (d << 22) | (t << 12);
			if (runs.empty() || starts.back() + runs.back().size() != linear) {
				runs.push_back(std::vector<uint8_t>());
				starts.push_back(linear);
			}
			runs.back().insert(runs.back().end(),ram + physical,ram + physical + 4096u);
		}
	}

	for (size_t i = 0;i < runs.size();i++) {
		DebugMemoryRegion region;
		region.data = runs[i].data();
		region.size = runs[i].size();
		region.linear = starts[i];
		regions.push_back(region);
	}
	return !regions.empty();
}

void DEBUG_ImagesLocate(void)
{
	bool waiting = false;
	for (size_t i = 0;i < pendingImages.size();i++) waiting |= !pendingImages[i].complete;
	if (!waiting) return;

	const uint8_t *ram = (const uint8_t *)GetMemBase();
	if (ram == NULL) return;

	std::vector<std::vector<uint8_t> > runs;
	std::vector<DebugMemoryRegion> mapped;
	const bool paged = MappedMemory(runs,mapped);

	for (size_t i = 0;i < pendingImages.size();i++) {
		if (pendingImages[i].complete) continue;
		DebugPlacement placement;
		/* A paging host's client is found in the linear space it runs in; any other
		 * program (DOS/32A runs unpaged) is where RAM says it is. */
		bool complete = paged && DEBUG_LocateObjects(pendingImages[i].image.objects,mapped,placement);
		if (!complete && placement.empty())
			complete = DEBUG_LocateObjects(pendingImages[i].image.objects,ram,(size_t)MEM_TotalPages() * 4096u,placement);

		if (complete) DEBUG_ImageLoadedAt(pendingImages[i].program,placement);
		/* Some objects are there and some not yet, or never: what is placed is worth having,
		 * and the rest is asked for again. */
		else if (placement.size() > pendingImages[i].registered.size()) {
			pendingImages[i].registered = placement;
			RegisterPlaced(pendingImages[i],placement);
		}
	}
}

std::vector<DebugImageStatus> DEBUG_ImagesStatus(void)
{
	std::vector<DebugImageStatus> out;
	for (size_t i = 0;i < pendingImages.size();i++) {
		DebugImageStatus status;
		status.program = pendingImages[i].program;
		status.objects = pendingImages[i].image.objects.size();
		status.placed = pendingImages[i].registered;
		status.complete = pendingImages[i].complete;
		out.push_back(status);
	}
	return out;
}

/* True when the file starts like an LE, NE or PE image, bound to a stub or bare, or a D32X module. */
static bool LooksLikeImage(const char *program)
{
	uint16_t handle = 0;
	if (!DOS_OpenFile(program,OPEN_READ,&handle)) return false;

	uint8_t head[0x40];
	uint16_t got = sizeof(head);
	bool image = DOS_ReadFile(handle,head,&got) && got == sizeof(head);
	if (image) {
		const DebugBytes bytes(head,sizeof(head));
		if (bytes.u32(0) == 0x58323344) image = true;			/* D32X */
		else if (bytes.u16(0) == 0x5a4d || bytes.u16(0) == 0x4d5a) {
			uint32_t at = bytes.u32(0x3c);
			uint8_t sig[4];
			uint16_t want = sizeof(sig);
			image = DOS_SeekFile(handle,&at,DOS_SEEK_SET) && DOS_ReadFile(handle,sig,&want) && want == sizeof(sig);
			if (image) {
				const DebugBytes tag(sig,sizeof(sig));
				image = tag.u32(0) == 0x4550 || tag.u16(0) == 0x454c || tag.u16(0) == 0x584c || tag.u16(0) == 0x454e;
			}
		} else image = bytes.u16(0) == 0x454c || bytes.u16(0) == 0x584c || bytes.u16(0) == 0x454e;
	}
	DOS_CloseFile(handle);
	return image;
}

/* Reads a protected-mode executable and, when it is one, waits for its loader
 * to place it. False when the file is anything else. */
static bool WatchForLoader(const char *program,uint16_t psp)
{
	PendingImage pending;
	pending.program = program;
	pending.psp = psp;
	if (!LooksLikeImage(program) || !ReadGuestFile(program,pending.files.image,64u*1024u*1024u)) return false;
	if (!DEBUG_ReadImage(DebugBytes(pending.files.image.data(),pending.files.image.size()),pending.image)) return false;

	pending.files.hasMap = ReadSidecar(program,".MAP",pending.files.map);

	/* A program no loader ever places would be searched for on every query. */
	static const size_t MAX_PENDING = 8;
	if (pendingImages.size() >= MAX_PENDING) pendingImages.erase(pendingImages.begin());
	pendingImages.push_back(pending);
	DEBUG_Symbols().ClearProgram(program);
	return true;
}

static std::set<std::string> watchedPrograms;

void DEBUG_SymbolsOnProgramLoad(const char *program,bool isCom,uint16_t loadSeg,uint16_t psp,uint32_t imageBytes)
{
	if (program == NULL) return;

	/* Opening files here must leave no trace: the guest reads the DOS error
	 * code after EXEC returns, and a missing sidecar would overwrite it. */
	const uint16_t saved_errorcode = dos.errorcode;
	const uint32_t loadLinear = (uint32_t)loadSeg << 4u;

	/* A new program starts a new run: what an earlier one made its loader open is open again. */
	watchedPrograms.clear();
	for (size_t i = 0;i < pendingImages.size();i++)
		if (pendingImages[i].program == program) pendingImages.erase(pendingImages.begin() + i--);

	/* The loader is guest code that has not run yet: of a bound extender
	 * program the stub is all that is in memory. */
	if (!isCom && WatchForLoader(program,psp)) watchedPrograms.insert(program);
	else {
		ProgramFiles files;
		if (!isCom && HasAppendedData(program)) ReadGuestFile(program,files.image,64u*1024u*1024u);
		if (!isCom) files.hasMap = ReadSidecar(program,".MAP",files.map);
		RegisterSymbols(program,isCom,files,loadLinear,NULL,psp,imageBytes);
	}

	dos.errorcode = saved_errorcode;
}

/* A loader that is not the program, such as DPMILD16 or DPMILD32, opens the
 * executable it was asked to run: that open is the first sight of it. */
void DEBUG_SymbolsOnFileOpen(const char *file)
{
	static bool busy = false;
	if (file == NULL || busy) return;

	const size_t length = strlen(file);
	if (length < 4 || file[length - 4] != '.') return;
	static const char *const extensions[] = {"EXE","DLL","DRV","LE","LX","D32"};
	bool candidate = false;
	for (size_t i = 0;i < sizeof(extensions) / sizeof(extensions[0]);i++)
		candidate |= strcasecmp(file + length - 3,extensions[i]) == 0;
	if (!candidate || !watchedPrograms.insert(file).second) return;

	busy = true;
	const uint16_t saved_errorcode = dos.errorcode;
	WatchForLoader(file,dos.psp());
	dos.errorcode = saved_errorcode;
	busy = false;
}

#endif
