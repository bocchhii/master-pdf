#pragma once

#include <QLineF>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <vector>

// One letter of a page's text, in reading order. box: in points from the page's top left
// corner, as tall as its line (so a selection is even).
struct TextChar {
    QRectF box;
    char32_t c = 0;
    float size = 0;  // the font size
    float scale = 1;  // its letters made wider / narrower (1: as the font has them)
    float advance = 0;  // its width along its line
    int line = 0;    // which line it's on (a number for each line of the page)
    bool straight = true;  // on a straight (not turned) line
    // what it's written in (for retyping it): its font's name, colour, where its baseline
    // starts, the text block it's in, and flags: bold 1, italic 2, serif 4, mono 8, outlined 16 (filled and
    // stroked - a regular font drawn bold), invisible 32 (text render mode 3: a scan's text read - the scan shows
    // the letters; paper: the scan's colour round them)
    QString font;
    unsigned color = 0;
    QPointF origin;
    int block = 0;
    int flags = 0;
    float outline = 0;  // (outlined - flags 16: the outline's width, part of its size)
    unsigned paper = 0xFFFFFF;  // (invisible - flags 32: the scan's paper under it)
    // on a turned line (not straight across): the turn - degrees clockwise as shown, round a
    // point - and the letter straightened (its box and baseline as if not turned: how it's
    // edited), and its outline as shown (to take it out exactly)
    float angle = 0;
    QPointF pivot;
    QRectF sbox;
    QPointF sorigin;
    QPolygonF quad;
};

// A piece of a line in one look
struct TextRun {
    QString text;
    QString font;
    int flags = 0;
    double size = 12;
    unsigned color = 0;
    QPointF origin;  // its baseline's start (as written)
    QRectF box;
    double scale = 100;  // its letters' width, % of the font's
    double track = 0;    // room after each letter past its own width, 1/1000 of its size
    double outline = 0;  // (bold by its outline on the page - flags 16: the outline's width, part of its size)
    unsigned paper = 0xFFFFFF;  // (a scan's invisible text - flags 32: the paper under it)
};

// A line of the page that can be retyped (the Edit tool's): its letters in reading order
struct TextLine {
    QRectF rect;
    QString text;
    std::vector<TextRun> runs;
    bool rtl = false;   // mostly right to left (Arabic...)
    int block = 0;
    double baseline = 0;
    // a turned line (the PDF's own, or turned with the Edit tool): rect, runs and baseline are as
    // if straight; shown turned angle degrees clockwise round pivot; its letters' outlines as shown
    double angle = 0;
    QPointF pivot;
    std::vector<QPolygonF> quads;
    // (straight: each letter's pen on the page, and the letter - just its own letters taken out, not others over them)
    std::vector<std::pair<QPointF, char32_t>> pens;
    QPolygonF shown() const;  // (its box's corners as shown: turned)
    const TextRun &mainRun() const;  // the run with the most letters (its look is the line's)
};

// A place some words were found: on page `page`, rect (in points from its top left), and
// the line round it to show in the results: before, the words, after.
struct FindHit {
    int page = 0;
    QRectF rect;
    QString before, words, after;
};

// What letters were picked: one box per line (to tint), and their text.
struct TextPick {
    std::vector<QRectF> boxes;
    QString text;
};

class QImage;
class QRect;

