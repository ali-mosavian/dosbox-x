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

A `.MAP` is not needed when the EXE carries debug info. On every program load the
server finds the host file the guest launched, parses whatever debug info is in it,
and adds the symbols to the same index `dosbox_where`, `dosbox_symbols` and
`dosbox_bp_set` already use. No load step.

The host file is found through the guest's drive mounts — read from the launch
config's `[autoexec]` and updated by `dosbox_mount` — and then confirmed against the
MZ header fields `loadInfo` carries back, so two build directories holding the same
`QRENDER.EXE` do not resolve to each other. Add more places to look with
`dosbox_debuginfo {op:"paths", paths:[...]}`.

| format | how it is found | what comes out |
|---|---|---|
| Microsoft CodeView `NB05`–`NB11` | `NBxx` trailer at EOF, or the MZ image end | publics, module data, procedures, labels, object-module names, source line numbers |
| Microsoft CodeView `NB00`–`NB02` | as above | the subsection directory only; the pre-CV4 record layouts are unread |
| Borland TDINFO (`0x52FB`) | MZ image end; the format has no trailer | globals, module names. Line records are counted, not decoded — their layout is not published |
| Watcom (`0x8386`) | master header in the last 14 bytes | globals, module names |

`dosbox_debuginfo {op:"status"}` says what was found and which file it came from;
`op:"modules"` lists the object modules; `op:"lines"` maps an address to file:line.
