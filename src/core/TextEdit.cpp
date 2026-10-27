// Document's own text changed (the Edit tool, Add text): letters taken out of the page and
// written again - shaped by HarfBuzz (Arabic joined up and right to left, ligatures, kerning)
// and put in the page by MuPDF's PDF writer, which embeds the font.
// (No C++ objects are made inside fz_try blocks or the functions run in them.)

#include "core/Document.h"
#include "core/PageText.h"
#include "core/ScanFont.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <hb.h>

#include <QRegularExpression>
#include <QFile>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

// ---- shaping ----

struct HbFont {
    hb_blob_t *blob = nullptr;
    hb_face_t *face = nullptr;
    hb_font_t *font = nullptr;
    double upem = 1000;
    double ascender = 0.9, descender = -0.2;  // per point
};

std::mutex g_hbMutex;
QHash<QString, HbFont> g_hb;

HbFont *hbFont(const fonts::Found &f)
{
    const QString k = f.path + QLatin1Char('#') + QString::number(f.index);
    auto it = g_hb.find(k);
    if (it != g_hb.end())
        return &*it;
    HbFont h;
    h.blob = hb_blob_create_from_file(f.path.toUtf8().constData());
    h.face = hb_face_create(h.blob, unsigned(f.index));
    h.font = hb_font_create(h.face);
    h.upem = std::max(1u, hb_face_get_upem(h.face));
    hb_font_set_scale(h.font, int(h.upem), int(h.upem));
    hb_font_extents_t ext;
    if (hb_font_get_h_extents(h.font, &ext)) {
        h.ascender = ext.ascender / h.upem;
        h.descender = ext.descender / h.upem;
    }
    return &*g_hb.insert(k, h);
}

struct Glyph {
    unsigned gid;
    double x, y;  // from the line's start, in points (y: up)
    unsigned unicode;
    bool rtl;
    QString text;  // (the letters it draws: its cluster's, on its first glyph - a joined pair's glyph both, a mark's own glyph none)
};

// (Arabic-Indic digits - and Persian's: numbers, laid out left to right in Arabic text as any digits are)
bool arabicDigit(uint c)
{
    return (c >= 0x0660 && c <= 0x066C) || (c >= 0x06F0 && c <= 0x06F9);
}

bool strongRtl(uint c)
{
    return !arabicDigit(c) && ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF));
}

bool strongLtr(uint c)
{
    return !strongRtl(c) && (QChar::isLetter(c) || QChar::isDigit(c));
}

// The text laid out: its letters' glyphs in order across (right to left runs turned round,
// the line's runs in the order they're read), and how wide it is
std::vector<Glyph> shape(const QString &text, const fonts::Found &font, double size, double *width, bool *lineRtl,
                         double scale = 1, double track = 0)
{
    // (scale: the letters made wider / narrower; track: points more after each letter)
    std::lock_guard guard(g_hbMutex);
    std::vector<Glyph> out;
    *width = 0;
    HbFont *h = hbFont(font);
    const QList<uint> u = text.toUcs4();
    // the line's direction: its first letter's; runs of one direction (spaces and signs go with the run
    // they're in when the next letter is that way too - between two ways, the line's: "١٤٤٧/٤/٢١ هـ")
    // (a letter's - a number first, "١٢ جهاز", doesn't make an Arabic line left to right; just digits: as they are)
    bool base = false, found = false;
    for (uint c : u)
        if (strongRtl(c) || (strongLtr(c) && !QChar::isDigit(c))) {
            base = strongRtl(c);
            found = true;
            break;
        }
    // (no letter: Arabic's digits an Arabic text's - right to left - "١٢٩ " a run of an Arabic line rewritten alone, its space
    // after it on its left; else the first digit's way)
    for (uint c : u)
        if (!found && ((c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9))) {
            base = true;
            found = true;
        }
    for (uint c : u)
        if (!found && (strongRtl(c) || strongLtr(c))) {
            base = strongRtl(c);
            break;
        }
    if (lineRtl)
        *lineRtl = base;
    struct Run {
        int start, len;
        bool rtl;
    };
    // (as Unicode's bidi lays a line out - as the editor, Qt, shows it, and as it's read back: a number left to right
    // inside, numbers side by side right to left in a right-to-left line - "٦١٤٧١ ☎ ٢٤١٨٠٠٥ ٠١٧" -, Arabic's numbers in
    // a left-to-right one too; one sign between digits the number's: "١٤٤٧/٤/٢١")
    std::vector<pagetext::Bidi> types;
    for (uint c : u)
        types.push_back(pagetext::bidiType(c));
    const std::vector<int> levels = pagetext::bidiLevels(types, base);
    const std::vector<size_t> order = pagetext::bidiOrder(levels);
    // (the runs in the order across: letters next to each other, one level - a right-to-left run's the other way round)
    std::vector<Run> runs;
    for (size_t p = 0; p < order.size();) {
        const size_t first = order[p];
        const bool odd = levels[first] % 2 == 1;
        size_t q = p + 1;
        while (q < order.size() && levels[order[q]] == levels[first] && order[q] == (odd ? order[q - 1] - 1 : order[q - 1] + 1))
            ++q;
        const size_t lo = odd ? order[q - 1] : first;
        runs.push_back({int(lo), int(q - p), odd});
        p = q;
    }
    const double k = size / h->upem;
    double x = 0;
    for (const Run &r : runs) {
        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf32(buf, reinterpret_cast<const uint32_t *>(u.constData()), int(u.size()), unsigned(r.start), r.len);
        hb_buffer_set_direction(buf, r.rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        // (left to right: no "fi" / "ff" ligatures - MuPDF's PDF device takes a glyph's text from the
        // font, so "benefit" would read back as "beneﬁt" and Find / copying would miss it)
        static const hb_feature_t noLiga[] = {{HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u}, {HB_TAG('c', 'l', 'i', 'g'), 0, 0, ~0u}};
        // (right to left: no ligatures a font may leave out - Calibri's "في", "شر": a glyph of two letters is read back
        // with its letters either way round. Arabic's own, lam-alef, kept - read back by its one character)
        static const hb_feature_t noExtraLiga[] = {{HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u}, {HB_TAG('d', 'l', 'i', 'g'), 0, 0, ~0u}};
        hb_shape(h->font, buf, r.rtl ? noExtraLiga : noLiga, 2);
        unsigned n = 0;
        const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
        const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
        std::vector<unsigned> starts;  // (its clusters: where each one's letters start)
        for (unsigned g = 0; g < n; ++g)
            starts.push_back(info[g].cluster);
        std::sort(starts.begin(), starts.end());
        QSet<unsigned> named;
        for (unsigned g = 0; g < n; ++g) {
            const unsigned cl = info[g].cluster;
            QString letters;
            if (!named.contains(cl)) {
                named.insert(cl);
                const auto next = std::upper_bound(starts.begin(), starts.end(), cl);
                const unsigned end = std::min(next != starts.end() ? *next : unsigned(r.start + r.len), unsigned(u.size()));
                for (unsigned c = cl; c < end; ++c)
                    letters += QString::fromUcs4(reinterpret_cast<const char32_t *>(&u[int(c)]), 1);
                // (lam-alef: its one character - read back as its two letters, in order; another glyph of more than one
                // letter, right to left: in the order it's seen - read back turned round, as the line is)
                static const QHash<QString, QChar> lamAlef = {{QStringLiteral("لا"), QChar(0xFEFB)}, {QStringLiteral("لأ"), QChar(0xFEF7)},
                                                              {QStringLiteral("لإ"), QChar(0xFEF9)}, {QStringLiteral("لآ"), QChar(0xFEF5)}};
                if (r.rtl && lamAlef.contains(letters))
                    letters = lamAlef.value(letters);
                else if (r.rtl && letters.size() > 1)
                    std::reverse(letters.begin(), letters.end());
            }
            out.push_back({info[g].codepoint, x + pos[g].x_offset * k * scale, pos[g].y_offset * k,
                           cl < unsigned(u.size()) ? u[int(cl)] : 0u, r.rtl, letters});
            x += pos[g].x_advance * k * scale;
            if (g + 1 == n || info[g + 1].cluster != cl)  // (after each letter - a joined pair's last glyph)
                x += track;
        }
        hb_buffer_destroy(buf);
    }
    *width = x;
    return out;
}

void appendContent(fz_context *ctx, pdf_document *doc, pdf_obj *pageobj, fz_buffer *drawing)
{
    // what was there kept in q ... Q, the new drawing after it
    fz_buffer *start = nullptr, *end = nullptr;
    pdf_obj *arr = nullptr;
    fz_var(start);
    fz_var(end);
    fz_var(arr);
    fz_try(ctx) {
        start = fz_new_buffer(ctx, 4);
        fz_append_string(ctx, start, "q\n");
        end = fz_new_buffer(ctx, fz_buffer_storage(ctx, drawing, nullptr) + 8);
        fz_append_string(ctx, end, "\nQ\n");
        fz_append_buffer(ctx, end, drawing);
        pdf_obj *old = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
        arr = pdf_new_array(ctx, doc, 4);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, start, nullptr, 0));
        if (pdf_is_array(ctx, old))
            for (int i = 0; i < pdf_array_len(ctx, old); ++i)
                pdf_array_push(ctx, arr, pdf_array_get(ctx, old, i));
        else if (old)
            pdf_array_push(ctx, arr, old);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, end, nullptr, 0));
        pdf_dict_put(ctx, pageobj, PDF_NAME(Contents), arr);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, start);
        fz_drop_buffer(ctx, end);
        pdf_drop_obj(ctx, arr);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// the page's own resources (a copy put on the page if it shares its parent's)
pdf_obj *ownResources(fz_context *ctx, pdf_document *doc, pdf_obj *pageobj)
{
    pdf_obj *res = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
    if (res)
        return res;
    pdf_obj *inherited = pdf_dict_get_inheritable(ctx, pageobj, PDF_NAME(Resources));
    if (inherited) {
        pdf_obj *copy = pdf_deep_copy_obj(ctx, inherited);
        pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), copy);
    } else {
        pdf_dict_put_dict(ctx, pageobj, PDF_NAME(Resources), 2);
    }
    return pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
}

// superscript / subscript: its size (a part of the text's), how far up its baseline goes
constexpr double kRiseSize = 0.65;
double riseOf(int rise, double size)
{
    return rise > 0 ? 0.33 * size : rise < 0 ? -0.14 * size : 0.0;
}

QString collapse(const QString &s)
{
    QString out = s;
    out.replace(QChar(0xA0), QLatin1Char(' '));
    return out.simplified();
}

