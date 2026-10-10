/*
 * debug_image.cpp - locating a program's objects in memory by their content.
 */

#define _GNU_SOURCE 1

#include "debug_image.h"

#include <string.h>

#include <algorithm>
#include <map>
#include <set>

static const size_t ANCHOR_BYTES = 24;
static const size_t ANCHOR_MIN_DISTINCT = 8;
static const size_t ANCHORS_PER_OBJECT = 16;
/* A window found this often is not telling anything apart. */
static const size_t ANCHOR_MAX_HITS = 64;

struct Anchor {
	uint32_t offset;	/* within the object */
	const uint8_t *bytes;
};

/* A run of bytes the loader leaves alone and that is not a repeated fill. */
static bool AnchorAt(const DebugImagePage &page,size_t at)
{
	std::set<uint8_t> distinct;
	for (size_t i = 0;i < ANCHOR_BYTES;i++) {
		if (!page.fixed[at + i]) return false;
		distinct.insert(page.bytes[at + i]);
	}
	return distinct.size() >= ANCHOR_MIN_DISTINCT;
}

static void ObjectAnchors(const DebugImageObject &object,std::vector<Anchor> &out)
{
	for (size_t p = 0;p < object.pages.size();p++) {
		const DebugImagePage &page = object.pages[p];
		for (size_t at = 0;at + ANCHOR_BYTES <= page.bytes.size();at += ANCHOR_BYTES) {
			if (!AnchorAt(page,at)) continue;
			Anchor anchor;
			anchor.offset = page.offset + (uint32_t)at;
			anchor.bytes = &page.bytes[at];
			out.push_back(anchor);
			break;
		}
	}

	/* Evenly thinned: a big object's every page would cost a scan each. */
	while (out.size() > ANCHORS_PER_OBJECT) {
		std::vector<Anchor> thinned;
		for (size_t i = 0;i < out.size();i += 2) thinned.push_back(out[i]);
		out.swap(thinned);
	}
}

/* Found, with `base` set; or not found. An object with nothing to look for
 * (pages of zeros, say) is `unlocatable`, which is no reason to wait. */
static bool LocateObject(const DebugImageObject &object,const std::vector<DebugMemoryRegion> &memory,uint32_t &base,
                         bool &unlocatable)
{
	std::vector<Anchor> anchors;
	ObjectAnchors(object,anchors);
	unlocatable = anchors.empty();

	std::map<uint32_t,size_t> votes;
	for (size_t a = 0;a < anchors.size();a++) {
		std::vector<uint32_t> hits;
		for (size_t r = 0;r < memory.size() && hits.size() <= ANCHOR_MAX_HITS;r++) {
			const uint8_t *from = memory[r].data;
			const uint8_t *end = memory[r].data + memory[r].size;
			while (from < end) {
				const uint8_t *hit = (const uint8_t *)memmem(from,(size_t)(end - from),anchors[a].bytes,ANCHOR_BYTES);
				if (hit == NULL) break;
				hits.push_back(memory[r].linear + (uint32_t)(hit - memory[r].data));
				if (hits.size() > ANCHOR_MAX_HITS) break;
				from = hit + 1;
			}
		}
		if (hits.size() > ANCHOR_MAX_HITS) continue;
		for (size_t h = 0;h < hits.size();h++)
			if (hits[h] >= anchors[a].offset) votes[hits[h] - anchors[a].offset]++;
	}

	size_t best = 0;
	size_t leaders = 0;
	for (std::map<uint32_t,size_t>::const_iterator it = votes.begin();it != votes.end();++it) {
		if (it->second > best) {
			best = it->second;
			leaders = 0;
		}
		if (it->second == best) {
			leaders++;
			base = it->first;
		}
	}
	return best > 0 && leaders == 1;
}

bool DEBUG_LocateObjects(const std::vector<DebugImageObject> &objects,const std::vector<DebugMemoryRegion> &memory,
                         DebugPlacement &out)
{
	out.clear();
	for (size_t o = 0;o < objects.size();o++) {
		uint32_t base = 0;
		bool unlocatable = false;
		if (LocateObject(objects[o],memory,base,unlocatable)) out[objects[o].index] = base;
		else if (!unlocatable) return false;
	}
	return !out.empty();
}

bool DEBUG_LocateObjects(const std::vector<DebugImageObject> &objects,const uint8_t *memory,size_t memorySize,
                         DebugPlacement &out)
{
	DebugMemoryRegion whole;
	whole.data = memory;
	whole.size = memorySize;
	return DEBUG_LocateObjects(objects,std::vector<DebugMemoryRegion>(1,whole),out);
}

bool DEBUG_ReadImage(const DebugBytes &data,DebugImage &out)
{
	typedef bool (*Reader)(const DebugBytes &,DebugImage &);
	static const Reader readers[] = {DEBUG_LeImage,DEBUG_NeImage};

	for (size_t i = 0;i < sizeof(readers) / sizeof(readers[0]);i++) {
		DebugImage image;
		if (!readers[i](data,image) || image.objects.empty()) continue;
		out = image;
		return true;
	}
	return false;
}
