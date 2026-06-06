/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <cctype>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#endif
  
#include "dosbox.h"
#include "cpu.h"
#include "paging.h"
#include "debug.h"
#include "logging.h"
#include "dos_inc.h"
#include "support.h"
#include "video.h"
#include "menu.h"
#include "SDL.h"

extern bool gbk, isDBCSCP(), isKanji1(uint8_t chr), shiftjis_lead_byte(int c);

void upcase(std::string &str) {
	int (*tf)(int) = std::toupper;
	std::transform(str.begin(), str.end(), str.begin(), tf);
}

void lowcase(std::string &str) {
	int (*tf)(int) = std::tolower;
	std::transform(str.begin(), str.end(), str.begin(), tf);
}

void trim(std::string &str) {
    const char whitespace[] = " \r\t\f\n";
	const auto empty_pfx = str.find_first_not_of(whitespace);
	if (empty_pfx == std::string::npos) {
		str.clear(); // whole string is filled with whitespace
		return;
	}
	const auto empty_sfx = str.find_last_not_of(whitespace);
	str.erase(empty_sfx + 1);
	str.erase(0, empty_pfx);
}

char *strchr_dbcs(char *str, char ch) {
    bool lead = false;
    int lastpos = -1;
    if ((ch == '\\' && (IS_PC98_ARCH || isDBCSCP())) || (ch == '|' && (IS_PC98_ARCH || (isDBCSCP() && !((dos.loaded_codepage == 936 || IS_PDOSV) && !gbk))))) {
        for (size_t i=0; i<strlen(str); i++) {
            if (lead) lead = false;
            else if ((IS_PC98_ARCH && shiftjis_lead_byte(str[i])) || (isDBCSCP() && isKanji1(str[i]))) lead = true;
            else if (str[i] == ch) {lastpos = i;break;}
        }
        return lastpos>-1 ? str + lastpos : NULL;
    } else
        return strchr(str, ch);
}

char *strrchr_dbcs(char *str, char ch) {
    bool lead = false;
    int lastpos = -1;
    if ((ch == '\\' && (IS_PC98_ARCH || isDBCSCP())) || (ch == '|' && (IS_PC98_ARCH || (isDBCSCP() && !((dos.loaded_codepage == 936 || IS_PDOSV) && !gbk))))) {
        for (size_t i=0; i<strlen(str); i++) {
            if (lead) lead = false;
            else if ((IS_PC98_ARCH && shiftjis_lead_byte(str[i])) || (isDBCSCP() && isKanji1(str[i]))) lead = true;
            else if (str[i] == ch) lastpos = i;
        }
        return lastpos>-1 ? str + lastpos : NULL;
    } else
        return strrchr(str, ch);
}

char *strtok_dbcs(char *s, const char *d) {
    if (!IS_PC98_ARCH && !isDBCSCP()) return strtok(s, d);
    static char* input = NULL;
    if (s != NULL) input = s;
    if (input == NULL) return NULL;
    char* result = new char[strlen(input) + 1];
    int i = 0;
    bool lead = false;
    for (; input[i] != '\0'; i++) {
        if (!lead && ((IS_PC98_ARCH && shiftjis_lead_byte(input[i])) || (isDBCSCP() && isKanji1(input[i])))) {
            result[i] = input[i];
            lead = true;
        } else if (input[i] != d[0] || lead) {
            result[i] = input[i];
            lead = false;
        } else {
            result[i] = '\0';
            input = input + i + 1;
            return result;
        }
    }
    result[i] = '\0';
    input = NULL;
    return result;
}

bool check_last_split_char(const char *name, size_t len, char split)
{
	bool tail = false;
	if((IS_PC98_ARCH || isDBCSCP()) && split == '\\') {
		bool lead = false;
		for(size_t pos = 0 ; pos < len ; pos++) {
			if(lead) lead = false;
        	else if ((IS_PC98_ARCH && shiftjis_lead_byte(name[pos])) || (isDBCSCP() && isKanji1(name[pos]))) lead = true;
			else if(pos == len - 1 && name[pos] == split) tail = true;
		}
	} else if(len > 0) {
		if(name[len - 1] == split) tail = true;
	}
	return tail;
}

/* 
	Ripped some source from freedos for this one.

*/


/*
 * replaces all instances of character o with character c
 */

