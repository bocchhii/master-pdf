#include "core/Document.h"

#include <QSet>
#include <map>
#include <QTransform>

#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QImage>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <new>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// (No C++ objects are made inside fz_try blocks: MuPDF's errors jump out of them, past
// anything that would need cleaning up.)

namespace {

// MuPDF's locks, shared by every context: it calls these to keep its caches consistent
// when several threads draw at once.
std::mutex g_locks[FZ_LOCK_MAX];

void lockFn(void *, int lock) { g_locks[lock].lock(); }
void unlockFn(void *, int lock) { g_locks[lock].unlock(); }

fz_locks_context g_lockContext = {nullptr, lockFn, unlockFn};

// After Undo / Redo: MuPDF's list of the fonts it has put in the PDF (to use them again) made
// anew - a font Undo took out would otherwise be used again, its letters read back wrong
void dropFontObj(fz_context *ctx, void *obj)
{
    pdf_drop_obj(ctx, static_cast<pdf_obj *>(obj));
}

void forgetFonts(fz_context *ctx, pdf_document *doc)
{
    pdf_drop_resource_tables(ctx, doc);
    doc->resources.fonts = fz_new_hash_table(ctx, 4096, sizeof(pdf_font_resource_key), -1, dropFontObj);
}

// MuPDF's notes to the console (harmless ones about its colour caches, and errors the
// app already shows in its own messages): not printed
void quiet(void *, const char *) {}

// The file at path put in place of target (or as it, if there's none): one step, so a
// failed save never leaves half a file
bool replaceFile(const QString &from, const QString &target, QString *error)
{
#ifdef Q_OS_WIN
    if (MoveFileExW(reinterpret_cast<const wchar_t *>(from.utf16()),
                    reinterpret_cast<const wchar_t *>(target.utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (error)
        *error = GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_SHARING_VIOLATION
                     ? QStringLiteral("The file is in use (is it open in another program?)")
                     : QStringLiteral("Windows couldn't write the file (error %1).").arg(GetLastError());
    QFile::remove(from);
    return false;
#else
    QFile::remove(target);
    if (QFile::rename(from, target))
        return true;
    if (error)
        *error = QStringLiteral("The file couldn't be written.");
    return false;
#endif
}

// The full save's options: unused parts dropped, duplicates merged, everything compressed
pdf_write_options fullSave()
{
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 3;
    o.do_compress = 1;
    o.do_compress_images = 1;
    o.do_compress_fonts = 1;
    return o;
}

}  // namespace

std::shared_ptr<Document> Document::open(const QString &path, QString *error)
{
    std::shared_ptr<Document> d(new Document());
    d->path_ = path;
    d->ctx_ = fz_new_context(nullptr, &g_lockContext, FZ_STORE_DEFAULT);
    if (!d->ctx_) {
        if (error)
            *error = QStringLiteral("The PDF engine couldn't start.");
        return nullptr;
    }
    fz_context *ctx = d->ctx_;
    fz_set_warning_callback(ctx, quiet, nullptr);  // (the threads' copies take these on)
    fz_set_error_callback(ctx, quiet, nullptr);
    // read whole: the file stays free to be saved over (MuPDF takes UTF-8 names on Windows too)
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return nullptr;
    }
    const QByteArray data = f.readAll();
    f.close();
    const QByteArray name = path.toUtf8();
    bool ok = true, locked = false;
    QString message;
    fz_buffer *buf = nullptr;
    fz_var(buf);
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        buf = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char *>(data.constData()),
                                             size_t(data.size()));
        d->doc_ = fz_open_document_with_buffer(ctx, name.constData(), buf);
        if (fz_needs_password(ctx, d->doc_))
            locked = true;
        else {
            d->pdf_ = pdf_document_from_fz_document(ctx, d->doc_);
            if (d->pdf_)
                pdf_enable_journal(ctx, d->pdf_);
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        ok = false;
        message = QString::fromUtf8(fz_caught_message(ctx));
    }
    if (ok && locked) {
        ok = false;
        message = QStringLiteral("It's locked with a password.");
    }
    if (ok) {
        std::lock_guard guard(d->docMutex_);
        d->measure();
        ok = d->geometry()->sizes.size() > 0;
        if (!ok)
            message = QStringLiteral("It has no pages.");
    }
    if (!ok) {
        if (error)
            *error = message;
        return nullptr;
    }
    d->savedStep_ = d->journalStep();
    return d;
}

std::shared_ptr<Document> Document::create(QString *error)
{
    std::shared_ptr<Document> d(new Document());
    d->ctx_ = fz_new_context(nullptr, &g_lockContext, FZ_STORE_DEFAULT);
    if (!d->ctx_) {
        if (error)
            *error = QStringLiteral("The PDF engine couldn't start.");
        return nullptr;
    }
    fz_set_warning_callback(d->ctx_, quiet, nullptr);
    fz_set_error_callback(d->ctx_, quiet, nullptr);
    bool ok = true;
    fz_try(d->ctx_) {
        fz_register_document_handlers(d->ctx_);
        d->pdf_ = pdf_create_document(d->ctx_);
        d->doc_ = reinterpret_cast<fz_document *>(d->pdf_);
        pdf_enable_journal(d->ctx_, d->pdf_);
    }
    fz_catch(d->ctx_) {
        ok = false;
    }
    if (!ok)
        return nullptr;
    std::lock_guard guard(d->docMutex_);
    d->measure();
    d->savedStep_ = -1;  // (never saved: changed from the start)
    return d;
}

QByteArray Document::snapshot()
{
    QByteArray out;
    std::lock_guard guard(docMutex_);
    if (!pdf_)
        return out;
    fz_buffer *buf = nullptr;
    fz_output *o = nullptr;
    fz_var(buf);
    fz_var(o);
    fz_try(ctx_) {
        buf = fz_new_buffer(ctx_, 1 << 16);
        o = fz_new_output_with_buffer(ctx_, buf);
        pdf_write_options plain = pdf_default_write_options;
        pdf_write_document(ctx_, pdf_, o, &plain);
        fz_close_output(ctx_, o);
        unsigned char *data = nullptr;
        const size_t len = fz_buffer_storage(ctx_, buf, &data);
        out = QByteArray(reinterpret_cast<const char *>(data), qsizetype(len));
    }
    fz_always(ctx_) {
        fz_drop_output(ctx_, o);
        fz_drop_buffer(ctx_, buf);
    }
    fz_catch(ctx_) {
        out.clear();
    }
    return out;
}

Document::~Document()
{
    if (!ctx_)
        return;
    for (fz_display_list *list : lists_)
        fz_drop_display_list(ctx_, list);
    for (auto &[k, font] : fzFonts_)
        fz_drop_font(ctx_, font);
    fz_drop_document(ctx_, doc_);
    for (auto &[id, clone] : clones_)
        fz_drop_context(clone);
    fz_drop_context(ctx_);
}

fz_context *Document::threadContext()
{
    std::lock_guard guard(ctxMutex_);
    auto &clone = clones_[std::this_thread::get_id()];
    if (!clone)
        clone = fz_clone_context(ctx_);
    return clone;
}

std::shared_ptr<const Document::Geometry> Document::geometry() const
{
    std::lock_guard guard(geomMutex_);
    return geom_;
}

int Document::pageCount() const
{
    auto g = geometry();
    return g ? int(g->sizes.size()) : 0;
}

QSizeF Document::pageSize(int page) const
{
    auto g = geometry();
    if (!g || page < 0 || page >= int(g->sizes.size()))
        return {612, 792};
    return g->sizes[size_t(page)];
}

// (docMutex_ held)
void Document::measure()
{
    auto g = std::make_shared<Geometry>();
    fz_context *ctx = ctx_;
    fz_try(ctx) {
        // (MuPDF's map of the page tree, made again: Undo and Redo put back the tree itself
        // but not the map, so it would still give the pages in their old order)
        if (pdf_)
            pdf_drop_page_tree_internal(ctx, pdf_);
        const int n = fz_count_pages(ctx, doc_);
        g->sizes.reserve(size_t(n));
        g->origins.reserve(size_t(n));
        for (int i = 0; i < n; ++i) {
            fz_page *page = fz_load_page(ctx, doc_, i);
            const fz_rect r = fz_bound_page(ctx, page);
            fz_drop_page(ctx, page);
            g->sizes.emplace_back(r.x1 - r.x0, r.y1 - r.y0);
            g->origins.emplace_back(r.x0, r.y0);
        }
    }
    fz_catch(ctx) {
    }
    // what was drawn and read of the pages is forgotten (a thread still drawing from an old
    // display list keeps its own hold on it)
    for (fz_display_list *&list : lists_) {
        fz_drop_display_list(ctx_, list);
        list = nullptr;
    }
    lists_.assign(g->sizes.size(), nullptr);
    annots_.assign(g->sizes.size(), nullptr);
    {
        std::lock_guard guard(textMutex_);
        texts_.assign(g->sizes.size(), nullptr);
        ++textGen_;
    }
    std::lock_guard guard(geomMutex_);
    geom_ = std::move(g);
}

fz_display_list *Document::keepDisplayList(int page, QPointF *origin)
{
    std::lock_guard guard(docMutex_);
    if (page < 0 || page >= int(lists_.size()))
        return nullptr;
    fz_display_list *&list = lists_[size_t(page)];
    if (!list) {
        fz_context *ctx = ctx_;
        fz_page *p = nullptr;
        fz_var(p);
        fz_try(ctx) {
            p = fz_load_page(ctx, doc_, page);
            list = fz_new_display_list_from_page(ctx, p);
        }
        fz_always(ctx) {
            fz_drop_page(ctx, p);
        }
        fz_catch(ctx) {
            list = nullptr;
        }
    }
    if (!list)
        return nullptr;
    *origin = geometry()->origins[size_t(page)];
    return fz_keep_display_list(ctx_, list);
}

QImage Document::render(int page, double scale, const QRect &area)
{
    if (area.isEmpty())
        return {};
    QPointF o;
    fz_display_list *list = keepDisplayList(page, &o);
    if (!list)
        return {};
    fz_context *ctx = threadContext();
    QImage out;
    fz_pixmap *pix = nullptr;
    fz_device *dev = nullptr;
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx) {
        const fz_irect box = {area.left(), area.top(), area.left() + area.width(),
                              area.top() + area.height()};
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        const fz_matrix ctm = fz_concat(fz_translate(float(-o.x()), float(-o.y())),
                                        fz_scale(float(scale), float(scale)));
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_display_list(ctx, list, dev, ctm, fz_rect_from_irect(box), nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }
    if (pix) {
        const int w = fz_pixmap_width(ctx, pix);
        const int h = fz_pixmap_height(ctx, pix);
        const int stride = int(fz_pixmap_stride(ctx, pix));
        out = QImage(fz_pixmap_samples(ctx, pix), w, h, stride, QImage::Format_RGB888).copy();
        fz_drop_pixmap(ctx, pix);
    }
    fz_drop_display_list(ctx, list);
    return out;
}

QByteArray Document::fontProgram(const QString &name)
{
    std::lock_guard guard(textMutex_);
    return fontPrograms_.value(name);
}

namespace {

// (the letters both filled and outlined - text render mode 2, the way a PDF made without a bold font
// makes its letters bold: a regular font, its outline stroked round them - their pens' places, rounded
// to a quarter point: each seen as bold)
struct StrokedText {
    fz_device super;
    QSet<qint64> *filled;
    QHash<qint64, float> *stroked;  // (the outline's width there, in the page's units)
    QSet<qint64> *unseen;           // (text not drawn - render mode 3: a scan's text read)
};

qint64 penKey(float x, float y)
{
    return (qint64(std::lround(x * 4)) << 32) ^ (qint64(std::lround(y * 4)) & 0xffffffff);
}

void collectPens(const fz_text *text, fz_matrix ctm, QSet<qint64> *into, QHash<qint64, float> *widths = nullptr, float width = 0)
{
    for (const fz_text_span *sp = text->head; sp; sp = sp->next)
        for (int i = 0; i < sp->len; ++i) {
            const fz_point q = fz_transform_point_xy(sp->items[i].x, sp->items[i].y, ctm);
            if (into)
                into->insert(penKey(q.x, q.y));
            if (widths)
                widths->insert(penKey(q.x, q.y), width);
        }
}

// (each letter's pen place - filled and outlined - and the outline's width; unseen: the letters not drawn)
QHash<qint64, float> boldStroked(fz_context *ctx, fz_display_list *list, QSet<qint64> *unseen)
{
    QSet<qint64> filled;
    QHash<qint64, float> stroked, both;
    fz_device *dev = nullptr;
    fz_var(dev);
    fz_try(ctx) {
        auto *d = fz_new_derived_device(ctx, StrokedText);
        d->filled = &filled;
        d->stroked = &stroked;
        d->unseen = unseen;
        d->super.ignore_text = [](fz_context *, fz_device *dv, const fz_text *t, fz_matrix m) {
            collectPens(t, m, reinterpret_cast<StrokedText *>(dv)->unseen);
        };
        d->super.fill_text = [](fz_context *, fz_device *dv, const fz_text *t, fz_matrix m, fz_colorspace *, const float *, float,
                                fz_color_params) { collectPens(t, m, reinterpret_cast<StrokedText *>(dv)->filled); };
        d->super.stroke_text = [](fz_context *, fz_device *dv, const fz_text *t, const fz_stroke_state *st, fz_matrix m, fz_colorspace *,
                                  const float *, float, fz_color_params) {
            collectPens(t, m, nullptr, reinterpret_cast<StrokedText *>(dv)->stroked, (st ? st->linewidth : 1) * fz_matrix_expansion(m));
        };
        dev = &d->super;
        fz_run_display_list(ctx, list, dev, fz_identity, fz_infinite_rect, nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
    }
    fz_catch(ctx) {
        return {};
    }
    for (auto it = stroked.begin(); it != stroked.end(); ++it)
        if (filled.contains(it.key()))
            both.insert(it.key(), it.value());
    return both;
}

}  // namespace

std::shared_ptr<const std::vector<TextChar>> Document::text(int page)
{
    int gen;
    {
        std::lock_guard guard(textMutex_);
        if (page < 0 || page >= int(texts_.size()))
            return std::make_shared<const std::vector<TextChar>>();
        if (texts_[size_t(page)])
            return texts_[size_t(page)];
        gen = textGen_;
    }
    QPointF o;
    fz_display_list *list = keepDisplayList(page, &o);
    std::vector<std::vector<TextChar>> lines;
    if (list) {
        fz_context *ctx = threadContext();
        fz_stext_page *st = nullptr;
        fz_var(st);
        fz_try(ctx) {
            fz_stext_options opts = {};
            opts.flags = FZ_STEXT_PRESERVE_LIGATURES | FZ_STEXT_PRESERVE_WHITESPACE |
                         FZ_STEXT_MEDIABOX_CLIP;
            st = fz_new_stext_page_from_display_list(ctx, list, &opts);
        }
        fz_catch(ctx) {
            st = nullptr;
        }
        QSet<qint64> unseen;
        const QHash<qint64, float> outlined = st ? boldStroked(ctx, list, &unseen) : QHash<qint64, float>();
        if (st) {  // (read out of MuPDF's structures - nothing in them throws)
            int blockNo = 0;
            QHash<fz_font *, std::pair<QString, int>> fontsSeen;  // (each font's name and kind, once)
            for (fz_stext_block *b = st->first_block; b; b = b->next) {
                if (b->type != FZ_STEXT_BLOCK_TEXT)
                    continue;
                ++blockNo;
                const size_t blockFirst = lines.size();  // (its lines: from here)
                for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
                    const bool straight = std::abs(l->dir.y) < 0.01f && l->dir.x > 0;
                    std::vector<TextChar> line;
                    for (fz_stext_char *c = l->first_char; c; c = c->next) {
                        if (c->c == 0x200B || c->c == 0x200C || c->c == 0x200D || c->c == 0x200E || c->c == 0x200F || c->c == 0x2060 ||
                            c->c == 0xFEFF)
                            continue;  // (zero-width marks draw nothing - and MuPDF may put one far from its line)
                        const fz_rect r = fz_rect_from_quad(c->quad);
                        TextChar ch;
                        // (a no-break space read back as a space: fonts that draw both alike get it
                        // read as the no-break one)
                        ch.c = c->c == 0xA0 ? U' ' : (c->c == 0x2010 || c->c == 0x2011 || c->c == 0xAD) ? U'-' : char32_t(c->c);  // (Word's hyphen, non-breaking hyphen, and a soft hyphen shown: a plain one - fonts draw them alike, and read the plain one back as soft)
                        ch.size = c->size;
                        // (letters made wider / narrower: MuPDF's size is the square root of the size
                        // across times the size up - the size up is its box's height over its font's
                        // ascender to descender; the size across over it, how much wider)
                        if (c->font) {
                            const float span = fz_font_ascender(ctx, c->font) - fz_font_descender(ctx, c->font);
                            const float tall = std::hypot(c->quad.ul.x - c->quad.ll.x, c->quad.ul.y - c->quad.ll.y);
                            if (span > 0.3f && tall > 0 && c->size > 0) {
                                const float up = tall / span, wide = c->size * c->size / (up * up);
                                if (std::abs(wide - 1) > 0.03f && wide > 0.2f && wide < 5) {
                                    ch.size = up;
                                    ch.scale = wide;
                                }
                            }
                        }
                        ch.straight = straight;
                        ch.line = int(lines.size());
                        ch.advance = std::hypot(c->quad.ur.x - c->quad.ul.x, c->quad.ur.y - c->quad.ul.y);
                        // (as tall as its line, so a selection is even)
                        const float y0 = straight ? l->bbox.y0 : r.y0;
                        const float y1 = straight ? l->bbox.y1 : r.y1;
                        ch.box = QRectF(QPointF(r.x0 - o.x(), y0 - o.y()), QPointF(r.x1 - o.x(), y1 - o.y()));
                        // (what it's written in, to retype it the same)
                        ch.origin = QPointF(c->origin.x - o.x(), c->origin.y - o.y());
                        ch.color = unsigned(c->color) & 0xFFFFFF;
                        ch.block = blockNo;
                        if (c->font) {
                            auto it = fontsSeen.find(c->font);
                            if (it == fontsSeen.end()) {
                                const int flags = (fz_font_is_bold(ctx, c->font) ? 1 : 0) | (fz_font_is_italic(ctx, c->font) ? 2 : 0) |
                                                  (fz_font_is_serif(ctx, c->font) ? 4 : 0) | (fz_font_is_monospaced(ctx, c->font) ? 8 : 0);
                                it = fontsSeen.insert(c->font, {QString::fromUtf8(fz_font_name(ctx, c->font)), flags});
                                if (c->font->buffer) {  // (its own program: the editor shows its letters in it)
                                    unsigned char *data = nullptr;
                                    const size_t len = fz_buffer_storage(ctx, c->font->buffer, &data);
                                    bool fresh = false;
                                    {
                                        std::lock_guard fontGuard(textMutex_);
                                        fresh = !fontPrograms_.contains(it->first) && len > 0;
                                        if (fresh)
                                            fontPrograms_.insert(it->first, QByteArray(reinterpret_cast<const char *>(data), qsizetype(len)));
                                    }
                                    // (... and text retyped in it stays in it, when it has the letters)
                                    if (fresh)
                                        fonts::addEmbedded(it->first, QByteArray(reinterpret_cast<const char *>(data), qsizetype(len)), flags);
                                }
                            }
                            ch.font = it->first;
                            ch.flags = it->second;
                        }
                        if (!outlined.isEmpty() && outlined.contains(penKey(c->origin.x, c->origin.y))) {
                            ch.flags |= 1 | 16;  // (a regular font made bold by its outline: bold, as it's seen)
                            ch.outline = c->size > 0 ? outlined.value(penKey(c->origin.x, c->origin.y)) / c->size : 0;
                        }
                        if (!unseen.isEmpty() && unseen.contains(penKey(c->origin.x, c->origin.y)))
                            ch.flags |= 32;  // (not drawn: a scan's text read - the scan shows it)
                        if (!straight)  // (turned: its outline as shown kept)
                            ch.quad = QPolygonF({QPointF(c->quad.ul.x - o.x(), c->quad.ul.y - o.y()),
                                                 QPointF(c->quad.ur.x - o.x(), c->quad.ur.y - o.y()),
                                                 QPointF(c->quad.lr.x - o.x(), c->quad.lr.y - o.y()),
                                                 QPointF(c->quad.ll.x - o.x(), c->quad.ll.y - o.y())});
                        line.push_back(ch);
                    }
                    // an Arabic line with numbers: put in reading order by where its letters are - MuPDF reads
                    // Arabic-Indic digits as right to left letters and jumbles a number, a space between each
                    // pair. Right to left: each letter from the right; a number (digits, the signs between them,
                    // Latin words) as one piece, left to right inside
                    auto digit = [](char32_t c) { return (c >= U'0' && c <= U'9') || (c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9); };
                    auto across = [&](char32_t c) { return digit(c) || (c < 0x250 && QChar::isLetter(c)); };
                    int arabicLetters = 0, latinLetters = 0, digits = 0;
                    for (const TextChar &ch : line) {
                        arabicLetters += pagetext::isArabic(ch.c) && !digit(ch.c) ? 1 : 0;
                        latinLetters += ch.c < 0x250 && QChar::isLetter(ch.c) ? 1 : 0;
                        digits += digit(ch.c) ? 1 : 0;
                    }
                    bool indic = false;  // (Arabic-Indic digits in it: a number of an Arabic line, read alone)
                    for (const TextChar &ch : line)
                        indic = indic || (ch.c >= 0x660 && ch.c <= 0x669) || (ch.c >= 0x6F0 && ch.c <= 0x6F9);  // (MuPDF reads them as Persian's)
                    // a line with Arabic in it - its letters, its digits: in reading order by where its letters are, as Unicode's
                    // bidi reads them (the order MuPDF gives is its own: Arabic's digits jumbled, a space put where it went back).
                    // Right to left if its rightmost letter is Arabic's, or it's just numbers in Arabic's digits
                    if (straight && digits > 0 && (arabicLetters > 0 || indic)) {
                        bool rtlLine = false;
                        double right = -1e9;
                        for (const TextChar &ch : line)
                            if (QChar::isLetter(ch.c) && !digit(ch.c) && !QChar::isMark(ch.c) && ch.origin.x() > right) {
                                right = ch.origin.x();
                                rtlLine = pagetext::isArabic(ch.c);
                            }
                        if (latinLetters == 0)
                            rtlLine = true;
                        std::vector<TextChar> kept;
                        for (const TextChar &ch : line)  // (MuPDF's spaces of no width, put in where it went back)
                            if (!(ch.c == U' ' && ch.advance < 0.1f * std::max(1.0f, ch.size)))
                                kept.push_back(ch);
                        // (across, left to right - Arabic's drawn at one place, a letter and the mark on it, lam-alef's two: the
                        // other way round from MuPDF's order, read in it once turned round)
                        std::vector<size_t> byX(kept.size());
                        for (size_t n = 0; n < kept.size(); ++n)
                            byX[n] = n;
                        std::stable_sort(byX.begin(), byX.end(), [&](size_t a, size_t b) {
                            const double da = kept[a].origin.x(), db = kept[b].origin.x();
                            if (std::abs(da - db) > 0.01)
                                return da < db;
                            if (pagetext::isRtl(kept[a].c) || pagetext::isRtl(kept[b].c))
                                return a > b;
                            return false;
                        });
                        {  // (a space MuPDF put between two digits with no room for one: a number's digits a little apart)
                            std::vector<size_t> real;
                            for (size_t k = 0; k < byX.size(); ++k) {
                                const TextChar &ch = kept[byX[k]];
                                if (ch.c == U' ') {
                                    size_t p = k, q = k;
                                    while (p > 0 && kept[byX[p - 1]].c == U' ')
                                        --p;
                                    while (q + 1 < byX.size() && kept[byX[q + 1]].c == U' ')
                                        ++q;
                                    if (p > 0 && q + 1 < byX.size()) {
                                        const TextChar &a = kept[byX[p - 1]], &b = kept[byX[q + 1]];
                                        const double room = b.origin.x() - (a.origin.x() + a.advance);
                                        if (digit(a.c) && digit(b.c) && room < 0.15 * std::max(1.0f, a.size))
                                            continue;
                                    }
                                }
                                real.push_back(byX[k]);
                            }
                            byX = std::move(real);
                        }
                        // (its pieces apart - a gap wider than 1.3 of the size, as the page's lines are cut into pieces - each right
                        // to left or not as the piece itself is read, its Arabic against its Latin, as its line's way is told: the
                        // e-mail address and phone numbers beside an Arabic address read as a line of their own. The pieces in the
                        // line's way)
                        auto rtlOf = [&](size_t from, size_t to) {
                            int arabic = 0, latin = 0;
                            for (size_t k = from; k < to; ++k) {
                                const char32_t c = kept[byX[k]].c;
                                arabic += pagetext::isArabic(c) ? 1 : 0;
                                latin += c < 128 && QChar::isLetter(c) ? 1 : 0;
                            }
                            return arabic > 0 && arabic >= latin;
                        };
                        std::vector<std::pair<size_t, size_t>> parts;
                        for (size_t k = 0; k < byX.size(); ++k) {
                            const TextChar &ch = kept[byX[k]];
                            const bool gap = k > 0 && ch.origin.x() - (kept[byX[k - 1]].origin.x() + kept[byX[k - 1]].advance) > 1.3 * std::max(1.0f, ch.size);
                            if (parts.empty() || gap)
                                parts.push_back({k, k + 1});
                            else
                                parts.back().second = k + 1;
                        }
                        if (rtlOf(0, byX.size()) || rtlLine)
                            std::reverse(parts.begin(), parts.end());
                        std::vector<TextChar> ordered;
                        for (const auto &[from, to] : parts) {
                            std::vector<pagetext::Bidi> types;
                            for (size_t k = from; k < to; ++k)
                                types.push_back(pagetext::bidiType(kept[byX[k]].c));
                            for (size_t p : pagetext::bidiOrder(pagetext::bidiLevels(types, rtlOf(from, to))))
                                ordered.push_back(kept[byX[from + p]]);
                        }
                        line = std::move(ordered);
                    }
                    // (Persian's ١ ٧ ... - the same shapes as Arabic's, and read back as them by the font - Arabic's in a
                    // line with Arabic in it: but Persian's own ۴ ۵ ۶, or a line with Persian's letters)
                    if (digits > 0 && (arabicLetters > 0 || indic)) {
                        bool persian = false;
                        for (const TextChar &ch : line)
                            persian = persian || ch.c == 0x67E || ch.c == 0x686 || ch.c == 0x698 || ch.c == 0x6AF || ch.c == 0x6A9 ||
                                      ch.c == 0x6CC || (ch.c >= 0x6F4 && ch.c <= 0x6F6);
                        if (!persian)
                            for (TextChar &ch : line)
                                if (ch.c >= 0x6F0 && ch.c <= 0x6F9)
                                    ch.c = char32_t(0x660 + (ch.c - 0x6F0));
                    }
                    // brackets in Arabic: drawn mirrored (the opening one on the right looks like ")") and read back by
                    // their look - turned back where they're between right-to-left letters
                    for (size_t n = 0; n < line.size(); ++n) {
                        static const char32_t pairs[][2] = {{U'(', U')'}, {U'[', U']'}, {U'{', U'}'}, {U'<', U'>'}, {0xAB, 0xBB}};
                        char32_t other = 0;
                        for (const auto &p : pairs)
                            other = line[n].c == p[0] ? p[1] : line[n].c == p[1] ? p[0] : other;
                        if (!other)
                            continue;
                        auto side = [&](int step) {  // (the nearest letter that way: 1 right to left, -1 left to right, 0 none)
                            for (long m = long(n) + step; m >= 0 && m < long(line.size()); m += step) {
                                const char32_t c = line[size_t(m)].c;
                                if (pagetext::isRtl(c))
                                    return 1;
                                if (QChar::isLetterOrNumber(c) && !(c >= 0x660 && c <= 0x669) && !(c >= 0x6F0 && c <= 0x6F9))
                                    return -1;
                            }
                            return 0;
                        };
                        const int before = side(-1), after = side(1);
                        if (before >= 0 && after >= 0 && before + after > 0)
                            line[n].c = other;
                    }
                    if (!straight && !line.empty())
                        for (TextChar &ch : line)
                            ch.angle = float(std::atan2(l->dir.y, l->dir.x) * 180 / M_PI);
                    lines.push_back(std::move(line));
                }
                // its turned lines, read straightened (as the Python app's straightened_blocks):
                // those turned alike made one group, turned back round their middle - each
                // letter's box and baseline as if straight
                std::map<int, std::vector<size_t>> turns;
                for (size_t n = blockFirst; n < lines.size(); ++n)
                    if (!lines[n].empty() && !lines[n].front().straight)
                        turns[int(std::lround(lines[n].front().angle * 10))].push_back(n);
                int group = 0;
                for (const auto &[key, members] : turns) {
                    QRectF all;
                    for (size_t n : members)
                        for (const TextChar &ch : lines[n])
                            all |= ch.quad.boundingRect();
                    const QPointF pivot = all.center();
                    const float angle = lines[members.front()].front().angle;
                    QTransform back;
                    back.translate(pivot.x(), pivot.y());
                    back.rotate(-angle);
                    back.translate(-pivot.x(), -pivot.y());
                    ++group;
                    for (size_t n : members)
                        for (TextChar &ch : lines[n]) {
                            ch.pivot = pivot;
                            ch.sbox = back.map(ch.quad).boundingRect();
                            ch.sorigin = back.map(ch.origin);
                            ch.block = 1000000 + blockNo * 100 + group;  // (a paragraph of its own)
                        }
                }
            }
            fz_drop_stext_page(ctx, st);
        }
        // a scan's text: the paper under each line (what an edit of it covers the scan's letters with)
        if (!unseen.isEmpty()) {
            fz_pixmap *pix = nullptr;
            fz_var(pix);
            fz_try(ctx) {
                pix = fz_new_pixmap_from_display_list(ctx, list, fz_identity, fz_device_rgb(ctx), 0);
            }
            fz_catch(ctx) {
                pix = nullptr;
            }
            if (pix) {
                const QImage im = QImage(fz_pixmap_samples(ctx, pix), fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix),
                                         int(fz_pixmap_stride(ctx, pix)), QImage::Format_RGB888)
                                      .copy();
                const QPointF at = QPointF(fz_pixmap_x(ctx, pix), fz_pixmap_y(ctx, pix)) - o;
                fz_drop_pixmap(ctx, pix);
                for (std::vector<TextChar> &line : lines) {
                    QRectF box;
                    for (const TextChar &ch : line)
                        if (ch.flags & 32)
                            box |= ch.box;
                    if (box.isEmpty())
                        continue;
                    const unsigned paper = pagetext::paperIn(im, box.translated(-at).adjusted(-2, -1, 2, 1).toAlignedRect());
                    // (its ink as the scan shows it - text not drawn has no colour of its own read back: an edit of it
                    // written in the scan's dark grey, a blue heading's blue. Drawn 3 times as big: its strokes' middles
                    // not blurred into its paper)
                    unsigned ink = 0;
                    {
                        const fz_matrix k = fz_scale(3, 3);
                        const QRectF r = box.translated(o);
                        const fz_irect ib = fz_round_rect(fz_transform_rect(fz_make_rect(float(r.left()), float(r.top()), float(r.right()), float(r.bottom())), k));
                        fz_pixmap *lp = nullptr;
                        fz_device *dev = nullptr;
                        fz_var(lp);
                        fz_var(dev);
                        fz_try(ctx) {
                            lp = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), ib, nullptr, 0);
                            fz_clear_pixmap_with_value(ctx, lp, 255);
                            dev = fz_new_draw_device(ctx, fz_identity, lp);
                            fz_run_display_list(ctx, list, dev, k, fz_rect_from_irect(ib), nullptr);
                            fz_close_device(ctx, dev);
                        }
                        fz_always(ctx) {
                            fz_drop_device(ctx, dev);
                        }
                        fz_catch(ctx) {
                            fz_drop_pixmap(ctx, lp);
                            lp = nullptr;
                        }
                        if (lp) {
                            const QImage big(fz_pixmap_samples(ctx, lp), fz_pixmap_width(ctx, lp), fz_pixmap_height(ctx, lp), int(fz_pixmap_stride(ctx, lp)),
                                             QImage::Format_RGB888);
                            ink = pagetext::inkIn(big, big.rect());
                            fz_drop_pixmap(ctx, lp);
                        }
                    }
                    for (TextChar &ch : line)
                        if (ch.flags & 32)
                            ch.paper = paper, ch.color = ink;
                }
                // (a scanned page's lines one ink - each line's measured is a little off, and its words would be written as
                // pieces of different looks - but a line clearly of another colour: a blue heading)
                std::map<int, std::array<std::vector<int>, 3>> blockInks;
                for (const auto &line : lines)
                    if (!line.empty() && (line.front().flags & 32) && line.front().color)
                        for (int c = 0; c < 3; ++c)
                            blockInks[0][size_t(c)].push_back(int(line.front().color >> (16 - 8 * c)) & 0xFF);
                for (auto &line : lines) {
                    if (line.empty() || !(line.front().flags & 32) || !line.front().color)
                        continue;
                    auto &bi = blockInks[0];
                    unsigned mid = 0;
                    int apart = 0;
                    for (int c = 0; c < 3; ++c) {
                        std::vector<int> v = bi[size_t(c)];
                        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                        mid = (mid << 8) | unsigned(v[v.size() / 2]);
                        apart += std::abs(v[v.size() / 2] - (int(line.front().color >> (16 - 8 * c)) & 0xFF));
                    }
                    if (apart < 60)
                        for (TextChar &ch : line)
                            if (ch.flags & 32)
                                ch.color = mid;
                }
            }
        }
        fz_drop_display_list(ctx, list);
    }
    {  // (an Arabic page - not a Persian one: digits read back as Persian's - a font's shared shapes - Arabic's, even in
       // a line of just digits)
        bool arabic = false, persian = false;
        for (const auto &line : lines)
            for (const TextChar &ch : line) {
                arabic = arabic || (ch.c >= 0x620 && ch.c <= 0x64A);
                persian = persian || ch.c == 0x67E || ch.c == 0x686 || ch.c == 0x698 || ch.c == 0x6AF || ch.c == 0x6A9 || ch.c == 0x6CC ||
                          (ch.c >= 0x6F4 && ch.c <= 0x6F6);
            }
        if (arabic && !persian)
            for (auto &line : lines)
                for (TextChar &ch : line)
                    if (ch.c >= 0x6F0 && ch.c <= 0x6F9)
                        ch.c = char32_t(0x660 + (ch.c - 0x6F0));
    }
    auto made = std::make_shared<const std::vector<TextChar>>(pagetext::mend(std::move(lines)));
    std::lock_guard guard(textMutex_);
    if (gen == textGen_ && page < int(texts_.size()) && !texts_[size_t(page)])
        texts_[size_t(page)] = made;
    return made;
}

