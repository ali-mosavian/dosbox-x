/*
 * debug_image.h - finding where a protected-mode loader put a program's
 * objects, and telling the symbol store.
 *
 * A DOS extender loads the program itself, in guest code, so the EXEC the
 * emulator sees is only its stub. What the loader did is visible in memory:
 * each object's pages are there, bytes the loader did not rewrite intact.
 */

#ifndef DOSBOX_DEBUG_IMAGE_H
#define DOSBOX_DEBUG_IMAGE_H

#include "debug_symfmt.h"

/* A page of an object as the file holds it. fixed[i] is 0 for a byte the
 * loader rewrites on relocation, which memory will not match. */
struct DebugImagePage {
	uint32_t offset = 0;		/* within the object */
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> fixed;
};

struct DebugImageObject {
	uint16_t index = 0;		/* 1-based, the number linkers and debug info use */
	uint32_t size = 0;
	bool code = false;
	std::vector<DebugImagePage> pages;
};

/* A name the executable itself exports, at an address within one of its objects. */
struct DebugImageExport {
	std::string name;
	uint16_t object = 0;		/* 1-based */
	uint32_t offset = 0;
};

/* What a protected-mode executable says about itself, whatever its format. */
struct DebugImage {
	std::vector<DebugImageObject> objects;
	std::vector<DebugImageExport> exports;
};

/* Each reader: false unless the bytes are its format. */
bool DEBUG_LeImage(const DebugBytes &data,DebugImage &out);
bool DEBUG_NeImage(const DebugBytes &data,DebugImage &out);

/* Tries every format in turn. Adding one is a reader and a line in the table. */
bool DEBUG_ReadImage(const DebugBytes &data,DebugImage &out);

/* A run of guest memory whose first byte is at `linear` in the address space the program runs in. */
struct DebugMemoryRegion {
	const uint8_t *data = NULL;
	size_t size = 0;
	uint32_t linear = 0;
};

/*
 * Where each object sits in `memory`, by finding its pages. An object with
 * no page that survives, such as one that is all zeros, is left out, as is
 * one whose pages match in more than one place. False unless every object
 * that has content was placed: a half-loaded image answers nothing yet.
 */
bool DEBUG_LocateObjects(const std::vector<DebugImageObject> &objects,const std::vector<DebugMemoryRegion> &memory,
                         DebugPlacement &out);

/* The same for memory with no paging in the way: one run from address 0. */
bool DEBUG_LocateObjects(const std::vector<DebugImageObject> &objects,const uint8_t *memory,size_t memorySize,
                         DebugPlacement &out);

#endif
