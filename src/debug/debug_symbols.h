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

/* The image-loaded event: a protected-mode program's objects were placed at
 * these addresses (by object number), so its symbols can be registered. */
void DEBUG_ImageLoadedAt(const std::string &program,const DebugPlacement &placement);

/* Looks in guest memory for programs the emulator has seen start but whose
 * loader has not been seen finishing, and raises the event for each found.
 * Cheap when none are waiting; call before answering a symbol query. */
void DEBUG_ImagesLocate(void);

#endif

#endif
