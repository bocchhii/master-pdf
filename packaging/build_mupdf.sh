#!/usr/bin/env bash
# Builds MuPDF 1.24.3 - the version the app is written for (Windows gets the same one from MSYS2) - as static
# libraries, against the system's freetype, harfbuzz, jpeg... (the ones the app links as well), into a folder:
#   bash packaging/build_mupdf.sh <folder>        (Linux and macOS)
set -e
VER=1.24.3
mkdir -p "$1"
PREFIX="$(cd "$1" && pwd)"
WORK="$(mktemp -d)"
curl -fsSL -o "$WORK/mupdf.tgz" "https://mupdf.com/downloads/archive/mupdf-$VER-source.tar.gz"
tar -xzf "$WORK/mupdf.tgz" -C "$WORK"
cd "$WORK/mupdf-$VER-source"

# where the system's libraries' headers are (each one asked of pkg-config on its own: one missing doesn't lose the others)
FLAGS="-fPIC"
for lib in freetype2 harfbuzz libopenjp2 gumbo libjpeg zlib; do
    FLAGS="$FLAGS $(pkg-config --cflags "$lib" 2>/dev/null || true)"
done
if command -v brew >/dev/null; then
    FLAGS="$FLAGS -I$(brew --prefix)/include -I$(brew --prefix jpeg-turbo)/include"
fi
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
make -j"$JOBS" build=release USE_SYSTEM_LIBS=yes HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_LIBCRYPTO=no \
    XCFLAGS="$FLAGS" libs

mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp build/release/libmupdf.a build/release/libmupdf-third.a "$PREFIX/lib/"
cp -R include/mupdf "$PREFIX/include/"
echo "MuPDF $VER: $PREFIX"
