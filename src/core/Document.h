#pragma once

#include <QPainterPath>
#include <QHash>
#include <QImage>

#include <atomic>
#include <tuple>
#include <map>
#include <QPointF>
#include <QRect>
#include <QSizeF>
#include <QLineF>
#include <QString>
#include <QStringList>
#include <QColor>
#include <QTransform>

#include "core/Fonts.h"
#include "core/PageText.h"

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct fz_context;
struct fz_document;
struct fz_display_list;
struct pdf_document;

// An open PDF. Safe to render from several threads at once: each page is turned into a
// display list once (under a lock), then any thread draws parts of it with its own copy
// of the MuPDF context.
//
// The file is read whole, so it stays free to be saved over. Changes go through MuPDF's
// journal: Undo takes back one change at a time, keeping only what that change touched (never
// a copy of the whole file - the Python app's slowness).
// An annotation on a page, as read for the window (where it is, what it is)
struct Annot {
    int id = 0;            // its object number in the PDF (stays the same while it's changed)
    QString kind;          // "Highlight", "StrikeOut", "Underline", "Squiggly", "Ink", "Square"...
    QRectF rect;           // in points from the page's top left, as shown
    std::vector<QRectF> pieces;  // a text mark's pieces (one per line)
    QColor color;
    QString contents;      // what a note or text box says
    QString subject;       // ("Marker": a marker stroke)
    double fontSize = 12;  // a text box's
    QColor textColor;
    bool markup() const;   // highlight, strike-out, underline, squiggly
    bool movable() const;  // can be dragged about (text boxes, notes, drawings, shapes)
};

// A picture the page draws (step 6): where in the page's drawing it is, and where it shows
struct Picture {
    int index = 0;          // which of the page's pictures (in the order it draws them)
    int stream = 0;         // which of its content streams draws it
    qint64 start = 0, end = 0;  // the "/Name Do" there
    QByteArray name;        // its name in the page's resources
    int xref = 0;           // the picture itself (an object of the PDF)
    double ctm[6] = {1, 0, 0, 1, 0, 0};  // its square to the page's PDF space, where it's drawn
    QRectF rect;            // where it shows (points from the page's top left)
    std::vector<QPointF> corners;  // its corners as shown (turned: not upright)
    int width = 0, height = 0;     // its own size, in pixels
    bool textAfter = false;        // text is drawn after it (maybe over it)
    // a shape on a background saved into it (a logo on a slide's band...): its outline round it
    // (page points) - picked by it; rect is round the shape, full round the whole picture
    std::vector<QPointF> shape;
    QRectF full;
    bool clipped = false;  // (drawn clipped to its shape already: start .. end round that too)
    // cropped (the Crop tool): the part of its own square shown - (x, y, w, h), PDF's way up - and
    // its corners uncropped; null: all of it. The "/Name Do" itself, and the crop's q ... Q round it
    QRectF crop;
    std::vector<QPointF> whole;
    qint64 doStart = 0, doEnd = 0, cropStart = -1, cropEnd = -1;
    QByteArray cropMoves;  // (moves put inside the crop's q since: kept when it's cropped again)
    // not a picture but a drawing - lines, boxes, a frame (as Acrobat picks them): its painted paths,
    // each its instructions in a content stream (from its first point to its painting) and the matrix
    // there, and where it shows (page points)
    bool vector = false;
    struct Piece {
        int stream = 0;
        qint64 start = 0, end = 0;
        double ctm[6] = {1, 0, 0, 1, 0, 0};   // (the matrix where its path starts: a change put in there)
        double painted[4] = {1, 0, 0, 1};     // (the matrix it's painted with - a "cm" may come between)
        QRectF box;
        QPainterPath path;  // (its outline, page points - filled: letters drawn as shapes read from it)
        QColor fill;        // (its fill colour)
        int curves = 0;     // (how many curves it has: letters have many, boxes and lines none)
    };
    std::vector<Piece> pieces;
    std::vector<bool> whites;  // (while it's found: which pieces are filled white)
    bool letters = false;  // (a drawing of letters drawn as shapes - a label saved as outlines: read back as text)
    long order = 0;  // (where the page draws it: the pictures and drawings in that order)
};