// ---- changes ----

int Document::journalStep() const
{
    if (!pdf_)
        return 0;
    int steps = 0;
    return pdf_undoredo_state(ctx_, pdf_, &steps);
}

bool Document::dirty() const
{
    std::lock_guard guard(docMutex_);
    return pdf_ && journalStep() != savedStep_;
}

bool Document::canUndo() const
{
    std::lock_guard guard(docMutex_);
    return pdf_ && pdf_can_undo(ctx_, pdf_);
}

bool Document::canRedo() const
{
    std::lock_guard guard(docMutex_);
    return pdf_ && pdf_can_redo(ctx_, pdf_);
}

QString Document::undoName() const
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || !pdf_can_undo(ctx_, pdf_))
        return {};
    return QString::fromUtf8(pdf_undoredo_step(ctx_, pdf_, journalStep() - 1));
}

QString Document::redoName() const
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || !pdf_can_redo(ctx_, pdf_))
        return {};
    return QString::fromUtf8(pdf_undoredo_step(ctx_, pdf_, journalStep()));
}

bool Document::undo()
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || !pdf_can_undo(ctx_, pdf_))
        return false;
    ++revision_;
    {  // (the step undone: its page)
        const int step = journalStep();
        changedPage_ = step > 0 && step <= int(stepPage_.size()) ? stepPage_[size_t(step - 1)] : -1;
    }
    bool ok = true;
    fz_try(ctx_) {
        pdf_undo(ctx_, pdf_);
        forgetFonts(ctx_, pdf_);
    }
    fz_catch(ctx_) {
        ok = false;
    }
    measure();
    return ok;
}

