#!/bin/sh
# tests/symbols/dwarf/build.sh: SYMLED.EXE (LE, DOS/32A) and SYMPED.EXE (PE, HX), both with jwlink's DWARF.
# Needs llrm-c, jwasm and jwlink, and OW_ROOT naming an Open Watcom tree (its redistributable DOS/32A stub).
# The output is committed in ../fixtures.
set -e
cd "$(dirname "$0")"
llrm-c -m32 -g -O0 -fobject-format=omf -o prog.obj prog.c
jwasm -q -omf -Zi -Fo start.o start.asm
jwlink option quiet option map=SYMLED.MAP debug dwarf format os2 le option stub="$OW_ROOT/bld/redist/dos32a/stub32a.exe" name SYMLED.EXE file start.o file prog.obj
jwlink option quiet option map=SYMPED.MAP debug dwarf format windows nt runtime console name SYMPED.EXE file start.o file prog.obj
# locals.c needs llrm's own DWARF in the object (-gdwarf, DWARF 5): llrm-c from alim/feat/dwarf-omf or later
llrm-c -m32 -gdwarf -O0 -fobject-format=omf -o locals.obj locals.c
jwlink option quiet debug dwarf format os2 le option stub="$OW_ROOT/bld/redist/dos32a/stub32a.exe" name SYMLOC.EXE file start.o file locals.obj
# opt.c at -O2: a constant with no home (DW_OP_consts + DW_OP_stack_value), parameters moving between registers
llrm-c -m32 -gdwarf -O2 -fobject-format=omf -o opt.obj opt.c
llrm-c -m32 -gdwarf -O0 -fobject-format=omf -o sink.obj sink.c
jwlink option quiet debug dwarf format os2 le option stub="$OW_ROOT/bld/redist/dos32a/stub32a.exe" name SYMOPT.EXE file start.o file opt.obj file sink.obj
mv SYMLED.EXE SYMPED.EXE SYMLOC.EXE SYMOPT.EXE ../fixtures/
rm -f prog.obj locals.obj opt.obj sink.obj start.o SYMLED.MAP SYMPED.MAP
