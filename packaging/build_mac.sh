#!/usr/bin/env bash
# Builds "Master PDF.app" and puts it in a disk image (.dmg) to drag into Applications.
# Needs Qt 6 (macdeployqt on the PATH), MuPDF from packaging/build_mupdf.sh, and the Homebrew libraries the GitHub
# build installs (.github/workflows/release.yml). Run from the project folder on a Mac:
#   bash packaging/build_mac.sh [name of the .dmg, without .dmg] [version] [MuPDF's folder]
set -e
cd "$(dirname "$0")/.."
NAME="${1:-MasterPDF-macOS}"
VERSION="${2:-dev}"
MUPDF="${3:-$PWD/build-deps/mupdf}"
QT="$(dirname "$(dirname "$(command -v macdeployqt)")")"
BREW="$(brew --prefix)"

cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DMASTERPDF_VERSION="$VERSION" \
    -DCMAKE_PREFIX_PATH="$MUPDF;$QT;$BREW;$(brew --prefix jpeg-turbo)"
cmake --build build-mac -- -k 0  # (every error at once, not just the first)
APP=build-mac/MasterPDF.app

# the app's icon, from the same picture as Windows'
ICONSET=build-mac/MasterPDF.iconset
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s resources/icons/app.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) resources/icons/app.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/MasterPDF.icns"

# Qt and the other libraries copied into the app, then signed for this Mac (ad hoc: no developer account)
macdeployqt "$APP" -always-overwrite
# (macdeployqt misses libraries Homebrew's own ones load by @rpath - libwebp's libsharpyuv...: copied in too, and every
# library's Homebrew path pointed at its copy in the app - until nothing's left to copy)
FW="$APP/Contents/Frameworks"
while true; do
    added=0
    for bin in "$APP/Contents/MacOS/MasterPDF" "$FW"/*.dylib; do
        for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}'); do
            name="$(basename "$dep")"
            case "$dep" in
                @rpath/*|@loader_path/*)
                    src="$(ls "$BREW"/opt/*/lib/"$name" "$BREW"/lib/"$name" 2>/dev/null | head -n 1)" ;;
                "$BREW"/*|/usr/local/*)
                    src="$dep"
                    install_name_tool -change "$dep" "@executable_path/../Frameworks/$name" "$bin" 2>/dev/null || true ;;
                *) continue ;;
            esac
            if [ -n "$src" ] && [ ! -f "$FW/$name" ]; then
                cp -L "$src" "$FW/$name"
                chmod u+w "$FW/$name"
                install_name_tool -id "@executable_path/../Frameworks/$name" "$FW/$name"
                added=1
            fi
        done
    done
    [ $added = 1 ] || break
done
codesign --force --deep --sign - "$APP"

# the disk image: the app, and a shortcut to Applications to drag it onto
rm -rf build-mac/dmg
mkdir -p build-mac/dmg installer_output
cp -R "$APP" "build-mac/dmg/Master PDF.app"
ln -s /Applications build-mac/dmg/Applications
for attempt in 1 2 3; do  # (hdiutil now and then finds the disk busy: tried again)
    hdiutil create -volname "Master PDF" -srcfolder build-mac/dmg -ov -format UDZO "installer_output/$NAME.dmg" && break
    sleep 5
done
test -f "installer_output/$NAME.dmg"
echo "Built: installer_output/$NAME.dmg"
