/*
 * debug_symbols.h - the DOS loader's entry point into the symbol store.
 */

#ifndef DOSBOX_DEBUG_SYMBOLS_H
#define DOSBOX_DEBUG_SYMBOLS_H

#include "dosbox.h"

#if C_DEBUG

#include "debug_symstore.h"
#include "debug_image.h"

/* Called by the DOS EXEC loader once the image is in memory: reads whatever
 * debug info the program carries and registers it at the address it loaded
 * at. Silent and harmless when there is none. */
void DEBUG_SymbolsOnProgramLoad(const char *program,bool isCom,uint16_t loadSeg,uint16_t psp,uint32_t imageBytes);

/* Called when the guest opens a file: one that proves to be a protected-mode
 * executable is waited for like a program EXEC loaded, whichever loader asked. */
void DEBUG_SymbolsOnFileOpen(const char *file);

/* An image the emulator is waiting to see placed, or has seen. */
struct DebugImageStatus {
	std::string program;
	size_t objects = 0;
	DebugPlacement placed;		/* object number -> linear address, the ones found so far */
	bool complete = false;
};

std::vector<DebugImageStatus> DEBUG_ImagesStatus(void);

/* The image-loaded event: a protected-mode program's objects were placed at
 * these addresses (by object number), so its symbols can be registered. */
void DEBUG_ImageLoadedAt(const std::string &program,const DebugPlacement &placement);

/* Looks in guest memory for programs the emulator has seen start but whose
 * loader has not been seen finishing, and raises the event for each found.
 * Cheap when none are waiting; call before answering a symbol query. */
void DEBUG_ImagesLocate(void);

#endif

#endif
