# Symbol tests

`run_tests.sh [dosbox-x]` runs real programs under the debug socket and asks for symbols.

- `le_symbols.py`: `SYMPROBE.BAS` built by llrm-qb -m32 (plain, and `-g` with `debug codeview`), run by the
  emulator's own DOS/32A.
- `ne_symbols.py`: `ne/prog.c` built by `ne/build.sh`, run by HX's DPMILD16 + HDPMI16.
- `pe_symbols.py`: `pe/prog.c` + `pe/start.asm` built by `pe/build.sh`, run by HX's DPMILD32 + HDPMI32.
- `dwarf_symbols.py`, `dwarf16_symbols.py`: our prog.c linked with jwlink `debug dwarf` as LE, PE, 16-bit MZ and 16-bit NE (`dwarf/build.sh`, `dwarf16/build.sh`); no .MAP beside the images.

HX is third-party and never committed. Fetch it into an empty directory, outside the tree:

    mkdir -p ~/work/other/hxdos && cd ~/work/other/hxdos
    curl -LO https://github.com/Baron-von-Riedesel/HX/releases/download/v2.23/HXRT223.zip
    sha256sum HXRT223.zip   # 20510cf66d6c8704f66f908f8fbf9783198195c1e043a6ca9fb6adeadf8785c9
    unzip HXRT223.zip -d rt
    export HX_DOS=$PWD/rt/BIN

The tests run its programs only inside the emulator, and skip with a reason when `HX_DOS` is unset.
