#!/bin/sh
# tests/symbols/run_tests.sh [dosbox-x]: symbols of real programs, asked through the debug socket.
HERE=$(cd "$(dirname "$0")" && pwd)
DOSBOX=${1:-$HERE/../../src/dosbox-x}
status=0
for test in le_symbols ne_symbols pe_symbols; do
    python3 "$HERE/$test.py" "$DOSBOX" || status=1
done
exit $status