namespace pagetext {

// the paper's colour in part of a picture (a scan) - its lighter half's mean: what's round the letters (0xRRGGBB)
unsigned paperIn(const QImage &image, const QRect &area);
// ... and its ink's colour: its strokes' middles (0: no ink in it - black)
unsigned inkIn(const QImage &image, const QRect &area);

bool isRtl(char32_t c);     // Hebrew, Arabic...
bool isArabic(char32_t c);

// Unicode's bidi (its rules for a line, much as Qt lays out text): each letter's type, then its level - 0, 2 left to
// right, 1 right to left - in a line right to left or not; and the order the levels give (Unicode's reordering: the same
// both ways - reading order to the order across, and back)
enum class Bidi { L, R, AN, EN, CS, NI };  // (a Latin letter; Arabic's; Arabic's digits; 0-9; a sign between digits; the rest)
Bidi bidiType(char32_t c);
std::vector<int> bidiLevels(std::vector<Bidi> types, bool rtl);
std::vector<size_t> bidiOrder(const std::vector<int> &levels);

// Right-to-left text put in reading order, and the spaces a PDF leaves out put in -
// see the Python app's mend_rtl and gap_spaces. lines: each line's letters as MuPDF gives
// them; returns the page's letters in reading order.
std::vector<TextChar> mend(std::vector<std::vector<TextChar>> lines);

// Every place query is on the page, as you'd read them (top to bottom; a line left to
// right, or right to left if it's Arabic). Case, accents as typed, joined letter shapes and
// ligatures don't matter.
std::vector<FindHit> find(const std::vector<TextChar> &chars, int page, const QString &query);

// Where a point falls between the letters: before letter k (k), or after the last. -1 if no
// letter is within `near` points (near < 0: any distance).
int caret(const std::vector<TextChar> &chars, QPointF p, double near = -1);

// Letters lo to hi (not including hi)
TextPick letters(const std::vector<TextChar> &chars, int lo, int hi);

// The letters from point a to b, like dragging in a text editor (empty if a isn't near text)
TextPick pick(const std::vector<TextChar> &chars, QPointF a, QPointF b);

// The word (or whole line) at a point; empty if there's no letter there
TextPick around(const std::vector<TextChar> &chars, QPointF p, bool line);

// The letter (not a space) at a point, or -1
int charAt(const std::vector<TextChar> &chars, QPointF p);

// The lines that can be retyped (straight ones), cut where there's a gap far wider than a
// space (a tab, a table's columns)
std::vector<TextLine> lines(const std::vector<TextChar> &chars);
// A frame - a text box, as iLovePDF's (the Python app's group_frames): lines one under the other,
// lined up (left, centre or right), the same size, spaced as its lines are (a bigger gap - a
// heading's, a paragraph's spacing - starts another). Then its paragraphs (split_paragraphs):
// a first-line indent, a bigger gap, another size, a line that ended though the next one's first
// word would have fitted on it, or a bold label starting one. Lines by their indexes, top down.
struct TextFrame {
    std::vector<int> lines;
    QRectF rect;
    std::vector<std::vector<int>> paras;
    QRectF shown;         // (its outline, if wider than its lines: the markers beside them - "1", "*")
    bool hidden = false;  // (a marker shown inside another's outline: no outline of its own)
};
std::vector<TextFrame> frames(const std::vector<TextLine> &lines);
int frameOf(const std::vector<TextFrame> &frames, int line);
// the blocks grouped as Acrobat boxes a page's text (shown round them - editing keeps to the blocks)
struct TextBox {
    std::vector<int> frames;
    QRectF rect;
    std::vector<std::vector<int>> paras;  // (its paragraphs' lines, top down - what's edited as one)
    QRectF shown;         // (its outline as Acrobat draws it - with the markers beside its lines; null: rect)
    bool hidden = false;  // (a marker - "1", "*" - beside another box's line: shown in its outline)
};
// (the PDF's own structure, if it's tagged: each letter's paragraph element and whether it's in a
// table cell - Document::structureMarks)
struct StructMark {
    QPointF at;   // (a letter's origin, page points)
    int para;     // (its paragraph element - -1 none)
    bool cell;    // (inside a table cell)
};
// (rules: the page's drawn horizontal lines - text isn't boxed together across one: a table's rows)
std::vector<TextBox> boxes(const std::vector<TextLine> &lines, const std::vector<TextFrame> &frames,
                           const std::vector<StructMark> &marks = {}, const std::vector<QLineF> &rules = {});  // (its frame's index, or -1)
// how a paragraph (its lines, top down) is aligned in a block from left to right: "left", "center",
// "right" or "justify" (lines that start at one place of their own - indented bullets - and reach the
// right edge: justified or left, not right)
QString alignOf(const std::vector<TextLine> &lines, const std::vector<int> &para, double left, double right);
// The lines of the paragraph this one is in (in its frame): their indexes, top to bottom
std::vector<int> paragraph(const std::vector<TextLine> &lines, int index);

}  // namespace pagetext
