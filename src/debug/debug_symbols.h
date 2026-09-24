/*
 * debug_symbols.h - the DOS loader's entry point into the symbol store.
 */

#ifndef DOSBOX_DEBUG_SYMBOLS_H
#define DOSBOX_DEBUG_SYMBOLS_H

#include "dosbox.h"

#if C_DEBUG

#include "debug_symstore.h"

/* Called by the DOS EXEC loader once the image is in memory: reads whatever
 * debug info the program carries and registers it at the address it loaded
 * at. Silent and harmless when there is none. */
void DEBUG_SymbolsOnProgramLoad(const char *program,bool isCom,uint16_t loadSeg,uint16_t psp,uint32_t imageBytes);

#endif

#endif