// A link on a page: where it is, and where it goes - a web page (uri) or a page of this PDF
struct PdfLink {
    int index = 0;  // (its place in the page's list of links)
    QRectF rect;    // (points from the page's top left)
    QString uri;    // a web page (or mail...): empty for a page of this PDF
    int page = -1;  // the page it goes to (from 0)
};

// A bookmark (the PDF's table of contents): how deep, its title, where it goes (page from 0,
// -1: nowhere; at: a place on the page, null if none)
struct Bookmark {
    int level = 0;
    QString title;
    int page = -1;
    QPointF at;
};

// The page's straight lines and boxes as it paints them (for the exports: a table's rules, its cells' colours, frames):
// a filled box (a thin one: a rule), or a straight line drawn across or down - its rect as wide as it's drawn
struct LineArt {
    QRectF rect;  // (page points)
    bool filled = false;
    QColor color;
};

// Letters saved as drawn shapes (step 7: Convert to text): where they are, and their shapes
struct DrawingPart {
    int stream = 0;
    qint64 start = 0, end = 0;  // (the shape's instructions in that content stream)
};
struct Drawing {
    int index = 0;
    QRectF rect;
    QColor color;
    std::vector<DrawingPart> parts;
};

// A form field on a page (a signature box...)
struct FormField {
    int id = 0;
    QRectF rect;
    bool signature = false;
    QString name;
};

class Document {
public:
    static std::shared_ptr<Document> open(const QString &path, QString *error);
    // a new PDF with no pages yet, not saved anywhere (Create PDF from files)
    static std::shared_ptr<Document> create(QString *error);
    QByteArray snapshot();  // the PDF as it is now, as bytes (changes not saved yet too)
    ~Document();

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    QString path() const { return path_; }
    int pageCount() const;
    QSizeF pageSize(int page) const;  // in points, as shown (turned pages: turned)

    // Part of a page drawn: scale is pixels per point, area is in pixels of the whole page
    // drawn at that scale. A null image if it couldn't be drawn.
    QImage render(int page, double scale, const QRect &area);
    // ... without the letters inside these boxes (page points): what's under text being edited
    QImage renderHiding(int page, double scale, const QRect &area, const std::vector<QRectF> &hide);
    // ... drawn without the drawings the eraser takes (pen, marker, shapes): shown where it rubs, at once
    QImage renderWithoutDrawings(int page, double scale, const QRect &area);
    // a font the PDF has embedded, by its name in the page's letters (TextRun::font): its own
    // program (TrueType / OpenType...), kept as the pages' text is read - empty if none
    QByteArray fontProgram(const QString &name);

    // The page's letters in reading order (made once, then kept). Any thread.
    std::shared_ptr<const std::vector<TextChar>> text(int page);

    // ---- changes (the window's thread) ----
    bool editable() const { return pdf_ != nullptr; }  // (a PDF - not a picture or XPS)
    bool dirty() const;  // changed since it was opened or saved
    bool canUndo() const;
    bool canRedo() const;
    QString undoName() const;  // what Undo would take back ("rotate")
    QString redoName() const;
    bool undo();
    bool redo();

    bool rotatePages(const std::vector<int> &pages, int degrees, QString *error);
    bool deletePages(const std::vector<int> &pages, QString *error);
    bool movePages(const std::vector<int> &pages, int to, QString *error);  // before page `to`
    bool insertBlank(int at, QSizeF size, QString *error);
    // PDFs and pictures (a page each) put in from page `at` on: how many pages went in
    int insertFiles(const QStringList &paths, int at, QStringList *problems);
    bool extractPages(const std::vector<int> &pages, const QString &path, QString *error) const;