// the fonts a piece just written draws in (Type0, Identity-H: a code is its glyph's id) - each glyph it shows read back as
// the letters it draws. MuPDF makes a font's ToUnicode from its cmap: an Arabic letter's joined form reached only through
// the font's shaping (Simplified Arabic's, Calibri's, a scan's own font's) isn't in it, and read back as nothing - its
// word lost to Find, copying, the Edit tool. (A glyph the cmap names stays as it names it; a second glyph of one letter -
// a mark drawn apart - a zero-width space, left out as the page's text is read)
void mapGlyphs(fz_context *ctx, pdf_document *doc, pdf_obj *res, const QByteArray &drawn, const std::vector<Glyph> &glyphs)
{
    static const QRegularExpression tf(QStringLiteral("/([^\\s/\\[\\]<>()]+)\\s+[-+.\\d]+\\s+Tf"));
    QList<QByteArray> names;
    for (auto it = tf.globalMatch(QString::fromLatin1(drawn)); it.hasNext();) {
        const QByteArray n = it.next().captured(1).toLatin1();
        if (!names.contains(n))
            names << n;
    }
    for (const QByteArray &name : names) {
        pdf_obj *font = nullptr, *tu = nullptr;
        fz_buffer *b = nullptr;
        bool type0 = false, ok = true;
        fz_var(b);
        fz_try(ctx) {
            font = pdf_dict_gets(ctx, pdf_dict_get(ctx, res, PDF_NAME(Font)), name.constData());
            type0 = pdf_name_eq(ctx, pdf_dict_get(ctx, font, PDF_NAME(Subtype)), PDF_NAME(Type0)) &&
                    pdf_name_eq(ctx, pdf_dict_get(ctx, font, PDF_NAME(Encoding)), PDF_NAME(Identity_H));
            tu = pdf_dict_get(ctx, font, PDF_NAME(ToUnicode));
            if (type0 && pdf_is_stream(ctx, tu))
                b = pdf_load_stream(ctx, tu);
        }
        fz_catch(ctx) {
            ok = false;
        }
        QString old;
        if (b) {
            unsigned char *data = nullptr;
            const size_t len = fz_buffer_storage(ctx, b, &data);
            old = QString::fromLatin1(reinterpret_cast<const char *>(data), qsizetype(len));
            fz_drop_buffer(ctx, b);
        }
        if (!ok || !type0)
            continue;
        // (what it maps now: its bfchar pairs, its bfranges - from a start, or a list)
        std::map<unsigned, QString> map;
        auto utf16 = [](const QString &hex) {
            QString out;
            for (int i = 0; i + 4 <= hex.size(); i += 4)
                out += QChar(char16_t(hex.mid(i, 4).toUInt(nullptr, 16)));
            return out;
        };
        static const QRegularExpression part(QStringLiteral("begin(bfchar|bfrange)(.*?)end\\1"), QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression hex(QStringLiteral("<([0-9A-Fa-f]*)>|\\[|\\]"));
        for (auto it = part.globalMatch(old); it.hasNext();) {
            const auto m = it.next();
            QStringList tok;
            for (auto h = hex.globalMatch(m.captured(2)); h.hasNext();) {
                const auto t = h.next();
                tok << (t.captured(0).startsWith(QLatin1Char('<')) ? t.captured(1) : t.captured(0));
            }
            if (m.captured(1) == QLatin1String("bfchar")) {
                for (int i = 0; i + 1 < tok.size(); i += 2)
                    map[tok[i].toUInt(nullptr, 16)] = utf16(tok[i + 1]);
                continue;
            }
            for (int i = 0; i + 2 < tok.size();) {
                const unsigned lo = tok[i].toUInt(nullptr, 16), hi = tok[i + 1].toUInt(nullptr, 16);
                if (tok[i + 2] == QLatin1String("[")) {
                    int j = i + 3;
                    for (unsigned c = lo; j < tok.size() && tok[j] != QLatin1String("]"); ++j, ++c)
                        map[c] = utf16(tok[j]);
                    i = j + 1;
                } else {
                    QString dst = utf16(tok[i + 2]);
                    for (unsigned c = lo; c <= hi && !dst.isEmpty() && hi - lo < 65536; ++c) {
                        map[c] = dst;
                        dst.back() = QChar(char16_t(dst.back().unicode() + 1));
                    }
                    i += 3;
                }
            }
        }
        bool added = false;
        for (int pass = 0; pass < 2; ++pass)  // (letters first: a glyph drawn alone somewhere named by them)
            for (const Glyph &g : glyphs)
                if ((pass == 0) != g.text.isEmpty() && !map.count(g.gid) && g.gid > 0) {
                    map[g.gid] = g.text.isEmpty() ? QString(QChar(0x200B)) : g.text;
                    added = true;
                }
        if (!added)
            continue;
        QByteArray cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                          "/CIDSystemInfo <</Registry(Adobe)/Ordering(UCS)/Supplement 0>> def\n"
                          "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        const std::vector<std::pair<unsigned, QString>> all(map.begin(), map.end());
        for (size_t at = 0; at < all.size(); at += 100) {
            const size_t n = std::min<size_t>(100, all.size() - at);
            cmap += QByteArray::number(qulonglong(n)) + " beginbfchar\n";
            for (size_t k = at; k < at + n; ++k) {
                QByteArray dst;
                for (const QChar c : all[k].second)
                    dst += QByteArray::number(c.unicode(), 16).rightJustified(4, '0');
                cmap += "<" + QByteArray::number(all[k].first, 16).rightJustified(4, '0') + "> <" + dst + ">\n";
            }
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        fz_buffer *nb = nullptr;
        fz_var(nb);
        fz_try(ctx) {
            nb = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char *>(cmap.constData()), size_t(cmap.size()));
            if (pdf_is_stream(ctx, tu))
                pdf_update_stream(ctx, doc, tu, nb, 0);
            else
                pdf_dict_put_drop(ctx, font, PDF_NAME(ToUnicode), pdf_add_stream(ctx, doc, nb, nullptr, 0));
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, nb);
        }
        fz_catch(ctx) {
        }
    }
}

}  // namespace

// ---- fonts ----

fz_font *Document::fzFont(const fonts::Found &f)
{
    const std::string k = (f.path + QLatin1Char('#') + QString::number(f.index)).toStdString();
    auto it = fzFonts_.find(k);
    if (it != fzFonts_.end())
        return it->second;
    fz_font *font = nullptr;
    const QByteArray path = f.path.toUtf8();
    fz_try(ctx_) {
        font = fz_new_font_from_file(ctx_, nullptr, path.constData(), f.index, 0);
    }
    fz_catch(ctx_) {
        font = nullptr;
    }
    fzFonts_[k] = font;
    return font;
}

std::vector<ShapedGlyph> shapeText(const QString &text, const fonts::Found &font, double size, double *width)
{
    std::vector<ShapedGlyph> out;
    *width = 0;
    if (font.path.isEmpty())
        return out;
    for (const Glyph &g : shape(text, font, size, width, nullptr))
        out.push_back({g.gid, g.x, g.y, g.unicode, g.rtl});
    return out;
}

double Document::textWidth(const QString &text, const fonts::Found &font, double size, double scale, double track)
{
    if (font.path.isEmpty())
        return text.size() * (size * 0.5 * scale + track);
    double w = 0;
    shape(text, font, size, &w, nullptr, scale, track);
    return w;
}

// (a styled run bold by its outline - as the page had it: written so again, as thin)
fonts::Found withOutline(fonts::Found f, double outline)
{
    if (outline > 0 && f.bold && !f.realBold)
        f.outline = outline;
    return f;
}

fonts::Found Document::fontOf(const TextRun &run, const LineLook &look, const QString &text)
{
    fonts::Found f = fonts::findFont(run.font, run.flags);
    if (!look.family.isEmpty() || look.bold >= 0 || look.italic >= 0) {
        const bool bold = look.bold >= 0 ? look.bold == 1 : f.bold;
        const bool italic = look.italic >= 0 ? look.italic == 1 : f.italic;
        f = fonts::findFamily(look.family.isEmpty() ? f.family : look.family, bold, italic);
    }
    if ((run.flags & 16) && run.outline > 0 && f.bold && !f.realBold)  // (bold by its outline on the page: as thin again)
        f.outline = run.outline;
    return fonts::forText(f, text);
}

// ---- taking letters out, and writing them ----

void Document::removeText(pdf_page *pg, int page, const QRectF &rect)
{
    // just the middle of the line: the lines above and below aren't touched
    const QPointF o = geometry()->origins.at(size_t(page));
    const fz_rect r = fz_make_rect(float(rect.left() + o.x()), float(rect.top() + rect.height() * 0.3 + o.y()),
                                   float(rect.right() + o.x()), float(rect.bottom() - rect.height() * 0.3 + o.y()));
    pdf_annot *a = pdf_create_annot(ctx_, pg, PDF_ANNOT_REDACT);
    pdf_set_annot_rect(ctx_, a, r);
    pdf_drop_annot(ctx_, a);
    pdf_redact_options opts = {};
    opts.black_boxes = 0;
    opts.image_method = PDF_REDACT_IMAGE_NONE;
    opts.line_art = PDF_REDACT_LINE_ART_NONE;
    opts.text = PDF_REDACT_TEXT_REMOVE;
    pdf_redact_page(ctx_, pdf_, pg, &opts);
}

void Document::coverText(pdf_page *pg, int page, const QRectF &rect, unsigned paper, bool exact)
{
    // the paper's colour over the box - a little past it: the scan's letters' ink reaches past the text's box
    const QPointF o = geometry()->origins.at(size_t(page));
    const double h = rect.height();
    const QRectF r = (exact ? rect : rect.adjusted(-0.25 * h, -0.08 * h, 0.25 * h, 0.08 * h)).translated(o);
    fz_matrix ctm;
    pdf_page_transform(ctx_, pg, nullptr, &ctm);
    const fz_rect pr = fz_transform_rect(fz_make_rect(float(r.left()), float(r.top()), float(r.right()), float(r.bottom())),
                                         fz_invert_matrix(ctm));
    char op[200];
    fz_snprintf(op, sizeof op, "q %g %g %g rg %g %g %g %g re f Q\n", ((paper >> 16) & 255) / 255.0, ((paper >> 8) & 255) / 255.0,
                (paper & 255) / 255.0, pr.x0, pr.y0, pr.x1 - pr.x0, pr.y1 - pr.y0);
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(op), strlen(op));
    fz_try(ctx_) {
        appendContent(ctx_, pdf_, pg->obj, buf);
    }
    fz_always(ctx_) {
        fz_drop_buffer(ctx_, buf);
    }
    fz_catch(ctx_) {
        fz_rethrow(ctx_);
    }
}

