#pragma once

#include <QString>
#include <QStringList>

// The fonts on this computer (and the ones the app comes with, and the ones added in it), as
// the Python app keeps them: each font's names read from its file, so a PDF's font
// ("ABCDEF+TimesNewRomanPS-BoldMT") finds its file - or the closest one, as wide.
namespace fonts {

struct Found {
    QString path;        // empty: none
    int index = 0;       // (the font in a .ttc collection)
    QString family;      // its family's name ("Times New Roman")
    bool bold = false, italic = false;  // as wanted (made so if the file isn't)
    bool realBold = false, realItalic = false;  // as the file itself is
    double outline = 0;  // (made bold here - no bold file: its letters' outline this wide, part of the size; 0: the usual)
};

void startScan();  // (in the background, at start)
void waitScan();
void rescan();  // (My fonts changed: looked up again, now)
QString key(const QString &name);  // "ABCDEF+TimesNewRomanPS-BoldMT" -> "timesnewromanbold"

// the installed font closest to a PDF's font (flags: bold 1, italic 2, serif 4, mono 8)
Found findFont(const QString &pdfFont, int flags);
// the font of a family nearest bold / italic
Found findFamily(const QString &family, bool bold, bool italic);
QStringList families();  // every family, A to Z
bool hasLetters(const Found &f, const QString &text);  // all of them (spaces aside)
// the one to write text with: the font itself if it has the letters, else Arial or one of
// the app's own fonts that has them (Arabic...)
Found forText(Found f, const QString &text);
QString bundledFolder();  // the fonts the app comes with
QString myFolder();       // the fonts added in the app
// a PDF's own (embedded) font: found by its name as installed ones are, so text retyped in it
// stays in it (flags as findFont's). Just TrueType / OpenType with a Unicode map; a subset: its
// letters only - forText takes another font for text it doesn't have
void addEmbedded(const QString &pdfFont, const QByteArray &program, int flags);
bool isEmbedded(const Found &f);  // (one of those: not installed - Qt doesn't know it by its name)
QString embeddedFolder();

}  // namespace fonts
