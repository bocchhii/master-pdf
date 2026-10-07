#!/usr/bin/env bash
# Builds "Master PDF.app" and puts it in a disk image (.dmg) to drag into Applications.
# Needs Python 3.10+ with Tk. Run from the project folder on a Mac:
#   bash packaging/build_mac.sh [name of the .dmg, without .dmg]
set -e
cd "$(dirname "$0")/.."
NAME="${1:-MasterPDF-macOS}"

python3 -m pip install -r requirements.txt pyinstaller
python3 -m PyInstaller --noconfirm --clean --windowed --name "Master PDF" --icon icon.png \
    --osx-bundle-identifier com.bocchhii.masterpdf \
    --add-data "icon.ico:." --add-data "icon.png:." \
    --collect-all tkinterdnd2 --collect-all pymupdf --hidden-import PIL._tkinter_finder master_pdf.py

# the disk image: the app, and a shortcut to Applications to drag it onto
rm -rf build/dmg
mkdir -p build/dmg installer_output
cp -R "dist/Master PDF.app" build/dmg/
ln -s /Applications build/dmg/Applications
for attempt in 1 2 3; do  # (hdiutil now and then finds the disk busy: tried again)
    hdiutil create -volname "Master PDF" -srcfolder build/dmg -ov -format UDZO "installer_output/$NAME.dmg" && break
    sleep 5
done
test -f "installer_output/$NAME.dmg"
echo "Built: installer_output/$NAME.dmg"