    // ---- annotations (each change one step for Undo) ----
    std::vector<Annot> annots(int page);  // (the window's thread)
    // a text mark over these boxes: kind "Highlight", "StrikeOut", "Underline", "Squiggly"
    bool addMarkup(int page, const QString &kind, const std::vector<QRectF> &boxes, QColor color,
                   double opacity, QString *error);
    // a pen stroke, or a marker's (marker: it multiplies with the page, like a highlighter)
    bool addInk(int page, const std::vector<QPointF> &points, QColor color, double width,
                double opacity, bool marker, QString *error);
    bool addShape(int page, const QString &kind, QPointF a, QPointF b, QColor color, double width,
                  double opacity, QString *error);  // kind: rect, ellipse, line, arrow
    bool whiteOut(int page, const QRectF &rect, QColor color, QString *error);  // in the page itself
    bool moveAnnot(int page, int id, QPointF by, QString *error);
    bool deleteAnnot(int page, int id, QString *error);
    bool recolorAnnot(int page, int id, QColor color, QString *error);
    // the text marks taken off just these boxes (what's beside them stays marked)
    bool cutMarks(int page, const std::vector<QRectF> &holes, QString *error);
    // a note's text, or a text box's (id 0: a new text box at `at`); empty text: taken off
    bool setNoteText(int page, int id, const QString &text, QString *error);
    bool setTextBox(int page, int id, QPointF at, const QString &text, double size, QColor color,
                    QString *error);
    // the eraser rubbed along path (r across): drawings lose what it went over. false: nothing
    bool rubOut(int page, const std::vector<QPointF> &path, double r, QString *error);