bool Document::redo()
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || !pdf_can_redo(ctx_, pdf_))
        return false;
    ++revision_;
    {  // (the step redone: its page)
        const int step = journalStep();
        changedPage_ = step >= 0 && step < int(stepPage_.size()) ? stepPage_[size_t(step)] : -1;
    }
    bool ok = true;
    fz_try(ctx_) {
        pdf_redo(ctx_, pdf_);
        forgetFonts(ctx_, pdf_);
    }
    fz_catch(ctx_) {
        ok = false;
    }
    measure();
    return ok;
}

// One change Undo can take back: fn does it (with MuPDF calls only); if it fails half way,
// everything it did is taken back. (docMutex_ held by the caller.)
bool Document::change(const char *what, const std::function<void()> &fn, QString *error)
{
    if (!pdf_) {
        if (error)
            *error = QStringLiteral("Only PDFs can be changed.");
        return false;
    }
    ++revision_;  // (what's kept of the PDF elsewhere - links read... - is out of date)
    changedPage_ = pendingPage_;
    pendingPage_ = -1;
    setTurn(0, {});  // (a change before that failed half way: not turning)
    if (!changesOpen_ && journalStep() < savedStep_)
        savedStep_ = -1;  // (the saved version's steps are about to be dropped: never clean again)
    bool ok = true;
    const char *failed = nullptr;
    fz_var(failed);
    fz_try(ctx_) {
        pdf_begin_operation(ctx_, pdf_, what);
        fn();
        pdf_end_operation(ctx_, pdf_);
    }
    fz_catch(ctx_) {
        ok = false;
        failed = fz_caught_message(ctx_);
        if (error)
            *error = QString::fromUtf8(failed);
        fz_try(ctx_) {
            pdf_abandon_operation(ctx_, pdf_);
            forgetFonts(ctx_, pdf_);
        }
        fz_catch(ctx_) {
        }
    }
    if (ok && !changesOpen_) {  // (the step it made: its page, for Undo / Redo)
        const int step = journalStep();
        if (step > 0) {
            stepPage_.resize(size_t(step));
            stepPage_[size_t(step - 1)] = changedPage_;
        }
    }
    measure();
    return ok;
}

