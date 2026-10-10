#!/bin/sh
# tests/symbols/pe/build.sh: SYMPE.EXE, a 32-bit PE for HX's DPMI loader, with CodeView and a map.
# Needs llrm-c, jwasm and jwlink; the output is committed in ../fixtures.
set -e
cd "$(dirname "$0")"
llrm-c -m32 -g -O0 -fobject-format=coff -o prog.obj prog.c
jwasm -q -coff -Zi -Fo start.o start.asm
jwlink option quiet option map=SYMPE.MAP debug codeview format windows nt runtime console name SYMPE.EXE file start.o file prog.obj option start=_start
mv SYMPE.EXE SYMPE.MAP ../fixtures/
rm -f prog.obj start.o
