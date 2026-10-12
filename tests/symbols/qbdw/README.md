`vars.bas` through `llrm-qb vars.bas --dialect qb45 -fqb-runtime=llrm -O0 -gdwarf-2` and
`jwlink option quiet format dos name SYMQBDW.EXE debug dwarf file vars.obj library LLRMQB.LIB` (the runtime's archive of
tools/dosbatch/qb32.py): DWARF 2 in a 16-bit MZ, the module's variables among them, the array inside its module's function.

`SYMQBFD.EXE`: `qb/fn.bas` the same way (`-gdwarf-2`, jwlink `debug dwarf`): the function's parameters and local, read by
`locals` through the frame base (DWARF 2 locals of a 16-bit MZ).