void Document::beginChanges(const char *what)
{
    std::lock_guard guard(docMutex_);
    if (!pdf_)
        return;
    if (journalStep() < savedStep_)
        savedStep_ = -1;  // (as change does: the saved version's steps about to be dropped)
    fz_try(ctx_) {
        pdf_begin_operation(ctx_, pdf_, what);
        ++changesOpen_;
    }
    fz_catch(ctx_) {
    }
}

void Document::endChanges()
{
    std::lock_guard guard(docMutex_);
    if (!pdf_)
        return;
    if (!changesOpen_)
        return;
    fz_try(ctx_) {
        pdf_end_operation(ctx_, pdf_);
    }
    fz_catch(ctx_) {
    }
    --changesOpen_;
    ++revision_;
    measure();
}

bool Document::rotatePages(const std::vector<int> &pages, int degrees, QString *error)
{
    std::lock_guard guard(docMutex_);
    return change("rotate", [&] {
        for (int p : pages) {
            pdf_obj *obj = pdf_lookup_page_obj(ctx_, pdf_, p);
            const int now = pdf_to_int(ctx_, pdf_dict_get_inheritable(ctx_, obj, PDF_NAME(Rotate)));
            pdf_dict_put_int(ctx_, obj, PDF_NAME(Rotate), ((now + degrees) % 360 + 360) % 360);
        }
    }, error);
}

