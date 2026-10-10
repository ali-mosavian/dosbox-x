/*
 * debug_profile.cpp - attributing executed instructions and memory operands
 * to functions and source lines.
 *
 * The address space is cut into ranges, each one function and one source
 * line, or one stretch where nothing is counted. The core knows only the
 * range it is in; when execution leaves it, the counts that accrued since
 * the last change are added to that range's function and line, so the cost
 * of an instruction is the one comparison and the counter the core keeps
 * anyway. Calls and returns, found from the stack, give the inclusive counts.
 */

#include "debug_profile.h"

#if C_DEBUG

#include "cpu.h"
#include "dos_inc.h"
#include "dosrun.h"
#include "mem.h"
#include "regs.h"
#include "debug_symbols.h"
#include "debug_symstore.h"

#include <algorithm>
#include <map>
#include <vector>

bool debug_profiling = false;
uint32_t debug_profile_begin = 0, debug_profile_length = 0;

namespace {

const int32_t NO_FUNCTION = -1;

struct Range {
	uint32_t begin = 0;
	uint32_t end = 0;		/* one past; 0 means the end of the address space */
	int32_t function = NO_FUNCTION;
	int32_t line = -1;
	bool counted = false;
};

struct Function {
	std::string name;
	std::string program;
	uint64_t self = 0,selfMemory = 0,inclusive = 0,inclusiveMemory = 0,calls = 0;
	uint32_t active = 0;		/* frames of it on the stack: only the outermost adds to inclusive */
};

struct Line {
	std::string file;
	uint16_t line = 0;
	int32_t function = NO_FUNCTION;
	uint64_t self = 0,memory = 0;
};

struct Frame {
	uint32_t returnTo = 0;
	int32_t function = NO_FUNCTION;
	uint64_t instructions = 0,memory = 0;
};

std::vector<Range> ranges;
std::vector<Function> functions;
std::vector<Line> lines;
std::map<std::string,int32_t> functionIndex;
std::map<std::string,int32_t> lineIndex;
std::vector<Frame> frames;

uint32_t builtFor = 0;
bool built = false;
uint16_t builtPsp = 0;
int32_t current = -1;			/* the range being attributed to */
uint64_t lastInstructions = 0,lastMemory = 0;
uint64_t startInstructions = 0,startMemory = 0;

const char *UNKNOWN = "(unknown)";

int32_t FunctionIndex(const std::string &name,const std::string &program)
{
	const std::string key = program + "|" + name;
	const std::map<std::string,int32_t>::const_iterator found = functionIndex.find(key);
	if (found != functionIndex.end()) return found->second;
	Function function;
	function.name = name;
	function.program = program;
	functions.push_back(function);
	functionIndex[key] = (int32_t)functions.size() - 1;
	return (int32_t)functions.size() - 1;
}

int32_t LineIndex(const std::string &file,uint16_t line,int32_t function)
{
	const std::string key = file + ":" + std::to_string((unsigned int)line) + ":" + std::to_string(function);
	const std::map<std::string,int32_t>::const_iterator found = lineIndex.find(key);
	if (found != lineIndex.end()) return found->second;
	Line entry;
	entry.file = file;
	entry.line = line;
	entry.function = function;
	lines.push_back(entry);
	lineIndex[key] = (int32_t)lines.size() - 1;
	return (int32_t)lines.size() - 1;
}

/* Adds what accrued since the last change to the range that was current. */
void Flush()
{
	const uint64_t instructions = dosrun_instructions - lastInstructions;
	const uint64_t memory = dosrun_memory - lastMemory;
	lastInstructions = dosrun_instructions;
	lastMemory = dosrun_memory;
	if (current < 0 || (instructions == 0 && memory == 0)) return;

	/* Counted by the core but outside everything the table covers: kept, under its own name, so the
	 * functions always add up to the total. */
	const Range &range = ranges[(size_t)current];
	const int32_t function = range.function >= 0 ? range.function : FunctionIndex("(outside the program)","");
	functions[(size_t)function].self += instructions;
	functions[(size_t)function].selfMemory += memory;
	if (range.line >= 0) {
		lines[(size_t)range.line].self += instructions;
		lines[(size_t)range.line].memory += memory;
	}
}

struct Span {
	uint32_t begin,end;
};

/* Where the running program's code may be: its DOS memory block in real mode, the objects of the images the
 * loader placed in protected mode. */
std::vector<Span> ProgramSpans()
{
	std::vector<Span> spans;
	uint16_t owner = 0,start = 0,end = 0;
	builtPsp = dos.psp();
	if (DOS_MemoryBlockAt(builtPsp,owner,start,end) && owner == builtPsp) {
		const Span block = {(uint32_t)start << 4,(uint32_t)end << 4};
		spans.push_back(block);
	}
	std::vector<std::pair<uint32_t,uint32_t> > images;
	DEBUG_ImageSpans(images);
	for (size_t i = 0;i < images.size();i++) {
		const Span span = {images[i].first,images[i].second};
		spans.push_back(span);
	}

	std::sort(spans.begin(),spans.end(),[](const Span &a,const Span &b) { return a.begin < b.begin; });
	std::vector<Span> merged;
	for (size_t i = 0;i < spans.size();i++) {
		if (!merged.empty() && spans[i].begin <= merged.back().end) merged.back().end = std::max(merged.back().end,spans[i].end);
		else merged.push_back(spans[i]);
	}
	return merged;
}

const DebugSymbol *SymbolAt(const std::vector<const DebugSymbol *> &sorted,uint32_t linear)
{
	size_t lo = 0,hi = sorted.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		if (sorted[mid]->linear <= linear) lo = mid + 1;
		else hi = mid;
	}
	for (size_t i = lo;i > 0;i--) {
		const DebugSymbol *symbol = sorted[i - 1];
		if (symbol->hasSize && symbol->size > 0 && linear >= symbol->linear + symbol->size) continue;
		return symbol;
	}
	return NULL;
}

