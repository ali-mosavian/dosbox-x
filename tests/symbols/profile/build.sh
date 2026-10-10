#!/bin/sh
# tests/symbols/profile/build.sh: prof.c as an LE (DOS/32A), a PE (HX), a 16-bit MZ and a 16-bit NE (HX), and SYMPRX.EXE, the MZ without its key waits (for dosrun jobs), each with
# jwlink's DWARF. Needs llrm-c, jwasm and jwlink, and OW_ROOT naming an Open Watcom tree (its DOS/32A stub).
# The output is committed in ../fixtures.
set -e
cd "$(dirname "$0")"
llrm-c -m32 -g -O0 -fobject-format=omf -o prof32.obj prof.c
llrm-c -m16 -g -O0 -fobject-format=omf -o prof16.obj prof.c
jwasm -q -omf -Zi -Fo start32.o start32.asm
jwasm -q -omf -Zi -Fo start16.o start16.asm
jwasm -q -omf -Zi -Fo start16ne.o start16ne.asm
jwasm -q -omf -Zi -Fo start16run.o start16run.asm
jwlink option quiet debug dwarf format os2 le option stub="$OW_ROOT/bld/redist/dos32a/stub32a.exe" name SYMPRL.EXE file start32.o file prof32.obj
jwlink option quiet debug dwarf format windows nt runtime console name SYMPRP.EXE file start32.o file prof32.obj
jwlink option quiet debug dwarf format dos name SYMPRM.EXE file start16.o file prof16.obj
jwlink option quiet debug dwarf format dos name SYMPRX.EXE file start16run.o file prof16.obj
jwlink option quiet debug dwarf format os2 name SYMPRN.EXE file start16ne.o file prof16.obj
# DPMILD16 runs "DPMI-16" (5) clients; the linker writes the OS/2 target
python3 - <<'PY'
d = bytearray(open("SYMPRN.EXE", "rb").read())
d[int.from_bytes(d[0x3c:0x40], "little") + 0x36] = 5
open("SYMPRN.EXE", "wb").write(d)
PY
mv SYMPRL.EXE SYMPRP.EXE SYMPRM.EXE SYMPRN.EXE SYMPRX.EXE ../fixtures/
rm -f prof32.obj prof16.obj start32.o start16.o start16ne.o start16run.o