bool Document::deletePages(const std::vector<int> &pagesIn, QString *error)
{
    std::vector<int> pages = pagesIn;
    std::sort(pages.rbegin(), pages.rend());  // (from the end: the others keep their numbers)
    std::lock_guard guard(docMutex_);
    return change("delete pages", [&] {
        for (int p : pages)
            pdf_delete_page(ctx_, pdf_, p);
    }, error);
}

bool Document::movePages(const std::vector<int> &pagesIn, int to, QString *error)
{
    std::vector<int> pages = pagesIn;
    std::sort(pages.begin(), pages.end());
    const int k = to - int(std::count_if(pages.begin(), pages.end(), [to](int p) { return p < to; }));
    std::vector<pdf_obj *> objs(pages.size(), nullptr);
    std::lock_guard guard(docMutex_);
    const bool ok = change("move pages", [&] {
        for (size_t n = 0; n < pages.size(); ++n)
            objs[n] = pdf_keep_obj(ctx_, pdf_lookup_page_obj(ctx_, pdf_, pages[n]));
        for (size_t n = pages.size(); n-- > 0;)
            pdf_delete_page(ctx_, pdf_, pages[n]);
        for (size_t n = 0; n < objs.size(); ++n)
            pdf_insert_page(ctx_, pdf_, k + int(n), objs[n]);
    }, error);
    for (pdf_obj *o : objs)
        pdf_drop_obj(ctx_, o);
    return ok;
}

