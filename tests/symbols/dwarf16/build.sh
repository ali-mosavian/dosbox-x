#!/bin/sh
# tests/symbols/dwarf16/build.sh: SYMMZD.EXE (16-bit MZ) and SYMNED.EXE (16-bit NE for HX), both with jwlink's DWARF.
# Needs llrm-c, jwasm and jwlink; the output is committed in ../fixtures.
set -e
cd "$(dirname "$0")"
llrm-c -m16 -g -O0 -fobject-format=omf -o prog.obj prog.c
jwasm -q -omf -Zi -Fo start.o start.asm
jwasm -q -omf -Zi -Fo startne.o startne.asm
jwlink option quiet debug dwarf format dos name SYMMZD.EXE file start.o file prog.obj
jwlink option quiet debug dwarf format os2 name SYMNED.EXE file startne.o file prog.obj
# DPMILD16 runs "DPMI-16" (5) clients; the linker writes the OS/2 target
python3 - <<'PY'
d = bytearray(open("SYMNED.EXE", "rb").read())
d[int.from_bytes(d[0x3c:0x40], "little") + 0x36] = 5
open("SYMNED.EXE", "wb").write(d)
PY
mv SYMMZD.EXE SYMNED.EXE ../fixtures/
rm -f prog.obj start.o startne.o