    // ---- the page's own text (step 5: the Edit tool, Add text) ----
    struct LineLook {  // what's changed in a line's look (else as it was)
        QString family;              // another font
        int bold = -1, italic = -1;  // 1 / 0; -1: as it was
        double size = 0;             // 0: as it was
        QColor color;                // invalid: as it was
        // (Acrobat's Format panel) - each: as it was unless set
        QString align;               // left / center / right / justify (a block's lines)
        double lineSpacing = 0;      // a block's baselines apart, in lines (1.2: 1.2 x its size)
        double paraSpacing = -1;     // extra points between a block's paragraphs
        double scale = 0;            // the letters' width, in % (100: as the font has them)
        double track = -1e9;         // more room after each letter, in 1/1000 of its size
        int rise = -9;               // 1 superscript, -1 subscript, 0 neither
        QString list;                // "bullet:<sign>", "number:<form>" (1. 1) a. A. i. I.), "none"
        double boxLeft = -1e9, boxRight = -1e9;  // its box's sides (resized: its text wrapped in it again)
        // (the paragraphs - a block's, from 0 - its alignment, spacing and list are for: those the caret
        // or the letters selected were in while typing. Empty: all of them)
        std::vector<int> paras;
        // (the block's own step from a paragraph's last baseline to the next one's first - when just some
        // of its paragraphs are written again: a paragraph made with Enter spaced as the others)
        double paraGapWas = 0;
    };
    std::vector<TextLine> lines(int page);
    using StructMark = pagetext::StructMark;
    // (the tagged PDF's paragraphs: Structure.cpp - and, if asked, the page's drawn horizontal lines
    // (a table's rows, rules), page points)
    std::vector<StructMark> structureMarks(int page, std::vector<QLineF> *rules = nullptr);
    // a line written letter by letter: each run in its own look (the rich line editor)
    struct StyledRun {
        QString text;
        QString family;
        bool bold = false, italic = false, underline = false, strike = false;
        double size = 12;
        QColor color = Qt::black;
        double scale = 100;  // its letters' width, % (100: the font's)
        double track = 0;    // more room after each letter, 1/1000 of its size
        int rise = 0;        // 1 superscript, -1 subscript
        double outline = 0;  // (bold by its outline, as the page had it: the outline's width, part of the size)
    };
    bool retypeRuns(int page, const TextLine &line, const std::vector<StyledRun> &runs, QString *error);
    bool retypeLine(int page, const TextLine &line, const QString &text, const LineLook &look, QString *error);
    // a line of a paragraph retyped: the paragraph's words flowed again in its width
    bool retypeParagraph(int page, const std::vector<TextLine> &para, int edited, const QString &text,
                         const LineLook &look, QString *error);
    bool moveLine(int page, const TextLine &line, QPointF by, QString *error);
    bool scaleLine(int page, const TextLine &line, double k, bool fromRight, QString *error);
    bool deleteLine(int page, const TextLine &line, QString *error);
    // a frame - a block of paragraphs (pagetext::frames) - moved, deleted, or retyped: its text (its
    // paragraphs one to a line of `text`) laid out again in its width, with its own line and
    // paragraph spacing, first-line indents and alignment, in its main look (or look's)
    bool moveLines(int page, const std::vector<TextLine> &lines, QPointF by, QString *error);
    bool deleteLines(int page, const std::vector<TextLine> &lines, QString *error);
    bool retypeFrame(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                     const QString &text, const LineLook &look, QString *error);
    // ... each paragraph's letters in their own looks (the rich editor's)
    bool retypeFrameRuns(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                         const std::vector<std::vector<StyledRun>> &styled, QString *error, double boxLeft = -1e9,
                         double boxRight = -1e9, double paraGapWas = 0);
    // a line turned by degrees (clockwise as shown) round its middle - or, turned already, round
    // the point it's turned round: it stays text (straight again at 0)
    // (around: the point it's turned round - a block's middle, its lines turned as one; else its own)
    bool turnLine(int page, const TextLine &line, double degrees, QString *error, const QPointF *around = nullptr);
    // a block's lines turned round one point together: all taken out first, then all written turned
    // (one by one, taking a line out would cut the letters of one turned before it that now cross it)
    bool turnLines(int page, const std::vector<TextLine> &lines, double degrees, QPointF around, QString *error);
    // new text, its first line's top at `top`: each line's box (align: left, center, right,
    // justify; dir: ltr, rtl, or "" as its letters are)
    std::vector<QRectF> addText(int page, QPointF top, const QString &text, const QString &family, double size,
                                QColor color, bool bold, bool italic, const QString &align, const QString &dir,
                                QString *error);
    // (scale: the letters' width as a part of the font's; track: points more after each letter)
    double textWidth(const QString &text, const fonts::Found &font, double size, double scale = 1, double track = 0);
    fonts::Found fontOf(const TextRun &run, const LineLook &look, const QString &text);