void strreplace_dbcs(char * str,char o,char n) {
    bool lead = false;
	while (*str) {
        if (lead) lead = false;
        else if ((IS_PC98_ARCH && shiftjis_lead_byte(*str)) || (isDBCSCP() && isKanji1(*str))) lead = true;
		else if (*str==o) *str=n;
		str++;
	}
}

void strreplace(char * str,char o,char n) {
	while (*str) {
		if (*str==o) *str=n;
		str++;
	}
}
char *ltrim(char *str) { 
	while (*str && isspace(*reinterpret_cast<unsigned char*>(str))) str++;
	return str;
}

char *rtrim(char *str) {
	char *p;
	p = strchr(str, '\0');
	while (--p >= str && *reinterpret_cast<unsigned char*>(p) != '\f' && isspace(*reinterpret_cast<unsigned char*>(p))) {};

	p[1] = '\0';
	return str;
}

char *trim(char *str) {
	return ltrim(rtrim(str));
}

char * upcase(char * str) {
    for (char* idx = str; *idx ; idx++) *idx = toupper(*reinterpret_cast<unsigned char*>(idx));
    return str;
}

char * lowcase(char * str) {
	for(char* idx = str; *idx ; idx++)  *idx = tolower(*reinterpret_cast<unsigned char*>(idx));
	return str;
}

std::vector<std::string> split(const std::string& str, char split_char) {
    const char* cur = str.c_str();
    const char* str_end = str.c_str() + str.size();
    std::vector<std::string> chunks;
    while (cur < str_end) {
        const char* start = cur;
        while (start < str_end && *start == split_char)
            start++;
        const char* end = start + 1;
        while (end < str_end && *end != split_char)
            end++;
        cur = end + 1;
        chunks.push_back(std::string(start, end - start));
    }
    return chunks;
}

bool ScanCMDBool(char * cmd,char const * const check) {
	char * scan=cmd;size_t c_len=strlen(check);
	while ((scan=strchr(scan,'/'))) {
		/* found a / now see behind it */
		scan++;
		if (strncasecmp(scan,check,c_len)==0 && (scan[c_len]==' ' || scan[c_len]=='\t' || scan[c_len]=='/' || scan[c_len]==0)) {
		/* Found a math now remove it from the string */
			memmove(scan-1,scan+c_len,strlen(scan+c_len)+1);
			trim(scan-1);
			return true;
		}
	}
	return false;
}

/* This scans the command line for a remaining switch and reports it else returns 0*/
char * ScanCMDRemain(char * cmd) {
	char * scan,*found;
	if ((scan=found=strchr(cmd,'/'))) {
		while ( *scan && !isspace(*reinterpret_cast<unsigned char*>(scan)) ) scan++;
		*scan=0;
		return found;
	} else return nullptr;
}

char * StripWord(char *&line) {
	char * scan=line;
	scan=ltrim(scan);
	if (*scan=='"') {
		char * end_quote=strchr(scan+1,'"');
		if (end_quote) {
			*end_quote=0;
			line=ltrim(++end_quote);
			return (scan+1);
		}
	}
	char * begin=scan;
	for (char c = *scan ;(c = *scan);scan++) {
		if (isspace(*reinterpret_cast<unsigned char*>(&c))) {
			*scan++=0;
			break;
		}
	}
	line=scan;
	return begin;
}

char * StripArg(char *&line) {
       char * scan=line;
       int q=0;
       scan=ltrim(scan);
       char * begin=scan;
       for (char c = *scan ;(c = *scan);scan++) {
               if (*scan=='"') {
                       q++;
               } else if (q/2*2==q && isspace(*reinterpret_cast<unsigned char*>(&c))) {
			*scan++=0;
			break;
		}
	}
	line=scan;
	return begin;
}


Bits ConvDecWord(char * word) {
	bool negative=false;Bitu ret=0;
	if (*word=='-') {
		negative=true;
		word++;
	}
	while (char c=*word) {
		ret*=10u;
		ret+=(Bitu)c-'0';
		word++;
	}
	if (negative) return 0-(Bits)ret;
	else return (Bits)ret;
}

