#!/bin/bash
# tests/dosrun/run_tests.sh [dosbox-x]: run dosrun jobs against the programs beside this and check their events.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DOSBOX=$(cd "$(dirname "${1:-$HERE/../../src/dosbox-x}")" && pwd)/$(basename "${1:-dosbox-x}")
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
printf '[sdl]\nautolock=false\n[dosbox]\nmemsize=16\nstartbanner=false\nquit warning=false\n[cpu]\ncore=normal\n' > "$WORK/job.conf"
failed=0

# job LINES...: the job's events
job() {
    { printf '%s\n' "$@"; echo .; } | DOSRUN_FD=3 SDL_VIDEODRIVER=dummy "$DOSBOX" -nolog -conf "$WORK/job.conf" 3>&1 >/dev/null 2>&1
}

check() { # name condition-output
    if [ -n "$2" ]; then echo "PASS $1"; else echo "FAIL $1"; failed=1; fi
}

# A :write range stops the job at the write into it, not at one beside it.
events=$(job ':ms 10000' "mount w $HERE" 'w:' 'WRITES.COM' ':write 50010')
check write_stops_at_the_watched_byte "$(echo "$events" | grep '"ev":"write","address":"50010","size":1,"value":"42"')"
check write_ends_the_job_as_write "$(echo "$events" | grep '"ev":"end","reason":"write"')"

# Without one the program runs to its end.
events=$(job ':ms 10000' "mount w $HERE" 'w:' 'WRITES.COM')
check no_write_range_runs_to_exit "$(echo "$events" | grep '"ev":"end","reason":"exit"')"

# A :profile job counts by function and line: prof.c's run() calls outer() once, outer() calls inner(100) three
# times, and the loop line of inner() runs 300 trips. The functions' counts add up to the program's total.
SYMBOLS=$(cd "$HERE/../symbols/fixtures" && pwd)
events=$(job ':ms 10000' ':profile' "mount w $SYMBOLS" 'w:' 'SYMPRX.EXE')
profile=$(echo "$events" | grep '"ev":"profile"')
check profile_event_comes_before_the_end "$profile"
verdict=$(echo "$profile" | python3 -c '
import json, re, sys
p = json.loads(sys.stdin.read())
f = {re.sub(r"^_|_$|@\d+$", "", x["name"]): x for x in p["functions"]}
loop = [l for l in p["lines"] if l["file"].endswith("prof.c") and l["line"] == 8]
ok = (f["inner"]["calls"], f["outer"]["calls"], f["run"]["calls"]) == (3, 1, 1)
ok = ok and f["outer"]["inclusive"] == f["outer"]["self"] + f["inner"]["inclusive"]
ok = ok and len(loop) == 1 and loop[0]["self"] > 0 and loop[0]["self"] % 300 == 0
ok = ok and p["attributed_instructions"] == p["instructions"] > 0
print("ok" if ok else "")' 2>/dev/null)
check profile_counts_calls_loops_and_adds_up "$verdict"

# Without it, nothing is counted by function.
events=$(job ':ms 10000' "mount w $SYMBOLS" 'w:' 'SYMPRX.EXE')
check no_profile_event_without_the_option "$(echo "$events" | grep -v '"ev":"profile"' | grep '"ev":"end"')"

exit $failed
