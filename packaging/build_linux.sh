#!/usr/bin/env bash
# Builds MasterPDF-Linux.AppImage: the app as one file that runs on most Linux systems.
# Needs Qt 6 (its qmake on the PATH, or QMAKE set), MuPDF from packaging/build_mupdf.sh, and the libraries the
# GitHub build installs (.github/workflows/release.yml). Run from the project folder:
#   bash packaging/build_linux.sh [version] [MuPDF's folder]
set -e
cd "$(dirname "$0")/.."
VERSION="${1:-dev}"
MUPDF="${2:-$PWD/build-deps/mupdf}"
QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
QT="$(dirname "$(dirname "$QMAKE")")"

cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DMASTERPDF_VERSION="$VERSION" \
    -DCMAKE_PREFIX_PATH="$MUPDF;$QT"
cmake --build build-linux

# the AppImage's folder: the app with its fonts and the text reader's languages beside it, its menu entry, its icon
APPDIR=build-linux/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp build-linux/MasterPDF "$APPDIR/usr/bin/"
cp -R build-linux/fonts build-linux/tessdata "$APPDIR/usr/bin/"

# linuxdeploy copies in the libraries it needs (Qt's too) and packs the folder into one file (without FUSE: works
# in containers too)
TOOLS=build-linux/tools
mkdir -p "$TOOLS"
for tool in linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
            linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
    [ -x "$TOOLS/$(basename "$tool")" ] || curl -fsSL -o "$TOOLS/$(basename "$tool")" "https://github.com/linuxdeploy/$tool"
    chmod +x "$TOOLS/$(basename "$tool")"
done
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE PATH="$PWD/$TOOLS:$PATH"
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --executable "$APPDIR/usr/bin/MasterPDF" \
    --desktop-file packaging/master-pdf.desktop --icon-file packaging/master-pdf.png --plugin qt
# (the update check reaches GitHub over https: Qt's TLS plugin, if it wasn't taken)
if [ -d "$QT/plugins/tls" ] && [ ! -d "$APPDIR/usr/plugins/tls" ]; then
    mkdir -p "$APPDIR/usr/plugins"
    cp -R "$QT/plugins/tls" "$APPDIR/usr/plugins/"
fi
mkdir -p installer_output
LDAI_OUTPUT=installer_output/MasterPDF-Linux.AppImage linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --output appimage
test -f installer_output/MasterPDF-Linux.AppImage
echo "Built: installer_output/MasterPDF-Linux.AppImage"