bool Document::insertBlank(int at, QSizeF size, QString *error)
{
    std::lock_guard guard(docMutex_);
    pdf_obj *page = nullptr, *res = nullptr;
    fz_buffer *contents = nullptr;
    const bool ok = change("insert a page", [&] {
        const fz_rect box = {0, 0, float(size.width()), float(size.height())};
        res = pdf_new_dict(ctx_, pdf_, 1);
        contents = fz_new_buffer(ctx_, 1);
        page = pdf_add_page(ctx_, pdf_, box, 0, res, contents);
        pdf_insert_page(ctx_, pdf_, at, page);
    }, error);
    pdf_drop_obj(ctx_, page);
    pdf_drop_obj(ctx_, res);
    fz_drop_buffer(ctx_, contents);
    return ok;
}

int Document::insertFiles(const QStringList &paths, int at, QStringList *problems)
{
    // each file read first (pictures MuPDF can't read: through Qt, as PNG)
    struct Source {
        QString name;
        QByteArray data, magic;
        bool pdf = false;
    };
    std::vector<Source> sources;
    for (const QString &path : paths) {
        Source s;
        s.name = path.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (problems)
                *problems << QStringLiteral("%1: %2").arg(s.name, f.errorString());
            continue;
        }
        s.data = f.readAll();
        s.pdf = path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
        s.magic = path.toUtf8();
        sources.push_back(std::move(s));
    }
    std::lock_guard guard(docMutex_);
    if (!pdf_) {
        if (problems)
            *problems << QStringLiteral("Only PDFs can be changed.");
        return 0;
    }
    if (journalStep() < savedStep_)
        savedStep_ = -1;
    int added = 0;
    bool begun = false;
    fz_context *ctx = ctx_;
    fz_try(ctx) {
        pdf_begin_operation(ctx, pdf_, "insert pages");
        begun = true;
    }
    fz_catch(ctx) {
    }
    if (!begun)
        return 0;
    for (Source &s : sources) {
        QByteArray png;
        const char *failed = nullptr;
        int n = 0;
        // a picture MuPDF can't read itself: made a PNG by Qt first
        if (!s.pdf) {
            bool readable = true;
            fz_buffer *b = nullptr;
            fz_image *probe = nullptr;
            fz_var(b);
            fz_var(probe);
            fz_try(ctx) {
                b = fz_new_buffer_from_shared_data(ctx, reinterpret_cast<const unsigned char *>(s.data.constData()),
                                                   size_t(s.data.size()));
                probe = fz_new_image_from_buffer(ctx, b);
            }
            fz_always(ctx) {
                fz_drop_image(ctx, probe);
                fz_drop_buffer(ctx, b);
            }
            fz_catch(ctx) {
                readable = false;
            }
            if (!readable) {
                QImage im;
                if (im.loadFromData(s.data)) {
                    QBuffer out(&png);
                    out.open(QIODevice::WriteOnly);
                    im.save(&out, "PNG");
                    s.data = png;
                }
            }
        }
        fz_buffer *buf = nullptr;
        fz_document *src = nullptr;
        pdf_graft_map *map = nullptr;
        fz_image *img = nullptr;
        pdf_obj *res = nullptr, *xobj = nullptr, *ref = nullptr, *page = nullptr;
        fz_buffer *contents = nullptr;
        fz_var(buf);
        fz_var(src);
        fz_var(map);
        fz_var(img);
        fz_var(res);
        fz_var(ref);
        fz_var(page);
        fz_var(contents);
        fz_var(n);
        fz_try(ctx) {
            buf = fz_new_buffer_from_shared_data(ctx, reinterpret_cast<const unsigned char *>(s.data.constData()),
                                                 size_t(s.data.size()));
            if (s.pdf) {
                src = fz_open_document_with_buffer(ctx, "application/pdf", buf);
                if (fz_needs_password(ctx, src))
                    fz_throw(ctx, FZ_ERROR_GENERIC, "is locked with a password");
                pdf_document *sp = pdf_document_from_fz_document(ctx, src);
                if (!sp)
                    fz_throw(ctx, FZ_ERROR_GENERIC, "isn't a PDF");
                map = pdf_new_graft_map(ctx, pdf_);
                const int count = pdf_count_pages(ctx, sp);
                for (int i = 0; i < count; ++i) {
                    pdf_graft_mapped_page(ctx, map, at + added + i, sp, i);
                    ++n;
                }
            } else {  // a picture: a page its own size (at its resolution; 96 dpi if it says 72 or less)
                img = fz_new_image_from_buffer(ctx, buf);
                int xres = 96, yres = 96;
                fz_image_resolution(img, &xres, &yres);
                const float dx = xres > 72 ? float(xres) : 96.f, dy = yres > 72 ? float(yres) : 96.f;
                float w = img->w * 72 / dx, h = img->h * 72 / dy;
                const int orient = fz_image_orientation(ctx, img);
                if (orient >= 5 && orient <= 8)
                    std::swap(w, h);
                ref = pdf_add_image(ctx, pdf_, img);
                res = pdf_new_dict(ctx, pdf_, 1);
                xobj = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1);
                pdf_dict_puts(ctx, xobj, "Img", ref);
                // the picture the right way up, filling the page (the PDF's y goes up)
                const fz_matrix flipImage = {1, 0, 0, -1, 0, 1};
                const fz_matrix flipPage = {1, 0, 0, -1, 0, h};
                const fz_matrix m = fz_concat(fz_concat(flipImage, fz_concat(fz_image_orientation_matrix(ctx, img),
                                                                             fz_scale(w, h))),
                                              flipPage);
                contents = fz_new_buffer(ctx, 64);
                fz_append_printf(ctx, contents, "q %g %g %g %g %g %g cm /Img Do Q", m.a, m.b, m.c, m.d, m.e, m.f);
                page = pdf_add_page(ctx, pdf_, fz_make_rect(0, 0, w, h), 0, res, contents);
                pdf_insert_page(ctx, pdf_, at + added, page);
                n = 1;
            }
        }
        fz_always(ctx) {
            pdf_drop_graft_map(ctx, map);
            fz_drop_document(ctx, src);
            fz_drop_buffer(ctx, buf);
            fz_drop_image(ctx, img);
            pdf_drop_obj(ctx, res);
            pdf_drop_obj(ctx, ref);
            pdf_drop_obj(ctx, page);
            fz_drop_buffer(ctx, contents);
        }
        fz_catch(ctx) {
            failed = fz_caught_message(ctx);
        }
        if (failed) {
            if (problems)
                *problems << QStringLiteral("%1: %2").arg(s.name, QString::fromUtf8(failed));
            // (a PDF that went in part way: its pages stay; the count says how many)
        }
        added += n;
    }
    fz_try(ctx) {
        if (added)
            pdf_end_operation(ctx, pdf_);
        else
            pdf_abandon_operation(ctx, pdf_);
    }
    fz_catch(ctx) {
    }
    measure();
    return added;
}

