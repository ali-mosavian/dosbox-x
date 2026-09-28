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

exit $failed