// a straight line's own letters taken out of the page's content - on its runs' baselines, as big, inside their boxes:
// not the other text lying over it or under it (an area taken out took those too). None found: false
bool Document::takeOutLetters(pdf_page *pg, int page, const TextLine &line)
{
    struct Mine {
        const TextLine *line;
        fz_matrix page;  // (PDF space -> MuPDF's page space)
        QPointF o;
        int taken = 0;
    };
    Mine mine{&line, fz_identity, geometry()->origins.at(size_t(page))};
    pdf_page_transform(ctx_, pg, nullptr, &mine.page);
    pdf_sanitize_filter_options sopts = {};
    sopts.opaque = &mine;
    sopts.text_filter = [](fz_context *, void *opaque, int *ucs, int ucsLen, fz_matrix trm, fz_matrix ctm, fz_rect) -> int {
        Mine &w = *static_cast<Mine *>(opaque);
        const fz_matrix m = fz_concat(fz_concat(trm, ctm), w.page);
        if (std::abs(m.b) > 0.01 * std::abs(m.a) + 1e-6)
            return 0;  // (turned: not this line's)
        const QPointF at = QPointF(m.e, m.f) - w.o;
        const double size = std::sqrt(std::abs(double(m.a) * m.d - double(m.b) * m.c));
        if (!w.line->pens.empty()) {  // (its letters' own pens, as read: exactly these)
            for (const auto &[pen, c] : w.line->pens)
                if (std::abs(pen.x() - at.x()) < 0.35 && std::abs(pen.y() - at.y()) < 0.35 &&
                    (ucsLen < 1 || char32_t(ucs[0]) == c || c == 0xFFFD || ucs[0] == 0xFFFD)) {
                    ++w.taken;
                    return 1;
                }
            return 0;
        }
        for (const TextRun &r : w.line->runs)
            if (std::abs(at.y() - r.origin.y()) <= std::max(0.6, 0.15 * r.size) && std::abs(size - r.size) <= 0.3 * r.size &&
                at.x() >= r.box.left() - 1 && at.x() <= r.box.right() + 0.5) {
                ++w.taken;
                return 1;
            }
        return 0;
    };
    pdf_filter_factory list[2] = {{pdf_new_sanitize_filter, &sopts}, {nullptr, nullptr}};
    pdf_filter_options opts = {};
    opts.recurse = 1;
    opts.filters = list;
    pdf_filter_page_contents(ctx_, pdf_, pg, &opts);
    return mine.taken > 0;
}

void Document::takeOut(pdf_page *pg, int page, const TextLine &line)
{
    // a scan's text (not drawn - Scan's): the scan's letters under it covered too, with the paper round them
    for (const TextRun &r : line.runs)
        if (r.flags & 32) {
            coverText(pg, page, line.rect, r.paper);
            break;
        }
    if (std::abs(line.angle) < 0.01 && takeOutLetters(pg, page, line))
        return;
    if (std::abs(line.angle) < 0.01 || line.quads.empty()) {
        removeText(pg, page, line.rect);
        return;
    }
    // turned: just its letters - turned as it is, their pens inside its box - taken out of the page's
    // content (a redaction's spots took the letters under them too: text it was turned over)
    struct Out {
        QTransform un;   // (page points -> as if straight)
        QRectF box;
        double angle;
        fz_matrix page;  // (PDF space -> MuPDF's page space)
        QPointF o;
    };
    QTransform t;
    t.translate(line.pivot.x(), line.pivot.y());
    t.rotate(line.angle);
    t.translate(-line.pivot.x(), -line.pivot.y());
    Out out{t.inverted(), line.rect.adjusted(-1, -1, 1, 1), line.angle, fz_identity, geometry()->origins.at(size_t(page))};
    pdf_page_transform(ctx_, pg, nullptr, &out.page);
    pdf_sanitize_filter_options sopts = {};
    sopts.opaque = &out;
    sopts.text_filter = [](fz_context *, void *opaque, int *, int, fz_matrix trm, fz_matrix ctm, fz_rect) -> int {
        const Out &w = *static_cast<const Out *>(opaque);
        const fz_matrix m = fz_concat(fz_concat(trm, ctm), w.page);
        const double a = std::atan2(double(m.b), double(m.a)) * 180 / M_PI;
        if (std::abs(std::fmod(a - w.angle + 540.0, 360.0) - 180.0) > 0.6)
            return 0;  // (turned another way - straight text it lies over: kept)
        return w.box.contains(w.un.map(QPointF(m.e, m.f) - w.o)) ? 1 : 0;
    };
    pdf_filter_factory list[2] = {{pdf_new_sanitize_filter, &sopts}, {nullptr, nullptr}};
    pdf_filter_options opts = {};
    opts.recurse = 1;
    opts.filters = list;
    pdf_filter_page_contents(ctx_, pdf_, pg, &opts);
}

void Document::writeText(pdf_page *pg, int page, QPointF baseline, const QString &text, const fonts::Found &font,
                         double size, QColor color, double scale, double track)
{
    if (text.trimmed().isEmpty())
        return;
    fz_font *ff = font.path.isEmpty() ? nullptr : fzFont(font);
    if (!ff)
        ff = fz_new_base14_font(ctx_, "Helvetica");
    else
        fz_keep_font(ctx_, ff);
    double width = 0;
    std::vector<Glyph> glyphs;
    if (!font.path.isEmpty())
        glyphs = shape(text, font, size, &width, nullptr, scale, track);
    const QPointF o = geometry()->origins.at(size_t(page));
    const float x0 = float(baseline.x() + o.x()), y0 = float(baseline.y() + o.y());
    // made bold / slanted here when the font has no bold / italic of its own
    const bool fakeBold = font.bold && !font.realBold;
    const bool fakeItalic = font.italic && !font.realItalic;
    const float s = float(size);
    const float rgb[3] = {float(color.redF()), float(color.greenF()), float(color.blueF())};
    // (turned: each letter turned round the turn's point - it's still text)
    const bool turned = std::abs(turnDegrees_) > 0.01;
    const float cx = float(turnPivot_.x() + o.x()), cy = float(turnPivot_.y() + o.y());
    const fz_matrix turn = fz_concat(fz_concat(fz_translate(-cx, -cy), fz_rotate(float(turnDegrees_))), fz_translate(cx, cy));
    fz_text *t = nullptr;
    fz_device *dev = nullptr;
    fz_buffer *contents = nullptr;
    fz_stroke_state *stroke = nullptr;
    // (made out here: a C++ object isn't made inside fz_try - see the renaming below)
    std::vector<std::pair<std::string, int>> before[2];
    struct Renamed {
        int kind;
        std::string name;
        int was, now;
    };
    std::vector<Renamed> renamed;
    QByteArray drawn;
    fz_var(t);
    fz_var(dev);
    fz_var(contents);
    fz_var(stroke);
    fz_try(ctx_) {
        t = fz_new_text(ctx_);
        if (glyphs.empty()) {  // (no font file: PDF's own Helvetica, letter by letter)
            float x = x0;
            for (uint c : text.toUcs4()) {
                const int gid = fz_encode_character(ctx_, ff, int(c));
                fz_matrix trm = {s * float(scale), 0, fakeItalic ? 0.2f * s : 0, -s, x, y0};
                if (turned)
                    trm = fz_concat(trm, turn);
                fz_show_glyph(ctx_, t, ff, trm, gid, int(c), 0, 0, FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                x += fz_advance_glyph(ctx_, ff, gid, 0) * s * float(scale) + float(track);
            }
        } else {
            for (const Glyph &g : glyphs) {
                fz_matrix trm = {s * float(scale), 0, fakeItalic ? 0.2f * s : 0, -s, x0 + float(g.x), y0 - float(g.y)};
                if (turned)
                    trm = fz_concat(trm, turn);
                fz_show_glyph(ctx_, t, ff, trm, int(g.gid), int(g.unicode), 0, g.rtl ? 1 : 0,
                              g.rtl ? FZ_BIDI_RTL : FZ_BIDI_LTR, FZ_LANG_UNSET);
            }
        }
        // drawn into a content stream of the page's own (fonts into its resources)
        fz_rect mediabox;
        fz_matrix ctm;
        pdf_page_transform(ctx_, pg, &mediabox, &ctm);
        pdf_obj *res = ownResources(ctx_, pdf_, pg->obj);
        // (what the page's fonts and graphics states are named now: MuPDF's PDF device names its
        // own from F0 / GS0 again each time - a piece written after another would take the
        // other's names, and the other's letters would be drawn in its font)
        for (int k = 0; k < 2; ++k) {
            pdf_obj *dict = pdf_dict_get(ctx_, res, k == 0 ? PDF_NAME(Font) : PDF_NAME(ExtGState));
            for (int i = 0; i < pdf_dict_len(ctx_, dict); ++i)
                before[k].emplace_back(pdf_to_name(ctx_, pdf_dict_get_key(ctx_, dict, i)), pdf_to_num(ctx_, pdf_dict_get_val(ctx_, dict, i)));
        }
        contents = fz_new_buffer(ctx_, 256);
        dev = pdf_new_pdf_device(ctx_, pdf_, fz_invert_matrix(ctm), res, contents);
        fz_fill_text(ctx_, dev, t, fz_identity, fz_device_rgb(ctx_), rgb, 1, fz_default_color_params);
        if (fakeBold && !invisibleText_) {  // thickened: the letters' outlines drawn round them too
            stroke = fz_new_stroke_state(ctx_);
            stroke->linewidth = float(font.outline > 0 ? font.outline : 0.035) * s;  // (as the page had it, if it did)
            fz_stroke_text(ctx_, dev, t, stroke, fz_identity, fz_device_rgb(ctx_), rgb, 1, fz_default_color_params);
        }
        fz_close_device(ctx_, dev);
        // the names it gave: made the piece's own (MPf1, MPg1...), what they named before put back
        unsigned char *bytes = nullptr;
        const size_t len = fz_buffer_storage(ctx_, contents, &bytes);
        drawn = QByteArray(reinterpret_cast<const char *>(bytes), qsizetype(len));
        for (int k = 0; k < 2; ++k) {
            pdf_obj *dict = pdf_dict_get(ctx_, res, k == 0 ? PDF_NAME(Font) : PDF_NAME(ExtGState));
            for (int i = 0; i < pdf_dict_len(ctx_, dict); ++i) {
                const std::string name = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, dict, i));
                const int num = pdf_to_num(ctx_, pdf_dict_get_val(ctx_, dict, i));
                int was = -1;
                for (const auto &[n, v] : before[k])
                    if (n == name)
                        was = v;
                if (was == num)
                    continue;
                renamed.push_back({k, name, was, num});
            }
        }
        for (const Renamed &r : renamed) {
            pdf_obj *dict = pdf_dict_get(ctx_, res, r.kind == 0 ? PDF_NAME(Font) : PDF_NAME(ExtGState));
            char fresh[32];
            int n = 1;
            do
                fz_snprintf(fresh, sizeof fresh, r.kind == 0 ? "MPf%d" : "MPg%d", n++);
            while (pdf_dict_gets(ctx_, dict, fresh));
            pdf_dict_puts(ctx_, dict, fresh, pdf_dict_gets(ctx_, dict, r.name.c_str()));
            if (r.was > 0)
                pdf_dict_puts_drop(ctx_, dict, r.name.c_str(), pdf_new_indirect(ctx_, pdf_, r.was, 0));
            else
                pdf_dict_dels(ctx_, dict, r.name.c_str());
            // ("/F0" as a whole name in the piece's drawing: not "/F01")
            const QByteArray from = "/" + QByteArray(r.name.c_str()), to = "/" + QByteArray(fresh);
            for (qsizetype at = drawn.indexOf(from); at >= 0; at = drawn.indexOf(from, at + to.size())) {
                const qsizetype end = at + from.size();
                const char next = end < drawn.size() ? drawn[end] : ' ';
                if (QByteArray(" \n\r\t/[<(").contains(next))
                    drawn.replace(at, from.size(), to);
                else
                    at += from.size() - to.size();
            }
        }
        if (invisibleText_)  // (a scan's text read: there, not drawn - render mode 3, kept to this piece)
            drawn = "q 3 Tr\n" + drawn.replace(" 0 Tr", " 3 Tr").replace("\n0 Tr", "\n3 Tr") + "\nQ\n";
        fz_drop_buffer(ctx_, contents);
        contents = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(drawn.constData()),
                                                  size_t(drawn.size()));
        appendContent(ctx_, pdf_, pg->obj, contents);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        fz_drop_text(ctx_, t);
        fz_drop_buffer(ctx_, contents);
        fz_drop_stroke_state(ctx_, stroke);
        fz_drop_font(ctx_, ff);
    }
    fz_catch(ctx_) {
        fz_rethrow(ctx_);
    }
    if (!glyphs.empty())  // (its letters read back as written - every glyph's)
        mapGlyphs(ctx_, pdf_, ownResources(ctx_, pdf_, pg->obj), drawn, glyphs);
}

