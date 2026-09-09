# DOSBox-X Debug MCP

Canonical MCP server for the DOSBox-X debug socket in this repo.

## Setup

```bash
cd /Users/alim/work/other/dosbox-x-debug/mcp
npm ci
npm run build
```

Cursor MCP descriptor command:

```json
{
  "command": "node",
  "args": ["/Users/alim/work/other/dosbox-x-debug/mcp/build/index.js"]
}
```

The older `/Users/alim/work/other/voodoo-pmode/mcp` copy is left untouched for compatibility. Point Cursor at this package when switching to the canonical home.

## MAP And Load Info

Examples:

```json
{ "command": "D32TEST.EXE", "breakAtEntry": true, "mapFile": "/Users/alim/work/other/d32x/build/d32xtest/D32TEST.MAP" }
```

```json
{ "op": "resolve", "name": "D32WRAP+0x250" }
```

```json
{ "name": "entry" }
```

`dosbox_debug_load_program`, `dosbox_run_program`, and `dosbox_load_and_run_to` accept `mapFile`. `dosbox_map` loads/lists/resolves Microsoft LINK `.MAP` data. `dosbox_where` reports current registers, cached load metadata, and nearest MAP context.

## Debug Info In The Program Itself

A `.MAP` is not needed when the EXE carries debug info. The emulator reads it as DOS
EXEC loads the program, through its own DOS filesystem, and this server mirrors the
result into the same index `dosbox_where`, `dosbox_symbols` and `dosbox_bp_set`
already use. No load step, and nothing here parses a debug format any more: the
readers live in `src/debug/debug_symfmt*.cpp` and are reached over the debug socket
with `sym`, `sym_list`, `where` and `sym_load`.

Reading it inside the emulator is what makes the program's identity certain. There is
no mount table to walk and no MZ header to fingerprint against the running image, and
programs on image or zip drives work like any other.

| format | how it is found | what comes out |
|---|---|---|
| Microsoft CodeView `NB05`-`NB11` | `NBxx` trailer at EOF, or the MZ image end | publics, module data, procedures, labels, object-module names, source line numbers, types, and each proc's frame. Types read whether CVPACK has gathered them into one table or each module still carries its own, as jwasm and LINK leave them |
| Microsoft CodeView `NB00`-`NB02` | as above | the subsection directory only; the pre-CV4 record layouts are unread |
| Borland TDINFO (`0x52FB`) | MZ image end, or a `.TDS` beside the program; the format has no trailer | globals, module names, source files, line records, types, and scopes with their locals and parameters |
| Watcom (`0x8386`) | master header in the last 14 bytes | globals, module names |
| LINK `.MAP` | beside the program, only when it carries none of the above | publics |

`dosbox_debuginfo {op:"status"}` says what the emulator found; `op:"modules"` lists the
object modules; `op:"lines"` maps an address to file:line; `op:"load"` has the emulator
read another host file, an EXE or a `.MAP`.

## Lines, Locations And Variables

`dosbox_bp_set` takes a gdb-style `location` instead of an address: `udtbas.bas:29`,
`pr_add`, `pr_add+0x10`, `*0x82F4`. A line with no code of its own moves to the next
line that has some.

`dosbox_var` reads a variable by name -- a global, or a local or parameter of whatever
is running at CS:EIP, out of the frame or the register it lives in. `dosbox_locals`
lists every one in scope, innermost first.

A value comes back decoded when the debug info gave it a type: a structure field by
field, an array element by element. A BASIC array's symbol addresses a runtime
descriptor rather than the elements, so it is followed through to them; the element
width the type table declares is the guard against following what is not a descriptor.
