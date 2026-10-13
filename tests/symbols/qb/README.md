# qb: module variables of a BASIC program, as the debugger reads them

`vars.bas` is built by `llrm-qb dv.bas --dialect qb45 -fqb-runtime=llrm -O0 -g -o J.OBJ` (`--bc-codeview` for the first) and
linked in DOSBox with `LINK /NOE /CO J.OBJ,J.EXE,,LLRMQB.LIB;` by tools/dosbatch's tool directories:

- `SYMQBBC.EXE`: `--bc-codeview` (BASIC's CodeView 3 layout), QB45's LINK 3.69, which keeps names and offsets only.
- `SYMQBCV.EXE`: standard CodeView 4, VBDOS's LINK 5.31 (it runs CVPACK itself).

- `SYMQBJW.EXE`: standard CodeView 4, jwlink `debug codeview` with the runtime's archive (llrm #1528: the data segment's index in its
  records is the group's; before it was 0 and `var` found no variable).

LINK 3.69 and 5.10 write CodeView 2 and 3 (NB00, NB02) and drop CodeView 4 records: a CV4 object under them has no symbols.
jwlink keeps CV4 (`debug codeview`, NB05) unpacked.

`fn.bas` (`SYMQBFN.EXE`): a FUNCTION whose parameters are BASIC's by-reference ones, `-g` as above, jwlink `debug codeview` with the
runtime's archive. `locals` in `Add` gives each parameter's near pointer, and what it points at.

`SYMQBM3.EXE` (`near32.py`): `fn.bas` by `llrm-qb -m32 -g` (`-march=i486`, CodeView 4) linked into an LE by jwlink `debug codeview`
(tools/dosbatch/qbbuild.py's flat target): its by-reference parameters are 32-bit near pointers.