// ---- the Edit tool's changes ----

std::vector<TextLine> Document::lines(int page)
{
    return pagetext::lines(*text(page));
}

bool Document::retypeLine(int page, const TextLine &line, const QString &textIn, const LineLook &look, QString *error)
{
    const QString text = collapse(textIn);
    const TextRun &main = line.mainRun();
    const fonts::Found font = fontOf(main, look, text);
    const double size = look.size > 0 ? look.size : main.size;
    const QColor color = look.color.isValid() ? look.color : QColor::fromRgb(main.color);
    // (superscript / subscript, all of it: smaller, its baseline up / down)
    const int rise = look.rise > -9 ? look.rise : 0;
    const double base = rise ? size : 0;
    const double written = rise ? size * kRiseSize : size;
    // (its letters' width and spacing: as set - else as the page has them)
    const double scale = look.scale > 0 ? look.scale / 100 : main.scale / 100,
                 track = look.track > -1e8 ? look.track / 1000 * written : main.track / 1000 * written;
    const double w = textWidth(text, font, written, scale, track);
    // (right to left: it ends where it ended; else it starts where it started)
    const double x = line.rtl ? line.rect.right() - w : main.origin.x() - (main.box.left() - line.rect.left());
    std::lock_guard guard(docMutex_);
    return onPage(page, "change text", [&](pdf_page *pg) {
        takeOut(pg, page, line);
        setTurn(line.angle, line.pivot);  // (turned: written turned as it was)
        writeText(pg, page, QPointF(x, line.baseline - riseOf(rise, base)), text, font, written, color, scale, track);
        setTurn(0, {});
    }, error);
}

bool Document::retypeRuns(int page, const TextLine &line, const std::vector<StyledRun> &runsIn, QString *error)
{
    // each run in its own font, size and colour, one after another from where the line started
    // (right to left: from where it ended, leftwards); underline / strike-out as marks over the
    // run - the line's old marks of those kinds go first
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double width;
        bool underline, strike;
        double scale = 1, track = 0;
    };
    std::vector<Piece> pieces;
    const TextRun &main = line.mainRun();
    double x = line.rtl ? line.rect.right() : main.origin.x() - (main.box.left() - line.rect.left());
    for (const StyledRun &r : runsIn) {
        if (r.text.isEmpty())
            continue;
        // (superscript / subscript: smaller, raised / lowered - as Word and Acrobat)
        const double size = r.rise ? r.size * kRiseSize : r.size;
        Piece p{r.text, fonts::forText(withOutline(fonts::findFamily(r.family, r.bold, r.italic), r.outline), r.text), size, r.color, {}, 0,
                r.underline, r.strike, r.scale > 0 ? r.scale / 100 : 1, r.track / 1000 * size};
        p.width = textWidth(r.text, p.font, size, p.scale, p.track);
        if (line.rtl)
            x -= p.width;
        p.at = QPointF(x, line.baseline - riseOf(r.rise, r.size));
        if (!line.rtl)
            x += p.width;
        pieces.push_back(p);
    }
    const QPointF o = geometry()->origins.at(size_t(page));
    const QRectF old = line.rect.adjusted(-1, -1, 1, 1);
    std::lock_guard guard(docMutex_);
    return onPage(page, "change text", [&](pdf_page *pg) {
        // the line's own underline / strike-out marks (those on it alone) out
        for (pdf_annot *a = pdf_first_annot(ctx_, pg); a;) {
            pdf_annot *next = pdf_next_annot(ctx_, a);
            const enum pdf_annot_type type = pdf_annot_type(ctx_, a);
            if (type == PDF_ANNOT_UNDERLINE || type == PDF_ANNOT_STRIKE_OUT) {
                const fz_rect r = pdf_bound_annot(ctx_, a);
                if (old.contains(QRectF(QPointF(r.x0 - o.x(), r.y0 - o.y()), QPointF(r.x1 - o.x(), r.y1 - o.y()))))
                    pdf_delete_annot(ctx_, pg, a);
            }
            a = next;
        }
        takeOut(pg, page, line);
        setTurn(line.angle, line.pivot);
        for (const Piece &p : pieces) {
            if (p.text.trimmed().isEmpty())
                continue;
            writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
            for (int k = 0; k < 2; ++k) {
                if (!(k == 0 ? p.underline : p.strike) || std::abs(line.angle) > 0.01)  // (turned: no marks)
                    continue;
                const fz_rect box = fz_make_rect(float(p.at.x() + o.x()), float(p.at.y() - 0.8 * p.size + o.y()),
                                                 float(p.at.x() + p.width + o.x()), float(p.at.y() + 0.25 * p.size + o.y()));
                pdf_annot *a = pdf_create_annot(ctx_, pg, k == 0 ? PDF_ANNOT_UNDERLINE : PDF_ANNOT_STRIKE_OUT);
                fz_try(ctx_) {
                    const float c[3] = {float(p.color.redF()), float(p.color.greenF()), float(p.color.blueF())};
                    pdf_add_annot_quad_point(ctx_, a, fz_quad_from_rect(box));
                    pdf_set_annot_color(ctx_, a, 3, c);
                    pdf_update_annot(ctx_, a);
                }
                fz_always(ctx_) {
                    pdf_drop_annot(ctx_, a);
                }
                fz_catch(ctx_) {
                    fz_rethrow(ctx_);
                }
            }
        }
        setTurn(0, {});
    }, error);
}

bool Document::moveLine(int page, const TextLine &line, QPointF by, QString *error)
{
    // each piece in its own font, size and colour, where it was - moved
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double scale = 1, track = 0;  // (its letters' width and spacing, as the page has them)
    };
    std::vector<Piece> pieces;
    for (const TextRun &r : line.runs) {
        const QString t = r.text;
        if (t.trimmed().isEmpty())
            continue;
        Piece p{t, fontOf(r, {}, t), r.size, QColor::fromRgb(r.color), {}};
        p.scale = r.scale / 100;
        p.track = r.track / 1000 * p.size;
        bool rtl = false;
        double w = 0;
        if (!p.font.path.isEmpty())
            shape(t, p.font, r.size, &w, &rtl, p.scale, p.track);
        p.at = QPointF((rtl ? r.box.right() - w : r.origin.x()) + by.x(), r.origin.y() + by.y());
        pieces.push_back(p);
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "move", [&](pdf_page *pg) {
        takeOut(pg, page, line);
        setTurn(line.angle, line.pivot + by);  // (turned: its turn's point moved with it)
        for (const Piece &p : pieces)
            writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
        setTurn(0, {});
    }, error);
}

bool Document::scaleLine(int page, const TextLine &line, double k, bool fromRight, QString *error)
{
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double scale = 1, track = 0;  // (its letters' width and spacing, as the page has them)
    };
    std::vector<Piece> pieces;
    const double anchor = fromRight ? line.rect.right() : line.rect.left();
    for (const TextRun &r : line.runs) {
        if (r.text.trimmed().isEmpty())
            continue;
        Piece p{r.text, fontOf(r, {}, r.text), r.size * k, QColor::fromRgb(r.color), {}};
        p.scale = r.scale / 100;
        p.track = r.track / 1000 * p.size;
        bool rtl = false;
        double w = 0;
        if (!p.font.path.isEmpty())
            shape(r.text, p.font, p.size, &w, &rtl);
        const double start = rtl ? anchor + (r.box.right() - anchor) * k - w : anchor + (r.origin.x() - anchor) * k;
        p.at = QPointF(start, r.origin.y());
        pieces.push_back(p);
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "resize text", [&](pdf_page *pg) {
        takeOut(pg, page, line);
        setTurn(line.angle, line.pivot);
        for (const Piece &p : pieces)
            writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
        setTurn(0, {});
    }, error);
}

bool Document::deleteLine(int page, const TextLine &line, QString *error)
{
    std::lock_guard guard(docMutex_);
    return onPage(page, "delete text", [&](pdf_page *pg) { takeOut(pg, page, line); }, error);
}

