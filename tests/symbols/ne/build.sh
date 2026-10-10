#!/bin/sh
# tests/symbols/ne/build.sh: SYMNE.EXE, a 16-bit NE for HX's DPMI loader, with CodeView and a map.
# Needs llrm-c, jwasm and jwlink; the output is committed in ../fixtures.
set -e
cd "$(dirname "$0")"
llrm-c -m16 -g -O0 -fobject-format=omf -o prog.obj prog.c
jwasm -q -omf -Zi -Fo start.o start.asm
jwlink option quiet option map=SYMNE.MAP debug codeview format os2 name SYMNE.EXE file start.o file prog.obj
# the linker writes the OS/2 target; DPMILD16 runs "DPMI-16" (5) clients
python3 - <<'PY'
d = bytearray(open("SYMNE.EXE", "rb").read())
d[int.from_bytes(d[0x3c:0x40], "little") + 0x36] = 5
open("SYMNE.EXE", "wb").write(d)
PY
mv SYMNE.EXE SYMNE.MAP ../fixtures/
rm -f prog.obj start.o
