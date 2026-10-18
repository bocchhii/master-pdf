#pragma once

#include "core/Fonts.h"

#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QString>

#include <memory>
#include <vector>

// A line's glyphs as the app writes it (TextEdit.cpp's shaping: right to left runs turned round, in the order
// across): each glyph's id in the font, its pen place from the line's start (size's units; y up)
struct ShapedGlyph {
    unsigned gid;
    double x, y;
    unsigned unicode;
    bool rtl;
};
std::vector<ShapedGlyph> shapeText(const QString &text, const fonts::Found &font, double size, double *width);

// Scan's custom fonts (as Acrobat's): a font made of a scan's own letters. It's the font matched to the
// scan with its glyphs' outlines - each form of each letter seen (Arabic's joined forms too) - traced
// from the scan: every copy of a glyph found on the pages cut out where the line is laid over the scan,
// fitted onto the font's own glyph, the copies averaged (noise, specks, the scan's blur evened out) and
// traced smooth (potrace). Its other glyphs are the font's own, made as heavy as the scan's ink. Its
// shaping, spacing, joining are the font's: text written in it lies as it did, in the scan's look.
namespace scanfont {

class Builder {
public:
    explicit Builder(const fonts::Found &base);
    ~Builder();
    bool ok() const;
    const fonts::Found &base() const;
    // a line of the scan (grey: the page read, its pixels): its text written in the base font from origin
    // (its baseline's start, pixels), em pixels to the em, as wide as advance pixels; paper / ink: its
    // greys there
    // (returns how like the font's glyphs the scan's are, on the whole - 0..1: little, a scan not in this font's shapes)
    double addLine(const QImage &grey, const QString &text, QPointF origin, double em, double advance, int paper, int ink);
    // a word of a scan whose letters aren't this font's shapes (a letterhead's display Arabic): the word set in
    // the font laid over the scan's elastically - its columns matched (dynamic time warping) - and cut into its
    // glyphs where they fall, each glyph's piece and width the scan's (box: the word's, pixels; baseline: its y)
    // (its baseline and size measured on the scan - resize: the size used, else em's; measured: just its size asked
    // for, nothing added)
    void addWord(const QImage &grey, const QString &word, const QRect &box, double baseline, double em, int paper, int ink,
                 double *measured = nullptr, bool resize = true);
    double wordScale(const QImage &grey, const QString &word, const QRect &box, double em, int paper, int ink);
    // the font file (a TrueType font named name - "ArialBold" is made "MPScan1a2b3c-ArialBold"), bold as
    // the scan's; empty if too few letters were seen to make it worth it. traced: how many glyphs are the scan's
    QByteArray build(const QString &name, bool bold, int *traced, QString *psName);
    // (false: the font's own shapes for its right-to-left glyphs - Arabic's, joined to their neighbours - made as heavy and
    // as soft-edged as the scan's letters are, measured on the copies; not traced from them: their noise, a join or a dot
    // the scan lost, not copied into them. Latin's, standing alone, still copied as the scan has them)
    void setCopyShapes(bool copy);
    // (after build: how like the font's own glyphs the glyphs cut from words were - 0..1; -1: none cut. Little: the
    // scan's typeface isn't this font - a font of the scan's own letters is what looks like it)
    double likeness() const;
    double measureLikeness();  // (... measured before building it: whether it's worth building)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace scanfont
