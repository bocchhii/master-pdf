#!/usr/bin/env bash
# Builds MasterPDF-Linux.AppImage: the app as one file that runs on most Linux systems.
# Needs Python 3.10+ with Tk. Run from the project folder:  bash packaging/build_linux.sh
set -e
cd "$(dirname "$0")/.."

python -m pip install -r requirements.txt pyinstaller
python -m PyInstaller --noconfirm --clean --onefile --windowed --name MasterPDF \
    --add-data "icon.ico:." --add-data "icon.png:." \
    --collect-all tkinterdnd2 --collect-all pymupdf --hidden-import PIL._tkinter_finder master_pdf.py

# the AppImage's folder: the app, its menu entry and its icon
rm -rf build/AppDir
mkdir -p build/AppDir/usr/bin
cp dist/MasterPDF build/AppDir/usr/bin/MasterPDF
cp packaging/master-pdf.desktop build/AppDir/master-pdf.desktop
cp icon.png build/AppDir/master-pdf.png
cat > build/AppDir/AppRun <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/MasterPDF" "$@"
EOF
chmod +x build/AppDir/AppRun build/AppDir/usr/bin/MasterPDF

# appimagetool packs the folder into one file (run without FUSE: works in containers too)
TOOL=build/appimagetool-x86_64.AppImage
if [ ! -x "$TOOL" ]; then
    curl -L -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi
mkdir -p installer_output
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" build/AppDir installer_output/MasterPDF-Linux.AppImage
echo "Built: installer_output/MasterPDF-Linux.AppImage"