const DebugSourceLine *LineAtSorted(const std::vector<const DebugSourceLine *> &sorted,uint32_t linear)
{
	size_t lo = 0,hi = sorted.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		if (sorted[mid]->begin <= linear) lo = mid + 1;
		else hi = mid;
	}
	for (size_t i = lo;i > 0 && lo - i < 8;i--)
		if (linear < sorted[i - 1]->end) return sorted[i - 1];
	return NULL;
}

void Rebuild()
{
	const DebugSymbolStore &store = DEBUG_Symbols();
	const std::vector<Span> spans = ProgramSpans();

	std::vector<const DebugSymbol *> symbols;
	for (size_t i = 0;i < store.All().size();i++) symbols.push_back(&store.All()[i]);
	std::stable_sort(symbols.begin(),symbols.end(),[](const DebugSymbol *a,const DebugSymbol *b) {
		if (a->linear != b->linear) return a->linear < b->linear;
		return a->isFunction && !b->isFunction;
	});
	std::vector<const DebugSourceLine *> sourceLines;
	for (size_t i = 0;i < store.Lines().size();i++) sourceLines.push_back(&store.Lines()[i]);
	std::stable_sort(sourceLines.begin(),sourceLines.end(),
	                 [](const DebugSourceLine *a,const DebugSourceLine *b) { return a->begin < b->begin; });

	ranges.clear();
	uint32_t next = 0;
	for (size_t s = 0;s < spans.size();s++) {
		if (spans[s].begin > next) {
			Range outside;
			outside.begin = next;
			outside.end = spans[s].begin;
			ranges.push_back(outside);
		}

		std::vector<uint32_t> points;
		points.push_back(spans[s].begin);
		points.push_back(spans[s].end);
		for (size_t i = 0;i < symbols.size();i++) {
			if (symbols[i]->linear > spans[s].begin && symbols[i]->linear < spans[s].end) points.push_back(symbols[i]->linear);
			if (symbols[i]->hasSize && symbols[i]->linear + symbols[i]->size > spans[s].begin &&
			    symbols[i]->linear + symbols[i]->size < spans[s].end) points.push_back(symbols[i]->linear + symbols[i]->size);
		}
		for (size_t i = 0;i < sourceLines.size();i++) {
			if (sourceLines[i]->begin > spans[s].begin && sourceLines[i]->begin < spans[s].end) points.push_back(sourceLines[i]->begin);
			if (sourceLines[i]->end > spans[s].begin && sourceLines[i]->end < spans[s].end) points.push_back(sourceLines[i]->end);
		}
		std::sort(points.begin(),points.end());
		points.erase(std::unique(points.begin(),points.end()),points.end());

		for (size_t p = 0;p + 1 < points.size();p++) {
			Range range;
			range.begin = points[p];
			range.end = points[p + 1];
			range.counted = true;
			const DebugSymbol *symbol = SymbolAt(symbols,range.begin);
			range.function = symbol != NULL ? FunctionIndex(symbol->name,symbol->program) : FunctionIndex(UNKNOWN,"");
			const DebugSourceLine *line = LineAtSorted(sourceLines,range.begin);
			if (line != NULL) range.line = LineIndex(line->file,line->line,range.function);
			ranges.push_back(range);
		}
		next = spans[s].end;
	}
	Range tail;
	tail.begin = next;
	tail.end = 0;
	ranges.push_back(tail);

	built = true;
	builtFor = store.Generation();
}

