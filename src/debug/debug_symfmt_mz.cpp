/*
 * debug_symfmt_mz.cpp - MZ header and load image extents.
 */

#include "debug_symfmt.h"

#include <stdio.h>

#include <algorithm>

namespace dbgsym {

uint16_t Bytes::u16(size_t at) const {
	if (at + 2 > n) return 0;
	return (uint16_t)((uint16_t)p[at] | ((uint16_t)p[at+1] << 8));
}

uint32_t Bytes::u32(size_t at) const {
	if (at + 4 > n) return 0;
	return (uint32_t)p[at] | ((uint32_t)p[at+1] << 8) | ((uint32_t)p[at+2] << 16) | ((uint32_t)p[at+3] << 24);
}

std::string Bytes::latin1(size_t from,size_t to) const {
	if (from >= n) return std::string();
	if (to > n) to = n;
	if (to <= from) return std::string();
	return std::string((const char*)p + from,to - from);
}

std::string Bytes::pstr(size_t at,size_t *next) const {
	if (at >= n) {
		if (next != nullptr) *next = at;
		return std::string();
	}
	const size_t len = p[at];
	if (next != nullptr) *next = at + 1 + len;
	return latin1(at + 1,at + 1 + len);
}

Bytes Bytes::sub(size_t from,size_t len) const {
	if (from >= n) return Bytes();
	return Bytes(p + from,std::min(len,n - from));
}

bool ParseMzHeader(const Bytes &data,MzHeader &out) {
	if (data.size() < 28) return false;
	const uint16_t signature = data.u16(0);
	/* ZM is the rarer byte-swapped spelling some early linkers emitted. */
	if (signature != 0x5a4d && signature != 0x4d5a) return false;

	out.extraBytes = data.u16(2);
	out.pages = data.u16(4);
	out.relocationCount = data.u16(6);
	out.headerParagraphs = data.u16(8);
	out.minAlloc = data.u16(10);
	out.maxAlloc = data.u16(12);
	out.initSS = data.u16(14);
	out.initSP = data.u16(16);
	out.checksum = data.u16(18);
	out.initIP = data.u16(20);
	out.initCS = data.u16(22);
	out.relocationTableOffset = data.u16(24);
	out.overlay = data.u16(26);
	return true;
}

bool ParseMzImage(const Bytes &data,MzImage &out) {
	MzHeader header;
	if (!ParseMzHeader(data,header)) return false;

	const uint64_t imageOffset = (uint64_t)header.headerParagraphs << 4u;
	/* e_cblp is how much of the LAST page is used; zero means the page is full. */
	const uint64_t lastPage = header.extraBytes == 0 ? 512u : header.extraBytes;
	const uint64_t pagedSize = header.pages == 0 ? 0u : ((uint64_t)(header.pages - 1) * 512u + lastPage);
	const uint64_t imageSize = pagedSize > imageOffset ? pagedSize - imageOffset : 0u;

	out.header = header;
	out.imageOffset = imageOffset;
	out.imageSize = imageSize;
	out.appendedOffset = std::min(imageOffset + imageSize,(uint64_t)data.size());
	out.fileSize = data.size();
	return true;
}

std::string MzFingerprint(const MzHeader &header) {
	const uint16_t fields[11] = {
		header.extraBytes, header.pages, header.relocationCount,
		header.headerParagraphs, header.minAlloc, header.maxAlloc,
		header.initSS, header.initSP, header.initIP, header.initCS,
		header.relocationTableOffset
	};

	std::string out;
	char buf[16];
	for (size_t i = 0;i < 11;i++) {
		if (i != 0) out += ':';
		snprintf(buf,sizeof(buf),"%u",(unsigned int)fields[i]);
		out += buf;
	}
	return out;
}

bool ReadHostFile(const char *path,std::vector<uint8_t> &out) {
	FILE *f = fopen(path,"rb");
	if (f == nullptr) return false;

	out.clear();
	uint8_t buf[64u*1024u];
	size_t got;
	while ((got = fread(buf,1,sizeof(buf),f)) > 0) out.insert(out.end(),buf,buf + got);

	const bool ok = ferror(f) == 0;
	fclose(f);
	if (!ok) out.clear();
	return ok;
}

}
