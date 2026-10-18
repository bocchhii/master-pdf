#!/usr/bin/env bash
# Builds the Windows app into dist\ - MasterPDF.exe with everything it needs beside it (Qt, MuPDF's libraries, the
# text reader, the fonts) - ready for installer.iss. Run in MSYS2's UCRT64 terminal, from the project folder:
#   bash packaging/build_windows.sh [version]
set -e
cd "$(dirname "$0")/.."
VERSION="${1:-dev}"

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DMASTERPDF_VERSION="$VERSION"
cmake --build build-release

rm -rf dist
mkdir -p dist
cp build-release/MasterPDF.exe dist/
cp -R build-release/fonts build-release/tessdata dist/
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw dist/MasterPDF.exe

# every MSYS2 library the program and Qt's plugins load, and the ones those load (until nothing new turns up)
while true; do
    added=0
    for dll in $(find dist -iname '*.exe' -o -iname '*.dll' | xargs ldd 2>/dev/null | awk '{print $3}' | grep -iE '/ucrt64/bin/' | sort -u); do
        if [ ! -f "dist/$(basename "$dll")" ]; then
            cp "$dll" dist/
            added=1
        fi
    done
    [ $added = 1 ] || break
done
echo "Built: dist/MasterPDF.exe"
