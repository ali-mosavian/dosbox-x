/*
 * debug_symfmt_map.cpp - linker .MAP files: Microsoft LINK's layout, which
 * TLINK shares, and WLINK's, which JWlink writes.
 *
 * Not debug info the linker appends to the image: a separate text file the
 * user supplies. Every address in it is load-relative, so it needs the same
 * load base as the appended formats to become linear.
 */

#include "debug_symfmt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static std::string UpperOf(const std::string &text)
{
	std::string out = text;
	for (size_t i = 0;i < out.size();i++) out[i] = (char)toupper((unsigned char)out[i]);
	return out;
}

static std::vector<std::string> Tokens(const std::string &line)
{
	std::vector<std::string> out;
	size_t at = 0;
	while (at < line.size()) {
		at = line.find_first_not_of(" \t",at);
		if (at == std::string::npos) break;
		const size_t end = line.find_first_of(" \t",at);
		out.push_back(line.substr(at,end == std::string::npos ? std::string::npos : end - at));
		at = end == std::string::npos ? line.size() : end;
	}
	return out;
}

static bool IsHexText(const std::string &text)
{
	if (text.empty()) return false;
	for (size_t i = 0;i < text.size();i++)
		if (!isxdigit((unsigned char)text[i])) return false;
	return true;
}

/* LINK writes lengths and addresses as 0024CH; the suffix is not a digit. */
static bool ParseMapHex(const std::string &text,uint32_t &out)
{
	std::string body = text;
	if (!body.empty() && (body[body.size()-1] == 'H' || body[body.size()-1] == 'h')) body.erase(body.size()-1);
	if (!IsHexText(body)) return false;
	out = (uint32_t)strtoul(body.c_str(),NULL,16);
	return true;
}

static bool ParseColonAddress(const std::string &text,LinkMapAddress &out)
{
	const size_t colon = text.find(':');
	if (colon == std::string::npos) return false;

	uint32_t segment = 0;
	uint32_t offset = 0;
	const std::string left = text.substr(0,colon);
	const std::string right = text.substr(colon + 1);
	if (!IsHexText(left) || !IsHexText(right)) return false;
	if (!ParseMapHex(left,segment) || !ParseMapHex(right,offset)) return false;

	out.segment = (uint16_t)segment;
	out.offset = offset;
	out.mapOffset = (segment << 4) + offset;
	return true;
}

static bool IsHeader(const std::vector<std::string> &tokens,const char * const *words,size_t count)
{
	if (tokens.size() < count) return false;
	for (size_t i = 0;i < count;i++)
		if (UpperOf(tokens[i]) != words[i]) return false;
	return true;
}

