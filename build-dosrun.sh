#!/bin/sh
# The dosrun build: headless jobs on the normal core, nothing a test run does not use.
set -e
./autogen.sh
# libpng, zlib and pcap live under Homebrew's prefix on macOS; no SDL is linked.
if command -v brew >/dev/null; then
    prefix=$(brew --prefix)
    export CPPFLAGS="-I$prefix/include $CPPFLAGS" LDFLAGS="-L$prefix/lib $LDFLAGS"
fi
./configure --enable-headless --enable-debug --disable-dynamic-core --disable-mt32 --disable-libslirp \
    --disable-libfluidsynth --disable-printer --disable-freetype --disable-opengl --disable-x11 \
    --disable-sdlnet --disable-avcodec
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