bool Document::turnLines(int page, const std::vector<TextLine> &lines, double degrees, QPointF around, QString *error)
{
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double to;
        double scale = 1, track = 0;  // (its letters' width and spacing, as the page has them)
    };
    // (lines turned already - each straightened round its own point: from where each piece is shown,
    // turned round the block's middle as shown, all of them round that one point - one block)
    auto shownBy = [](double angle, QPointF pivot) {
        QTransform t;
        t.translate(pivot.x(), pivot.y());
        t.rotate(angle);
        t.translate(-pivot.x(), -pivot.y());
        return t;
    };
    bool turned = false;
    QRectF shownBox;
    for (const TextLine &line : lines) {
        turned = turned || std::abs(line.angle) > 0.01;
        shownBox |= line.shown().boundingRect();
    }
    if (turned)
        around = shownBox.center();
    std::vector<Piece> pieces;
    for (const TextLine &line : lines) {
        double to = std::fmod(line.angle + degrees + 540.0, 360.0) - 180.0;
        if (std::abs(to) < 0.05)
            to = 0;
        // (a straight point of this line -> the straight point of its new place, round `around`)
        const QTransform move = turned ? shownBy(line.angle, line.pivot) * shownBy(degrees, around) * shownBy(to, around).inverted()
                                       : QTransform();
        for (const TextRun &r : line.runs) {
            if (r.text.trimmed().isEmpty())
                continue;
            Piece p{r.text, fontOf(r, {}, r.text), r.size, QColor::fromRgb(r.color), {}, to};
            p.scale = r.scale / 100;
            p.track = r.track / 1000 * p.size;
            bool rtl = false;
            double w = 0;
            if (!p.font.path.isEmpty())
                shape(r.text, p.font, r.size, &w, &rtl, p.scale, p.track);
            p.at = move.map(QPointF(rtl ? r.box.right() - w : r.origin.x(), r.origin.y()));
            pieces.push_back(p);
        }
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "rotate text", [&](pdf_page *pg) {
        for (const TextLine &line : lines)
            takeOut(pg, page, line);
        for (const Piece &p : pieces) {
            setTurn(p.to, around);
            writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
        }
        setTurn(0, {});
    }, error);
}

bool Document::turnLine(int page, const TextLine &line, double degrees, QString *error, const QPointF *around)
{
    // each piece as it is, in its own font, size and colour - turned
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double scale = 1, track = 0;  // (its letters' width and spacing, as the page has them)
    };
    std::vector<Piece> pieces;
    for (const TextRun &r : line.runs) {
        if (r.text.trimmed().isEmpty())
            continue;
        Piece p{r.text, fontOf(r, {}, r.text), r.size, QColor::fromRgb(r.color), {}};
        p.scale = r.scale / 100;
        p.track = r.track / 1000 * p.size;
        bool rtl = false;
        double w = 0;
        if (!p.font.path.isEmpty())
            shape(r.text, p.font, r.size, &w, &rtl);
        p.at = QPointF(rtl ? r.box.right() - w : r.origin.x(), r.origin.y());
        pieces.push_back(p);
    }
    double to = std::fmod(line.angle + degrees + 540.0, 360.0) - 180.0;  // (-180 .. 180)
    if (std::abs(to) < 0.05)
        to = 0;
    const QPointF pivot = around ? *around : std::abs(line.angle) > 0.01 ? line.pivot : line.rect.center();
    std::lock_guard guard(docMutex_);
    return onPage(page, "rotate text", [&](pdf_page *pg) {
        takeOut(pg, page, line);
        setTurn(to, pivot);
        for (const Piece &p : pieces)
            writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
        setTurn(0, {});
    }, error);
}

bool Document::moveLines(int page, const std::vector<TextLine> &lines, QPointF by, QString *error)
{
    // each line's pieces in their own font, size and colour, where they were - moved
    struct Piece {
        QString text;
        fonts::Found font;
        double size;
        QColor color;
        QPointF at;
        double scale = 1;  // (a stand-in font wider than the page's: narrowed to the room it had)
        double track = 0;  // (its letter spacing, points)
    };
    std::vector<std::vector<Piece>> all;
    for (const TextLine &line : lines) {
        std::vector<Piece> pieces;
        for (size_t i = 0; i < line.runs.size(); ++i) {
            const TextRun &r = line.runs[i];
            if (r.text.trimmed().isEmpty())
                continue;
            Piece p{r.text, fontOf(r, {}, r.text), r.size, QColor::fromRgb(r.color), {}};
            p.scale = r.scale / 100;
            p.track = r.track / 1000 * p.size;
            bool rtl = false;
            double w = 0;
            if (!p.font.path.isEmpty())
                shape(r.text, p.font, r.size, &w, &rtl, p.scale, p.track);
            // (its room: to where the next piece starts - else its own box)
            const double room = !rtl && i + 1 < line.runs.size() && line.runs[i + 1].origin.x() > r.origin.x()
                                    ? line.runs[i + 1].origin.x() - r.origin.x()
                                    : r.box.width();
            if (!rtl && w > room + 0.2 && room > 0)
                p.scale *= std::max(0.8, room / w);
            p.at = QPointF((rtl ? r.box.right() - w : r.origin.x()) + by.x(), r.origin.y() + by.y());
            pieces.push_back(p);
        }
        all.push_back(pieces);
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "move", [&](pdf_page *pg) {
        for (const TextLine &line : lines)
            takeOut(pg, page, line);
        for (size_t k = 0; k < lines.size(); ++k) {
            setTurn(lines[k].angle, lines[k].pivot + by);
            for (const Piece &p : all[k])
                writeText(pg, page, p.at, p.text, p.font, p.size, p.color, p.scale, p.track);
        }
        setTurn(0, {});
    }, error);
}

bool Document::deleteLines(int page, const std::vector<TextLine> &lines, QString *error)
{
    std::lock_guard guard(docMutex_);
    return onPage(page, "delete text", [&](pdf_page *pg) {
        for (const TextLine &line : lines)
            takeOut(pg, page, line);
    }, error);
}

bool Document::retypeFrame(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                           const QString &textIn, const LineLook &look, QString *error)
{
    return retypeFrame(page, lines, paras, textIn, look, nullptr, error);
}

bool Document::retypeFrameRuns(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                               const std::vector<std::vector<StyledRun>> &styled, QString *error, double boxLeft, double boxRight,
                               double paraGapWas)
{
    QStringList texts;
    for (const std::vector<StyledRun> &para : styled) {
        QString t;
        for (const StyledRun &r : para)
            t += r.text;
        texts << t;
    }
    LineLook box;  // (its block's sides - when just its paragraphs from one on are written again)
    box.boxLeft = boxLeft;
    box.boxRight = boxRight;
    box.paraGapWas = paraGapWas;
    return retypeFrame(page, lines, paras, texts.join(QLatin1Char('\n')), box, &styled, error);
}