    // ---- pictures (step 6) ----
    std::vector<Picture> pictures(int page);
    // (drawings: where it draws curves, pictures...; hidden: its letters drawn clipped away - not seen)
    std::vector<LineArt> lineArt(int page, std::vector<QRectF> *drawings = nullptr, std::vector<QPointF> *hidden = nullptr);
    // moved / resized / turned: move is in the page's space as shown (points from its top
    // left); drawn there in the page's own drawing (what's over it stays over it)
    bool transformPicture(int page, const Picture &pic, const QTransform &move, const char *what, QString *error);
    bool deletePicture(int page, const Picture &pic, QString *error);
    bool layerPicture(int page, const Picture &pic, bool front, QString *error);  // to front / back
    // several changes made one (one Undo step: a group of objects moved, text merged): MuPDF's
    // journal takes the changes inside as parts of it
    void beginChanges(const char *what);
    void endChanges();
    int changesOpen_ = 0;  // (inside beginChanges ... endChanges: the journal can't be asked where it is)
    // one step: drawn after the next picture over it (forward) / before the one before it
    bool stepPicture(int page, const Picture &pic, bool forward, QString *error);
    // cropped to a part of its own square (x, y, w, h - PDF's way up; all of it: uncropped)
    bool cropPicture(int page, const Picture &pic, const QRectF &unit, QString *error);
    // a picture (a file, or image if it's not null) put in box - fit: its shape kept, as big
    // as fits; replacing: in that picture's place, instead of it
    bool placePicture(int page, const QString &path, const QImage &image, const QRectF &box, bool fit,
                      const Picture *replacing, QString *error);
    QImage pictureImage(const Picture &pic);  // (its own pixels)
    // a picture's text read (OCR): the picture put back without its letters (cleaned), and each line
    // written as real text where it was - its baseline's start, size, colour, as wide as it was
    struct OcrLine {
        QString text;
        QPointF baseline;  // (page points: its start)
        double size = 12, width = 0;
        QColor color = Qt::black;
        bool bold = false, serif = false;
        bool mono = false;  // (a fixed-width font: code)
        QString family;     // (the font matched to it - Scan's; empty: Arial / Times New Roman / Courier New)
        QString fontName;   // (Scan's own font made of the scan's letters - added as an embedded one; empty: family's)
    };
    bool pictureToText(int page, const Picture &pic, const QImage &cleaned, const QRectF &box, const std::vector<OcrLine> &lines,
                       QString *error);
    // letters drawn as shapes (a drawing with letters set) made real text: its shapes taken out, each
    // line written turned by degrees (clockwise as shown) round pivot - baselines as if it weren't; and
    // the pictures drawn over them with a copy of them in their pixels (PowerPoint's) put back cleaned
    bool drawingToText(int page, const Picture &pic, double degrees, QPointF pivot, const std::vector<OcrLine> &lines,
                       const std::vector<std::pair<Picture, QImage>> &cleaned, QString *error);
    // Scan (as Acrobat's Scan & OCR): a scanned page's lines read written over it as invisible text - the
    // scan as it was, its text found, picked, copied; editing a line covers the scan's letters with its paper
    // and writes it in the font matched (lines: as pictureToText's - width: from the first letter's ink to the last's)
    bool scanText(int page, const std::vector<OcrLine> &lines, QString *error);
    // ... and its pictures - a logo, a signature, a stamp - each a picture of its own over the scan (box: page
    // points; image: just its marks, see-through round them): picked, moved, deleted as a picture (Acrobat's).
    // The scan itself (background) put back as cleaned (its bytes: a JPEG) - those marks taken out of it, paper
    // where they were; no background: the scan under each covered with its paper
    struct ScanPicture {
        QRectF box;
        QImage image;
        unsigned paper = 0xFFFFFF;
    };
    bool scanPictures(int page, const std::vector<ScanPicture> &pictures, const Picture *background, const QByteArray &cleaned,
                      QString *error);
    bool seeThroughAt(const Picture &pic, QPointF at);
    // a picture's layout, as Word's (Wrap.cpp): "inline", "square", "tight", "through", "top_bottom"
    // (text flows round these), "behind", "front" (the usual: none kept). Choosing one lays the
    // page's text out round the pictures - one change; moving or resizing such a picture too.
    static bool flowing(const QString &mode);
    QString pictureLayout(int page, const Picture &pic);
    bool setPictureLayout(int page, const Picture &pic, const QString &mode, bool *pastBottom, QString *error);  // (its own see-through parts there: clicks go through)
    std::vector<FormField> fields(int page);
    // letters saved as drawn shapes (step 7), and taken out - with the lines read (text, where)
    // written in their place, real text (none: just deleted)
    std::vector<Drawing> drawings(int page);
    // links: read; added, changed, deleted (to a web page: uri; else to page toPage)
    std::vector<PdfLink> links(int page);
    bool addLink(int page, const QRectF &rect, const QString &uri, int toPage, QString *error);
    bool changeLink(int page, const PdfLink &link, const QString &uri, int toPage, QString *error);
    bool deleteLink(int page, const PdfLink &link, QString *error);
    std::vector<Bookmark> bookmarks();
    int revision() const { return revision_.load(); }  // (a new number at each change, undo, redo)
    bool removeDrawing(int page, const Drawing &d, const std::vector<std::pair<QString, QRectF>> &lines, QString *error);
    // a drawing picked up (a Picture with vector set): moved / sized / turned - each of its paths in its
    // own q ... Q with the move
    bool transformDrawing(int page, const Picture &pic, const QTransform &move, const char *what, QString *error);
    bool deleteField(int page, int id, QString *error);