bool Document::extractPages(const std::vector<int> &pages, const QString &path, QString *error) const
{
    const QString tmp = path + QStringLiteral(".saving");
    const QByteArray tmpName = tmp.toUtf8();
    std::lock_guard guard(docMutex_);
    if (!pdf_) {
        if (error)
            *error = QStringLiteral("Only PDFs can be changed.");
        return false;
    }
    fz_context *ctx = ctx_;
    pdf_document *dst = nullptr;
    pdf_graft_map *map = nullptr;
    bool ok = true;
    const char *failed = nullptr;
    fz_var(dst);
    fz_var(map);
    fz_try(ctx) {
        dst = pdf_create_document(ctx);
        map = pdf_new_graft_map(ctx, dst);
        for (int p : pages)
            pdf_graft_mapped_page(ctx, map, -1, pdf_, p);
        const pdf_write_options opts = fullSave();
        pdf_save_document(ctx, dst, tmpName.constData(), &opts);
    }
    fz_always(ctx) {
        pdf_drop_graft_map(ctx, map);
        pdf_drop_document(ctx, dst);
    }
    fz_catch(ctx) {
        ok = false;
        failed = fz_caught_message(ctx);
    }
    if (!ok) {
        if (error)
            *error = QString::fromUtf8(failed);
        QFile::remove(tmp);
        return false;
    }
    return replaceFile(tmp, path, error);
}

bool Document::save(const QString &path, QString *error)
{
    const QString tmp = path + QStringLiteral(".saving");
    const QByteArray tmpName = tmp.toUtf8();
    std::lock_guard guard(docMutex_);
    if (!pdf_) {
        if (error)
            *error = QStringLiteral("Only PDFs can be saved.");
        return false;
    }
    // written out as it is now (the open document itself isn't touched, so Undo still works
    // after saving), then that copy cleaned up and compressed into the file
    fz_context *ctx = ctx_;
    fz_buffer *buf = nullptr;
    fz_output *out = nullptr;
    fz_stream *stm = nullptr;
    pdf_document *copy = nullptr;
    bool ok = true;
    const char *failed = nullptr;
    fz_var(buf);
    fz_var(out);
    fz_var(stm);
    fz_var(copy);
    fz_try(ctx) {
        buf = fz_new_buffer(ctx, 1 << 16);
        out = fz_new_output_with_buffer(ctx, buf);
        pdf_write_options plain = pdf_default_write_options;
        pdf_write_document(ctx, pdf_, out, &plain);
        fz_close_output(ctx, out);
        stm = fz_open_buffer(ctx, buf);
        copy = pdf_open_document_with_stream(ctx, stm);
        const pdf_write_options opts = fullSave();
        pdf_save_document(ctx, copy, tmpName.constData(), &opts);
    }
    fz_always(ctx) {
        pdf_drop_document(ctx, copy);
        fz_drop_stream(ctx, stm);
        fz_drop_output(ctx, out);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        ok = false;
        failed = fz_caught_message(ctx);
    }
    if (!ok) {
        if (error)
            *error = QString::fromUtf8(failed);
        QFile::remove(tmp);
        return false;
    }
    if (!replaceFile(tmp, path, error))
        return false;
    path_ = path;
    savedStep_ = journalStep();
    return true;
}

// ---- the page's straight lines and boxes (for the exports) ----

namespace {

struct ArtGrab {
    fz_device super;
    std::vector<LineArt> *out;
    std::vector<QRectF> *drawings;  // (where it draws curves, slanted lines, pictures, shading)
    std::vector<QPointF> *hidden;   // (letters drawn outside what's shown - clipped away)
    QPointF origin;
    std::vector<fz_rect> clips;     // (what's shown now: the clips' boxes, one in another)
    fz_rect mask = fz_infinite_rect;
    void drew(fz_rect r)
    {
        if (drawings && !fz_is_empty_rect(r) && !fz_is_infinite_rect(r))
            drawings->push_back(QRectF(QPointF(r.x0, r.y0) - origin, QPointF(r.x1, r.y1) - origin));
    }
    void clip(fz_rect r) { clips.push_back(clips.empty() ? r : fz_intersect_rect(clips.back(), r)); }
    void letters(const fz_text *text, fz_matrix m)
    {
        if (!hidden || clips.empty())
            return;
        const fz_rect shown = clips.back();
        for (const fz_text_span *sp = text->head; sp; sp = sp->next)
            for (int i = 0; i < sp->len; ++i) {
                const fz_point q = fz_transform_point_xy(sp->items[i].x, sp->items[i].y, m);
                if (q.x < shown.x0 - 1 || q.x > shown.x1 + 1 || q.y < shown.y0 - 1 || q.y > shown.y1 + 1)
                    hidden->push_back(QPointF(q.x, q.y) - origin);
            }
    }
};

// a path's pieces as its corners (a curve: the piece left out - letters, logos, round corners)
struct Walked {
    std::vector<std::vector<fz_point>> pieces;
    bool curved = false;
};

Walked walk(fz_context *ctx, const fz_path *path)
{
    Walked w;
    fz_path_walker walker = {};
    walker.moveto = [](fz_context *, void *a, float x, float y) { static_cast<Walked *>(a)->pieces.push_back({{x, y}}); };
    walker.lineto = [](fz_context *, void *a, float x, float y) {
        auto &p = static_cast<Walked *>(a)->pieces;
        if (p.empty())
            p.push_back({});
        p.back().push_back({x, y});
    };
    walker.curveto = [](fz_context *, void *a, float, float, float, float, float, float) { static_cast<Walked *>(a)->curved = true; };
    walker.closepath = [](fz_context *, void *a) {
        auto &p = static_cast<Walked *>(a)->pieces;
        if (!p.empty() && p.back().size() > 1)
            p.back().push_back(p.back().front());
    };
    walker.rectto = [](fz_context *, void *a, float x0, float y0, float x1, float y1) {
        static_cast<Walked *>(a)->pieces.push_back({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}});
    };
    fz_walk_path(ctx, path, &walker, &w);
    return w;
}

QColor rgbOf(fz_context *ctx, fz_colorspace *cs, const float *color, fz_color_params params)
{
    if (!cs || !color)
        return {};
    float rgb[3] = {0, 0, 0};
    fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, nullptr, params);
    return QColor::fromRgbF(std::clamp(rgb[0], 0.f, 1.f), std::clamp(rgb[1], 0.f, 1.f), std::clamp(rgb[2], 0.f, 1.f));
}

}  // namespace