void DEBUG_ParseLinkMap(const std::string &text,const char *sourceName,LinkMapFile &out)
{
	static const char * const SEGMENT_HEADER[5] = {"START","STOP","LENGTH","NAME","CLASS"};
	static const char * const GROUP_HEADER[2] = {"ORIGIN","GROUP"};
	static const char * const ENTRY_HEADER[4] = {"PROGRAM","ENTRY","POINT","AT"};
	/* WLINK: rows name first, addresses as seg:off, sizes as bare hex. */
	static const char * const WLINK_SEGMENT_HEADER[5] = {"SEGMENT","CLASS","GROUP","ADDRESS","SIZE"};
	static const char * const WLINK_GROUP_HEADER[3] = {"GROUP","ADDRESS","SIZE"};
	static const char * const WLINK_SYMBOL_HEADER[2] = {"ADDRESS","SYMBOL"};

	enum { SECTION_NONE, SECTION_SEGMENTS, SECTION_GROUPS, SECTION_PUBLICS,
	       SECTION_WLINK_SEGMENTS, SECTION_WLINK_GROUPS } section = SECTION_NONE;
	out.sourceName = sourceName != NULL ? sourceName : "<memory>";

	size_t at = 0;
	while (at <= text.size()) {
		const size_t eol = text.find('\n',at);
		std::string line = text.substr(at,eol == std::string::npos ? std::string::npos : eol - at);
		at = eol == std::string::npos ? text.size() + 1 : eol + 1;
		/* LINK writes CRLF; a \r left on the last token renames every public. */
		if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);

		const std::vector<std::string> tokens = Tokens(line);
		if (tokens.empty()) continue;

		if (IsHeader(tokens,SEGMENT_HEADER,5)) { section = SECTION_SEGMENTS; continue; }
		if (IsHeader(tokens,WLINK_SEGMENT_HEADER,5)) { section = SECTION_WLINK_SEGMENTS; continue; }
		if (IsHeader(tokens,WLINK_GROUP_HEADER,3)) { section = SECTION_WLINK_GROUPS; continue; }
		if (IsHeader(tokens,WLINK_SYMBOL_HEADER,2)) { section = SECTION_PUBLICS; continue; }
		if (IsHeader(tokens,GROUP_HEADER,2)) { section = SECTION_GROUPS; continue; }
		if (UpperOf(line).find("PUBLICS BY VALUE") != std::string::npos) { section = SECTION_PUBLICS; continue; }

		if (IsHeader(tokens,ENTRY_HEADER,4) && tokens.size() >= 5) {
			if (ParseColonAddress(tokens[4],out.entryPoint)) out.hasEntryPoint = true;
			section = SECTION_NONE;
			continue;
		}

		if (section == SECTION_SEGMENTS && tokens.size() >= 5) {
			LinkMapSegment segment;
			if (ParseMapHex(tokens[0],segment.start) &&
			    ParseMapHex(tokens[1],segment.stop) &&
			    ParseMapHex(tokens[2],segment.length)) {
				segment.name = tokens[3];
				segment.className = tokens[4];
				out.segments.push_back(segment);
				continue;
			}
		}

		if (section == SECTION_WLINK_SEGMENTS && tokens.size() >= 5) {
			LinkMapSegment segment;
			LinkMapAddress start;
			if (ParseColonAddress(tokens[3],start) && ParseMapHex(tokens[4],segment.length)) {
				segment.name = tokens[0];
				segment.className = tokens[1];
				segment.start = start.mapOffset;
				segment.stop = segment.start + (segment.length ? segment.length - 1 : 0);
				out.segments.push_back(segment);
				continue;
			}
		}

		if (section == SECTION_WLINK_GROUPS && tokens.size() >= 3) {
			LinkMapGroup group;
			if (ParseColonAddress(tokens[1],group.address)) {
				group.name = tokens[0];
				out.groups.push_back(group);
				continue;
			}
		}

		if (tokens.size() < 2) continue;

		/* WLINK marks an unreferenced symbol's address with *, a locally
		 * referenced one's with +. */
		std::string first = tokens[0];
		if (!first.empty() && (first[first.size()-1] == '*' || first[first.size()-1] == '+')) first.erase(first.size()-1);

		LinkMapAddress address;
		if (!ParseColonAddress(first,address)) continue;

		if (section == SECTION_GROUPS) {
			LinkMapGroup group;
			group.name = tokens[1];
			group.address = address;
			out.groups.push_back(group);
			continue;
		}

		if (section == SECTION_PUBLICS) {
			/* Rows for absolute symbols put "Abs" between the address and the
			 * name. Their value is a constant, not in the load image: keyed
			 * as "Abs", the first one named BC's main module code. */
			if (tokens[1] == "Abs") continue;
			LinkMapPublic symbol;
			symbol.name = tokens[1];
			symbol.address = address;
			if (out.publics.find(symbol.name) == out.publics.end()) out.publics[symbol.name] = symbol;
			const std::string upper = UpperOf(symbol.name);
			if (out.publics.find(upper) == out.publics.end()) out.publics[upper] = symbol;
		}
	}
}

bool DEBUG_IsCodeClass(const std::string &className)
{
	const std::string upper = UpperOf(className);
	return upper.size() >= 4 && upper.compare(upper.size() - 4,4,"CODE") == 0;
}

std::vector<DebugSegment> DEBUG_LinkMapSegments(const LinkMapFile &map)
{
	std::vector<DebugSegment> out;
	for (size_t i = 0;i < map.segments.size();i++) {
		const LinkMapSegment &segment = map.segments[i];
		if (segment.length == 0) continue;
		DebugSegment range;
		range.imageOffset = segment.start;
		range.endOffset = segment.start + segment.length;
		range.code = DEBUG_IsCodeClass(segment.className);
		out.push_back(range);
	}
	return out;
}

bool DEBUG_ReadLinkMapFile(const char *path,LinkMapFile &out)
{
	std::vector<uint8_t> raw;
	if (!DEBUG_ReadHostFile(path,raw)) return false;
	DEBUG_ParseLinkMap(std::string((const char*)raw.data(),raw.size()),path,out);
	return true;
}

static const LinkMapSegment *FindMapSegment(const LinkMapFile &map,const std::string &name)
{
	const std::string upper = UpperOf(name);
	for (size_t i = 0;i < map.segments.size();i++)
		if (UpperOf(map.segments[i].name) == upper) return &map.segments[i];
	return NULL;
}

static const LinkMapPublic *FindMapPublic(const LinkMapFile &map,const std::string &name)
{
	std::map<std::string,LinkMapPublic>::const_iterator found = map.publics.find(name);
	if (found == map.publics.end()) found = map.publics.find(UpperOf(name));
	return found == map.publics.end() ? NULL : &found->second;
}

/* 0x1F, 1FH, 31 and 1F all mean the same thing in a hand-typed spec. */
static bool ParseNumberSpec(const std::string &text,uint32_t &out)
{
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		return ParseMapHex(text.substr(2),out);

	bool allDecimal = true;
	for (size_t i = 0;i < text.size();i++)
		if (!isdigit((unsigned char)text[i])) allDecimal = false;
	if (allDecimal && !text.empty()) {
		out = (uint32_t)strtoul(text.c_str(),NULL,10);
		return true;
	}
	return ParseMapHex(text,out);
}