    // Saved to path: written next to it first, then put in its place - so a failed save never
    // leaves a broken file. Unused parts dropped, the rest compressed (like the Python app).
    bool save(const QString &path, QString *error);

private:
    struct Geometry {
        std::vector<QSizeF> sizes;
        std::vector<QPointF> origins;
    };

    Document() = default;
    fz_context *threadContext();
    fz_display_list *keepDisplayList(int page, QPointF *origin);  // (dropped by the caller)
    std::shared_ptr<const Geometry> geometry() const;
    void measure();  // the pages' sizes again (after a change); forgets what was drawn
    bool change(const char *what, const std::function<void()> &fn, QString *error);
    int journalStep() const;
    // the page the last change / Undo / Redo touched (-1: not one page - draw them all again)
    int changedPage_ = -1, pendingPage_ = -1;
    std::vector<int> stepPage_;  // (each journal step's page)
public:
    int lastChangedPage() const { return changedPage_; }

private:
    bool onPage(int page, const char *what, const std::function<void(struct pdf_page *)> &fn,
                QString *error);
    std::vector<std::shared_ptr<std::vector<Annot>>> annots_;  // (under docMutex_)
    struct fz_font *fzFont(const fonts::Found &f);  // (under docMutex_; kept)
    std::unordered_map<std::string, struct fz_font *> fzFonts_;
    // (inside an operation, docMutex_ held; page points from the page's top left)
    void removeText(struct pdf_page *pg, int page, const QRectF &rect);
    // (while lines are taken out - moved, deleted: the letters of the other lines over them, left where they are)
    std::vector<QPointF> keepPens_;
    void keepOthers(int page, const std::vector<TextLine> &taking);
    bool takeOutLetters(struct pdf_page *pg, int page, const TextLine &line);
    bool retypeFrame(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                     const QString &text, const LineLook &look, const std::vector<std::vector<StyledRun>> *styled,
                     QString *error);
    // a line's letters out: a straight line's band, a turned line's letters one by one
    void takeOut(struct pdf_page *pg, int page, const TextLine &line);
    // (seeThrough, if given: the picture from its pixels - its colours, and its see-through parts as a mask of their own,
    // their colours kept: no dark fringe where it's drawn smooth - not from bytes)
    void placeIn(struct pdf_page *pg, int page, const QByteArray &bytes, const QRectF &r, const Picture *replacing,
                 const QImage *seeThrough = nullptr);
    void editPieces(struct pdf_page *pg, const Picture &pic,
                    const std::function<QByteArray(const Picture::Piece &, const QByteArray &)> &change);
    // what writeText writes next: turned (degrees clockwise as shown, round pivot) - 0: straight
    void setTurn(double degrees, QPointF pivot)
    {
        turnDegrees_ = degrees;
        turnPivot_ = pivot;
    }
    double turnDegrees_ = 0;
    QPointF turnPivot_;
    bool invisibleText_ = false;  // (what writeText writes next: not drawn - render mode 3, Scan's text)
    // (a scan's letters under rect covered with its paper - a little past it; exact: just rect)
    void coverText(struct pdf_page *pg, int page, const QRectF &rect, unsigned paper, bool exact = false);
    void writeText(struct pdf_page *pg, int page, QPointF baseline, const QString &text, const fonts::Found &font,
                   double size, QColor color, double scale = 1, double track = 0);