std::vector<LineArt> Document::lineArt(int page, std::vector<QRectF> *drawings, std::vector<QPointF> *hidden)
{
    std::vector<LineArt> out;
    QPointF o;
    fz_display_list *list = keepDisplayList(page, &o);
    if (!list)
        return out;
    fz_context *ctx = threadContext();
    fz_device *dev = nullptr;
    fz_var(dev);
    fz_try(ctx) {
        auto *d = fz_new_derived_device(ctx, ArtGrab);
        d->out = &out;
        d->drawings = drawings;
        d->hidden = hidden;
        d->origin = o;
        new (&d->clips) std::vector<fz_rect>();  // (the device's memory is MuPDF's: its C++ member made here, undone below)
        d->mask = fz_infinite_rect;
        // filled: each piece that's a box, across and down
        d->super.fill_path = [](fz_context *ctx, fz_device *dv, const fz_path *path, int, fz_matrix m, fz_colorspace *cs, const float *color,
                                float alpha, fz_color_params params) {
            auto *g = reinterpret_cast<ArtGrab *>(dv);
            if (alpha < 0.1f)
                return;
            const Walked w = walk(ctx, path);
            const QColor c = rgbOf(ctx, cs, color, params);
            if (w.curved) {
                g->drew(fz_bound_path(ctx, path, nullptr, m));
                return;
            }
            if (!c.isValid())
                return;
            std::vector<QRectF> boxes;
            for (const auto &piece : w.pieces) {
                if (piece.size() < 4 || piece.size() > 5)
                    continue;
                QRectF box;
                bool square = true;
                for (size_t i = 0; i < piece.size(); ++i) {
                    const fz_point a = fz_transform_point(piece[i], m), b = fz_transform_point(piece[(i + 1) % piece.size()], m);
                    square = square && (std::abs(a.x - b.x) < 0.3f || std::abs(a.y - b.y) < 0.3f);
                    box |= QRectF(QPointF(a.x, a.y) - g->origin, QSizeF(0.001, 0.001));
                }
                if (square && box.width() > 0.05 && box.height() > 0.05)
                    boxes.push_back(box);
                else if (!square)  // (an arrow's head, a triangle...)
                    g->drew(fz_bound_path(ctx, path, nullptr, m));
            }
            // (a box with a box inside it: a hole - a frame, drawn as a ring - its four sides; boxes in boxes in boxes:
            // filled, hole, filled... - rings in rings)
            std::vector<int> depth(boxes.size());
            for (size_t i = 0; i < boxes.size(); ++i)
                for (size_t j = 0; j < boxes.size(); ++j)
                    if (i != j && boxes[j].contains(boxes[i]) && boxes[j] != boxes[i])
                        ++depth[i];
            for (size_t j = 0; j < boxes.size(); ++j) {
                if (depth[j] % 2)
                    continue;
                const QRectF &o = boxes[j];
                bool ring = false;
                for (size_t i = 0; i < boxes.size(); ++i) {
                    if (depth[i] != depth[j] + 1 || !o.contains(boxes[i]))
                        continue;
                    const QRectF &h = boxes[i];
                    for (const QRectF &side : {QRectF(QPointF(o.left(), o.top()), QPointF(o.right(), h.top())),
                                               QRectF(QPointF(o.left(), h.bottom()), QPointF(o.right(), o.bottom())),
                                               QRectF(QPointF(o.left(), h.top()), QPointF(h.left(), h.bottom())),
                                               QRectF(QPointF(h.right(), h.top()), QPointF(o.right(), h.bottom()))})
                        if (side.width() > 0.05 && side.height() > 0.05)
                            g->out->push_back({side, true, c});
                    ring = true;
                }
                if (!ring)
                    g->out->push_back({o, true, c});
            }
        };
        // stroked: each piece's straight lines across and down, as wide as they're drawn
        d->super.stroke_path = [](fz_context *ctx, fz_device *dv, const fz_path *path, const fz_stroke_state *st, fz_matrix m,
                                  fz_colorspace *cs, const float *color, float alpha, fz_color_params params) {
            auto *g = reinterpret_cast<ArtGrab *>(dv);
            if (alpha < 0.1f)
                return;
            const Walked w = walk(ctx, path);
            const QColor c = rgbOf(ctx, cs, color, params);
            if (!c.isValid())
                return;
            const double half = std::max(0.25, double(st ? st->linewidth : 1) * fz_matrix_expansion(m) / 2);
            bool slanted = w.curved;
            for (const auto &piece : w.pieces)
                for (size_t i = 0; i + 1 < piece.size(); ++i) {
                    const fz_point a = fz_transform_point(piece[i], m), b = fz_transform_point(piece[i + 1], m);
                    const QPointF p = QPointF(a.x, a.y) - g->origin, q = QPointF(b.x, b.y) - g->origin;
                    if (std::abs(p.y() - q.y()) < 0.3)
                        g->out->push_back({QRectF(QPointF(std::min(p.x(), q.x()) - half, p.y() - half), QPointF(std::max(p.x(), q.x()) + half, p.y() + half)), false, c});
                    else if (std::abs(p.x() - q.x()) < 0.3)
                        g->out->push_back({QRectF(QPointF(p.x() - half, std::min(p.y(), q.y()) - half), QPointF(p.x() + half, std::max(p.y(), q.y()) + half)), false, c});
                    else
                        slanted = true;
                }
            if (slanted)
                g->drew(fz_bound_path(ctx, path, st, m));
        };
        // (the clips: what's drawn after them shows only inside them - text outside, clipped away, isn't seen)
        d->super.clip_path = [](fz_context *ctx, fz_device *dv, const fz_path *path, int, fz_matrix m, fz_rect) {
            reinterpret_cast<ArtGrab *>(dv)->clip(fz_bound_path(ctx, path, nullptr, m));
        };
        d->super.clip_stroke_path = [](fz_context *ctx, fz_device *dv, const fz_path *path, const fz_stroke_state *st, fz_matrix m, fz_rect) {
            reinterpret_cast<ArtGrab *>(dv)->clip(fz_bound_path(ctx, path, st, m));
        };
        d->super.clip_text = [](fz_context *ctx, fz_device *dv, const fz_text *text, fz_matrix m, fz_rect) {
            reinterpret_cast<ArtGrab *>(dv)->clip(fz_bound_text(ctx, text, nullptr, m));
        };
        d->super.clip_stroke_text = [](fz_context *ctx, fz_device *dv, const fz_text *text, const fz_stroke_state *st, fz_matrix m, fz_rect) {
            reinterpret_cast<ArtGrab *>(dv)->clip(fz_bound_text(ctx, text, st, m));
        };
        d->super.clip_image_mask = [](fz_context *, fz_device *dv, fz_image *, fz_matrix m, fz_rect) {
            reinterpret_cast<ArtGrab *>(dv)->clip(fz_transform_rect(fz_unit_rect, m));
        };
        d->super.begin_mask = [](fz_context *, fz_device *dv, fz_rect area, int, fz_colorspace *, const float *, fz_color_params) {
            reinterpret_cast<ArtGrab *>(dv)->mask = area;
        };
        d->super.end_mask = [](fz_context *, fz_device *dv, fz_function *) {
            auto *g = reinterpret_cast<ArtGrab *>(dv);
            g->clip(g->mask);
        };
        d->super.pop_clip = [](fz_context *, fz_device *dv) {
            auto *g = reinterpret_cast<ArtGrab *>(dv);
            if (!g->clips.empty())
                g->clips.pop_back();
        };
        d->super.fill_text = [](fz_context *, fz_device *dv, const fz_text *t, fz_matrix m, fz_colorspace *, const float *, float, fz_color_params) {
            reinterpret_cast<ArtGrab *>(dv)->letters(t, m);
        };
        d->super.stroke_text = [](fz_context *, fz_device *dv, const fz_text *t, const fz_stroke_state *, fz_matrix m, fz_colorspace *, const float *,
                                  float, fz_color_params) { reinterpret_cast<ArtGrab *>(dv)->letters(t, m); };
        // (pictures, shading: where they are)
        d->super.fill_image = [](fz_context *, fz_device *dv, fz_image *, fz_matrix m, float alpha, fz_color_params) {
            if (alpha >= 0.1f)
                reinterpret_cast<ArtGrab *>(dv)->drew(fz_transform_rect(fz_unit_rect, m));
        };
        d->super.fill_image_mask = [](fz_context *, fz_device *dv, fz_image *, fz_matrix m, fz_colorspace *, const float *, float alpha,
                                      fz_color_params) {
            if (alpha >= 0.1f)
                reinterpret_cast<ArtGrab *>(dv)->drew(fz_transform_rect(fz_unit_rect, m));
        };
        d->super.fill_shade = [](fz_context *ctx, fz_device *dv, fz_shade *shade, fz_matrix m, float alpha, fz_color_params) {
            if (alpha >= 0.1f)
                reinterpret_cast<ArtGrab *>(dv)->drew(fz_bound_shade(ctx, shade, m));
        };
        dev = &d->super;
        fz_run_display_list(ctx, list, dev, fz_identity, fz_infinite_rect, nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        if (dev)
            reinterpret_cast<ArtGrab *>(dev)->clips.~vector();
        fz_drop_device(ctx, dev);
        fz_drop_display_list(ctx, list);
    }
    fz_catch(ctx) {
        out.clear();
    }
    return out;
}
