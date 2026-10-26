#pragma once

// Convert (Acrobat's Export PDF): a PDF written as another kind of file - Word, Excel, PowerPoint, pictures, web pages,
// Rich Text, PostScript, text, XML. Its pages read as paragraphs (headings found by their size), pictures, tables and
// comments, then written in the format's own way. Safe on a thread of its own (the Document locks itself).

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

class Document;

namespace convert {

enum class Format {
    Docx,           // Word Document
    Doc,            // Word 97-2003 Document (made from the .docx by Word, or LibreOffice)
    Xlsx,           // Microsoft Excel Workbook
    XmlSpreadsheet, // XML Spreadsheet 2003
    Pptx,           // PowerPoint presentation
    Jpeg,
    Jpeg2000,
    Tiff,
    Png,
    HtmlSingle,     // a single HTML page
    HtmlMulti,      // HTML pages - split at the headings or the bookmarks
    Rtf,            // Rich Text Format
    Eps,            // Encapsulated PostScript (a file a page)
    Ps,             // PostScript
    TextAccessible, // text in reading order, headings, lists, pictures and pages said
    TextPlain,
    Xml,            // XML 1.0 (as Acrobat's tagged XML)
};

struct Options {
    // Word, Rich Text, PowerPoint
    bool flowing = true;        // the text flowing as paragraphs (else: each page's lines where they are, page by page)
    bool comments = true;       // the PDF's comments in it
    bool images = true;         // its pictures in it
    bool recognize = true;      // pages with no text (scanned): their text read (OCR)
    QString languages = QStringLiteral("ara+eng");
    // spreadsheets
    int sheets = 0;             // 0: one worksheet for the whole PDF, 1: one for each table, 2: one for each page
    bool regionalNumbers = true;  // (the computer's decimal and thousands signs - else these)
    QChar decimal = QLatin1Char('.'), thousands = QLatin1Char(',');
    // pictures
    int dpi = 150;
    int colour = 0;             // 0: as it is (colour), 1: colour, 2: greyscale, 3: black and white
    int quality = 70;           // JPEG / JPEG 2000: 0..100
    bool progressive = false;   // JPEG
    int tiffCompression = 1;    // 0: none, 1: LZW, 2: ZIP (black and white: CCITT G4)
    bool tiffOneFile = false;   // TIFF: every page in one file
    bool allImages = false;     // the PDF's own pictures, not its pages
    // web pages
    int htmlNav = 0;            // a single page's navigation: 0 none, 1 its headings, 2 the PDF's bookmarks
    int htmlSplit = 0;          // pages: split at 0 the headings, 1 the bookmarks
    // PostScript
    int firstPage = 0, lastPage = -1;  // (from 0; -1: the last)
    bool binary = false;        // (the pictures' bytes as they are - else ASCII85, safe to send anywhere)
    // text, XML
    QString encoding = QStringLiteral("UTF-8");
    bool bookmarks = true;      // XML: the PDF's bookmarks too
    bool xmlImages = true;      // XML: its pictures written as files
    bool subfolder = true;
    QString folder = QStringLiteral("images");
    QString prefix;
    QString imageFormat = QStringLiteral("jpg");  // jpg, png, tif
    int downsample = 0;         // (dpi; 0: as they are)
};

struct Progress {
    std::function<void(double done, const QString &doing)> step = [](double, const QString &) {};
    const std::atomic<bool> *cancel = nullptr;
};

// the PDF written to `path` as `format`: the files written (some formats write several: a picture a page...) - none,
// and why, if it couldn't be
QStringList run(Document *doc, Format format, const QString &path, const Options &options, const Progress &progress,
                QString *error);

// (the file's extension a format writes: "docx", "jpg"...)
QString extension(Format format);

}  // namespace convert