static std::string Trimmed(const std::string &text)
{
	const size_t from = text.find_first_not_of(" \t");
	if (from == std::string::npos) return std::string();
	const size_t to = text.find_last_not_of(" \t");
	return text.substr(from,to - from + 1);
}

bool DEBUG_ResolveLinkMapSymbol(const LinkMapFile &map,const std::string &requested,uint32_t loadLinear,
                                LinkMapResolution &out,std::string &error)
{
	const std::string trimmed = Trimmed(requested);
	out.requested = requested;

	LinkMapAddress direct;
	if (ParseColonAddress(trimmed,direct)) {
		out.name = trimmed;
		out.offset = direct.offset;
		out.mapOffset = direct.mapOffset;
		out.linear = loadLinear + direct.mapOffset;
		out.explanation = trimmed + " = loadLinear " + DEBUG_Hex(loadLinear) +
		                  " + map offset " + DEBUG_Hex(direct.mapOffset) + " = " + DEBUG_Hex(out.linear);
		return true;
	}

	const std::string upper = UpperOf(trimmed);
	if (upper == "ENTRY" || upper == "$ENTRY") {
		if (!map.hasEntryPoint) {
			error = "LINK map has no Program entry point line";
			return false;
		}
		out.name = "entry";
		out.offset = map.entryPoint.offset;
		out.mapOffset = map.entryPoint.mapOffset;
		out.linear = loadLinear + map.entryPoint.mapOffset;
		out.explanation = "entry = loadLinear " + DEBUG_Hex(loadLinear) +
		                  " + map offset " + DEBUG_Hex(map.entryPoint.mapOffset) + " = " + DEBUG_Hex(out.linear);
		return true;
	}

	const size_t split = trimmed.find_first_of("+:");
	if (split != std::string::npos && split > 0) {
		const std::string segmentName = Trimmed(trimmed.substr(0,split));
		const std::string offsetText = Trimmed(trimmed.substr(split + 1));
		const LinkMapSegment *segment = FindMapSegment(map,segmentName);
		if (segment == NULL) {
			error = "Unknown MAP segment: " + segmentName;
			return false;
		}
		uint32_t offset = 0;
		if (!ParseNumberSpec(offsetText,offset)) {
			error = "Invalid MAP offset: " + offsetText;
			return false;
		}
		out.name = segment->name;
		out.offset = offset;
		out.mapOffset = segment->start + offset;
		out.linear = loadLinear + out.mapOffset;
		out.explanation = segment->name + "+" + DEBUG_Hex(offset) + " = loadLinear " + DEBUG_Hex(loadLinear) +
		                  " + segment " + DEBUG_Hex(segment->start) + " + offset " + DEBUG_Hex(offset) +
		                  " = " + DEBUG_Hex(out.linear);
		return true;
	}

	const LinkMapSegment *segment = FindMapSegment(map,trimmed);
	if (segment != NULL) {
		out.name = segment->name;
		out.offset = 0;
		out.mapOffset = segment->start;
		out.linear = loadLinear + segment->start;
		out.explanation = segment->name + " = loadLinear " + DEBUG_Hex(loadLinear) +
		                  " + segment " + DEBUG_Hex(segment->start) + " = " + DEBUG_Hex(out.linear);
		return true;
	}

	const LinkMapPublic *symbol = FindMapPublic(map,trimmed);
	if (symbol != NULL) {
		out.name = symbol->name;
		out.offset = symbol->address.offset;
		out.mapOffset = symbol->address.mapOffset;
		out.linear = loadLinear + symbol->address.mapOffset;
		out.explanation = symbol->name + " = loadLinear " + DEBUG_Hex(loadLinear) +
		                  " + public map offset " + DEBUG_Hex(symbol->address.mapOffset) +
		                  " = " + DEBUG_Hex(out.linear);
		return true;
	}

	error = "Unknown LINK map segment or public symbol: " + requested;
	return false;
}

bool DEBUG_DescribeMapAddress(const LinkMapFile &map,uint32_t loadLinear,uint32_t linear,std::string &out)
{
	const uint32_t mapOffset = linear - loadLinear;

	for (std::map<std::string,LinkMapPublic>::const_iterator it = map.publics.begin();it != map.publics.end();++it) {
		if (it->second.address.mapOffset == mapOffset) {
			out = it->second.name;
			return true;
		}
	}

	const LinkMapSegment *best = NULL;
	for (size_t i = 0;i < map.segments.size();i++) {
		const LinkMapSegment &segment = map.segments[i];
		if (segment.start > mapOffset || mapOffset > segment.stop) continue;
		if (best == NULL || segment.start > best->start) best = &segment;
	}
	if (best == NULL) return false;

	out = best->name + "+" + DEBUG_Hex(mapOffset - best->start);
	return true;
}