    // picture shapes (DocShapes.cpp; docMutex_ held)
    struct ShapeInfo {
        int kind = 0;  // 0 none (all picture), 1 judged against what's under it, 2 from the picture alone
        std::vector<QPointF> hull;
        QImage pieces;
    };
    QImage pictureRgb(int xref, int size);
    QImage pictureAlpha(int xref, int size);
    QImage underPicture(int page, const Picture &pic, QSize size);
    const ShapeInfo &shapeOf(int page, const Picture &pic);
    QByteArray shapeClip(int page, const Picture &pic);
    void applyShape(int page, Picture &pic);
    // (a picture clipped to its shape: its shape's outline, box and turned corners from its clip's points)
    void shapeFromClip(Picture &pic, const std::vector<QPointF> &points);
    // (its shape's outline - parts of its own width and height from its top left - framed in its own pixels and
    // put where it's shown: the frame turns with it)
    void frameFromHull(Picture &pic, const std::vector<QPointF> &hull);
    std::map<int, std::vector<QPointF>> outlines_;  // (each picture's shape found in its pixels - its own, so kept: turned, clipped, it's the same frame)
    std::vector<Picture> picturesLocked(int page);
    void layerIn(struct pdf_page *pg, const Picture &pic, bool front);  // (inside an operation)
    void behindText(struct pdf_page *pg, const Picture &pic);                            // (inside an operation)
    void drawPictureAt(struct pdf_page *pg, const Picture &pic, int stream, qint64 at);  // (inside an operation)
    // text wrapping round pictures (Wrap.cpp): what's kept in the page, the plan worked out
    // before a change (outside the lock), written in it
    struct WrapPic {
        int xref = 0;
        QRectF rect;  // (the whole picture's box)
        QString mode;
    };
    struct WrapFrame {
        double left = 0, right = 0, top = 0, bottom = 0, baseline = 0, pitch = 0;
        int align = 0;
        std::vector<QRectF> spots;  // (where its lines are now)
        // its own words and look, as it was (its lines may be pushed past the page's bottom)
        QString text, family;
        bool bold = false, italic = false, rtl = false;
        double size = 10;
        QRgb color = 0;
    };
    struct WrapState {
        std::vector<WrapPic> pics;
        std::vector<WrapFrame> frames;
    };
    struct FlowPlan {
        struct Row {
            QString text;
            QPointF at;
        };
        struct Frame {
            std::vector<QRectF> remove;
            std::vector<Row> rows;
            fonts::Found font;
            double size = 10;
            QColor color;
        };
        WrapState state;
        std::vector<Frame> frames;
        bool any = false, pastBottom = false;
    };
    WrapState readWrap(struct pdf_obj *pageobj);
    void writeWrap(struct pdf_obj *pageobj, const WrapState &s);
    WrapState wrapState(int page);
    std::vector<std::pair<double, double>> solidRows(int xref);
    FlowPlan planFlow(int page, const Picture *pic, const QRectF &newFull, const QString &mode);
    void applyFlow(struct pdf_page *pg, int page, const FlowPlan &plan);
    using ShapeKey = std::tuple<int, int, int, int, int, int>;  // (page, picture, where: rounded)
    static ShapeKey shapeKey(int page, int xref, const QRectF &r);
    void shapeMoved(int page, const Picture &pic, const QRectF &to);
    std::map<ShapeKey, ShapeInfo> shapes_;
    std::map<int, QImage> alphas_;

    QString path_;
    fz_context *ctx_ = nullptr;  // the document's own context (used under docMutex_)
    fz_document *doc_ = nullptr;
    pdf_document *pdf_ = nullptr;
    std::shared_ptr<const Geometry> geom_;
    mutable std::mutex geomMutex_;
    std::vector<fz_display_list *> lists_;
    mutable std::mutex docMutex_;
    std::atomic<int> revision_{0};
    std::mutex ctxMutex_;
    std::unordered_map<std::thread::id, fz_context *> clones_;  // a context per thread
    std::mutex textMutex_;
    QHash<QString, QByteArray> fontPrograms_;  // (under textMutex_)
    std::vector<std::shared_ptr<const std::vector<TextChar>>> texts_;
    int textGen_ = 0;  // (a new one at each change: text read before it is not kept)
    int savedStep_ = 0;  // where in the journal it was last saved
};
