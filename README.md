# Master PDF

A fast PDF editor with a Windows 98 look, for Windows, macOS and Linux. Version 2 is a C++ rewrite of the
Python app: built on **Qt 6** (the windows), **MuPDF** (reading and drawing PDFs) and **Tesseract** (reading
scanned text, Arabic and English).

## Download

Get the newest version from [Releases](https://github.com/bocchhii/master-pdf/releases/latest):

| System | File |
|---|---|
| Windows | `MasterPDF-Windows-Setup.exe` |
| macOS (Apple chips, M1 and newer) | `MasterPDF-Mac-AppleChip-M1-and-newer.dmg` |
| macOS (Intel) | `MasterPDF-Mac-Intel-chip.dmg` |
| Linux | `MasterPDF-Linux.AppImage` |

The app checks for a newer version when it starts (Help > Check for updates, or Settings > Updates). On
Windows it downloads and installs the update itself; on macOS and Linux it opens the new file in the browser.

macOS: the app isn't signed with an Apple developer account, so the first time, right-click it and choose
**Open**. Linux: make the AppImage executable (`chmod +x MasterPDF-Linux.AppImage`), then run it.

## Building

### Windows

1. Install [MSYS2](https://www.msys2.org/), then in its UCRT64 terminal:
   ```
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
     mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-tesseract-ocr \
     mingw-w64-ucrt-x86_64-leptonica mingw-w64-ucrt-x86_64-potrace mingw-w64-ucrt-x86_64-gumbo-parser
   pacman -U https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-libmupdf-1.24.3-2-any.pkg.tar.zst
   ```
2. Run `build.bat` (the program is built to `build\MasterPDF.exe`), then `run.bat` to start it.
3. For the installer: `bash packaging/build_windows.sh` puts the app with everything it needs in `dist\`;
   [Inno Setup](https://jrsoftware.org/isinfo.php) then makes `installer_output\MasterPDF-Windows-Setup.exe`
   from `installer.iss`.

### macOS and Linux

Qt 6, the libraries in `.github/workflows/release.yml`, then:

```
bash packaging/build_mupdf.sh build-deps/mupdf      # MuPDF 1.24.3, once
bash packaging/build_mac.sh                          # macOS: installer_output/*.dmg
bash packaging/build_linux.sh                        # Linux: installer_output/MasterPDF-Linux.AppImage
```

### Checking a build

`MasterPDF --self-test packaging/selftest.pdf shot.png` opens the PDF, reads its page with the text reader,
checks that the fonts and languages are found, saves a picture of the window and exits (0 = all went well).

## Releasing

Push a tag such as `v2.0.0`. GitHub Actions builds the four files above on Windows, macOS and Linux, runs the
self-test on each, and publishes them as a release. The version shown in the app comes from the tag.

## How it's built

| Folder | What's in it |
|---|---|
| `src/core` | The document: MuPDF, safe to draw from several threads; the text reader |
| `src/view` | The pages on screen: drawn in tiles on background threads |
| `src/app` | The main window, menus, tools, updates |
| `src/ui` | The Windows 98 look: title bars, buttons, dialogs, sounds |
| `packaging` | The build scripts for each system, the installer's pieces |

## Licence

MuPDF is used under the GNU AGPL v3, so this program is AGPL v3 as well.
