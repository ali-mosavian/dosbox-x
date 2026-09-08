# 16-bit DOS debug-info formats — parser reference

Source of record: OpenWatcom (`open-watcom/open-watcom-v2`, `master`), read from
`raw.githubusercontent.com`. Every layout below is transcribed from a file listed in
**Sources**. Anything not in a file I read is marked **UNKNOWN**.

All integers little-endian. All CodeView structs are `#pragma pack(1)` — `cv4.h` wraps
them in `pushpck1.h` / `poppck.h`, so the byte offsets below are exact, no padding.

Type shorthand used in `cv4f.h`/`cv4w.h`: `u1 u2 u4 u8` = unsigned 8/16/32/64,
`i1 i2 i4 i8` = signed.

---

## 1. Microsoft CodeView, packed CV4/CV5, appended to a linked MZ .EXE

### 1.1 Signatures

`cv4.h`, `#define CV_SIG_SIZE 4`:

    CV4_NB05  "NB05"
    CV4_NB07  "NB07"
    CV4_NB08  "NB08"
    CV4_NB09  "NB09"
    CV4_NB10  "NB10"
    CV4_NB11  "NB11"

**UNKNOWN: OpenWatcom nowhere states which NBxx means CV3 / CV4 / CV5.** `cv4.h`
lists the six names with no comment. Do not take the usual folklore mapping from this
document — it is not in the source.

What the source *does* establish, and it matters:

- **`wlink` writes `NB05`.** `bld/wl/c/dbgcv.c:684` — `memcpy( start.sig, CV4_NB05, CV_SIG_SIZE )`,
  and the comment at `dbgcv.c:777` reads "write DEBUG_TYPE_CODEVIEW data: CodeView NB05 data".
- **The OpenWatcom CodeView DIP only accepts `NB09`.** `bld/dip/codeview/c/cvld.c:163`
  and `:191` both `memcmp` against `CV4_NB09` and fail otherwise.

So OW's own CodeView reader will not load OW's own linker's CodeView output. A parser
should accept the signature *set*, not one member. A separate DIP, `bld/dip/hllcv`,
handles `NB00`/`NB02`/`NB04` (IBM HLL / CV3 era) and is not this format.

### 1.2 The 8-byte trailer/header — `cv_trailer` (`cv4.h`)

    off  size  field
    0    4     char      sig[4]      "NBxx"
    4    4     signed_32 offset
    ---  8 bytes total

**There are two of them, one at each end of the CV block**, and `offset` means a
*different thing* in each. From the writer, `dbgcv.c:684-688`:

    memcpy( start.sig, CV4_NB05, CV_SIG_SIZE );
    start.offset = SectAddrs[CVSECT_MODDIR].u.vm_ptr - CVBase;   /* head */
    PutInfo( CVBase, &start, sizeof( cv_trailer ) );
    start.offset = CVSize;                                       /* tail */
    PutInfo( CVBase + CVSize - sizeof( cv_trailer ), &start, sizeof( cv_trailer ) );

- **Head**, at `CVBase` (= the file offset of the `NBxx` bytes, which is the *base* for
  every `lfo` in the format): `offset` = `lfoDirectory`, the offset of the subsection
  directory **relative to CVBase**.
- **Tail**, at `CVBase + CVSize - 8` (last 8 bytes of the file when the CV block is
  appended last): `offset` = `CVSize`, the **total byte length of the CV block including
  both trailers**.

`CVSize` counts `2 * sizeof(cv_trailer)` plus all sections (`dbgcv.c:673-676`).

### 1.3 Finding the block in an appended MZ EXE

`cvld.c:151-169` (`TryFindTrailer`), verbatim logic:

    seek to EOF-8; pos = that offset
    read cv_trailer sig
    if sig.sig != "NB09" -> fail
    *sizep = sig.offset - sizeof(sig);     /* = CVSize - 8 */
    *offp  = pos - *sizep;                 /* = filesize - sig.offset  == CVBase */

So: **`CVBase = filesize - tail_trailer.offset`.** Then `cvld.c:185-193` seeks to
`CVBase` and re-checks the 4 signature bytes there.

