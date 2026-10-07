# Master PDF

Ever needed to fix a typo in a PDF, sign a form or put a few files together, and ended up on
some website you didn't trust with your documents? That's why I made this. Master PDF is a small
Windows app that edits your PDFs right on your own PC. No accounts, no ads, and your files never
leave your computer.

It looks and feels like Windows 98, because why not.

## What it can do

- **Edit the text:** click any text in the PDF and retype it, move it, resize it, or change its
  font, size, colour and style, like in Word. Pictures can be moved, resized and replaced too.
- **Mark it up:** highlight, underline and strike out text, draw with a pen or marker, add notes,
  shapes, links and your signature, and rub things out with the eraser.
- **Pictures and text together:** wrap the text round a picture (square, tight, top and bottom),
  or put it behind or in front of the text, like Word's Layout Options.
- **Pages:** add, delete, rotate and drag pages into a new order, add blank pages, or split,
  extract, compress and export pages as pictures.
- **Make PDFs:** put PDFs, pictures and Word documents together into one PDF.
- **Find:** search the whole PDF as you type, with every result listed and highlighted.

Just open a PDF (or drag one in), make your changes and hit Save. You can also switch between a
few themes from the Theme menu.

<img width="1917" height="1079" alt="masterpdf" src="https://github.com/user-attachments/assets/105dca45-f7bb-4684-afea-f87d375dc057" />

## Install

Everything is on the [Releases](../../releases/latest) page.

**Windows**

1. Download **MasterPDF-Windows-Setup.exe**.
2. Run it. If Windows says **"Windows protected your PC"**, click **More info → Run anyway**.
   That warning shows up for any app that isn't signed with a paid certificate. It doesn't mean
   the app is unsafe.
3. Click through the installer and open Master PDF from the Start menu.

**macOS**

1. Download **MasterPDF-Mac-AppleChip-M1-and-newer.dmg** if your Mac has an Apple chip (M1, M2,
   M3, M4...), or **MasterPDF-Mac-Intel-chip.dmg** if it's an older Mac with an Intel chip. (Not
   sure? Apple menu → About This Mac: it says "Chip: Apple M..." or "Processor: Intel".)
2. Open it and drag **Master PDF** into **Applications**.
3. The first time, macOS says it can't check the app. Open **System Settings → Privacy &
   Security**, scroll down and click **Open Anyway**. Like the Windows warning, it shows up for
   any app that isn't signed with a paid Apple certificate.

**Linux**

1. Download **MasterPDF-Linux.AppImage**.
2. Make it runnable (right-click → Properties → Allow executing as program, or
   `chmod +x MasterPDF-Linux.AppImage`) and double-click it.

The app tells you when there's a new version. On Windows it updates itself; on macOS and Linux it
downloads the new version for you to install the same way. Checking for updates is the only time
it goes online.

Turning Word documents into PDFs needs Microsoft Word or LibreOffice on Windows, and
[LibreOffice](https://www.libreoffice.org/) on macOS and Linux. Everything else works on its own.

## Run it from the code

You'll need [Python](https://www.python.org/downloads/) 3.10 or newer (on Linux, with Tk: the
`python3-tk` package).

```
pip install -r requirements.txt
python master_pdf.py
```

To build it yourself: on Windows install [Inno Setup](https://jrsoftware.org/isdl.php) and run
`build.bat`; on a Mac run `bash packaging/build_mac.sh`; on Linux run
`bash packaging/build_linux.sh`. New releases for all three are built automatically on GitHub when
a version tag (like `v1.2.0`) is pushed.

## Thanks to

- [PyMuPDF](https://pymupdf.readthedocs.io/) (built on [MuPDF](https://mupdf.com/)) for reading, drawing and writing the PDFs
- [Pillow](https://python-pillow.org/) for pictures
- [NumPy](https://numpy.org/) for the sounds
- [tkinterdnd2](https://github.com/Eliav2/tkinterdnd2) for drag and drop
- [PyInstaller](https://pyinstaller.org/) and [Inno Setup](https://jrsoftware.org/isinfo.php) for the app and installer

Each of these has its own license. PyMuPDF and MuPDF are under the AGPL, which applies if you
share the built app.

Made by **bocchi the old**. For more tools, check out [my GitHub profile](https://github.com/bocchhii).