size_t Locate(uint32_t linear)
{
	size_t lo = 0,hi = ranges.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		if (ranges[mid].begin <= linear) lo = mid + 1;
		else hi = mid;
	}
	return lo - 1;
}

bool Stale()
{
	return !built || builtFor != DEBUG_Symbols().Generation() || builtPsp != dos.psp();
}

} /* namespace */

void DEBUG_ProfileStart(void)
{
	functions.clear();
	lines.clear();
	functionIndex.clear();
	lineIndex.clear();
	frames.clear();
	built = false;
	current = -1;
	startInstructions = lastInstructions = dosrun_instructions;
	startMemory = lastMemory = dosrun_memory;
	debug_profile_begin = 0;
	debug_profile_length = 0;
	debug_profiling = true;
}

void DEBUG_ProfileStop(void)
{
	if (!debug_profiling) return;
	Flush();
	debug_profiling = false;
	debug_profile_length = 0;
	dosrun_counting = false;
}

void DEBUG_ProfileInvalidate(void)
{
	if (debug_profiling) debug_profile_length = 0;
}

void DEBUG_ProfileEnter(uint32_t linear)
{
	Flush();
	if (Stale()) {
		const uint64_t keepInstructions = lastInstructions,keepMemory = lastMemory;
		Rebuild();
		lastInstructions = keepInstructions;
		lastMemory = keepMemory;
	}
	current = (int32_t)Locate(linear);
	const Range &range = ranges[(size_t)current];
	debug_profile_begin = range.begin;
	debug_profile_length = range.end - range.begin;	/* 0 for the last range: every address from begin on is in it */
	dosrun_counting = range.counted;
}

void DEBUG_ProfileTransfer(uint32_t from,uint32_t fromSp,uint32_t fromBase,uint32_t to,uint32_t toBase)
{
	if (Stale()) return;
	const uint32_t mask = (uint32_t)cpu.stack.mask;
	const uint32_t size = cpu.code.big ? 4 : 2;
	const uint32_t top = SegPhys(ss) + (reg_esp & mask);
	const uint32_t sp = reg_esp & mask;

	/* A call leaves on the stack the address just past the instruction that made it. */
	if (sp < (fromSp & mask) && ((fromSp & mask) - sp) == size) {
		const uint32_t pushed = fromBase + (size == 4 ? mem_readd(top) : mem_readw(top));
		if (pushed > from && pushed - from <= 15) {
			const size_t range = Locate(to);
			Frame frame;
			frame.returnTo = pushed;
			frame.function = ranges[range].counted ? ranges[range].function : NO_FUNCTION;
			frame.instructions = dosrun_instructions;
			frame.memory = dosrun_memory;
			if (frame.function >= 0) {
				functions[(size_t)frame.function].calls++;
				functions[(size_t)frame.function].active++;
			}
			frames.push_back(frame);
			return;
		}
	}

	/* A return pops the address it goes to. */
	if (sp > (fromSp & mask) && !frames.empty()) {
		const uint32_t popped = SegPhys(ss) + (fromSp & mask);
		const uint32_t target = toBase + (size == 4 ? mem_readd(popped) : mem_readw(popped));
		if (target != to) return;
		size_t depth = 0;
		for (size_t i = frames.size();i > 0 && depth < 64;i--,depth++) {
			if (frames[i - 1].returnTo != to) continue;
			while (frames.size() >= i) {
				const Frame &frame = frames.back();
				if (frame.function >= 0) {
					Function &function = functions[(size_t)frame.function];
					function.active--;
					if (function.active == 0) {
						function.inclusive += dosrun_instructions - frame.instructions;
						function.inclusiveMemory += dosrun_memory - frame.memory;
					}
				}
				frames.pop_back();
			}
			break;
		}
	}
}