`cvld.c:328` then loads the directory from `off + CV_SIG_SIZE`, i.e. it reads the u32
at `CVBase+4` (the head trailer's `offset` field = `lfoDirectory`) and seeks to
`bias + lfoDirectory` (`cvld.c:210-215`).

**Base/bias rule for the whole format:** `iih->bias = CVBase` (`cvld.c:321`), and the
VM layer adds it on every read — `cvvirt.c:223`, `pg_start += iih->bias`. Therefore
**every `lfo` in the directory, and every intra-subsection offset, is relative to
CVBase**, never to the file.

Note the size the DIP maps is `CVSize - 8`, i.e. the tail trailer is excluded.

(For PE, `cvld.c:86-149` instead walks the PE debug directory looking for
`DEBUG_TYPE_CODEVIEW` and uses `dir.data_seek` / `dir.debug_size`. Not relevant to MZ.)

### 1.4 Subsection directory

`cv_subsection_directory` (`cv4.h`) — the "OMFDirHeader", **16 bytes**:

    off  size  field
    0    2     unsigned_16 cbDirHeader     must == 16
    2    2     unsigned_16 cbDirEntry      must == 12
    4    4     unsigned_32 cDir            number of entries
    8    4     unsigned_32 lfoNextDir
    12   4     unsigned_32 flags
    ---  16

`cvld.c:219-222` hard-rejects the file unless `cbDirHeader == sizeof(header)` and
`cbDirEntry == sizeof(entry)`. `dbgcv.c:689-693` writes `lfoNextDir = 0`, `flags = 0`.

`cv_directory_entry` (`cv4.h`) — the "OMFDirEntry", **12 bytes**, `cDir` of them
packed immediately after the header:

    off  size  field
    0    2     unsigned_16 subsection      an sst* value, see 1.5
    2    2     unsigned_16 iMod            1-based module index; IMH_GBL for global subsections
    4    4     unsigned_32 lfo             offset of subsection data, relative to CVBase
    8    4     unsigned_32 cb              byte length of the subsection
    ---  12

**On CV4 vs CV5 field widths:** the caller's brief expected `cb` to possibly be u16 in
one of the two. **In OpenWatcom there is only one definition** — `lfo` u32 and `cb` u32,
entry size 12 — and both the reader and the writer use it. OpenWatcom does not model a
narrower CV4 variant. **UNKNOWN** whether a genuine 16-bit-`cb` variant exists; nothing
I read describes one. Since `cbDirEntry` is in the header, a parser should trust it and
treat `!= 12` as unsupported (which is what OW does).

Global (non-per-module) subsections are found by `FindDirEntry( iih, IMH_GBL, kind )`
(`cvmisc.c:86-...`) — a linear scan matching both `subsection` and `iMod`.

### 1.5 `sst*` subsection kinds (`cv4.h`, `typedef enum { ... } sst`)

The enum is written with only the first value explicit, so these are the consecutive
values it yields:

    0x120  sstModule
    0x121  sstTypes
    0x122  sstPublic
    0x123  sstPublicSym
    0x124  sstSymbols
    0x125  sstAlignSym
    0x126  sstSrcLnSeg
    0x127  sstSrcModule
    0x128  sstLibraries
    0x129  sstGlobalSym
    0x12A  sstGlobalPub
    0x12B  sstGlobalTypes
    0x12C  sstMPC
    0x12D  sstSegMap
    0x12E  sstSegName
    0x12F  sstPreComp
    0x130  sstPreCompMap
    0x131  sstOffsetMap16
    0x132  sstOffsetMap32
    0x133  sstFileIndex
    0x134  sstStaticSym

### 1.6 `sstModule`

`cv_sst_module` (`cv4.h`), header **8 bytes**:

    off  size  field
    0    2     unsigned_16 ovlNumber
    2    2     unsigned_16 iLib
    4    2     unsigned_16 cSeg
    6    2     unsigned_16 Style
    8    ...   cv_seginfo SegInfo[cSeg]
    then       length-prefixed name

`Style` is a u16, not `char[2]`. `cv4.h` defines
`#define CV_DEBUG_STYLE ('V' << 8 | 'C')` — i.e. **bytes `'C','V'` in file order**.

`cv_seginfo` — **12 bytes** each:

    off  size  field
    0    2     unsigned_16 Seg
    2    2     unsigned_16 pad
    4    4     unsigned_32 offset
    8    4     unsigned_32 cbSeg
    ---  12

**Module name**, at `&SegInfo[cSeg]` i.e. `8 + 12*cSeg`, is a **Pascal string: one u8
length byte, then that many bytes, not NUL-terminated**. Confirmed by `cvmod.c` `ModName`:

    name = (char *)&mp->SegInfo[mp->cSeg];
    len  = *(unsigned_8 *)name;
    ++name;

(That routine then strips path and extension for display; the raw field is the full name.)

### 1.7 `sstGlobalPub` / `sstGlobalSym` / `sstStaticSym`

`cv_sst_global_pub_header` (`cv4.h`) — **16 bytes**:

    off  size  field
    0    2     unsigned_16 symhash      hash-function index; OW handles only 10
    2    2     unsigned_16 addrhash     hash-function index; OW handles only 12
    4    4     unsigned_32 cbSymbol     bytes of the symbol area
    8    4     unsigned_32 cbSymHash    bytes of the name-hash table
    12   4     unsigned_32 cbAddrHash   bytes of the address-hash table
    ---  16

Layout of the subsection, from `cde->lfo`:

    lfo + 0                                        header (16 bytes)
    lfo + 16                                       symbol area,     cbSymbol bytes
    lfo + 16 + cbSymbol                            name hash table, cbSymHash bytes
    lfo + 16 + cbSymbol + cbSymHash                addr hash table, cbAddrHash bytes

**The symbol area is a plain run of ordinary CV symbol records** — confirmed by
`cvsym.c` `TableWalkSym` (:1079-1086):

    base = cde->lfo + sizeof( *hdr );
    end  = base + hdr->cbSymbol;
    while( base < end ) { p = VMRecord(iih, base); skip = p->common.length + 2; ... }

Note there is **no leading u32 signature** on this subsection (contrast 1.9).
A parser that only wants "name -> seg:off" can ignore both hash tables entirely and
just walk the symbol area.

Record byte offsets referenced by `SymFillIn` are *relative to the start of the symbol
area*: `cvsym.c:983` and `:887` both do `*(unsigned_32*)p + cde->lfo + sizeof(*hdr)`.

#### Name hash table (`symhash == 10`), `cvsym.c:957-977`

    +0    u2   hash_buckets
    +2    u2   (skipped; hash_base advances by 2*sizeof(u16) = 4)
    +4         u4 bucket_sym_base[hash_buckets]   -- byte offset, see below
               u4 bucket_count[hash_buckets]
               then the chains: pairs of { u4 sym_offset, u4 hash }

    bucket's chain start = (bucket_sym_base[i]) + (base_of_counts + 4*hash_buckets)
    where base_of_counts = lfo + 16 + cbSymbol + 4

Bucket selected as `hash % hash_buckets`. Each chain entry is 8 bytes:
`[0]` = symbol offset within the symbol area, `[1]` = the name hash. OW compares the
stored hash first, then the actual name.

Hash function, `cvsym.c:900-927` (`CalcHash`) — uppercase-folding XOR with a rotate:

    end = 0;
    for( i = len & 3; i > 0; --i ) { end |= (name[len-1] & 0xdf); end <<= 8; --len; }
    len /= 4;  sum = 0;
    while( len-- ) { sum ^= (*(u4*)name) & 0xdfdfdfdf; sum = rotl32(sum, 4); name += 4; }
    return sum ^ end;

(Note the tail loop's `end |= ...; end <<= 8;` order — the final shift happens after the
last OR, so the low byte is zero for any non-multiple-of-4 length. Transcribe it exactly;
it is what the tables were built with.)

#### Address hash table (`addrhash == 12`), `cvsym.c:827-891`

    +0    u2   num_segs
    +2    u2   (skipped; hash_base advances by 4)
    +4         u4 seg_base[num_segs]      byte offset, relative as below
               u4 seg_count[num_segs]     number of (offset,symoff) pairs for that segment
               then per segment, `seg_count` entries of { u4 sym_offset, u4 addr_offset }

    chain start = seg_base[i] + (hash_base + 4*num_segs) + 4*num_segs

Segment `i` corresponds to logical segment `i+1` (`chk.mach.segment = i + 1`).
Offsets within a segment's run are **sorted ascending** (`cvsym.c:856`, "offsets are
sorted, so we can binary search this sucker").

### 1.8 Generic symbol record framing, packed form

`s_common` (`cv4w.h:334-337`):

    off  size  field
    0    2     u2 length     byte count of everything AFTER this field
    2    2     u2 code       an S_* value
    4    ...   payload
    ---  next record at  cur + length + 2

`length` **excludes itself** and includes the `code` field. Every walker in the DIP
advances by `p->common.length + sizeof( p->common.length )` — `cvsym.c:57, 553, 611,
646, 705, 1086, 1670`, `cvmod.c` `GetCompInfo`.

**Names inside symbol records are Pascal strings**: u8 length, then that many bytes,
no terminator. `cvsym.c:172-175`:

    name = (const char *)p + skip;
    *name_len_p = *(unsigned_8 *)name;
    *name_p = &name[1];

where `skip` is the fixed size of the record for its `code` (the table at
`cvsym.c:101-171` is exactly the "how long is the fixed part" map).

### 1.9 `S_*` codes (`cv4syms.h`)

    0x0001 S_COMPILE      0x0002 S_REGISTER    0x0003 S_CONSTANT   0x0004 S_UDT
    0x0005 S_SSEARCH      0x0006 S_END         0x0007 S_SKIP       0x0008 S_CVRESERVE
    0x0009 S_OBJNAME      0x000a S_ENDARG      0x000b S_COBOLUDT   0x000c S_MANYREG
    0x000d S_RETURN       0x000e S_ENTRYTHIS

    16:16 segmented
    0x0100 S_BPREL16      0x0101 S_LDATA16     0x0102 S_GDATA16    0x0103 S_PUB16
    0x0104 S_LPROC16      0x0105 S_GPROC16     0x0106 S_THUNK16    0x0107 S_BLOCK16
    0x0108 S_WITH16       0x0109 S_LABEL16     0x010a S_CEXMODEL16 0x010b S_VFTPATH16
    0x010c S_REGREL16

    16:32 segmented
    0x0200 S_BPREL32      0x0201 S_LDATA32     0x0202 S_GDATA32    0x0203 S_PUB32
    0x0204 S_LPROC32      0x0205 S_GPROC32     0x0206 S_THUNK32    0x0207 S_BLOCK32
    0x0208 S_WITH32       0x0209 S_LABEL32     0x020a S_CEXMODEL32 0x020b S_VFTPATH32
    0x020c S_REGREL32     0x020d S_LTHREAD32   0x020e S_GTHREAD32

    CV Pack optimizations
    0x0400 S_PROCREF      0x0401 S_DATAREF     0x0402 S_ALIGN

    CodeView 5.0
    0x1009 S_PUB32_NEW

### 1.10 16-bit symbol record layouts

Offsets below are **from the start of the record**, i.e. the 4-byte `s_common` is
included. Field structs from `cv4f.h`; the `s_common`+payload composition from `cv4w.h`.

**S_PUB16 (0x0103), S_LDATA16 (0x0101), S_GDATA16 (0x0102)** — identical shape
(`cs_pub16`, `cs_ldata16`, `cs_gdata16`); `cvsym.c:121-125` uses one `skip` for all three:

    0    u2  length
    2    u2  code
    4    u2  offset
    6    u2  segment
    8    u2  type
    10   u1  name_len
    11   ..  name bytes

**S_BPREL16 (0x0100)** — `cs_bprel16`:

    0    u2  length
    2    u2  code
    4    i2  offset        SIGNED, frame-pointer relative
    6    u2  type
    8    u1  name_len
    9    ..  name

**S_REGREL16 (0x010c)** — `cs_regrel16`:

    0    u2  length
    2    u2  code
    4    i2  offset        SIGNED
    6    u2  reg
    8    u2  type
    10   u1  name_len
    11   ..  name

**S_LPROC16 (0x0104), S_GPROC16 (0x0105)** — `cs_lproc16` / `cs_gproc16`, same struct:

    0    u2  length
    2    u2  code
    4    u4  pParent
    8    u4  pEnd
    12   u4  pNext
    16   u2  proc_length
    18   u2  debug_start
    20   u2  debug_end
    22   u2  offset
    24   u2  segment
    26   u2  proctype
    28   u1  flags         cv_proc bitfield, LSB-first: fpo:1 interr:1 far_ret:1
                           never:1 unused:4
    29   u1  name_len
    30   ..  name

**S_LABEL16 (0x0109)** — `cs_label16`:

    0    u2  length
    2    u2  code
    4    u2  offset
    6    u2  segment
    8    u1  near_far
    9    u1  name_len
    10   ..  name

**S_BLOCK16 (0x0107)** — `cs_block16`:

    0    u2  length
    2    u2  code
    4    u4  pParent
    8    u4  pEnd
    12   u2  length_of_block
    14   u2  offset
    16   u2  segment
    18   u1  name_len
    19   ..  name

**S_WITH16 (0x0108)** — `cs_with16`: same first 18 bytes as `cs_block16`, then a
`value[]` blob rather than a name.

**S_THUNK16 (0x0106)** — `cs_thunk16`:

    0    u2  length
    2    u2  code
    4    u4  pParent
    8    u4  pEnd
    12   u4  pNext
    16   u2  offset
    18   u2  segment
    20   u2  length_of_thunk
    22   u1  ordinal       cv_ordinal: 0 NOTYPE, 1 ADJUSTOR, 2 VCALL, 3 PCODE
    23   u1  name_len
    24   ..  name, then an ordinal-dependent variant blob

**S_SSEARCH (0x0005)** — `cs_ssearch`, needed to enter a module's symbol run:

    0    u2  length
    2    u2  code
    4    u4  sym_off       offset of first scope symbol, relative to the subsection lfo
    8    u2  segment

**S_COMPILE (0x0001)** — `cs_compile`:

    0    u2  length
    2    u2  code
    4    u1  machine       cv_machine_class
    5    u1  language      cv_lang
    6    u2  flags         cv_compile bitfield
    8    ..  version[] (length-prefixed)

`cv_lang` (`cv4f.h`):

    0 LANG_C   1 LANG_CPP   2 LANG_FORTRAN   3 LANG_MASM
    4 LANG_PASCAL   5 LANG_BASIC   6 LANG_COBOL

`cv_machine_class` (`cv4f.h`):

    0x00 MACH_INTEL_8080    0x01 MACH_INTEL_8086   0x02 MACH_INTEL_80286
    0x03 MACH_INTEL_80386   0x04 MACH_INTEL_80486  0x05 MACH_INTEL_PENTIUM
    0x10 MACH_MIPS_R4000    0x20 MACH_MC68000      0x21 MACH_MC68010
    0x22 MACH_MC68020       0x23 MACH_MC68030      0x24 MACH_MC68040
    0x30 MACH_DECALPHA

`cvld.c:296` dispatches on `machine & 0xf0`: `MACH_INTEL_8080` (i.e. 0x00-0x0f) -> x86,
`MACH_DECALPHA` -> Alpha, anything else -> invalid.

`cv_compile` flags bitfield (`cv4f.h`), LSB-first:

    bit  0     PCodePresent:1
    bits 1-2   FloatPrecision:2
    bits 3-4   FloatPackage:2      0 HARDWARE, 1 EMULATOR, 2 ALTMATH
    bits 5-7   AmbientData:3       cv_ambient: 0 NEAR, 1 FAR, 2 HUGE
    bits 8-10  AmbientCode:3       cv_ambient
    bit  11    Mode32:1
    bits 12-15 Reserved:4

**S_OBJNAME (0x0009)** — `cs_objname`: `4 u4 signature`, then a length-prefixed name.

**S_PROCREF (0x0400) / S_DATAREF (0x0401)** are indirections, not definitions.
`cvsym.c:61-72`: they carry `module` and `offset`; the real record lives at
`FindDirEntry( iih, module, sstAlignSym )->lfo + offset`.

**S_ALIGN (0x0402)** is padding — skip by the normal framing rule (`cvsym.c:56-59`).

### 1.11 Per-module symbol subsections (`sstSymbols` / `sstAlignSym` / `sstStaticSym`)

**These begin with a u32 signature, then the symbol records.** `cvsym.c:536-537` and
`cvmod.c` `GetCompInfo`:

    vm   = cde->lfo + sizeof( unsigned_32 );
    left = cde->cb  - sizeof( unsigned_32 );

`cv4.h` defines `#define CV_OMF_SIG 0x00000001`, which is what that u32 holds.
(`sstGlobalPub`/`sstGlobalSym` do **not** have this — they have the 16-byte hash header
instead.)

### 1.12 `sstSegMap`

`cv_sst_seg_map` (`cv4.h`):

    off  size  field
    0    2     unsigned_16 cSeg        segment count
    2    2     unsigned_16 cSegLog     logical segment count
    4    ...   seg_desc segdesc[]

`seg_desc` — **20 bytes** each:

    off  size  field
    0    2     unsigned_16 flags
    2    2     unsigned_16 ovl
    4    2     unsigned_16 group
    6    2     unsigned_16 frame
    8    2     unsigned_16 iSegName
    10   2     unsigned_16 iClassName
    12   4     unsigned_32 offset
    16   4     unsigned_32 cbseg
    ---  20

`flags` bitfield (`struct seg_desc_flags`, `cv4.h`), LSB-first:

    bit  0     fRead
    bit  1     fWrite
    bit  2     fExecute
    bit  3     f32Bit
    bits 4-7   res3
    bit  8     fSel
    bit  9     fAbs
    bits 10-11 res2
    bit  12    fGroup
    bits 13-15 res

**Two things the reader does that a parser must copy:**

- OW allocates and copies **`cSegLog`** descriptors, not `cSeg` — `cvld.c:265-271`,
  `size = map->cSegLog * sizeof(map->segdesc[0])`, `iih->map_count = map->cSegLog`.
- **Logical segment numbers are 1-based** into that array: `cvld.c:375` and `:383`,
  `map = &iih->mapping[log-1]`.

Translation of a logical address to a real one (`MapLogical`, `cvld.c:379-388`):

    real_segment = mapping[log_seg - 1].frame
    real_offset  = log_offset + mapping[log_seg - 1].offset
    section      = mapping[log_seg - 1].ovl

`SegIsExecutable` (`cvld.c:371-377`) reads `mapping[log-1].u.b.fExecute`.

### 1.13 `sstSrcModule` — line numbers

Three nested tables. Header, `cv_sst_src_module_header` (`cv4.h`):

    off  size  field
    0    2     unsigned_16 cFile
    2    2     unsigned_16 cSeg
    4    4*cFile  unsigned_32 baseSrcFile[cFile]   offsets rel. to the subsection lfo
    then       start/end pairs: cSeg * { unsigned_32 start, unsigned_32 end }
    then       unsigned_16 seg[cSeg]
    then       pad to alignment

The trailing `start_end[]` / `seg[]` / `pad` are commented-out members in `cv4.h` and
**OpenWatcom's reader never touches them** — it only reads `cFile`, `cSeg` and
`baseSrcFile[]` (`cvcue.c` `WalkFileList`). Treat their layout as documented-but-unverified.

**File table**, `cv_sst_src_module_file_table`, at `lfo + baseSrcFile[i]`:

    off  size  field
    0    2     unsigned_16 cSeg
    2    2     unsigned_16 pad
    4    4*cSeg  unsigned_32 baseSrcLn[cSeg]   offsets rel. to the subsection lfo
    then       start/end pairs: cSeg * { unsigned_32 start, unsigned_32 end }
    then       u1 name_len; name bytes

The name position is **confirmed arithmetic**, not a guess — `cvcue.c` `CueFile`:

    offset = file_table + offsetof( ..., baseSrcLn ) + fp->cSeg * (sizeof(unsigned_32) * 3);

`offsetof(baseSrcLn)` is 4 and `cSeg * 12` = `cSeg*4` (baseSrcLn) + `cSeg*8`
(start/end pairs), which is what pins the intervening pairs array down.

> `cvcue.c` carries an explicit warning here, worth reproducing:
> *"Doc says the length is unsigned_16, cvpack says unsigned_8. testing on real files
> confirm unsigned_8."* — **the file name length is a single byte.** OW reads
> `*(unsigned_8 *)p`.

**Line-number table**, `cv_sst_src_module_line_number`, at `lfo + baseSrcLn[j]`:

    off  size  field
    0    2     unsigned_16 Seg
    2    2     unsigned_16 cPair
    4    4*cPair  unsigned_32 offset[cPair]
    then     2*cPair  unsigned_16 linenumber[cPair]
    then     pad

Confirmed by both accessors in `cvcue.c`:

    CueLine: linenumber[pair] at  line_table + 4 + cPair*4 + pair*2
    CueAddr: segment = lp->Seg;  offset = offset[pair]  at  line_table + 4 + pair*4

So entry `k` of a line table is the pair `(offset[k], linenumber[k])`, and the address
is `MapLogical( Seg, offset[k] )` — `Seg` is a **logical** segment, run it through
`sstSegMap` per 1.12.

> Bug to be aware of when cross-checking against OW: `cvcue.c` `CueAddr` maps only
> `sizeof(unsigned_16)` bytes but dereferences the pointer as `unsigned_32 *`. The
> field is u32; OW's mapping request is short. Do not copy that.

### 1.14 Other subsection structs present in `cv4.h`

Defined in the header but not exercised by the DIP, so layout is from the header only:

`cv_sst_public_16` — presumably the `sstPublic` element (**16-bit**):

    0    2     unsigned_16 offset
    2    2     unsigned_16 seg
    4    2     unsigned_16 type
    6    ..    char name[]        (header writes `char name[1]`; encoding UNKNOWN,
                                   but everything else in CV is u8-length-prefixed)

`cv_sst_public_32` — same with `unsigned_32 offset` at 0, `seg` at 4, `type` at 6, name at 8.

`cv_sst_src_lne_seg` — the `sstSrcLnSeg` element. `cv4.h` shows a commented-out leading
`char name[1]`, then:

    u2 seg
    u2 cPair
    then line_offset_parms[]

`cv_sst_global_types_header` — `sstGlobalTypes`:

    off  size  field
    0    4     unsigned_32 flags
    4    4     unsigned_32 cType
    8    4*cType  unsigned_32 offType[cType]
    then       the type records themselves

`cvld.c:338-340` computes the base of the type records as
`cde->lfo + offsetof(offType) + cType * 4` = `lfo + 8 + 4*cType`, and each
`offType[i]` is an offset relative to that base.

### 1.15 CV4 vs CV5 differences actually present in OpenWatcom

Only two, in what I read:

- `S_PUB32_NEW = 0x1009`, commented "Codeview 5.0 Symbols" in `cv4syms.h`.
- The signature accepted (`NB05` written, `NB09` required by the DIP).

Directory header, directory entry, `sstSegMap` and `sstSrcModule` have **one** definition
each, shared. **UNKNOWN:** any CV4/CV5 width difference in the directory structures —
OpenWatcom does not model one.

### 1.16 Minimum path to "name -> segment:offset" (the common case)

1. `CVBase = filesize - u32_at(filesize-4)`; check 4 sig bytes at `CVBase` and at
   `filesize-8` are the same `NBxx`.
2. `lfoDirectory = u32_at(CVBase+4)`. Seek `CVBase + lfoDirectory`.
3. Read the 16-byte dir header; check `cbDirHeader==16`, `cbDirEntry==12`; read `cDir`
   entries of 12 bytes.
4. Find the entry with `subsection == sstSegMap (0x12D)`; parse `cSegLog` `seg_desc`s
   (20 bytes each) at `CVBase + lfo + 4`. Index them **1-based**.
5. Find the entry with `subsection == sstGlobalPub (0x12A)`. Read the 16-byte header at
   `CVBase + lfo`. Walk `cbSymbol` bytes starting at `CVBase + lfo + 16`, framing each
   record as `[u16 length][u16 code][payload]`, stepping `length + 2`.
6. For each `S_PUB16 (0x0103)`: `offset = u16@4`, `segment = u16@6`, `name` = Pascal
   string at byte 10. Map through 1.12:
   `real_seg = segdesc[segment-1].frame`, `real_off = offset + segdesc[segment-1].offset`.

`sstGlobalSym (0x129)` and `sstStaticSym (0x134)` have the same header and are walked
identically; they carry `S_LDATA16`/`S_GDATA16`/`S_LPROC16`/`S_GPROC16` etc.

---

## 2. Borland / Turbo Debugger

> **OpenWatcom has NO Borland/TDS reader.** `bld/dip/` contains exactly:
> `c`, `codeview`, `dipdump`, `doc`, `dwarf`, `export`, `hllcv`, `javavm`, `mapsym`,
> `skel`, `watcom`. Nothing Borland. Everything in this section therefore comes from
> **other open-source projects, named per claim** — this is the one section not backed
> by OpenWatcom.

### 2.0 There are TWO Borland formats, and the brief conflates them

| | 16-bit "TDINFO" | 32-bit "TDS" / TD32 |
|---|---|---|
| signature | `0x52FB` u16 (**bytes `FB 52`**) | ASCII `"FB09"` or `"FB0A"` |
| target | DOS MZ, 16-bit | NE / PE / LX, 32-bit |
| shape | flat counted tables + a name pool | CodeView-style subsection directory |
| names | index into a NUL-separated name pool | index into `sstNames` |

**For a 16-bit DOS MZ .EXE you want §2.1, not §2.2.** The `FB0A` the brief names is the
32-bit Borland C++ Builder signature. `FB09` is the Delphi one. Both are documented in
§2.2 because the brief asked for them, but they do not appear in 16-bit DOS output.

**UNKNOWN: any signature literally spelled `FB 0A` as raw bytes at the start of a file.**
`"FB0A"` is four ASCII characters (`46 42 30 41`); `0x52FB` is the 16-bit magic. I found
no third thing matching the brief's description.

### 2.1 16-bit TDINFO — appended to a DOS MZ .EXE

**Source: `ramikg/tdinfo-parser`, `tdinfo_structs.py`** —
`https://github.com/ramikg/tdinfo-parser`, a `construct` schema for Borland TLink
symbolic debug info, written for DOS. This is the only complete open-source 16-bit
description I found. All widths are little-endian.

#### How it is found — there is NO trailer

The debug block starts **immediately after the load image as the MZ header itself
declares it**. From the MZ header at offset 0:

    off  size  field
    0    2     "MZ"
    2    2     used_bytes_in_last_page
    4    2     file_size_in_pages

    if used_bytes_in_last_page == 0: used = 512 else: used = used_bytes_in_last_page
    tdinfo_offset = file_size_in_pages * 512 - (512 - used)

That is `calculate_extra_information_offset` in `tdinfo_structs.py`. So unlike CodeView
and Watcom, **you do not search backwards from EOF** — you compute the end of the MZ
image and the TDINFO header is there. Validate by checking the magic.

#### `TDINFO_HEADER_STRUCT` — 48 bytes fixed + `extension_size` bytes

    off  size  field
    0    2     u16 magic_number              must be 0x52FB (bytes FB 52)
    2    1     u8  minor_version
    3    1     u8  major_version             note: MINOR precedes MAJOR
    4    4     u32 names_pool_size_in_bytes
    8    2     u16 names_count
    10   2     u16 types_count
    12   2     u16 members_count
    14   2     u16 symbols_count
    16   2     u16 globals_count
    18   2     u16 modules_count
    20   2     u16 locals_count
    22   2     u16 scopes_count
    24   2     u16 line_numbers_count
    26   2     u16 source_files_count
    28   2     u16 segments_count
    30   2     u16 correlations_count
    32   14    padding
    46   2     u16 extension_size
    48   N     padding, extension_size bytes

(Field offsets are cumulative from the struct as written; the two padding runs are
`Padding(14)` and `Padding(this.extension_size)`.)

#### Table order, immediately after the header

Every table is a flat array of fixed-size records, counted by the header. No offsets,
no directory — you walk them in this exact order:

    symbol_records        symbols_count      * 9 bytes
    module_records        modules_count      * 16 bytes
    source_file_records   source_files_count * 6 bytes    (layout not parsed by this tool)
    line_number_records   line_numbers_count * 4 bytes    (layout not parsed by this tool)
    scope_records         scopes_count       * 12 bytes
    segment_records       segments_count     * 16 bytes
    correlation_records   correlations_count * 8 bytes    (layout not parsed by this tool)
    type_records          types_count        * 8 bytes
    member_records        members_count      * 5 bytes

**The name pool is at the very END of the file**, not here:

    Seek(-names_pool_size_in_bytes, whence=EOF)
    name_pool = names_count * NUL-terminated ASCII strings

**Name indices are 1-based**: `_get_name_from_pool` raises on index 0 and returns
`name_pool[name_index - 1]`.

#### `SYMBOL_RECORD_STRUCT` — 9 bytes

    off  size  field
    0    2     u16 index        1-based index into the NAME POOL
    2    2     u16 type         1-based index into type_records
    4    2     u16 offset
    6    2     u16 segment
    8    1     bitfield: 5 bits padding (high), then 3 bits symbol_class (low)

`symbol_class` occupies the **low 3 bits** of byte 8 (`BitStruct(Padding(5),
BitsInteger(3))`, and `construct`'s `BitStruct` is MSB-first, so the 5 pad bits are the
high ones):

    0 STATIC     1 ABSOLUTE   2 AUTO      3 PASCAL_VAR
    4 REGISTER   5 CONSTANT   6 TYPEDEF   7 STRUCT_UNION_OR_ENUM

**Name -> segment:offset**: `_apply_global_symbol` computes
`ea = image_base + symbol.segment * 0x10 + symbol.offset`, and treats
`symbol_class == STATIC (0)` as the global-symbol case. For `AUTO (2)` the `offset`
field is a **signed** BP-relative displacement — the parser does
`offset - 0x10000 if offset > 0x7fff else offset`.

#### `MODULE_RECORD_STRUCT` — 16 bytes

    off  size  field
    0    2     u16 name        1-based name-pool index
    2    14    padding (contents not parsed by this tool)

#### `SEGMENT_RECORD_STRUCT` — 16 bytes

    off  size  field
    0    2     u16 module        1-based index into module_records
    2    2     u16 code_segment
    4    2     u16 code_offset
    6    2     u16 code_length
    8    2     u16 scope_index   1-based index into scope_records
    10   2     u16 scope_count
    12   4     padding

#### `SCOPE_RECORD_STRUCT` — 12 bytes

    off  size  field
    0    2     u16 symbol_index   1-based index into symbol_records
    2    2     u16 symbol_count
    4    2     u16 parent         1-based; 0 = none
    6    2     u16 function
    8    2     u16 offset
    10   2     u16 length

A scope's code address is `image_base + segment_record.code_segment * 0x10 +
(parent == 0 ? scope.offset : scope_records[parent-1].offset)`.

#### `TYPE_RECORD_STRUCT` — 8 bytes, `MEMBER_RECORD_STRUCT` — 5 bytes

    TYPE:    0 u8 id (TypeId)   1 u16 name   3 u16 size   5 u8 class_type   6 u16 member_type
    MEMBER:  0 u8 info          1 u16 name   3 u16 type

`member.info == 0xC0` terminates a struct's member list. `TypeId` values (partial, the
ones a 16-bit parser cares about): `VOID 0`, `SCHAR 4`, `SINT 5`, `SLONG 6`, `UCHAR 8`,
`UINT 9`, `ULONG 10`, `PCHAR 12`, `FLOAT 13`, `DOUBLE 15`, `NEAR 21`, `FAR 22`,
`SEG 23`, `NEAR386 24`, `FAR386 25`, `ARRAY 26`, `PARRAY 28`, `STRUCT 30`, `UNION 31`,
`ENUM 34`, `FUNCTION 35`, `LABEL 36`, `SET 37`, `BOOL 40`, `FUNCPROTOTYPE 44`,
`OBJECT 46`, `WORDBOOL 54`, `LONGBOOL 55`. (Full list in the source.)

#### Line numbers — **NOT AVAILABLE from this source**

`tdinfo_structs.py` skips both tables:

    Padding(this.tdinfo_header.source_files_count * 6)
    Padding(this.tdinfo_header.line_numbers_count * 4)

So I have the **record sizes** (6 and 4 bytes) and the **counts and position in the
table order**, but **not the field layouts**. The tool's README points at Borland's own
`TDUMP` for "a more complete parsing of the debug information". **UNKNOWN: the 16-bit
source-file and line-number record field layouts.**

### 2.2 32-bit TDS (`FB09` / `FB0A`)

**Source: `project-jedi/jcl`, `jcl/source/windows/JclTD32.pas`** —
`https://github.com/project-jedi/jcl/blob/master/jcl/source/windows/JclTD32.pas`, by
Flier Lu. Its header carries a long prose specification, quoted below in substance.
Corroborated for the header by `Josko/tds2pdb`, `src/tds_parser.cpp`.

#### Signatures

    Borland32BitSymbolFileSignatureForDelphi = $39304246   // 'FB09'
    Borland32BitSymbolFileSignatureForBCB    = $41304246   // 'FB0A'

Stored as a DWORD; on disk the bytes are `46 42 30 39` / `46 42 30 41` — i.e. plainly
the ASCII `"FB09"` / `"FB0A"`. `tds_parser.cpp` reads it as a 4-character string and
compares to `"FB0A"`. The doc block says the signature is "FBxx, where xx is the
version number".

#### Finding it — an 8-byte trailer at EOF, CodeView-style

`TJclTD32FileSignature`, 8 bytes:

    off  size  field
    0    4     DWORD Signature
    4    4     DWORD Offset
    ---  8

The doc block, verbatim in substance: the block goes at the end of the .EXE, after the
header plus load image, overlays and resource information; the last eight bytes are a
signature and a long file offset **from the end of the file** (`lfoBase`), and

    lfaBase = length of the file - lfoBase

is the base address of the debug block relative to the start of the file. **All other
file offsets in the block are relative to `lfaBase`.** At the base address the signature
is repeated, followed by the long displacement to the subsection directory (`lfoDir`).

`IsTD32DebugInfoValid` implements exactly that: read 8 bytes at `end - 8`; if the
signature matches and `Offset <= size`, read 8 bytes at `end - Offset` and require the
signature again. `LfaToVa(Lfa) = Base + Lfa`.

This is structurally identical to CodeView §1.2/§1.3, with `"FB09"`/`"FB0A"` where
CodeView has `"NBxx"`.

**All subsections start on a long word boundary** and are internally naturally aligned.

#### Subsection directory — identical shape to CodeView

`TDirectoryHeader`, 16 bytes:

    off  size  field
    0    2     Word  Size            length of this structure (16)
    2    2     Word  DirEntrySize    length of each entry (12)
    4    4     DWORD DirEntryCount
    8    4     DWORD lfoNextDir      offset from lfoBase of the next directory
    12   4     DWORD Flags
    ---  16

`TDirectoryEntry`, 12 bytes:

    off  size  field
    0    2     Word  SubsectionType
    2    2     Word  ModuleIndex
    4    4     DWORD Offset          from lfoBase
    8    4     DWORD Size
    ---  12

`tds_parser.cpp` asserts `dir_header_size == 16` and `dir_entry_size == 12`,
independently confirming both.

#### Subsection types

    $120  SUBSECTION_TYPE_MODULE
    $121  SUBSECTION_TYPE_TYPES
    $124  SUBSECTION_TYPE_SYMBOLS
    $125  SUBSECTION_TYPE_ALIGN_SYMBOLS
    $127  SUBSECTION_TYPE_SOURCE_MODULE
    $129  SUBSECTION_TYPE_GLOBAL_SYMBOLS
    $12B  SUBSECTION_TYPE_GLOBAL_TYPES
    $130  SUBSECTION_TYPE_NAMES

Same numbering as CodeView's `sst*` (§1.5) for the ones they share. **`$130` is
`sstNames` here, where CodeView calls it `sstPreCompMap`** — that is a genuine Borland
divergence, and `sstNames` is central to the format.

Image layout per the doc block:

    FB09 Header
      sstModule[1..n]
      sstAlignSym[1] sstSrcModule[1] ... sstAlignSym[n] sstSrcModule[n]
      sstGlobalSym
      sstGlobalTypes
      sstNames
      SubSection Directory
    FB09 Trailer

Directory entries for `sstModule` precede all other entries.

#### `sstNames` ($130) — the name pool. THIS IS THE BIG DIFFERENCE FROM CODEVIEW

    off  size  field
    0    4     DWORD Count
    4    ..    Count entries, each: u8 length, then the bytes, then a NUL

**Borland records carry a `NameIndex` (a DWORD index into this pool), NOT an inline
Pascal string** — the opposite of CodeView §1.8. A parser written for CodeView symbol
records will read Borland ones as garbage.

`AnalyseNames` also documents a real trap: **the length byte is only correct modulo
256.** For names longer than 255 the code skips `len`, then keeps advancing by 256 until
it lands on the NUL:

    Len := Ord(pszName^); Inc(pszName);
    <name starts here>
    Inc(pszName, Len);
    while pszName^ <> #0 do Inc(pszName, 256);
    Inc(pszName, 1);

So the NUL, not the length byte, is authoritative for the end of a name.

#### `sstModule` ($120)

`TModuleInfo`:

    off  size  field
    0    2     Word  OverlayNumber
    2    2     Word  LibraryIndex     index into sstLibraries, if linked from a library
    4    2     Word  SegmentCount
    6    2     Word  DebuggingStyle
    8    4     DWORD NameIndex        into sstNames
    12   4     DWORD TimeStamp        from the OBJ file
    16   12    DWORD Reserved[3]      set to 0
    28   ..    TSegmentInfo Segments[SegmentCount]

`TSegmentInfo`, 12 bytes:

    off  size  field
    0    2     Word  Segment
    2    2     Word  Flags        $0000 = data segment, $0001 = code segment
    4    4     DWORD Offset       offset in segment where the code starts
    8    4     DWORD Size
    ---  12

Compare CodeView §1.6: same idea, but the name is an index at a fixed offset instead of
a trailing Pascal string, and there are extra `NameIndex`/`TimeStamp`/`Reserved` fields.

#### Symbol record framing ($124 / $125 / $129)

`TSymbolInfos` — the subsection begins with a DWORD signature, then the records:

    off  size  field
    0    4     DWORD Signature
    4    ..    symbol records

(Same as CodeView §1.11's `CV_OMF_SIG` u32.)

`TSymbolInfo` header — identical framing to CodeView §1.8:

    off  size  field
    0    2     Word Size          bytes following this field
    2    2     Word SymbolType
    4    ..    payload

Symbol type codes are CodeView's (§1.9): `$0001 COMPILE`, `$0002 REGISTER`,
`$0003 CONST`, `$0004 UDT`, `$0005 SSEARCH`, `$0006 END`, `$0007 SKIP`,
`$0008 CVRESERVE`, `$0009 OBJNAME`; `$0100..$010B` the 16:16 family
(`BPREL16 LDATA16 GDATA16 PUB16 LPROC16 GPROC16 THUNK16 BLOCK16 WITH16 LABEL16
CEXMODEL16 VFTPATH16`); `$0200..$020B` the 16:32 family.

Note the 16-bit codes are *defined* in this unit but `AnalyseAlignSymbols` only
dispatches the 32-bit ones — this is a 32-bit reader.

#### 32-bit record payloads (offsets from the start of the record)

**`SYMBOL_TYPE_PUB32 ($0203)`, `LDATA32 ($0201)`, `GDATA32 ($0202)`** —
`TSymbolDataInfo`, all three share it:

    0    2     Word  Size
    2    2     Word  SymbolType
    4    4     DWORD Offset
    8    2     Word  Segment
    10   2     Word  Reserved
    12   4     DWORD TypeIndex
    16   4     DWORD NameIndex     <- into sstNames
    ---  20

**`SYMBOL_TYPE_LPROC32 ($0204)` / `GPROC32 ($0205)`** — `TSymbolProcInfo`:

    0    2     Word  Size
    2    2     Word  SymbolType
    4    4     DWORD pParent
    8    4     DWORD pEnd
    12   4     DWORD pNext
    16   4     DWORD Size          length in bytes of the procedure
    20   4     DWORD DebugStart
    24   4     DWORD DebugEnd
    28   4     DWORD Offset
    32   2     Word  Segment
    34   4     DWORD ProcType
    38   1     Byte  NearFar       0 = near, 4 = far
    39   1     Byte  Reserved
    40   4     DWORD NameIndex
    ---  44

**`SYMBOL_TYPE_LABEL32 ($0209)`** — `TSymbolLabelInfo`:

    0    2     Word  Size
    2    2     Word  SymbolType
    4    4     DWORD Offset
    8    2     Word  Segment
    10   1     Byte  NearFar       0 = near, 4 = far
    11   1     Byte  Reserved
    12   4     DWORD NameIndex
    ---  16

**`SYMBOL_TYPE_OBJNAME ($0009)`** — `TSymbolObjNameInfo`:

    4    4     DWORD Signature
    8    4     DWORD NameIndex

The doc block adds: for every `GPROC32` emitted, a `GPROCREF` symbol must be fabricated
into `SUBSECTION_TYPE_GLOBAL_SYMBOLS`.

#### `sstSrcModule` ($127) — line numbers

Base addresses of all the tables below are **relative to the start of the sstSrcModule
subsection**. Structure, per the doc block:

    Module header
      Information for source file 1
        Information for segment 1 .. n
      ...
      Information for source file n

`TSourceModuleInfo` (module header):

    off  size  field
    0    2     Word  FileCount
    2    2     Word  SegmentCount
    4    4*F   DWORD BaseSrcFiles[FileCount]     offsets from the sstSrcModule start
    then       TOffsetPair SegmentAddress[SegmentCount]   (2 x DWORD: start, end)
    then       Word SegmentIndexes[SegmentCount]
    then       a pad word if SegmentCount is odd

`TSourceFileEntry` (at `sstSrcModule_start + BaseSrcFiles[i]`):

    off  size  field
    0    2     Word  SegmentCount
    2    4     DWORD NameIndex                    <- into sstNames, NOT a Pascal string
    6    4*S   DWORD BaseSrcLines[SegmentCount]   offsets from the sstSrcModule start
    then       TOffsetPair SegmentAddress[SegmentCount]

> Careful: the Pascal source has the commented-out tail listing `SegmentAddress` and
> then `Name: ShortString`, while the live field list has `NameIndex: DWORD` at offset 2.
> The live declaration is what the code compiles and uses. Treat the trailing
> `ShortString` comment as stale — but this is the one place in §2.2 where the source
> contradicts itself, so verify against a real file.

`TLineMappingEntry` (at `sstSrcModule_start + BaseSrcLines[j]`):

    off  size  field
    0    2     Word  SegmentIndex
    2    2     Word  PairCount
    4    4*P   DWORD Offsets[PairCount]        offsets within the code segment
    then       Word LineNumbers[PairCount]     parallel array
    then       a zero word if PairCount is odd, for alignment

Same pair-of-parallel-arrays design as CodeView §1.13, but the offsets are DWORDs
(32-bit) and the file name is an index rather than an inline string.

#### `sstGlobalTypes` ($12B)

    off  size  field
    0    4     DWORD Count
    4    4*C   DWORD Offsets[Count]    offset of each type record from the table start
    then       the type records

Each type record is forced to start on a long word boundary, but **the length of the
type string is NOT adjusted by the pad count**.

---

## 3. Watcom's own debug format, and DWARF

### 3A. Watcom ("WAT") debug info

#### Where it lives

**Appended past the end of the MZ image, with the master header as the LAST 14 bytes
of the file.** `wdbginfo.h`'s own diagram, verbatim:

        +=======================+
        |       EXE file        |
        +=======================+
        |       Overlays        |
        +=======================+
        |    Any Other Stuff    |
        +=======================+ <-- start of debugging info
        | source language table |
        +-----------------------+
        | segment address table |
        +-----------------------+
        |  section debug info   |   repeated for each overlay & root
        +-----------------------+
        |  master debug header  |
        +=======================+ <-- end of file

The linker writes the master header last — `dbginfo.c:910`, `WriteLoad( Master,
sizeof( Master ) )` is the final statement of the whole DBI writer. `loaddos.c`
computes the MZ header's `file_size`/`mod_size` from the root image only, so the
debug block deliberately lies **beyond** the length the MZ header declares.

`option symfile` diverts the block to a separate file instead (`dbgall.c` `DBIWrite`).

#### Signature and versions (`wdbginfo.h`)

    #define FOX_SIGNATURE1        0x8300
    #define FOX_SIGNATURE2        0x8301
    #define WAT_RES_SIG           0x8302
    #define WAT_DBG_SIGNATURE     0x8386

    #define OLD_EXE_MAJOR_VERSION 2
    #define EXE_MAJOR_VERSION     3
    #define EXE_MINOR_VERSION     0
    #define OBJ_MAJOR_VERSION     1
    #define OBJ_MINOR_VERSION     3

**No `COMENT` class 0xA1 is involved.** In Watcom's headers `0xA1` is the *opcode*
`CMD_LEDATA32`, and as a comment class `CMT_MS_OMF`; `0xA3` is `CMT_LIBMOD`. Neither
carries Watcom debug info. See "OMF carriage" below for what actually does.

#### `master_dbg_header` — 14 bytes, `#pragma pack(1)`, little-endian

    off  size  field
    0    2     unsigned_16 signature       must be 0x8386
    2    1     unsigned_8  exe_major_ver   3 = current, 2 = old ("V2"); else bad-version
    3    1     unsigned_8  exe_minor_ver   must be <= 0
    4    1     unsigned_8  obj_major_ver   must == 1
    5    1     unsigned_8  obj_minor_ver   must be <= 3
    6    2     unsigned_16 lang_size       bytes of the source-language table
    8    2     unsigned_16 segment_size    bytes of the segment address table
    10   4     unsigned_32 debug_size      total block size, INCLUDING this header
    ---  14

**`exe_major_ver` 2 vs 3 is the format version that matters** — it is the only switch
the DIP acts on (`iih->v2`), and it changes the global-symbol record and the meaning of
every `mod` field. `obj_major_ver`/`obj_minor_ver` are negotiated up from the object
files (`dbginfo.c:176-184`) and are a compatibility check, not a layout switch.

`debug_size` includes the header: `DBISize` is initialised to `sizeof(Master)`
(`dbginfo.c:122`).

#### Finding the block — `watldsym.c` `DoPermInfo()` (:244-266)

    seek to filesize - 14; end = that offset; read 14 bytes
    while sig in { 0x8300, 0x8301, 0x8302 }:      /* other Watcom trailers stacked at EOF */
        if debug_size > end -> invalid
        end -= debug_size; seek(end); read 14 bytes
    if sig != 0x8386 -> not Watcom debug info

Block start = `end + 14 - debug_size`. Note the skip loop assumes every stacked trailer
has the same shape: u16 signature at +0, u32 size at +10.

#### Block layout, in file order

    debug_block_start + 0                      source language table, lang_size bytes
    debug_block_start + lang_size              segment address table, segment_size bytes
    then                                       section debug info, repeated
    filesize - 14                              master_dbg_header

- **Source language table**: concatenated NUL-terminated strings, `"C"` always first
  (`dbginfo.c:896-901` writes `node->len + 1` per entry). `mod_dbg_info.language` is a
  **byte offset into this table**.
- **Segment address table**: an array of `unsigned_16` link-time segment values.
  `num_segs = segment_size / sizeof(addr_seg)` and `addr_seg` is `word` = u16
  (`watldsym.c:296`, `machtype.h:57`).
- **Section count is not stored.** Walk `section_size` from the first section header
  forward until you reach `end` (`watldsym.c` `GetNumSect`).

#### `section_dbg_header` — 18 bytes, packed LE

    off  size  field
    0    4     unsigned_32 mod_offset
    4    4     unsigned_32 gbl_offset
    8    4     unsigned_32 addr_offset
    12   4     unsigned_32 section_size
    16   2     unsigned_16 section_id      overlay ref; 0 = root
    ---  18

All three offsets are **relative to the start of this section header** — `watldsym.c:195`
passes `header.mod_offset + pos` where `pos = DCTell` taken *before* the header read.

Validity, enforced at `watldsym.c:145-158`: `mod_offset <= gbl_offset <= addr_offset
< section_size`. **If `mod_offset == gbl_offset` the section is an empty overlay
placeholder — skip it entirely** (both `GetNumSect` and `ProcSectionsInfo` special-case it).

So within one section:

    [hdr+0 .. mod_offset)            demand-load link tables and $$SYMBOLS/$$TYPES/line data
    [mod_offset  .. gbl_offset)      module info section
    [gbl_offset  .. addr_offset)     global symbol section
    [addr_offset .. section_size)    address info section

#### Address type — `addr48_ptr`, 6 bytes (`machtype.h:110-121`)

    off  size  field
    0    4     addr48_off offset     (dword)
    4    2     addr_seg   segment    (word)
    ---  6

Used verbatim on disk, **including on 16-bit targets** — the offset field is 32 bits
regardless. `addr32_off` is `word` and `addr48_off` is `dword` (`machtype.h:55-57`).

#### Global symbols — the section you want

Records run back-to-back from `gbl_offset` to `addr_offset`. **There is no count field;
walk by record size.**

**V3 (`exe_major_ver == 3`) — `v3_gbl_info`, `10 + N` bytes:**

    off  size  field
    0    4     u32 addr.offset
    4    2     u16 addr.segment       link-time segment; map via the segment table
    6    2     u16 mod                module INDEX (add the section's module base)
    8    1     u8  kind
    9    1     u8  name length N
    10   N     name bytes, NOT NUL-terminated

**V2 (`exe_major_ver == 2`) — `gbl_info`, `9 + N` bytes:**

    off  size  field
    0    4     u32 addr.offset
    4    2     u16 addr.segment
    6    2     u16 mod                byte OFFSET from the module-info section start
    8    1     u8  name length N
    9    N     name bytes

**There is no `kind` byte in V2.**

Both pinned by the reader macros, `watgbl.c:56-60`:

    #define GBL_NAME(c,g)     ((g)->name + (((c)->v2)?1+0:1+1))
    #define GBL_NAMELEN(c,g)  ((unsigned char)(g)->name[((c)->v2)?0:1])
    #define GBL_SIZE(c,g)     ( ((c)->v2) ? ((unsigned char)(g)->name[0] + sizeof(gbl_info) + 0) \
                                          : ((unsigned char)(g)->name[1] + sizeof(gbl_info) + 1) )

(`gbl_info.name` sits at offset 8, so in V3 `name[0]` is the `kind` byte and `name[1]`
is the length byte — the two structs overlay cleanly.)

`kind` bit values (`wdbginfo.h`):

    0x01  GBL_KIND_STATIC    module-static, visible only from its own module
    0x02  GBL_KIND_DATA
    0x04  GBL_KIND_CODE

Names are the **mangled/linker** names — C++ arrives mangled, MS `__stdcall` keeps `@n`.

**Relocating the segment**: `gbl.addr.segment` is a link-time value. `AddressMap()`
(`watldsym.c:394-411`) scans `map_segs[0..num_segs)` for an equal value and substitutes
the loaded one. For static analysis the stored `segment:offset` is already the
load-image address relative to the program base.

#### Module info — `mod_dbg_info`, `21 + N` bytes

    off  size  field
    0    2     u16 language        byte offset into the source-language table
    2    6     demand_info di[0]   DMND_LOCALS
    8    6     demand_info di[1]   DMND_TYPES
    14   6     demand_info di[2]   DMND_LINES
    20   1     u8  name length N
    21   N     name (source file path)

`demand_info` — 6 bytes:

    off  size  field
    0    4     unsigned_32 info_off
    4    2     union { u16 size;      /* V2: byte size of the demand info */
                       u16 entries; } /* V3: number of link-table entries */

`demand_kind` enum (`wdbginfo.h`): `DMND_LOCALS = 0`, `DMND_TYPES = 1`,
`DMND_LINES = 2`, `MAX_DMND = 3`.

Module **index** = ordinal position within the section's module-info run, plus the
section's `mod_base_idx` (running total of preceding sections' module counts).

V3 demand access: `info_off` is a byte offset into the section's link-table area, which
is an array of `unsigned_32` file offsets relative to the section header start. Entry
`i` of kind `k` is at file offset `link[ di[k].info_off/4 + i ]`, sized
`link[...+1] - link[...]`. In V2, `info_off` is a direct offset and `u.size` the size.

#### Address info — `seg_dbg_info`, variable

    off  size  field
    0    4     u32 base.offset
    4    2     u16 base.segment
    6    2     u16 count
    8    6*n   addr_dbg_info addr[n]

`addr_dbg_info` — 6 bytes: `unsigned_32 size`, `unsigned_16 mod`.

**`count` carries a flag in its top bit.** `#define SEG_COUNT_MASK 0x7fff`
(`wdbginfo.h`): `n = count & 0x7fff`, and `count & 0x8000` marks a range belonging to
the linker's `NonSect`. Ranges are consecutive — entry `i` starts at
`base.offset + sum(size[0..i))`. `mod == 0xffff` means "no module". In V2 `mod` is again
a byte offset, converted via `ModOff2Idx`.

#### Line numbers

    line_dbg_info      6 bytes:  u16 line, u32 code_offset (offset from segment base)
    v2_line_segment:   u16 segment, u16 count, then count * line_dbg_info
    v3_line_segment:   u32 segment, u16 count, then count * line_dbg_info

`segment` is an offset into the address-info class.

#### OMF carriage (the object file, not the EXE)

Two mechanisms, **neither a COMENT class 0xA1 or 0xA3**:

1. **Named segments** (`bld/cg/intel/c/x86omf.c:259-262`):

        segment "$$SYMBOLS"  class "DEBSYM"   -> locals  (DMND_LOCALS)
        segment "$$TYPES"    class "DEBTYP"   -> types   (DMND_TYPES)

   The linker matches on the **class** names `DEBSYM` / `DEBTYP`
   (`bld/wl/h/specials.h:58-59`, `dbgall.c:141-152`) and copies the contents verbatim
   into the section's demand-loaded area.

2. **A COMENT linker directive** carrying the object-format version and source language:
   `CMD_COMENT` (0x88), class `CMT_LINKER_DIRECTIVE = 0xFE`, subcode
   `LDIR_SOURCE_LANGUAGE = 'D'` (0x44), payload `u8 major`, `u8 minor`, then the
   language name string (`objomf.c` `LinkDirective`, `dbgall.c` `DBIP1Source`). These
   become `obj_major_ver`/`obj_minor_ver` and the language table. A legacy byte-swapped
   form also exists: class byte 0x80 with `attribute == 0xFE`.

Relevant COMENT classes from `bld/watcom/h/pcobj.h`, for disambiguation:

    0x9b CMT_WAT_PROC_MODEL    0x9f CMT_DEFAULT_LIBRARY   0xa1 CMT_MS_OMF
    0xa3 CMT_LIBMOD            0xaa CMT_EASY_OMF          0xe9 CMT_DEPENDENCY (Borland)
    0xfe CMT_LINKER_DIRECTIVE  0xff CMT_SOURCE_NAME

### 3B. DWARF from OpenWatcom, 16-bit targets

**Version: DWARF 2.** `bld/watcom/h/dwarf.h:44-49` says the header is derived from
"DWARF Debugging Information Format ... Version 2, Draft 6, dated April 12, 1993" and
defines `DWARF_IMPL_VERSION 2`. The writer stamps it literally —
`bld/dwarf/dw/c/dwinfo.c:116`, `CLIWriteU16( cli, DW_DEBUG_INFO, 2 )`.

The `DW_AT_producer` string is versioned separately and is **not** the DWARF version:
`"WATCOM"` (Watcom 10.x), `"V1.0 WATCOM"` (Watcom 11 / early OW, the current default),
`"V2.0 WATCOM"` (OW 2.0+).

**16-bit DOS is supported — DWARF is not 32-bit-only here.** `bld/cg/c/dfsyms.c` has
explicit `_TARG_8086` paths (`:401-403` sets `cu->segment_size = 2`; `:1010-1012`
`DW_FLAG_PTR_TYPE_FAR16`), and the linker's DWARF writer branches on `MK_16BIT`
(`bld/wl/c/dbgdwarf.c:251, 563, 571`). Per `docs/doc/lg/lddebug.gml:24`, DWARF is the
linker's **default** debug format (`"DWARF" (the default), "WATCOM", "CODEVIEW", or
"NOVELL"`).

**In the object file**: eight OMF segments, all of class `DWARF`, all flagged
`SEG_USE_32` even on 16-bit targets (`bld/cg/intel/c/x86dfsup.c:64-88`):

    .debug_info  .debug_pubnames  .debug_aranges  .debug_line
    .debug_loc   .debug_abbrev    .debug_macinfo  .debug_str

`x86omf.c:663-700` is the `_TARG_8086` branch that gives a USE32 segment a 4-byte
SEGDEF length and emits `CMD_SEGDEF32` — how a 16-bit object carries these.

**In the linked DOS MZ EXE**: `dbgdwarf.c` `DwarfWrite()` builds a **complete ELF32
image** (`ELFCLASS32`, `ELFDATA2LSB`, `e_type = ET_EXEC`, `e_machine = EM_386`, with a
section header table and `.shstrtab`) holding the `.debug_*` sections, appends it to the
load file, then `DwarfWriteTrailer()` appends a 16-byte **TIS trailer**
(`bld/watcom/h/tistrail.h`), little-endian:

    off  size  field
    0    4     char[4]     signature   "TIS\0"  = 54 49 53 00
    4    4     unsigned_32 vendor      0 = TIS_TRAILER_VENDOR_TIS
    8    4     unsigned_32 type        0 = TIS_TRAILER_TYPE_TIS_DWARF
    12   4     unsigned_32 size        bytes appended, INCLUDING this 16-byte trailer
    ---  16

`dbgdwarf.c:843-855`: `MPUT_LE_32_UN( &trailer.size, curr_off + sizeof( TISTrailer ) )`.

Reader, `bld/dip/dwarf/c/dfld.c` `find_TIS_trailer()`:

    seek filesize - 16; read
    if signature != "TIS"  -> assume a bare ELF, start = 0
    else  start -= size - 16; seek(start); repeat until vendor==0 && type==0

Then parse the ELF header at `start`, walk `e_shnum` section headers and match names
against a 9-name table (the eight above plus `.WATCOM_references`); **every section
offset is `sh_offset + start`**. `.debug_info`, `.debug_abbrev` and `.debug_aranges`
must all be non-empty or the DIP fails.

So on a DOS MZ the DWARF payload sits past the MZ image, in the same place a Watcom
trailer would. The two are alternatives per link (`DEBUG DWARF` vs `DEBUG WATCOM`),
though the Watcom reader's trailer-skip loop anticipates stacked trailers in general.

---

## Sources actually read

CodeView (section 1), all under
`https://raw.githubusercontent.com/open-watcom/open-watcom-v2/master/`:

- `bld/watcom/h/cv4.h` — signatures, sst enum, directory, module, segmap, srcmodule, globalpub header
- `bld/watcom/h/cv4syms.h` — every `S_*` code
- `bld/watcom/h/cv4f.h` — the `cs_*` fixed-field payload structs
- `bld/watcom/h/cv4w.h` — `s_common` framing and the `s_*` record composition
- `bld/dip/codeview/h/cvinfo.h` — DIP internals, `DIRECTORY_BLOCK_ENTRIES`
- `bld/dip/codeview/c/cvld.c` — trailer discovery, base/bias, directory load, segmap load, MapLogical
- `bld/dip/codeview/c/cvmisc.c` — `FindDirEntry`, `WalkDirList`, numeric leaves
- `bld/dip/codeview/c/cvsym.c` — record framing, name extraction, global tables, hash
- `bld/dip/codeview/c/cvcue.c` — `sstSrcModule` file and line tables
- `bld/dip/codeview/c/cvmod.c` — `sstModule` name, `GetCompInfo`
- `bld/dip/codeview/c/cvvirt.c` — proof that `lfo` is CVBase-relative (`pg_start += iih->bias`)
- `bld/wl/c/dbgcv.c` — the writer: both trailers, what each `offset` means, `NB05`
- `bld/dip/hllcv/h/hllinfo.h`, `bld/dip/hllcv/c/hllld.c` — the *other* DIP; `NB00`/`NB02`/`NB04`

Watcom + DWARF (section 3), same prefix:

- `bld/watcom/h/wdbginfo.h` — the layout diagram, signatures, versions, every struct
- `bld/watcom/h/machtype.h` — `addr48_ptr`, `addr_seg`, `addr48_off`
- `bld/watcom/h/pcobj.h` — OMF record opcodes and COMENT classes
- `bld/watcom/h/tistrail.h` — the TIS trailer
- `bld/watcom/h/dwarf.h` — `DWARF_IMPL_VERSION 2`, producer strings
- `bld/dip/watcom/h/dipwat.h`
- `bld/dip/watcom/c/watldsym.c` — trailer discovery, section walk, `AddressMap`
- `bld/dip/watcom/c/watgbl.c` — the `GBL_*` macros that pin V2/V3 record shapes
- `bld/dip/watcom/c/watmod.c`, `wataddr.c`, `watdmnd.c`
- `bld/dip/dwarf/c/dfld.c` — `find_TIS_trailer`, ELF section lookup
- `bld/dwarf/dw/c/dwinfo.c` — writes the literal version 2
- `bld/wl/c/dbginfo.c` — the writer: master header last, lang/segment/debug sizes
- `bld/wl/c/dbgall.c`, `dbgdwarf.c`, `objomf.c`, `loaddos.c`
- `bld/wl/h/specials.h` — `DEBSYM` / `DEBTYP` class names
- `bld/cg/c/dfsyms.c`, `bld/cg/intel/c/x86omf.c`, `x86dfsup.c` — 16-bit DWARF emission
- `docs/doc/lg/lddebug.gml` — DWARF is the linker's default

Borland (section 2) — **not OpenWatcom**, each URL named with its claim:

- `https://github.com/ramikg/tdinfo-parser` — `tdinfo_structs.py`, `tdinfo_parser.py`,
  `README.md`. The 16-bit TDINFO schema (§2.1): magic 0x52FB, the MZ-derived offset,
  every table and record layout, the name pool at EOF.
  Raw: `https://raw.githubusercontent.com/ramikg/tdinfo-parser/master/tdinfo_structs.py`
- `https://github.com/project-jedi/jcl/blob/master/jcl/source/windows/JclTD32.pas` —
  the 32-bit TDS/TD32 spec (§2.2): the prose doc block, `FB09`/`FB0A`, the 8-byte
  trailer and `lfaBase` rule, directory header/entry, subsection type constants,
  `sstNames`, `sstModule`, symbol framing and payloads, `sstSrcModule`, `sstGlobalTypes`.
  Raw: `https://raw.githubusercontent.com/project-jedi/jcl/master/jcl/source/windows/JclTD32.pas`
- `https://github.com/Josko/tds2pdb` — `src/tds_parser.cpp`. Independent confirmation
  only: `"FB0A"` is read as a 4-char ASCII string, then an int32 offset, then a
  directory header asserted to be 16 bytes with 12-byte entries. The rest of that
  parser is a stub.
  Raw: `https://raw.githubusercontent.com/Josko/tds2pdb/master/src/tds_parser.cpp`
- `https://sourceforge.net/p/tds2dbg/wiki/Main_Page/` — checked, carries **no** binary
  format detail; listed so nobody re-checks it.
- `https://github.com/NationalSecurityAgency/ghidra/issues/3877` — checked, a feature
  request with no format detail. Same reason.

## Not found / unknown (section 2, Borland)

- **OpenWatcom has no Borland/TDS reader at all.** Section 2 is the only part of this
  document not sourced from OpenWatcom.
- **16-bit source-file record layout** (6 bytes each) and **16-bit line-number record
  layout** (4 bytes each). `tdinfo_structs.py` skips both with `Padding`, so I have
  their sizes, counts and position in the table order, but no field layouts. This is
  the one gap that matters for line numbers on 16-bit DOS. Borland's own `TDUMP` is
  the pointer the tool's README gives.
- **16-bit correlation record layout** (8 bytes each) — skipped the same way.
- **The 14 padding bytes in `TDINFO_HEADER_STRUCT`** and the **14 padding bytes in
  `MODULE_RECORD_STRUCT`** — real fields, contents unknown.
- **A raw-byte `FB 0A` signature** as the brief describes. `"FB0A"` is ASCII; the
  16-bit magic is `0x52FB`. I found nothing matching the literal description.
- **Which Borland versions emit which format**, and whether any 16-bit Borland linker
  ever emits the `FB09`/`FB0A` container. Not established by anything I read.
- **`sstLibraries`, `sstTypes` ($121), `sstSymbols` ($124) payloads** in the 32-bit
  format — `JclTD32.pas` names them but does not lay them out.
- **`TSourceFileEntry`'s name field**: the live Pascal declaration says
  `NameIndex: DWORD` at offset 2, the commented-out tail says a trailing `ShortString`.
  Contradiction inside one source file; unresolved, flagged in place.
- **`DebuggingStyle` values** in `TModuleInfo` — field named, values not given.

## Not found / unknown (section 1)

- **Which NBxx corresponds to CV3 / CV4 / CV5.** Not stated anywhere in OpenWatcom.
- **A CV4-vs-CV5 directory-entry width difference.** OpenWatcom has one 12-byte entry
  definition and validates `cbDirEntry` against it.
- **`NB07`, `NB08`, `NB10`, `NB11` semantics.** Defined in `cv4.h` and used nowhere in
  the files I read.
- **`sstSrcModule` header trailing arrays** (start/end pairs, `seg[]`, pad) are from
  `cv4.h` comments only; OpenWatcom's reader never parses them, so they are unverified
  against real data.
- `sstPublicSym`, `sstTypes`, `sstLibraries`, `sstSegName`, `sstFileIndex`, `sstMPC`,
  `sstPreComp*`, `sstOffsetMap*` layouts — enumerated but not read by the DIP and not
  laid out in the headers I fetched. (`sstPublic` and `sstSrcLnSeg` have structs in
  `cv4.h`; see 1.14. Their string encoding is not established by any code I read.)

## Not found / unknown (section 3, Watcom + DWARF)

- **No format specification document for the Watcom debug format exists in the repo.**
  `bld/dip/doc/` holds only `dip.doc` and `mod.doc`, which document the DIP *interface*,
  not the on-disk layout. The ASCII diagram at the top of `wdbginfo.h` is the only prose.
- **Local-symbol and type record layouts inside `$$SYMBOLS` / `$$TYPES`.** Only how to
  locate and size those blobs was established. Layouts would be in
  `bld/dip/watcom/c/watlcl.c`, `wattype.c` and their headers, which were not opened.
- **`FOX_SIGNATURE1` (0x8300), `FOX_SIGNATURE2` (0x8301) and `WAT_RES_SIG` (0x8302)
  trailer layouts**, beyond the fact that the DIP's skip loop reads them with the same
  shape as the Watcom one (u16 sig at +0, u32 size at +10). No defining source read.
- **`NON_SECT_INFO`'s literal define** was not found in the linker headers fetched. Its
  value is pinned from the reader side anyway: `SEG_COUNT_MASK` is `0x7fff` and the flag
  is `count & ~SEG_COUNT_MASK`, so it is `0x8000`.
- **The user-facing compiler flag for 16-bit DWARF** (`wcc -hd` or similar) — 16-bit
  DWARF support was established in the code generator and linker, but the compiler
  front end's option table was not read.
- **`pointer_uint` width in the demand link table.** The linker writes `unsigned_32`
  entries, so the on-disk width is 4; the DIP reads with `sizeof(pointer_uint)`, which
  looks host-dependent. Trust the writer: 4 bytes.
