#!/bin/sh
# tests/symbols/vtime/build.sh: VTINT10.COM and VTINT21.COM, committed in ../fixtures. Needs jwasm.
set -e
cd "$(dirname "$0")"
jwasm -q -bin -DVECTOR=10h -DFUNCTION=3 -Fo ../fixtures/VTINT10.COM vtint.asm
jwasm -q -bin -DVECTOR=21h -DFUNCTION=2Ah -Fo ../fixtures/VTINT21.COM vtint.asm