bool Document::retypeFrame(int page, const std::vector<TextLine> &lines, const std::vector<std::vector<int>> &paras,
                           const QString &textIn, const LineLook &look, const std::vector<std::vector<StyledRun>> *styled,
                           QString *error)
{
    if (lines.empty())
        return false;
    // its look: its main line's main run's (the line with the most letters)
    size_t mainLine = 0;
    for (size_t k = 0; k < lines.size(); ++k)
        if (lines[k].text.size() > lines[mainLine].text.size())
            mainLine = k;
    const TextRun &main = lines[mainLine].mainRun();
    QStringList paraTexts;
    for (const QString &p : textIn.split(QLatin1Char('\n')))
        paraTexts << collapse(p);
    const fonts::Found font = fontOf(main, look, paraTexts.join(QLatin1Char(' ')));
    const double size = look.size > 0 ? look.size : main.size;
    const QColor color = look.color.isValid() ? look.color : QColor::fromRgb(main.color);
    // its frame: as wide as its widest line
    double left = 1e9, right = -1e9;
    for (const TextLine &l : lines) {
        left = std::min(left, l.rect.left());
        right = std::max(right, l.rect.right());
    }
    double room = std::max(10.0, right - left);
    const double k = main.size > 0 ? size / main.size : 1;  // (another size: its spacing with it)
    // its spacing (the Python app's frame_geometry): baselines in a paragraph, from a paragraph
    // to the next, each paragraph's first-line indent
    std::vector<double> pitches, gaps, indents, rests;  // (rests: its lines after the first - their own indent)
    for (size_t n = 0; n < paras.size(); ++n) {
        const std::vector<int> &p = paras[n];
        if (p.size() > 1) {
            const TextLine &second = lines[size_t(p[1])];
            rests.push_back(second.rtl ? std::max(0.0, right - second.rect.right()) : std::max(0.0, second.rect.left() - left));
        } else {
            rests.push_back(-1);  // (one line: as the block's others - below)
        }
        for (size_t m = 1; m < p.size(); ++m)
            pitches.push_back(lines[size_t(p[m])].baseline - lines[size_t(p[m - 1])].baseline);
        if (n > 0 && !paras[n - 1].empty() && !p.empty())
            gaps.push_back(lines[size_t(p.front())].baseline - lines[size_t(paras[n - 1].back())].baseline);
        const TextLine &first = lines[size_t(p.front())];
        indents.push_back(first.rtl ? std::max(0.0, right - first.rect.right()) : std::max(0.0, first.rect.left() - left));
    }
    auto median = [](std::vector<double> v, double otherwise) {
        if (v.empty())
            return otherwise;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // (a paragraph of one line: its lines after the first - once it wraps - as the block's others' are: at
    // the block's side if it indents first lines, under its first if its paragraphs are indented whole)
    {
        std::vector<double> known;
        for (double r : rests)
            if (r >= 0)
                known.push_back(r);
        const double usual = median(known, -1);
        for (size_t n = 0; n < rests.size(); ++n)
            if (rests[n] < 0)
                rests[n] = usual >= 0 ? std::min(usual, indents[n]) : indents[n];
    }
    // (line spacing: every baseline's step - a paragraph's extra room on top of it, as it was unless set)
    const double wasLeading = median(pitches, main.size * 1.2) * k;
    const double wasGap = median(gaps, look.paraGapWas > 0 ? look.paraGapWas : wasLeading / k * 1.5) * k;
    const double leading = look.lineSpacing > 0 ? look.lineSpacing * size : wasLeading;
    const double leadingAll = leading;
    const double paraGap = look.lineSpacing <= 0 && look.paraSpacing < 0 ? wasGap  // (as it was)
                           : leading + (look.paraSpacing >= 0 ? look.paraSpacing : std::max(0.0, wasGap - wasLeading));
    // how its lines lined up: justified, centred, right or left
    // (justified: a paragraph's lines but its last as wide as the frame - so at least one such line)
    bool justified = false, notJustified = false, centered = true, rightAligned = true, leftAligned = true;
    for (const std::vector<int> &p : paras)
        for (size_t m = 0; m < p.size(); ++m) {
            const QRectF &r = lines[size_t(p[m])].rect;
            const bool lastOfPara = m + 1 == p.size();
            if (!lastOfPara) {
                if (std::abs(r.right() - right) > 2 || (m > 0 && std::abs(r.left() - left) > 2))
                    notJustified = true;
                else
                    justified = true;
            } else if (p.size() > 1 && (lines[size_t(p[m])].rtl ? std::abs(r.right() - right) > 2 : std::abs(r.left() - left) > 2)) {
                notJustified = true;  // (its last line not at its start: centred / right - not justified)
            }
            centered = centered && std::abs(r.center().x() - (left + right) / 2) <= 2;
            rightAligned = rightAligned && std::abs(r.right() - right) <= 2;
            leftAligned = leftAligned && std::abs(r.left() - left) <= 2;
        }
    justified = justified && !notJustified;
    const bool rtl = lines[mainLine].rtl;
    // (its box resized: as wide as it is now - its lines wrapped again in it)
    const double wasLeft = left;
    const QString blockAlign = !look.align.isEmpty() ? look.align
                               : justified ? "justify"
                               : (centered && !leftAligned && !rightAligned && lines.size() > 1) ? "center"
                               : (rtl || (rightAligned && !leftAligned)) ? "right" : "left";
    // each paragraph as it was (a block may hold a title, a byline, body text - as Acrobat's): its
    // own alignment, its own line spacing, its own gap above it (unless the panel sets them)
    std::vector<QString> paraAligns;
    std::vector<double> paraLeads, paraGaps;
    // (the paragraphs the panel's alignment, spacing and list are for - all, or those picked while typing)
    auto chosen = [&](size_t n) { return look.paras.empty() || std::find(look.paras.begin(), look.paras.end(), int(n)) != look.paras.end(); };
    for (size_t n = 0; n < paras.size(); ++n) {
        const std::vector<int> &p = paras[n];
        QString a = blockAlign;
        if ((look.align.isEmpty() || !chosen(n)) && !p.empty()) {
            a = pagetext::alignOf(lines, p, left, right);
        }
        paraAligns.push_back(a);
        std::vector<double> own;
        const double psize = p.empty() ? main.size : lines[size_t(p.front())].mainRun().size;
        for (size_t m = 1; m < p.size(); ++m) {  // (pieces of one line read apart - no step: left out)
            const double step = lines[size_t(p[m])].baseline - lines[size_t(p[m - 1])].baseline;
            if (step > 0.5 * psize)
                own.push_back(step);
        }
        const double own0 = own.empty() ? median(pitches, main.size * 1.2) / std::max(0.1, main.size) * psize : median(own, 0);
        paraLeads.push_back(look.lineSpacing > 0 && chosen(n) ? leading : own0 * k);
        if (n > 0 && !paras[n - 1].empty() && !p.empty())
            paraGaps.push_back((look.lineSpacing > 0 || look.paraSpacing >= 0) && (chosen(n) || chosen(n - 1))  // (round the ones picked)
                                   ? paraGap
                                   : (lines[size_t(p.front())].baseline - lines[size_t(paras[n - 1].back())].baseline) * k);
        else
            paraGaps.push_back(paraGap);
    }
    if (look.boxLeft > -1e8 || look.boxRight > -1e8) {
        left = look.boxLeft > -1e8 ? look.boxLeft : left;
        right = look.boxRight > -1e8 ? look.boxRight : right;
        room = std::max(10.0, right - left);
        for (double &in : indents)  // (its first-line indents: as they were, from its left side)
            in = std::min(in, room / 2);
        for (double &in : rests)
            in = std::min(in, room / 2);
    }
    (void)wasLeft;
    // (its alignment chosen: the paragraphs it's for lose their indents - normal paragraphs, at the box's
    // side; a list's hanging indent comes from its sign, and stays)
    if (!look.align.isEmpty())
        for (size_t n = 0; n < indents.size(); ++n)
            if (look.paras.empty() || std::find(look.paras.begin(), look.paras.end(), int(n)) != look.paras.end()) {
                indents[n] = 0;
                if (n < rests.size())
                    rests[n] = 0;
            }
    // each word in its own look (its font, bold / italic, size, colour), as the Python app keeps a
    // frame's runs: the words kept, matched in order to the paragraph's words as they were, keep
    // theirs; words typed in take the look of the word before them. The text bar's look (look)
    // over all of them.
    struct Look {
        fonts::Found font;
        double size;
        QColor color;
        double space;
        bool underline = false, strike = false;  // (marks over it: the rich editor's)
        double scale = 1, track = 0;              // (its letters' width, points more after each)
        int rise = 0;                             // (superscript / subscript)
    };
    std::vector<Look> looks;
    // (the panel's letter width / spacing, over every word's)
    auto formed = [&](Look &l) {
        if (look.rise > -9 && look.rise != l.rise) {  // (superscript / subscript, all of it)
            l.size = look.rise ? l.size * kRiseSize : l.size / kRiseSize;
            l.rise = look.rise;
        }
        if (look.scale > 0)
            l.scale = look.scale / 100;
        if (look.track > -1e8)
            l.track = look.track / 1000 * l.size;
        l.space = textWidth(QStringLiteral(" "), l.font, l.size, l.scale, l.track);
    };
    auto lookOf = [&](const TextRun &r, const QString &text) {
        Look l{fontOf(r, look, text), look.size > 0 ? look.size : r.size, look.color.isValid() ? look.color : QColor::fromRgb(r.color), 0};
        l.scale = r.scale / 100;  // (its letters' width and spacing as the page has them - unless the panel sets them)
        l.track = r.track / 1000 * l.size;
        formed(l);
        looks.push_back(l);
        return int(looks.size()) - 1;
    };
    const int mainLook = lookOf(main, paraTexts.join(QLatin1Char(' ')));
    looks[size_t(mainLook)].font = font;
    looks[size_t(mainLook)].size = size;
    looks[size_t(mainLook)].color = color;
    formed(looks[size_t(mainLook)]);
    // the paragraphs' words as they were, each with its look (its first letter's run's)
    std::vector<std::vector<std::pair<QString, int>>> was(paras.size());
    for (size_t n = 0; n < paras.size(); ++n)
        for (int li : paras[n]) {
            bool open = false;  // (a word going on into the next run - a bold label's comma...)
            for (const TextRun &r : lines[size_t(li)].runs) {
                int runLook = -1;
                for (const QChar ch : r.text) {
                    if (ch.isSpace()) {
                        open = false;
                        continue;
                    }
                    if (open) {
                        was[n].back().first += ch;
                    } else {
                        if (runLook < 0)
                            runLook = lookOf(r, r.text);
                        was[n].emplace_back(QString(ch), runLook);
                        open = true;
                    }
                }
            }
        }
    struct Word {
        QString text;
        int look;
        double width;
        bool glued = false;  // (no space before it: a word's piece in another look - a label's colon)
        bool alone = false;  // (written on its own: a list's sign, its text lined up after it)
    };
    std::vector<std::vector<Word>> words(size_t(paraTexts.size()));
    if (styled) {  // (from the rich editor: each letter's look as it was set there)
        for (size_t n = 0; n < styled->size() && n < words.size(); ++n) {
            bool open = false;
            for (const StyledRun &r : (*styled)[n]) {
                int runLook = -1;
                for (const QChar ch : r.text) {
                    if (ch.isSpace()) {
                        open = false;
                        continue;
                    }
                    if (runLook < 0) {
                        const double sz = (r.size > 0 ? r.size : size) * (r.rise ? kRiseSize : 1);
                        Look l{fonts::forText(withOutline(fonts::findFamily(r.family, r.bold, r.italic), r.outline), r.text), sz, r.color, 0,
                               r.underline, r.strike, r.scale > 0 ? r.scale / 100 : 1, r.track / 1000 * sz, r.rise};
                        l.space = textWidth(QStringLiteral(" "), l.font, l.size, l.scale, l.track);
                        looks.push_back(l);
                        runLook = int(looks.size()) - 1;
                    }
                    if (open && words[n].back().look == runLook) {
                        words[n].back().text += ch;
                        continue;
                    }
                    words[n].push_back({QString(ch), runLook, 0, open});  // (open: glued to the piece before)
                    open = true;
                }
            }
            for (Word &w : words[n]) {
                const Look &lk = looks[size_t(w.look)];
                w.width = textWidth(w.text, lk.font, lk.size, lk.scale, lk.track);
            }
        }
    }
    for (int n = 0; n < int(paraTexts.size()) && !styled; ++n) {
        const QStringList now = paraTexts[n].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const std::vector<std::pair<QString, int>> &old = n < int(was.size()) ? was[size_t(n)] : std::vector<std::pair<QString, int>>{};
        // (the longest run of words in common, in order: those keep their looks)
        const size_t A = old.size(), B = size_t(now.size());
        std::vector<std::vector<int>> L(A + 1, std::vector<int>(B + 1, 0));
        for (size_t i = A; i-- > 0;)
            for (size_t j = B; j-- > 0;)
                L[i][j] = old[i].first == now[int(j)] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
        std::vector<int> lookOfNew(B, -1);
        for (size_t i = 0, j = 0; i < A && j < B;) {
            if (old[i].first == now[int(j)]) {
                lookOfNew[j] = old[i].second;
                ++i;
                ++j;
            } else if (L[i + 1][j] >= L[i][j + 1]) {
                ++i;
            } else {
                ++j;
            }
        }
        int last = old.empty() ? mainLook : old.front().second;
        for (size_t j = 0; j < B; ++j) {  // (typed in: as the word before it)
            if (lookOfNew[j] < 0)
                lookOfNew[j] = last;
            last = lookOfNew[j];
            const Look &lk = looks[size_t(lookOfNew[j])];
            words[size_t(n)].push_back({now[int(j)], lookOfNew[j], textWidth(now[int(j)], lk.font, lk.size, lk.scale, lk.track)});
        }
    }
    // a list (the panel's bullets / numbers): each paragraph's own sign or number taken off, the
    // new one put first; its lines after the first start under its text (a hanging indent)
    std::vector<double> hangs(words.size(), 0.0);
    {
        auto isSign = [](const QString &w) {
            static const QRegularExpression number(QStringLiteral("^(\\d{1,3}|[a-zA-Z]|[ivxlcIVXLC]{1,5})[.)]$"));
            static const QStringList signs = {"\u2022", "\u25E6", "\u25AA", "\u25A0", "\u2013", "-", "\u27A2", "\u2713", "\u25CF", "\u25CB"};
            return signs.contains(w) || number.match(w).hasMatch();
        };
        // (each paragraph's: a sign at its start, and its words' room after it - kept, if no list asked)
        int listed = 0;  // (the paragraphs numbered so far)
        for (size_t n = 0; n < words.size(); ++n) {
            std::vector<Word> &ws = words[n];
            const bool had = !ws.empty() && isSign(ws.front().text);
            const bool listing = !look.list.isEmpty() && chosen(n);
            if (listing && had)
                ws.erase(ws.begin());
            if (!listing || look.list == QLatin1String("none") || ws.empty()) {
                // (a sign, or a heading's number - "1.3", "1.4.4", "2." - with a tab's room after it on the page:
                // that room kept, its text lined up after it as it was - not closed up to a space)
                static const QRegularExpression headingNumber(QStringLiteral("^(\\d{1,3}(\\.\\d{1,3})*\\.?|[A-Z]\\.|[IVXLC]{1,5}\\.)$"));
                // (its first word with the pieces glued to it - "1" + ".3" in two looks: one)
                size_t unitEnd = 1;
                QString unit = ws.front().text;
                double unitWidth = ws.front().width;
                while (unitEnd < ws.size() && ws[unitEnd].glued) {
                    unit += ws[unitEnd].text;
                    unitWidth += ws[unitEnd].width;
                    ++unitEnd;
                }
                if (!listing && ws.size() > unitEnd && (had || headingNumber.match(unit).hasMatch()) && n < paras.size() &&
                    !paras[n].empty()) {
                    const TextLine &fl = lines[size_t(paras[n].front())];
                    std::vector<const TextChar *> cs;
                    if (const auto chars = text(page); chars && !fl.rtl && std::abs(fl.angle) < 0.01)
                        for (const TextChar &ch : *chars)
                            if (std::abs(ch.origin.y() - fl.baseline) < 0.6 && ch.box.center().x() >= fl.rect.left() - 1 &&
                                ch.box.center().x() <= fl.rect.right() + 1 && !QChar::isSpace(uint(ch.c)) && ch.c != 0xA0)
                                cs.push_back(&ch);
                    std::sort(cs.begin(), cs.end(), [](const TextChar *a, const TextChar *b) { return a->box.left() < b->box.left(); });
                    // (the first word's letters, then the next one's start)
                    const int firstLen = int(QString(unit).remove(QLatin1Char(' ')).size());
                    if (int(cs.size()) > firstLen) {
                        const double room = (cs[size_t(firstLen)]->box.left() - cs.front()->box.left()) * k;
                        Word &last = ws[unitEnd - 1];
                        const double space = looks[size_t(last.look)].space;
                        if (room > unitWidth + 1.2 * space) {
                            last.width += room - unitWidth - space;  // (its last piece: the room after it)
                            last.alone = true;
                            hangs[n] = room;
                            continue;
                        }
                    }
                }
                if (had && !listing && ws.size() > 1)
                    hangs[n] = ws.front().width + looks[size_t(ws.front().look)].space;
                continue;
            }
            ++listed;
            QString sign = look.list.section(QLatin1Char(':'), 1);
            if (look.list.startsWith(QLatin1String("number"))) {  // 1. 1) a. A. i. I.
                const int num = listed;
                auto roman = [](int v) {
                    static const std::pair<int, const char *> parts[] = {{10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
                    QString s;
                    for (const auto &[k, p] : parts)
                        while (v >= k) {
                            s += QLatin1String(p);
                            v -= k;
                        }
                    return s;
                };
                const QString form = sign.isEmpty() ? QStringLiteral("1.") : sign;
                const QChar kind = form.at(0), end = form.at(form.size() - 1);
                const QString value = kind == QLatin1Char('a') ? QString(QChar('a' + (num - 1) % 26))
                                      : kind == QLatin1Char('A') ? QString(QChar('A' + (num - 1) % 26))
                                      : kind == QLatin1Char('i') ? roman(num)
                                      : kind == QLatin1Char('I') ? roman(num).toUpper()
                                                                 : QString::number(num);
                sign = value + end;
            }
            const int lk = ws.front().look;
            Word w{sign, lk, textWidth(sign, looks[size_t(lk)].font, looks[size_t(lk)].size, looks[size_t(lk)].scale, looks[size_t(lk)].track),
                   false, true};
            hangs[n] = std::max(w.width + looks[size_t(lk)].space, 1.5 * looks[size_t(lk)].size);
            w.width = hangs[n] - looks[size_t(lk)].space;  // (its room: the text after it lined up)
            ws.insert(ws.begin(), w);
        }
    }
    // the paragraphs, each flowed into lines as wide as the frame
    struct Piece {
        QString text;
        int look;
        QPointF at;
        double width = 0;
    };
    std::vector<Piece> pieces;
    double y = lines[size_t(paras.empty() ? 0 : paras.front().front())].baseline;
    for (int n = 0; n < int(words.size()); ++n) {
        const std::vector<Word> &ws = words[size_t(n)];
        // (its own: as it was - a paragraph typed in: as the block's)
        const QString align = n < int(paraAligns.size()) ? paraAligns[size_t(n)] : blockAlign;
        const double leading = n < int(paraLeads.size()) ? paraLeads[size_t(n)] : leadingAll;
        if (n > 0)
            y += ws.empty() ? leading : n < int(paraGaps.size()) ? paraGaps[size_t(n)] : paraGap;
        if (ws.empty())
            continue;
        // (aligned where its lines start - right to left: the right)
        const bool atStart = align == "justify" || align == (rtl ? "right" : "left");
        const double indent = atStart && n < int(indents.size()) ? indents[size_t(n)] : 0;
        // (its lines after the first: where they were - under its text, past a list's sign; a first-line
        // indent not carried down)
        const double rest = atStart && n < int(rests.size()) ? rests[size_t(n)] : 0;
        const double hang = (hangs[size_t(n)] > 0 ? indent : rest) + hangs[size_t(n)];
        std::vector<std::vector<Word>> rows(1);
        double used = 0, spaced = 0;  // (the row's width so far, and how much of it is spaces)
        for (size_t q = 0; q < ws.size();) {
            // (a word and the pieces glued to it: one unit, never broken)
            size_t r = q + 1;
            double unit = ws[q].width;
            while (r < ws.size() && ws[r].glued)
                unit += ws[r++].width;
            const double width = room - (rows.size() == 1 ? indent : hang);
            // (justified: its spaces may be a little narrower, as Word squeezes them - a line it fitted
            // still fits, and isn't broken again a word sooner)
            const double squeeze = align == "justify" ? 0.9 : 1.0;
            const double gap = rows.back().empty() ? 0 : looks[size_t(rows.back().back().look)].space;
            if (!rows.back().empty() && used - (1 - squeeze) * (spaced + gap) + unit > width + 0.5) {
                rows.emplace_back();
                used = spaced = 0;
            }
            const double before = rows.back().empty() ? 0 : looks[size_t(rows.back().back().look)].space;
            used += before + unit;
            spaced += before;
            for (size_t k = q; k < r; ++k)
                rows.back().push_back(ws[k]);
            q = r;
        }
        for (size_t m = 0; m < rows.size(); ++m) {
            if (m > 0)
                y += leading;
            const std::vector<Word> &row = rows[m];
            const double first = m == 0 ? indent : hang;
            const double width = room - first;
            double total = 0, spaces = 0;
            auto spaceAfter = [&](size_t q) {  // (none before a glued piece)
                return q + 1 < row.size() && !row[q + 1].glued ? looks[size_t(row[q].look)].space : 0.0;
            };
            for (size_t q = 0; q < row.size(); ++q) {
                total += row[q].width;
                spaces += spaceAfter(q);
            }
            const bool lastRow = m + 1 == rows.size();
            int gaps = 0;
            double fixed = 0;  // (a list's sign: its room as it is)
            for (size_t q = 0; q + 1 < row.size(); ++q) {
                if (row[q].alone)
                    fixed += looks[size_t(row[q].look)].space;
                else
                    gaps += row[q + 1].glued ? 0 : 1;
            }
            // (word by word, the gaps shared: a justified line - and a last line its squeezed spaces fitted,
            // a little narrower so it doesn't run past the box. Not one whose gaps would be over 1.25 of its
            // size - a few long words: read back, gaps that wide part a line into pieces, as columns)
            const double sizeHere = row.empty() ? size : looks[size_t(row.front().look)].size;
            const double share = gaps > 0 ? (width - total - fixed) / double(gaps) : 0;
            const bool spread = !lastRow && share <= 1.25 * sizeHere;
            const bool squeeze = total + spaces + fixed > width + 0.01;
            if (align == "justify" && gaps > 0 && (spread || squeeze)) {
                const double gap = share;
                double x = rtl ? right - first : left + first;  // (right to left: from the right end, leftwards)
                for (size_t q = 0; q < row.size(); ++q) {
                    const double after = q + 1 >= row.size() ? 0 : row[q].alone ? looks[size_t(row[q].look)].space : !row[q + 1].glued ? gap : 0;
                    if (rtl) {  // (each word after the first with its space before it - drawn in the room on its right, read
                                // back between the words: Arabic's gaps aren't read as spaces)
                        x -= row[q].width;
                        const bool spaced = q > 0 && !row[q].glued && !row[q - 1].alone;
                        // (a number, a Latin word - no right-to-left letter: written left to right, its space would go on its
                        // left; written apart, in the room on its right, where it's read)
                        const bool ownWay = std::any_of(row[q].text.begin(), row[q].text.end(), [](QChar c) { return strongRtl(c.unicode()); });
                        if (spaced && !ownWay) {
                            pieces.push_back({row[q].text, row[q].look, QPointF(x, y), row[q].width});
                            pieces.push_back({QStringLiteral(" "), row[q].look, QPointF(x + row[q].width, y), 0});
                        } else {
                            pieces.push_back({(spaced ? QStringLiteral(" ") : QString()) + row[q].text, row[q].look, QPointF(x, y), row[q].width});
                        }
                        x -= after;
                    } else {
                        pieces.push_back({row[q].text, row[q].look, QPointF(x, y), row[q].width});
                        x += row[q].width + after;
                    }
                }
                continue;
            }
            const double w = total + spaces;
            double x = align == "center" ? left + (room - w) / 2
                       : (align == "right" || (align == "justify" && rtl)) ? right - first - w : left + first;
            if (rtl)  // (right to left: from the right end, leftwards)
                x += w;
            // words in one look written together (with their spaces - right to left, the space before the next look's
            // words written with them: read back between them, as Arabic's gaps aren't)
            bool spaceBefore = false;
            for (size_t q = 0; q < row.size();) {
                size_t r = q;
                QString text = spaceBefore ? QStringLiteral(" ") : QString();
                double pw = 0;
                while (r < row.size() && row[r].look == row[q].look && (r == q || (!row[r].alone && !row[q].alone))) {
                    if (r > q && !row[r].glued) {
                        text += QLatin1Char(' ');
                        pw += looks[size_t(row[r - 1].look)].space;
                    }
                    text += row[r].text;
                    pw += row[r].width;
                    ++r;
                }
                const double after = spaceAfter(r - 1);
                spaceBefore = rtl && after > 0;
                if (rtl) {
                    x -= pw;
                    pieces.push_back({text, row[q].look, QPointF(x, y), pw});
                    x -= after;
                } else {
                    pieces.push_back({text, row[q].look, QPointF(x, y), pw});
                    x += pw + after;
                }
                q = r;
            }
        }
    }
    const TextLine &head = lines.front();
    const QPointF o = geometry()->origins.at(size_t(page));
    QRectF block;
    for (const TextLine &l : lines)
        block |= l.rect;
    block.adjust(-1, -1, 1, 1);
    std::lock_guard guard(docMutex_);
    return onPage(page, "change text", [&](pdf_page *pg) {
        for (const TextLine &l : lines)
            takeOut(pg, page, l);
        if (styled) {  // (the block's underline / strike-out marks: as the editor has them now)
            for (pdf_annot *a = pdf_first_annot(ctx_, pg); a;) {
                pdf_annot *next = pdf_next_annot(ctx_, a);
                const enum pdf_annot_type type = pdf_annot_type(ctx_, a);
                if (type == PDF_ANNOT_UNDERLINE || type == PDF_ANNOT_STRIKE_OUT) {
                    const fz_rect r = pdf_bound_annot(ctx_, a);
                    if (block.contains(QRectF(QPointF(r.x0 - o.x(), r.y0 - o.y()), QPointF(r.x1 - o.x(), r.y1 - o.y()))))
                        pdf_delete_annot(ctx_, pg, a);
                }
                a = next;
            }
        }
        setTurn(head.angle, head.pivot);
        for (const Piece &p : pieces) {
            const Look &lk = looks[size_t(p.look)];
            writeText(pg, page, p.at - QPointF(0, lk.rise > 0 ? 0.33 * lk.size / kRiseSize : lk.rise < 0 ? -0.14 * lk.size / kRiseSize : 0),
                      p.text, lk.font, lk.size, lk.color, lk.scale, lk.track);
            for (int k = 0; k < 2 && std::abs(head.angle) < 0.01; ++k) {
                if (!(k == 0 ? lk.underline : lk.strike))
                    continue;
                const fz_rect box = fz_make_rect(float(p.at.x() + o.x()), float(p.at.y() - 0.8 * lk.size + o.y()),
                                                 float(p.at.x() + p.width + o.x()), float(p.at.y() + 0.25 * lk.size + o.y()));
                pdf_annot *a = pdf_create_annot(ctx_, pg, k == 0 ? PDF_ANNOT_UNDERLINE : PDF_ANNOT_STRIKE_OUT);
                fz_try(ctx_) {
                    const float c[3] = {float(lk.color.redF()), float(lk.color.greenF()), float(lk.color.blueF())};
                    pdf_add_annot_quad_point(ctx_, a, fz_quad_from_rect(box));
                    pdf_set_annot_color(ctx_, a, 3, c);
                    pdf_update_annot(ctx_, a);
                }
                fz_always(ctx_) {
                    pdf_drop_annot(ctx_, a);
                }
                fz_catch(ctx_) {
                    fz_rethrow(ctx_);
                }
            }
        }
        setTurn(0, {});
    }, error);
}

bool Document::retypeParagraph(int page, const std::vector<TextLine> &para, int edited, const QString &text,
                               const LineLook &look, QString *error)
{
    if (para.size() < 2 || edited < 0 || edited >= int(para.size()))
        return retypeLine(page, para.at(size_t(std::max(0, edited))), text, look, error);
    // the paragraph's words, the line retyped among them
    QStringList words;
    for (int n = 0; n < int(para.size()); ++n)
        words += (n == edited ? collapse(text) : para[size_t(n)].text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const TextLine &me = para[size_t(edited)];
    const TextRun &main = me.mainRun();
    const fonts::Found font = fontOf(main, look, words.join(QLatin1Char(' ')));
    const double size = look.size > 0 ? look.size : main.size;
    const QColor color = look.color.isValid() ? look.color : QColor::fromRgb(main.color);
    // its frame: as wide as its widest line; its lines a pitch apart
    double left = 1e9, right = -1e9;
    for (const TextLine &l : para) {
        left = std::min(left, l.rect.left());
        right = std::max(right, l.rect.right());
    }
    const double pitch = (para.back().baseline - para.front().baseline) / double(para.size() - 1);
    // how its lines lined up: justified, centred, right or left
    bool justified = true, centered = true, rightAligned = true, leftAligned = true;
    for (size_t n = 0; n < para.size(); ++n) {
        const QRectF &r = para[n].rect;
        if (n + 1 < para.size() && (std::abs(r.right() - right) > 2 || std::abs(r.left() - left) > 2))
            justified = false;
        if (std::abs(r.center().x() - (left + right) / 2) > 2)
            centered = false;
        if (std::abs(r.right() - right) > 2)
            rightAligned = false;
        if (std::abs(r.left() - left) > 2)
            leftAligned = false;
    }
    const bool rtl = me.rtl;
    const QString align = justified && !leftAligned ? "justify"
                          : justified ? "justify"
                          : (centered && !leftAligned && !rightAligned) ? "center"
                          : (rtl || (rightAligned && !leftAligned)) ? "right" : "left";
    const double room = right - left;
    // the words flowed into lines as wide as the frame
    std::vector<QStringList> rows(1);
    const double space = textWidth(QStringLiteral(" "), font, size);
    double used = 0;
    for (const QString &w : words) {
        const double ww = textWidth(w, font, size);
        if (!rows.back().isEmpty() && used + space + ww > room + 0.5) {
            rows.emplace_back();
            used = 0;
        }
        used += (rows.back().isEmpty() ? 0 : space) + ww;
        rows.back() << w;
    }
    struct Piece {
        QString text;
        QPointF at;
    };
    std::vector<Piece> pieces;
    for (size_t n = 0; n < rows.size(); ++n) {
        const double y = para.front().baseline + pitch * double(n);
        const QString line = rows[n].join(QLatin1Char(' '));
        const double w = textWidth(line, font, size);
        const bool last = n + 1 == rows.size();
        if (align == "justify" && !last && rows[n].size() > 1 && !rtl) {  // word by word, the gaps shared
            double total = 0;
            for (const QString &word : rows[n])
                total += textWidth(word, font, size);
            const double gap = (room - total) / double(rows[n].size() - 1);
            double x = left;
            for (const QString &word : rows[n]) {
                pieces.push_back({word, QPointF(x, y)});
                x += textWidth(word, font, size) + gap;
            }
            continue;
        }
        const double x = align == "center" ? left + (room - w) / 2 : align == "right" || (align == "justify" && rtl) ? right - w : left;
        pieces.push_back({line, QPointF(x, y)});
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "change text", [&](pdf_page *pg) {
        for (const TextLine &l : para)
            takeOut(pg, page, l);
        setTurn(para.front().angle, para.front().pivot);
        for (const Piece &p : pieces)
            writeText(pg, page, p.at, p.text, font, size, color);
        setTurn(0, {});
    }, error);
}

std::vector<QRectF> Document::addText(int page, QPointF top, const QString &text, const QString &family, double size,
                                      QColor color, bool bold, bool italic, const QString &align, const QString &dir,
                                      QString *error)
{
    const fonts::Found font = fonts::forText(fonts::findFamily(family, bold, italic), text);
    double ascent = 0.9, descent = -0.2;
    if (!font.path.isEmpty()) {
        std::lock_guard guard(g_hbMutex);
        HbFont *h = hbFont(font);
        ascent = h->ascender;
        descent = h->descender;
    }
    const QStringList rowsIn = text.split(QLatin1Char('\n'));
    std::vector<double> widths;
    double full = 0;
    int last = 0;
    for (int n = 0; n < rowsIn.size(); ++n) {
        widths.push_back(textWidth(rowsIn[n], font, size));
        full = std::max(full, widths.back());
        if (!rowsIn[n].trimmed().isEmpty())
            last = n;
    }
    struct Piece {
        QString text;
        QPointF at;
    };
    std::vector<Piece> pieces;
    std::vector<QRectF> boxes;
    for (int n = 0; n < rowsIn.size(); ++n) {
        const QString row = rowsIn[n];
        if (row.trimmed().isEmpty())
            continue;
        const double y = top.y() + ascent * size + n * size * 1.2;
        bool rtl = dir == "rtl";
        if (dir.isEmpty()) {
            double w = 0;
            if (!font.path.isEmpty())
                shape(row, font, size, &w, &rtl);
        }
        const QStringList words = row.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        double x0 = top.x(), w = widths[size_t(n)];
        if (align == "justify" && n < last && words.size() > 1 && !rtl) {  // (the gaps share the room left)
            double total = 0;
            for (const QString &word : words)
                total += textWidth(word, font, size);
            const double gap = (full - total) / double(words.size() - 1);
            double x = top.x();
            for (const QString &word : words) {
                pieces.push_back({word, QPointF(x, y)});
                x += textWidth(word, font, size) + gap;
            }
            w = full;
        } else {
            const QString how = align == "justify" ? (rtl ? "right" : "left") : align;
            x0 = top.x() + (how == "center" ? (full - w) / 2 : how == "right" ? full - w : 0);
            pieces.push_back({row, QPointF(x0, y)});
        }
        boxes.emplace_back(QPointF(x0, y - ascent * size), QPointF(x0 + w, y - descent * size));
    }
    if (pieces.empty())
        return {};
    std::lock_guard guard(docMutex_);
    const bool ok = onPage(page, "add text", [&](pdf_page *pg) {
        for (const Piece &p : pieces)
            writeText(pg, page, p.at, p.text, font, size, color);
    }, error);
    return ok ? boxes : std::vector<QRectF>{};
}