namespace {

std::string Quoted(const std::string &text)
{
	std::string out = "\"";
	for (size_t i = 0;i < text.size();i++) {
		const char c = text[i];
		if (c == '"' || c == '\\') out += '\\';
		if ((unsigned char)c < 0x20) continue;
		out += c;
	}
	return out + "\"";
}

std::string Number(uint64_t value) { return std::to_string((unsigned long long)value); }

} /* namespace */

std::string DEBUG_ProfileJson(void)
{
	if (debug_profiling) Flush();

	/* Frames still open (a program that exits inside its callees) count up to now, outermost only. */
	std::vector<Function> shown = functions;
	std::vector<uint32_t> open(functions.size(),0);
	for (size_t i = 0;i < frames.size();i++)
		if (frames[i].function >= 0) open[(size_t)frames[i].function]++;
	for (size_t i = 0;i < frames.size();i++) {
		const int32_t f = frames[i].function;
		if (f < 0) continue;
		open[(size_t)f]--;
		if (open[(size_t)f] == 0) {
			shown[(size_t)f].inclusive += dosrun_instructions - frames[i].instructions;
			shown[(size_t)f].inclusiveMemory += dosrun_memory - frames[i].memory;
		}
	}

	uint64_t attributed = 0,attributedMemory = 0;
	std::vector<size_t> order;
	for (size_t i = 0;i < shown.size();i++) {
		attributed += shown[i].self;
		attributedMemory += shown[i].selfMemory;
		order.push_back(i);
	}
	std::sort(order.begin(),order.end(),[&](size_t a,size_t b) { return shown[a].self > shown[b].self; });

	std::string out = "\"instructions\":" + Number(dosrun_instructions - startInstructions) +
	                  ",\"memory\":" + Number(dosrun_memory - startMemory) +
	                  ",\"attributed_instructions\":" + Number(attributed) +
	                  ",\"attributed_memory\":" + Number(attributedMemory) + ",\"functions\":[";
	for (size_t k = 0;k < order.size();k++) {
		const Function &f = shown[order[k]];
		out += (k ? "," : "") + std::string("{\"name\":") + Quoted(f.name) + ",\"program\":" + Quoted(f.program) +
		       ",\"self\":" + Number(f.self) + ",\"self_memory\":" + Number(f.selfMemory) +
		       ",\"inclusive\":" + Number(f.inclusive ? f.inclusive : f.self) + ",\"inclusive_memory\":" +
		       Number(f.inclusive ? f.inclusiveMemory : f.selfMemory) + ",\"calls\":" + Number(f.calls) + "}";
	}
	out += "],\"lines\":[";
	std::vector<size_t> lineOrder;
	for (size_t i = 0;i < lines.size();i++) lineOrder.push_back(i);
	std::sort(lineOrder.begin(),lineOrder.end(),[&](size_t a,size_t b) { return lines[a].self > lines[b].self; });
	for (size_t k = 0;k < lineOrder.size();k++) {
		const Line &l = lines[lineOrder[k]];
		out += (k ? "," : "") + std::string("{\"file\":") + Quoted(l.file) + ",\"line\":" + Number(l.line) +
		       ",\"function\":" + Quoted(l.function >= 0 ? functions[(size_t)l.function].name : "") +
		       ",\"self\":" + Number(l.self) + ",\"memory\":" + Number(l.memory) + "}";
	}
	return out + "]";
}

#endif
