#!/bin/sh
# tests/symbols/run_tests.sh [dosbox-x]: symbols of real programs, asked through the debug socket.
HERE=$(cd "$(dirname "$0")" && pwd)
exec python3 "$HERE/le_symbols.py" "${1:-$HERE/../../src/dosbox-x}"
