#!/bin/sh
# The dosrun build: headless jobs on the normal core, nothing a test run does not use.
set -e
./autogen.sh
./configure --enable-sdl2 --enable-debug --disable-dynamic-core --disable-mt32 --disable-libslirp \
    --disable-libfluidsynth --disable-printer --disable-freetype --disable-opengl --disable-x11 \
    --disable-sdlnet --disable-avcodec
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