Bits ConvHexWord(char * word) {
	Bitu ret=0;
	while (char c=toupper(*reinterpret_cast<unsigned char*>(word))) {
		ret*=16;
		if (c>='0' && c<='9') ret+=(Bitu)c-'0';
		else if (c>='A' && c<='F') ret+=10u+((Bitu)c-'A');
		word++;
	}
	return (Bits)ret;
}

double ConvDblWord(char * word) {
    (void)word;//UNUSED
	return 0.0f;
}

int utf8_encode(char **ptr, const char *fence, uint32_t code) {
    int uchar_size=1;
    char *p = *ptr;

    if (!p) return UTF8ERR_NO_ROOM;
    if (code >= (uint32_t)0x80000000UL) return UTF8ERR_INVALID;
    if (p >= fence) return UTF8ERR_NO_ROOM;

    if (code >= 0x4000000) uchar_size = 6;
    else if (code >= 0x200000) uchar_size = 5;
    else if (code >= 0x10000) uchar_size = 4;
    else if (code >= 0x800) uchar_size = 3;
    else if (code >= 0x80) uchar_size = 2;

    if ((p+uchar_size) > fence) return UTF8ERR_NO_ROOM;

    switch (uchar_size) {
        case 1: *p++ = (char)code;
            break;
        case 2: *p++ = (char)(0xC0 | (code >> 6));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 3: *p++ = (char)(0xE0 | (code >> 12));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 4: *p++ = (char)(0xF0 | (code >> 18));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 5: *p++ = (char)(0xF8 | (code >> 24));
            *p++ = (char)(0x80 | ((code >> 18) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 6: *p++ = (char)(0xFC | (code >> 30));
            *p++ = (char)(0x80 | ((code >> 24) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 18) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
    }

    *ptr = p;
    return 0;
}

int utf8_decode(const char **ptr,const char *fence) {
    const char *p = *ptr;
    int uchar_size=1;
    int ret = 0,c;

    if (!p) return UTF8ERR_NO_ROOM;
    if (p >= fence) return UTF8ERR_NO_ROOM;

    ret = (unsigned char)(*p);
    if (ret >= 0xFE) { p++; return UTF8ERR_INVALID; }
    else if (ret >= 0xFC) uchar_size=6;
    else if (ret >= 0xF8) uchar_size=5;
    else if (ret >= 0xF0) uchar_size=4;
    else if (ret >= 0xE0) uchar_size=3;
    else if (ret >= 0xC0) uchar_size=2;
    else if (ret >= 0x80) { p++; return UTF8ERR_INVALID; }

    if ((p+uchar_size) > fence)
        return UTF8ERR_NO_ROOM;

    switch (uchar_size) {
        case 1: p++;
            break;
        case 2: ret = (ret&0x1F)<<6; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 3: ret = (ret&0xF)<<12; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 4: ret = (ret&0x7)<<18; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 5: ret = (ret&0x3)<<24; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<18;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 6: ret = (ret&0x1)<<30; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<24;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<18;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
    }

    *ptr = p;
    return ret;
}

int utf16le_encode(char **ptr, const char *fence, uint32_t code) {
    char *p = *ptr;

    if (!p) return UTF8ERR_NO_ROOM;
    if (code > 0x10FFFF) return UTF8ERR_INVALID;
    if (code > 0xFFFF) { /* UTF-16 surrogate pair */
        uint32_t lo = (code - 0x10000) & 0x3FF;
        uint32_t hi = ((code - 0x10000) >> 10) & 0x3FF;
        if ((p+2+2) > fence) return UTF8ERR_NO_ROOM;
        *p++ = (char)( (hi+0xD800)       & 0xFF);
        *p++ = (char)(((hi+0xD800) >> 8) & 0xFF);
        *p++ = (char)( (lo+0xDC00)       & 0xFF);
        *p++ = (char)(((lo+0xDC00) >> 8) & 0xFF);
    }
    else if ((code&0xF800) == 0xD800) { /* do not allow accidental surrogate pairs (0xD800-0xDFFF) */
        return UTF8ERR_INVALID;
    }
    else {
        if ((p+2) > fence) return UTF8ERR_NO_ROOM;
        *p++ = (char)( code       & 0xFF);
        *p++ = (char)((code >> 8) & 0xFF);
    }

    *ptr = p;
    return 0;
}

int utf16le_decode(const char **ptr,const char *fence) {
    const char *p = *ptr;
    unsigned int ret,b=2;

    if (!p) return UTF8ERR_NO_ROOM;
    if ((p+1) >= fence) return UTF8ERR_NO_ROOM;

    ret = (unsigned char)p[0];
    ret |= ((unsigned int)((unsigned char)p[1])) << 8;
    if (ret >= 0xD800U && ret <= 0xDBFFU)
        b=4;
    else if (ret >= 0xDC00U && ret <= 0xDFFFU)
        { p++; return UTF8ERR_INVALID; }

    if ((p+b) > fence)
        return UTF8ERR_NO_ROOM;

    p += 2;
    if (ret >= 0xD800U && ret <= 0xDBFFU) {
        /* decode surrogate pair */
        unsigned int hi = ret & 0x3FFU;
        unsigned int lo = (unsigned char)p[0];
        lo |= ((unsigned int)((unsigned char)p[1])) << 8;
        p += 2;
        if (lo < 0xDC00U || lo > 0xDFFFU) return UTF8ERR_INVALID;
        lo &= 0x3FFU;
        ret = ((hi << 10U) | lo) + 0x10000U;
    }

    *ptr = p;
    return (int)ret;
}

#if C_DEBUG
#include <curses.h>
#endif
#if defined(WIN32)
void DOSBox_ConsolePauseWait();
#endif
bool sdl_wait_on_error();

static void WriteCrashSegment(FILE *f, const char *name, const SegNames seg)
{
	fprintf(f, "%-2s sel=%04X base=%08X limit=%08X expanddown=%d\n",
	        name,
	        (unsigned int)SegValue(seg),
	        (unsigned int)SegPhys(seg),
	        (unsigned int)SegLimit(seg),
	        Segs.expanddown[seg] ? 1 : 0);
}

static void WriteCrashStack(FILE *f)
{
	const LinearPt stack_linear = (LinearPt)(SegPhys(ss) + reg_esp);

	fprintf(f, "\nGuest stack near SS:ESP (linear %08X):\n", (unsigned int)stack_linear);
	for (unsigned int row = 0; row < 8; row++) {
		const LinearPt line = stack_linear + (row * 16);
		fprintf(f, "%08X  ", (unsigned int)line);
		for (unsigned int i = 0; i < 16; i++) {
			uint8_t value = 0;
			if (mem_readb_checked(line + i, &value))
				fprintf(f, "?? ");
			else
				fprintf(f, "%02X ", (unsigned int)value);
		}
		fprintf(f, "\n");
	}
}

static void WriteHostBacktrace(FILE *f)
{
#if defined(__APPLE__) || defined(__linux__)
	fprintf(f, "\nHost stack trace:\n");
	void *frames[64];
	const int count = backtrace(frames, 64);
	char **symbols = backtrace_symbols(frames, count);
	if (symbols != nullptr) {
		for (int i = 0; i < count; i++) {
			fprintf(f, "%02d  %s\n", i, symbols[i]);
		}
		free(symbols);
	} else {
		fprintf(f, "(backtrace_symbols failed)\n");
	}
#else
	fprintf(f, "\nHost stack trace unavailable on this platform.\n");
#endif
}

static void WriteHostRegisters(FILE *f)
{
	fprintf(f, "\nHost registers:\n");
#if defined(__aarch64__)
	uint64_t regs[31] = {};
	uint64_t sp = 0;
	uint64_t fp = 0;
	uint64_t lr = 0;

#define READ_ARM64_REG(index) __asm__ volatile("mov %0, x" #index : "=r"(regs[index]))
	READ_ARM64_REG(0);
	READ_ARM64_REG(1);
	READ_ARM64_REG(2);
	READ_ARM64_REG(3);
	READ_ARM64_REG(4);
	READ_ARM64_REG(5);
	READ_ARM64_REG(6);
	READ_ARM64_REG(7);
	READ_ARM64_REG(8);
	READ_ARM64_REG(9);
	READ_ARM64_REG(10);
	READ_ARM64_REG(11);
	READ_ARM64_REG(12);
	READ_ARM64_REG(13);
	READ_ARM64_REG(14);
	READ_ARM64_REG(15);
	READ_ARM64_REG(16);
	READ_ARM64_REG(17);
	READ_ARM64_REG(18);
	READ_ARM64_REG(19);
	READ_ARM64_REG(20);
	READ_ARM64_REG(21);
	READ_ARM64_REG(22);
	READ_ARM64_REG(23);
	READ_ARM64_REG(24);
	READ_ARM64_REG(25);
	READ_ARM64_REG(26);
	READ_ARM64_REG(27);
	READ_ARM64_REG(28);
	READ_ARM64_REG(29);
	READ_ARM64_REG(30);
#undef READ_ARM64_REG
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	__asm__ volatile("mov %0, x29" : "=r"(fp));
	__asm__ volatile("mov %0, x30" : "=r"(lr));

	for (unsigned int i = 0; i < 31; i += 2) {
		if (i + 1 < 31) {
			fprintf(f, "x%-2u=%016llX x%-2u=%016llX\n",
			        i,
			        (unsigned long long)regs[i],
			        i + 1,
			        (unsigned long long)regs[i + 1]);
		} else {
			fprintf(f, "x%-2u=%016llX\n", i, (unsigned long long)regs[i]);
		}
	}
	fprintf(f, "sp =%016llX fp =%016llX lr =%016llX return=%p\n",
	        (unsigned long long)sp,
	        (unsigned long long)fp,
	        (unsigned long long)lr,
	        __builtin_return_address(0));
#elif defined(__x86_64__)
	uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0;
	uint64_t rsi = 0, rdi = 0, rbp = 0, rsp = 0;
	uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0;
	uint64_t r12 = 0, r13 = 0, r14 = 0, r15 = 0;
	uint64_t flags = 0;
	__asm__ volatile("mov %%rax, %0" : "=r"(rax));
	__asm__ volatile("mov %%rbx, %0" : "=r"(rbx));
	__asm__ volatile("mov %%rcx, %0" : "=r"(rcx));
	__asm__ volatile("mov %%rdx, %0" : "=r"(rdx));
	__asm__ volatile("mov %%rsi, %0" : "=r"(rsi));
	__asm__ volatile("mov %%rdi, %0" : "=r"(rdi));
	__asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
	__asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
	__asm__ volatile("mov %%r8, %0" : "=r"(r8));
	__asm__ volatile("mov %%r9, %0" : "=r"(r9));
	__asm__ volatile("mov %%r10, %0" : "=r"(r10));
	__asm__ volatile("mov %%r11, %0" : "=r"(r11));
	__asm__ volatile("mov %%r12, %0" : "=r"(r12));
	__asm__ volatile("mov %%r13, %0" : "=r"(r13));
	__asm__ volatile("mov %%r14, %0" : "=r"(r14));
	__asm__ volatile("mov %%r15, %0" : "=r"(r15));
	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
	fprintf(f, "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
	        (unsigned long long)rax,
	        (unsigned long long)rbx,
	        (unsigned long long)rcx,
	        (unsigned long long)rdx);
	fprintf(f, "RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
	        (unsigned long long)rsi,
	        (unsigned long long)rdi,
	        (unsigned long long)rbp,
	        (unsigned long long)rsp);
	fprintf(f, "R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
	        (unsigned long long)r8,
	        (unsigned long long)r9,
	        (unsigned long long)r10,
	        (unsigned long long)r11);
	fprintf(f, "R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
	        (unsigned long long)r12,
	        (unsigned long long)r13,
	        (unsigned long long)r14,
	        (unsigned long long)r15);
	fprintf(f, "RFLAGS=%016llX return=%p\n",
	        (unsigned long long)flags,
	        __builtin_return_address(0));
#else
	fprintf(f, "Unavailable for this host architecture.\n");
#endif
	fprintf(f, "note: captured inside E_Exit crash-report generation, not from a host signal context.\n");
}

static void WriteCrashReport(const char *message)
{
	char path[256];
	time_t now = time(nullptr);
	snprintf(path, sizeof(path), "build/dosbox-crash-%ld.log", (long)now);

	FILE *f = fopen(path, "w");
	if (f == nullptr) {
		f = fopen("dosbox-crash.log", "w");
	}
	if (f == nullptr) return;

	fprintf(f, "DOSBox-X crash report\n");
	fprintf(f, "timestamp_unix: %ld\n", (long)now);
	fprintf(f, "fatal: %s\n", message != nullptr ? message : "(null)");

	fprintf(f, "\n[guest]\n");
	fprintf(f, "mode: pmode=%d vm86=%d cpl=%lu mpl=%lu paging=%d wp=%d code_big=%d stack_big=%d\n",
	        cpu.pmode ? 1 : 0,
	        GETFLAG(VM) ? 1 : 0,
	        (unsigned long)cpu.cpl,
	        (unsigned long)cpu.mpl,
	        paging.enabled ? 1 : 0,
	        paging.wp ? 1 : 0,
	        cpu.code.big ? 1 : 0,
	        cpu.stack.big ? 1 : 0);
	fprintf(f, "control: CR0=%08X CR2=%08X CR3=%08X CR4=%08X\n",
	        (unsigned int)CPU_GET_CRX(0),
	        (unsigned int)CPU_GET_CRX(2),
	        (unsigned int)CPU_GET_CRX(3),
	        (unsigned int)CPU_GET_CRX(4));
	fprintf(f, "tables: GDTR base=%08X limit=%08lX IDTR base=%08X limit=%08lX LDTR=%04lX TR=%04lX\n",
	        (unsigned int)CPU_SGDT_base(),
	        (unsigned long)CPU_SGDT_limit(),
	        (unsigned int)CPU_SIDT_base(),
	        (unsigned long)CPU_SIDT_limit(),
	        (unsigned long)CPU_SLDT(),
	        (unsigned long)CPU_STR());

	fprintf(f, "\nGuest registers:\n");
	fprintf(f, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n",
	        (unsigned int)reg_eax,
	        (unsigned int)reg_ebx,
	        (unsigned int)reg_ecx,
	        (unsigned int)reg_edx);
	fprintf(f, "ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
	        (unsigned int)reg_esi,
	        (unsigned int)reg_edi,
	        (unsigned int)reg_ebp,
	        (unsigned int)reg_esp);
	fprintf(f, "CS=%04X DS=%04X ES=%04X SS=%04X FS=%04X GS=%04X EIP=%08X FLAGS=%08X\n",
	        (unsigned int)SegValue(cs),
	        (unsigned int)SegValue(ds),
	        (unsigned int)SegValue(es),
	        (unsigned int)SegValue(ss),
	        (unsigned int)SegValue(fs),
	        (unsigned int)SegValue(gs),
	        (unsigned int)reg_eip,
	        (unsigned int)reg_flags);

	fprintf(f, "\nGuest segment caches:\n");
	WriteCrashSegment(f, "CS", cs);
	WriteCrashSegment(f, "DS", ds);
	WriteCrashSegment(f, "ES", es);
	WriteCrashSegment(f, "SS", ss);
	WriteCrashSegment(f, "FS", fs);
	WriteCrashSegment(f, "GS", gs);
	WriteCrashStack(f);

	fprintf(f, "\n[host]\n");
	fprintf(f, "core=%s cycles=%lld cycle_left=%lld cycle_max=%lld\n",
	        core_mode,
	        (long long)CPU_Cycles,
	        (long long)CPU_CycleLeft,
	        (long long)CPU_CycleMax);
	WriteHostRegisters(f);
	WriteHostBacktrace(f);

	fclose(f);
	LOG_MSG("Crash report written to %s", path);
}

static char buf[1024];           //greater scope as else it doesn't always gets thrown right (linux/gcc2.95)
void E_Exit(const char * format,...) {
#if C_DEBUG && C_HEAVY_DEBUG
 	DEBUG_HeavyWriteLogInstruction();
#endif
	va_list msg;
	va_start(msg,format);
	vsnprintf(buf,sizeof(buf),format,msg);
	va_end(msg);
	buf[sizeof(buf) - 1] = '\0';
	strcat(buf,"\n");
	LOG_MSG("E_Exit: %s\n",buf);
	WriteCrashReport(buf);
#if defined(WIN32)
	/* Most Windows users DON'T run DOSBox-X from the command line! */
	MessageBox(GetHWND(), buf, "E_Exit", MB_OK | MB_ICONEXCLAMATION);
#endif
#if C_DEBUG
	endwin();
#endif
	fprintf(stderr, "E_Exit: %s\n", buf);
	SDL_Quit();
	if (sdl_wait_on_error()) {
#if defined(WIN32)
        DOSBox_ConsolePauseWait();
#endif
    }
	exit(1);
}
