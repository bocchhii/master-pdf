// Document's annotations: read for the window, and changed (each change one step for Undo).
// (No C++ objects are made inside fz_try blocks or the functions run in them: MuPDF's errors
// jump out past anything that would need cleaning up. What's needed is worked out first.)

#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QImage>
#include <QPainter>
#include <QPainterPath>

namespace {

bool isMarkup(const QString &k)
{
    return k == QLatin1String("Highlight") || k == QLatin1String("StrikeOut") ||
           k == QLatin1String("Underline") || k == QLatin1String("Squiggly");
}

QColor toQColor(int n, const float *c)
{
    if (n == 1)
        return QColor::fromRgbF(c[0], c[0], c[0]);
    if (n == 3)
        return QColor::fromRgbF(c[0], c[1], c[2]);
    if (n == 4)
        return QColor::fromCmykF(c[0], c[1], c[2], c[3]).toRgb();
    return {};
}

struct Rgb {
    float v[3];
};
Rgb rgb(const QColor &c)
{
    return {{float(c.redF()), float(c.greenF()), float(c.blueF())}};
}

pdf_annot *findAnnot(fz_context *ctx, pdf_page *page, int id)
{
    for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
        if (pdf_to_num(ctx, pdf_annot_obj(ctx, a)) == id)
            return a;
    fz_throw(ctx, FZ_ERROR_GENERIC, "the annotation isn't there any more");
}

// A marker stroke multiplies with the page, like a real highlighter: the text under it stays
// black. (MuPDF draws the stroke's appearance; its graphics states are told to multiply.)
void multiplyBlend(fz_context *ctx, pdf_document *doc, pdf_annot *annot)
{
    pdf_obj *ap = pdf_annot_ap(ctx, annot);
    if (!ap)
        return;
    pdf_obj *res = pdf_dict_get(ctx, ap, PDF_NAME(Resources));
    if (!res)
        res = pdf_dict_put_dict(ctx, ap, PDF_NAME(Resources), 1);
    pdf_obj *states = pdf_dict_get(ctx, res, PDF_NAME(ExtGState));
    if (!states)
        states = pdf_dict_put_dict(ctx, res, PDF_NAME(ExtGState), 1);
    for (int i = 0; i < pdf_dict_len(ctx, states); ++i)
        pdf_dict_put(ctx, pdf_dict_get_val(ctx, states, i), PDF_NAME(BM), PDF_NAME(Multiply));
    pdf_obj *mine = pdf_dict_puts_dict(ctx, states, "MPm", 1);
    pdf_dict_put(ctx, mine, PDF_NAME(BM), PDF_NAME(Multiply));
    fz_buffer *old = nullptr, *now = nullptr;
    fz_var(old);
    fz_var(now);
    fz_try(ctx) {
        old = pdf_load_stream(ctx, ap);
        now = fz_new_buffer(ctx, fz_buffer_storage(ctx, old, nullptr) + 16);
        fz_append_string(ctx, now, "/MPm gs\n");
        fz_append_buffer(ctx, now, old);
        pdf_update_stream(ctx, doc, ap, now, 0);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, old);
        fz_drop_buffer(ctx, now);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

bool isMarker(fz_context *ctx, pdf_annot *a)
{
    const char *subj = pdf_to_text_string(ctx, pdf_dict_gets(ctx, pdf_annot_obj(ctx, a), "Subj"));
    return subj && !strcmp(subj, "Marker");
}

bool sameLine(const QRectF &a, const QRectF &b)
{
    return std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top()) >= 0.5 * std::min(a.height(), b.height());
}

// what's left of a mark's piece with the parts under holes (on its line) taken out
std::vector<QRectF> cutOut(const QRectF &rect, const std::vector<QRectF> &holes)
{
    std::vector<QRectF> pieces{rect};
    for (const QRectF &h : holes) {
        if (!sameLine(rect, h))
            continue;
        std::vector<QRectF> rest;
        for (const QRectF &r : pieces) {
            if (h.right() <= r.left() || h.left() >= r.right()) {
                rest.push_back(r);
                continue;
            }
            if (h.left() - r.left() > 0.5)
                rest.emplace_back(QPointF(r.left(), r.top()), QPointF(h.left(), r.bottom()));
            if (r.right() - h.right() > 0.5)
                rest.emplace_back(QPointF(h.right(), r.top()), QPointF(r.right(), r.bottom()));
        }
        pieces = std::move(rest);
    }
    return pieces;
}


// ---- the eraser: exactly what it went over taken out of a drawing - a clip at the start of its
// appearance, all of it but the places rubbed (their outline, kept on the drawing in PDF space, all
// the rubs so far as one shape; moved with it). (A soft mask, as the Python app had, MuPDF doesn't
// apply to an annotation's appearance - and its group lost the marker's multiply.) ----

bool erasable(enum pdf_annot_type t)
{
    return t == PDF_ANNOT_INK || t == PDF_ANNOT_LINE || t == PDF_ANNOT_SQUARE || t == PDF_ANNOT_CIRCLE || t == PDF_ANNOT_POLY_LINE ||
           t == PDF_ANNOT_POLYGON;
}

// the appearance that draws it, its box, and the matrix from its own space to the page's PDF space
// (PDF puts its box, so transformed, onto the annotation's Rect)
pdf_obj *appearanceOf(fz_context *ctx, pdf_obj *annot, fz_rect *box, fz_matrix *toPdf)
{
    pdf_obj *form = pdf_dict_getp(ctx, annot, "AP/N");
    if (!pdf_is_stream(ctx, form))
        return nullptr;
    *box = pdf_dict_get_rect(ctx, form, PDF_NAME(BBox));
    const fz_matrix m = pdf_dict_get_matrix(ctx, form, PDF_NAME(Matrix));
    const fz_rect rect = pdf_dict_get_rect(ctx, annot, PDF_NAME(Rect));
    const fz_rect shown = fz_transform_rect(*box, m);
    const float w = shown.x1 - shown.x0, h = shown.y1 - shown.y0;
    const float sx = w > 0 ? (rect.x1 - rect.x0) / w : 1, sy = h > 0 ? (rect.y1 - rect.y0) / h : 1;
    *toPdf = fz_concat(m, fz_make_matrix(sx, 0, 0, sy, rect.x0 - shown.x0 * sx, rect.y0 - shown.y0 * sy));
    return form;
}

const char *const kClipStart = "%MPDF erased\n", *const kClipEnd = "%MPDF erased end\n";

// the clip put at the start of its appearance (again, after its appearance was made anew - moved,
// recoloured): its box, and the places rubbed (even-odd: those left out) - shifted by MPDFShift
void applyErased(fz_context *ctx, pdf_document *doc, pdf_annot *annot)
{
    pdf_obj *obj = pdf_annot_obj(ctx, annot);
    pdf_obj *erased = pdf_dict_gets(ctx, obj, "MPDFErased");
    if (!pdf_is_stream(ctx, erased))
        return;
    fz_rect box;
    fz_matrix toPdf;
    pdf_obj *form = appearanceOf(ctx, obj, &box, &toPdf);
    if (!form)
        return;
    pdf_obj *shift = pdf_dict_gets(ctx, obj, "MPDFShift");
    const float dx = pdf_array_get_real(ctx, shift, 0), dy = pdf_array_get_real(ctx, shift, 1);
    const fz_matrix back = fz_invert_matrix(toPdf);
    fz_buffer *path = nullptr, *old = nullptr, *now = nullptr;
    fz_var(path);
    fz_var(old);
    fz_var(now);
    fz_try(ctx) {
        path = pdf_load_stream(ctx, erased);
        old = pdf_load_stream(ctx, form);
        unsigned char *data = nullptr;
        size_t len = fz_buffer_storage(ctx, old, &data);
        // (a clip put there before: taken off - the appearance's own drawing after it)
        size_t from = 0;
        if (len >= strlen(kClipStart) && memcmp(data, kClipStart, strlen(kClipStart)) == 0) {
            for (size_t k = 0; k + strlen(kClipEnd) <= len; ++k)
                if (memcmp(data + k, kClipEnd, strlen(kClipEnd)) == 0) {
                    from = k + strlen(kClipEnd);
                    break;
                }
        }
        now = fz_new_buffer(ctx, len + 256 + fz_buffer_storage(ctx, path, nullptr));
        fz_append_string(ctx, now, kClipStart);
        // (in PDF space - moved as it was - and back to the appearance's own)
        fz_append_printf(ctx, now, "%g %g %g %g %g %g cm 1 0 0 1 %g %g cm\n", back.a, back.b, back.c, back.d, back.e, back.f, dx, dy);
        const fz_rect all = fz_transform_rect(box, toPdf);
        fz_append_printf(ctx, now, "%g %g %g %g re\n", all.x0 - dx - 50, all.y0 - dy - 50, all.x1 - all.x0 + 100, all.y1 - all.y0 + 100);
        fz_append_buffer(ctx, now, path);
        fz_append_printf(ctx, now, "\nW* n 1 0 0 1 %g %g cm %g %g %g %g %g %g cm\n", -dx, -dy, toPdf.a, toPdf.b, toPdf.c, toPdf.d, toPdf.e, toPdf.f);
        fz_append_string(ctx, now, kClipEnd);
        fz_append_data(ctx, now, data + from, len - from);
        pdf_update_stream(ctx, doc, form, now, 0);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, path);
        fz_drop_buffer(ctx, old);
        fz_drop_buffer(ctx, now);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// moved (dx, dy in PDF space) / recoloured - its appearance made anew: what was rubbed out of it
// goes with it
void keepErased(fz_context *ctx, pdf_document *doc, pdf_annot *annot, float dx, float dy)
{
    pdf_obj *obj = pdf_annot_obj(ctx, annot);
    if (!pdf_is_stream(ctx, pdf_dict_gets(ctx, obj, "MPDFErased")))
        return;
    pdf_obj *shift = pdf_dict_gets(ctx, obj, "MPDFShift");
    const float x = pdf_array_get_real(ctx, shift, 0) + dx, y = pdf_array_get_real(ctx, shift, 1) + dy;
    pdf_obj *arr = pdf_new_array(ctx, doc, 2);
    pdf_array_push_real(ctx, arr, x);
    pdf_array_push_real(ctx, arr, y);
    pdf_dict_puts_drop(ctx, obj, "MPDFShift", arr);
    applyErased(ctx, doc, annot);
}

}  // namespace

bool Annot::markup() const
{
    return isMarkup(kind);
}

bool Annot::movable() const
{
    static const char *kinds[] = {"FreeText", "Text", "Ink", "Square", "Circle", "Line", "PolyLine", "Polygon"};
    for (const char *k : kinds)
        if (kind == QLatin1String(k))
            return true;
    return false;
}

bool Document::onPage(int page, const char *what, const std::function<void(pdf_page *)> &fn, QString *error)
{
    // (docMutex_ held)
    if (page < 0 || page >= int(lists_.size())) {
        if (error)
            *error = QStringLiteral("There's no such page.");
        return false;
    }
    pendingPage_ = page;  // (the change is on this page alone)
    return change(what, [&] {
        pdf_page *pg = nullptr;
        fz_var(pg);
        fz_try(ctx_) {
            pg = pdf_load_page(ctx_, pdf_, page);
            fn(pg);
        }
        fz_always(ctx_) {
            fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

std::vector<Annot> Document::annots(int page)
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || page < 0 || page >= int(lists_.size()))
        return {};
    if (annots_.size() != lists_.size())
        annots_.assign(lists_.size(), nullptr);
    if (annots_[size_t(page)])
        return *annots_[size_t(page)];
    auto list = std::make_shared<std::vector<Annot>>();
    const QPointF o = geometry()->origins[size_t(page)];
    pdf_page *pg = nullptr;
    fz_var(pg);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        for (pdf_annot *a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a)) {
            const enum pdf_annot_type type = pdf_annot_type(ctx_, a);
            if (type == PDF_ANNOT_LINK || type == PDF_ANNOT_POPUP || type == PDF_ANNOT_WIDGET)
                continue;
            list->emplace_back();
            Annot &x = list->back();
            x.id = pdf_to_num(ctx_, pdf_annot_obj(ctx_, a));
            x.kind = QString::fromLatin1(pdf_string_from_annot_type(ctx_, type));
            const fz_rect r = pdf_bound_annot(ctx_, a);
            x.rect = QRectF(QPointF(r.x0 - o.x(), r.y0 - o.y()), QPointF(r.x1 - o.x(), r.y1 - o.y()));
            int n = 0;
            float c[4] = {0, 0, 0, 0};
            pdf_annot_color(ctx_, a, &n, c);
            x.color = toQColor(n, c);
            x.contents = QString::fromUtf8(pdf_annot_contents(ctx_, a));
            const char *subj = pdf_to_text_string(ctx_, pdf_dict_gets(ctx_, pdf_annot_obj(ctx_, a), "Subj"));
            x.subject = QString::fromUtf8(subj ? subj : "");
            if (pdf_annot_has_quad_points(ctx_, a))
                for (int q = 0; q < pdf_annot_quad_point_count(ctx_, a); ++q) {
                    const fz_rect p = fz_rect_from_quad(pdf_annot_quad_point(ctx_, a, q));
                    x.pieces.emplace_back(QPointF(p.x0 - o.x(), p.y0 - o.y()), QPointF(p.x1 - o.x(), p.y1 - o.y()));
                }
            if (type == PDF_ANNOT_FREE_TEXT) {
                const char *font = nullptr;
                float size = 12, tc[4] = {0, 0, 0, 0};
                int tn = 0;
                pdf_annot_default_appearance(ctx_, a, &font, &size, &tn, tc);
                x.fontSize = size;
                x.textColor = toQColor(tn, tc);
            }
        }
    }
    fz_always(ctx_) {
        fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
    }
    annots_[size_t(page)] = list;
    return *list;
}

bool Document::addMarkup(int page, const QString &kind, const std::vector<QRectF> &boxes, QColor color,
                         double opacity, QString *error)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    std::vector<fz_quad> quads;
    for (const QRectF &b : boxes)
        quads.push_back(fz_quad_from_rect(fz_make_rect(float(b.left() + o.x()), float(b.top() + o.y()),
                                                       float(b.right() + o.x()), float(b.bottom() + o.y()))));
    const enum pdf_annot_type type = kind == "Highlight" ? PDF_ANNOT_HIGHLIGHT
                                     : kind == "StrikeOut" ? PDF_ANNOT_STRIKE_OUT
                                     : kind == "Underline" ? PDF_ANNOT_UNDERLINE : PDF_ANNOT_SQUIGGLY;
    const char *what = kind == "Highlight" ? "highlight" : kind == "StrikeOut" ? "strike out"
                       : kind == "Underline" ? "underline" : "squiggly underline";
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, what, [&](pdf_page *pg) {
        pdf_annot *a = pdf_create_annot(ctx_, pg, type);
        fz_try(ctx_) {
            for (const fz_quad &q : quads)
                pdf_add_annot_quad_point(ctx_, a, q);
            pdf_set_annot_color(ctx_, a, 3, c.v);
            if (opacity < 1)
                pdf_set_annot_opacity(ctx_, a, float(opacity));
            pdf_update_annot(ctx_, a);
        }
        fz_always(ctx_) {
            pdf_drop_annot(ctx_, a);
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

bool Document::addInk(int page, const std::vector<QPointF> &points, QColor color, double width, double opacity,
                      bool marker, QString *error)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    std::vector<fz_point> pts;
    for (const QPointF &p : points)
        pts.push_back(fz_make_point(float(p.x() + o.x()), float(p.y() + o.y())));
    if (pts.size() == 1)  // (a click: a dot)
        pts.push_back(fz_make_point(pts[0].x + 0.5f, pts[0].y));
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, marker ? "marker" : "draw", [&](pdf_page *pg) {
        pdf_annot *a = pdf_create_annot(ctx_, pg, PDF_ANNOT_INK);
        fz_try(ctx_) {
            pdf_clear_annot_ink_list(ctx_, a);
            pdf_add_annot_ink_list(ctx_, a, int(pts.size()), pts.data());
            pdf_set_annot_color(ctx_, a, 3, c.v);
            pdf_set_annot_border_width(ctx_, a, float(width));
            if (opacity < 1)
                pdf_set_annot_opacity(ctx_, a, float(opacity));
            if (marker)
                pdf_dict_puts_drop(ctx_, pdf_annot_obj(ctx_, a), "Subj", pdf_new_text_string(ctx_, "Marker"));
            pdf_update_annot(ctx_, a);
            if (marker)
                multiplyBlend(ctx_, pdf_, a);
        }
        fz_always(ctx_) {
            pdf_drop_annot(ctx_, a);
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

bool Document::addShape(int page, const QString &kind, QPointF a, QPointF b, QColor color, double width,
                        double opacity, QString *error)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    const fz_point p0 = fz_make_point(float(a.x() + o.x()), float(a.y() + o.y()));
    const fz_point p1 = fz_make_point(float(b.x() + o.x()), float(b.y() + o.y()));
    const fz_rect box = fz_make_rect(std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x),
                                     std::max(p0.y, p1.y));
    const bool rect = kind == "rect", ellipse = kind == "ellipse", arrow = kind == "arrow";
    if ((rect || ellipse) && (box.x1 - box.x0 < 1 || box.y1 - box.y0 < 1))
        return false;
    const char *what = rect ? "draw a rectangle" : ellipse ? "draw an ellipse" : arrow ? "draw an arrow" : "draw a line";
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, what, [&](pdf_page *pg) {
        pdf_annot *an = pdf_create_annot(ctx_, pg, rect ? PDF_ANNOT_SQUARE : ellipse ? PDF_ANNOT_CIRCLE : PDF_ANNOT_LINE);
        fz_try(ctx_) {
            if (rect || ellipse) {
                pdf_set_annot_rect(ctx_, an, box);
            } else {
                pdf_set_annot_line(ctx_, an, p0, p1);
                pdf_set_annot_line_ending_styles(ctx_, an, PDF_ANNOT_LE_NONE,
                                                 arrow ? PDF_ANNOT_LE_CLOSED_ARROW : PDF_ANNOT_LE_NONE);
                if (arrow)
                    pdf_set_annot_interior_color(ctx_, an, 3, c.v);
            }
            pdf_set_annot_color(ctx_, an, 3, c.v);
            pdf_set_annot_border_width(ctx_, an, float(width));
            if (opacity < 1)
                pdf_set_annot_opacity(ctx_, an, float(opacity));
            pdf_update_annot(ctx_, an);
        }
        fz_always(ctx_) {
            pdf_drop_annot(ctx_, an);
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

bool Document::whiteOut(int page, const QRectF &rect, QColor color, QString *error)
{
    if (rect.width() < 1 || rect.height() < 1)
        return false;
    const QPointF o = geometry()->origins.at(size_t(page));
    const fz_rect fr = fz_make_rect(float(rect.left() + o.x()), float(rect.top() + o.y()),
                                    float(rect.right() + o.x()), float(rect.bottom() + o.y()));
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, "white-out", [&](pdf_page *pg) {
        // a box of the colour drawn over the page: what was there is kept in q ... Q, the box
        // after it
        fz_rect mediabox;
        fz_matrix ctm;
        pdf_page_transform(ctx_, pg, &mediabox, &ctm);
        const fz_rect r = fz_transform_rect(fr, fz_invert_matrix(ctm));
        pdf_obj *pageobj = pg->obj;
        fz_buffer *start = nullptr, *end = nullptr;
        pdf_obj *arr = nullptr;
        fz_var(start);
        fz_var(end);
        fz_var(arr);
        fz_try(ctx_) {
            start = fz_new_buffer(ctx_, 4);
            fz_append_string(ctx_, start, "q\n");
            end = fz_new_buffer(ctx_, 128);
            fz_append_printf(ctx_, end, "\nQ\nq %g %g %g rg %g %g %g %g re f Q\n", c.v[0], c.v[1], c.v[2], r.x0, r.y0,
                             r.x1 - r.x0, r.y1 - r.y0);
            pdf_obj *old = pdf_dict_get(ctx_, pageobj, PDF_NAME(Contents));
            arr = pdf_new_array(ctx_, pdf_, 4);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, start, nullptr, 0));
            if (pdf_is_array(ctx_, old))
                for (int i = 0; i < pdf_array_len(ctx_, old); ++i)
                    pdf_array_push(ctx_, arr, pdf_array_get(ctx_, old, i));
            else if (old)
                pdf_array_push(ctx_, arr, old);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, end, nullptr, 0));
            pdf_dict_put(ctx_, pageobj, PDF_NAME(Contents), arr);
        }
        fz_always(ctx_) {
            fz_drop_buffer(ctx_, start);
            fz_drop_buffer(ctx_, end);
            pdf_drop_obj(ctx_, arr);
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

bool Document::moveAnnot(int page, int id, QPointF by, QString *error)
{
    const float dx = float(by.x()), dy = float(by.y());
    std::lock_guard guard(docMutex_);
    return onPage(page, "move", [&](pdf_page *pg) {
        pdf_annot *a = findAnnot(ctx_, pg, id);
        if (pdf_annot_has_ink_list(ctx_, a)) {  // (a drawing's lines are moved)
            const int strokes = pdf_annot_ink_list_count(ctx_, a);
            int total = 0;
            for (int s = 0; s < strokes; ++s)
                total += pdf_annot_ink_list_stroke_count(ctx_, a, s);
            fz_point *pts = static_cast<fz_point *>(fz_malloc(ctx_, sizeof(fz_point) * size_t(std::max(1, total))));
            int *counts = static_cast<int *>(fz_malloc(ctx_, sizeof(int) * size_t(std::max(1, strokes))));
            fz_try(ctx_) {
                int k = 0;
                for (int s = 0; s < strokes; ++s) {
                    counts[s] = pdf_annot_ink_list_stroke_count(ctx_, a, s);
                    for (int v = 0; v < counts[s]; ++v) {
                        const fz_point p = pdf_annot_ink_list_stroke_vertex(ctx_, a, s, v);
                        pts[k++] = fz_make_point(p.x + dx, p.y + dy);
                    }
                }
                pdf_set_annot_ink_list(ctx_, a, strokes, counts, pts);
            }
            fz_always(ctx_) {
                fz_free(ctx_, pts);
                fz_free(ctx_, counts);
            }
            fz_catch(ctx_) {
                fz_rethrow(ctx_);
            }
        } else if (pdf_annot_has_line(ctx_, a)) {
            fz_point p0, p1;
            pdf_annot_line(ctx_, a, &p0, &p1);
            pdf_set_annot_line(ctx_, a, fz_make_point(p0.x + dx, p0.y + dy), fz_make_point(p1.x + dx, p1.y + dy));
        } else if (pdf_annot_has_vertices(ctx_, a)) {
            for (int v = 0; v < pdf_annot_vertex_count(ctx_, a); ++v) {
                const fz_point p = pdf_annot_vertex(ctx_, a, v);
                pdf_set_annot_vertex(ctx_, a, v, fz_make_point(p.x + dx, p.y + dy));
            }
        } else {
            fz_rect r = pdf_annot_rect(ctx_, a);
            r.x0 += dx;
            r.x1 += dx;
            r.y0 += dy;
            r.y1 += dy;
            pdf_set_annot_rect(ctx_, a, r);
        }
        pdf_update_annot(ctx_, a);
        if (isMarker(ctx_, a))
            multiplyBlend(ctx_, pdf_, a);
        keepErased(ctx_, pdf_, a, dx, -dy);  // (rubbed out in places: that goes with it - PDF space is the other way up)
    }, error);
}

bool Document::deleteAnnot(int page, int id, QString *error)
{
    std::lock_guard guard(docMutex_);
    return onPage(page, "delete", [&](pdf_page *pg) {
        pdf_delete_annot(ctx_, pg, findAnnot(ctx_, pg, id));
    }, error);
}

bool Document::recolorAnnot(int page, int id, QColor color, QString *error)
{
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, "change colour", [&](pdf_page *pg) {
        pdf_annot *a = findAnnot(ctx_, pg, id);
        pdf_set_annot_color(ctx_, a, 3, c.v);
        pdf_update_annot(ctx_, a);
        if (isMarker(ctx_, a))
            multiplyBlend(ctx_, pdf_, a);
        keepErased(ctx_, pdf_, a, 0, 0);
    }, error);
}

bool Document::cutMarks(int page, const std::vector<QRectF> &holes, QString *error)
{
    // worked out first: each mark over the holes, and what's left of it
    struct Plan {
        int id;
        enum pdf_annot_type type;
        std::vector<fz_quad> left;
        Rgb color;
        float opacity;
        QByteArray contents;
    };
    std::vector<Plan> plans;
    const QPointF o = geometry()->origins.at(size_t(page));
    for (const Annot &a : annots(page)) {
        if (!a.markup())
            continue;
        bool over = false;
        for (const QRectF &r : a.pieces)
            for (const QRectF &h : holes)
                over = over || (sameLine(r, h) && std::min(r.right(), h.right()) - std::max(r.left(), h.left()) > 1);
        if (!over)
            continue;
        Plan p;
        p.id = a.id;
        p.type = a.kind == "Highlight" ? PDF_ANNOT_HIGHLIGHT : a.kind == "StrikeOut" ? PDF_ANNOT_STRIKE_OUT
                 : a.kind == "Underline" ? PDF_ANNOT_UNDERLINE : PDF_ANNOT_SQUIGGLY;
        for (const QRectF &r : a.pieces)
            for (const QRectF &piece : cutOut(r, holes))
                p.left.push_back(fz_quad_from_rect(fz_make_rect(float(piece.left() + o.x()), float(piece.top() + o.y()),
                                                                float(piece.right() + o.x()), float(piece.bottom() + o.y()))));
        p.color = rgb(a.color.isValid() ? a.color : QColor(Qt::yellow));
        p.opacity = 1;
        p.contents = a.contents.toUtf8();
        plans.push_back(std::move(p));
    }
    if (plans.empty())
        return false;
    std::lock_guard guard(docMutex_);
    return onPage(page, "remove highlight", [&](pdf_page *pg) {
        for (const Plan &p : plans) {
            pdf_annot *old = findAnnot(ctx_, pg, p.id);
            const float opacity = pdf_annot_opacity(ctx_, old);
            pdf_delete_annot(ctx_, pg, old);
            if (p.left.empty())
                continue;
            pdf_annot *a = pdf_create_annot(ctx_, pg, p.type);  // (what's left: the same kind, colour, see-through)
            fz_try(ctx_) {
                for (const fz_quad &q : p.left)
                    pdf_add_annot_quad_point(ctx_, a, q);
                pdf_set_annot_color(ctx_, a, 3, p.color.v);
                if (opacity < 1)
                    pdf_set_annot_opacity(ctx_, a, opacity);
                if (!p.contents.isEmpty())
                    pdf_set_annot_contents(ctx_, a, p.contents.constData());
                pdf_update_annot(ctx_, a);
            }
            fz_always(ctx_) {
                pdf_drop_annot(ctx_, a);
            }
            fz_catch(ctx_) {
                fz_rethrow(ctx_);
            }
        }
    }, error);
}

bool Document::setNoteText(int page, int id, const QString &text, QString *error)
{
    const QByteArray utf8 = text.toUtf8();
    std::lock_guard guard(docMutex_);
    return onPage(page, "change note", [&](pdf_page *pg) {
        pdf_annot *a = findAnnot(ctx_, pg, id);
        pdf_set_annot_contents(ctx_, a, utf8.constData());
        pdf_update_annot(ctx_, a);
    }, error);
}

bool Document::setTextBox(int page, int id, QPointF at, const QString &text, double size, QColor color,
                          QString *error)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    const QByteArray utf8 = text.toUtf8();
    std::vector<std::vector<int>> lines;  // (each line's letters, to measure in Helvetica)
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        lines.emplace_back();
        for (uint c : line.toUcs4())
            lines.back().push_back(int(c));
    }
    const Rgb c = rgb(color);
    std::lock_guard guard(docMutex_);
    return onPage(page, id ? "change text" : "add text", [&](pdf_page *pg) {
        if (id)
            pdf_delete_annot(ctx_, pg, findAnnot(ctx_, pg, id));
        if (utf8.trimmed().isEmpty())
            return;
        // as big as the text in Helvetica (the box's font), a little room round it
        fz_font *font = fz_new_base14_font(ctx_, "Helvetica");
        float w = 0;
        for (const auto &line : lines) {
            float lw = 0;
            for (int ch : line)
                lw += fz_advance_glyph(ctx_, font, fz_encode_character(ctx_, font, ch), 0) * float(size);
            w = std::max(w, lw);
        }
        fz_drop_font(ctx_, font);
        w += float(size) * 0.8f + 4;
        const float h = float(lines.size()) * float(size) * 1.25f + float(size) * 0.5f;
        const float x0 = float(at.x() + o.x()), y0 = float(at.y() + o.y());
        pdf_annot *a = pdf_create_annot(ctx_, pg, PDF_ANNOT_FREE_TEXT);
        fz_try(ctx_) {
            // (on a turned page: the box upright as the page is shown)
            const int turn = pdf_to_int(ctx_, pdf_dict_get_inheritable(ctx_, pg->obj, PDF_NAME(Rotate)));
            if (turn % 360)
                pdf_dict_put_int(ctx_, pdf_annot_obj(ctx_, a), PDF_NAME(Rotate), turn);
            pdf_set_annot_rect(ctx_, a, fz_make_rect(x0, y0, x0 + w, y0 + h));
            pdf_set_annot_contents(ctx_, a, utf8.constData());
            pdf_set_annot_default_appearance(ctx_, a, "Helv", float(size), 3, c.v);
            pdf_set_annot_border_width(ctx_, a, 0);
            pdf_update_annot(ctx_, a);
        }
        fz_always(ctx_) {
            pdf_drop_annot(ctx_, a);
        }
        fz_catch(ctx_) {
            fz_rethrow(ctx_);
        }
    }, error);
}

bool Document::rubOut(int page, const std::vector<QPointF> &pathIn, double r, QString *error)
{
    // every drawing it went over - pen and marker strokes, shapes - loses exactly what was under it
    // (a round brush r across): its places rubbed (in PDF space) the eraser's path added to; a drawing
    // all rubbed out goes
    if (pathIn.empty())
        return false;
    const QPointF o = geometry()->origins.at(size_t(page));
    struct Plan {
        int id = 0;
        QRectF box;          // (where it shows, PDF space)
        QByteArray erased;   // (its places rubbed before, as PDF path text, in PDF space as it was)
        QPointF shift;       // (moved since)
        QPainterPath ink;    // (a pen / marker drawing: its lines, as wide as they're drawn - none: a shape)
    };
    std::vector<Plan> plans;
    fz_matrix toPdfSpace = fz_identity;  // (MuPDF's page space -> PDF space)
    {
        struct Raw {
            int id;
            fz_rect box;
            fz_buffer *erased;
            float dx, dy, width;
            std::vector<std::vector<QPointF>> strokes;
        };
        std::vector<Raw> raw;
        std::lock_guard guard(docMutex_);
        pdf_page *pg = nullptr;
        fz_var(pg);
        fz_try(ctx_) {
            pg = pdf_load_page(ctx_, pdf_, page);
            fz_rect mediabox;
            fz_matrix pagectm;
            pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
            toPdfSpace = fz_invert_matrix(pagectm);
            QRectF reach(pathIn[0] + o, QSizeF(0, 0));
            for (const QPointF &p : pathIn)
                reach |= QRectF(p + o, QSizeF(0.01, 0.01));
            reach.adjust(-r, -r, r, r);
            for (pdf_annot *a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a)) {
                if (!erasable(pdf_annot_type(ctx_, a)))
                    continue;
                const fz_rect b = pdf_bound_annot(ctx_, a);
                if (!reach.intersects(QRectF(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1))))
                    continue;
                pdf_obj *obj = pdf_annot_obj(ctx_, a);
                raw.push_back({pdf_to_num(ctx_, obj), fz_transform_rect(b, toPdfSpace), nullptr, 0, 0, pdf_annot_border_width(ctx_, a), {}});
                Raw &rw = raw.back();
                pdf_obj *erased = pdf_dict_gets(ctx_, obj, "MPDFErased");
                if (pdf_is_stream(ctx_, erased)) {
                    rw.erased = pdf_load_stream(ctx_, erased);
                    pdf_obj *shift = pdf_dict_gets(ctx_, obj, "MPDFShift");
                    rw.dx = pdf_array_get_real(ctx_, shift, 0);
                    rw.dy = pdf_array_get_real(ctx_, shift, 1);
                }
                if (pdf_annot_type(ctx_, a) == PDF_ANNOT_INK)
                    for (int st = 0; st < pdf_annot_ink_list_count(ctx_, a); ++st) {
                        rw.strokes.emplace_back();
                        for (int v = 0; v < pdf_annot_ink_list_stroke_count(ctx_, a, st); ++v) {
                            const fz_point q = pdf_annot_ink_list_stroke_vertex(ctx_, a, st, v);
                            rw.strokes.back().emplace_back(q.x, q.y);  // (PDF space, as the ink list is)
                        }
                    }
            }
        }
        fz_always(ctx_) {
            fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
        }
        fz_catch(ctx_) {
            for (Raw &rw : raw)
                fz_drop_buffer(ctx_, rw.erased);
            raw.clear();
        }
        for (Raw &rw : raw) {
            Plan pl;
            pl.id = rw.id;
            pl.box = QRectF(QPointF(rw.box.x0, rw.box.y0), QPointF(rw.box.x1, rw.box.y1));
            if (rw.erased) {
                unsigned char *d = nullptr;
                const size_t n = fz_buffer_storage(ctx_, rw.erased, &d);
                pl.erased = QByteArray(reinterpret_cast<const char *>(d), qsizetype(n));
                fz_drop_buffer(ctx_, rw.erased);
            }
            pl.shift = QPointF(rw.dx, rw.dy);
            if (!rw.strokes.empty()) {
                QPainterPath lines;
                for (const auto &st : rw.strokes) {
                    if (st.empty())
                        continue;
                    lines.moveTo(st.front());
                    for (const QPointF &q : st)
                        lines.lineTo(q);
                    if (st.size() == 1)
                        lines.lineTo(st.front() + QPointF(0.01, 0));
                }
                QPainterPathStroker pen;
                pen.setWidth(std::max(0.5f, rw.width));
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                pl.ink = pen.createStroke(lines).simplified();
            }
            plans.push_back(std::move(pl));
        }
    }
    // the eraser's path, as an outline in PDF space
    QPainterPath line;
    for (size_t k = 0; k < pathIn.size(); ++k) {
        const fz_point q = fz_transform_point_xy(float(pathIn[k].x() + o.x()), float(pathIn[k].y() + o.y()), toPdfSpace);
        if (k == 0)
            line.moveTo(q.x, q.y);
        else
            line.lineTo(q.x, q.y);
    }
    if (pathIn.size() == 1)
        line.lineTo(line.currentPosition() + QPointF(0.01, 0));
    QPainterPathStroker brush;
    brush.setWidth(2 * r * std::sqrt(std::abs(double(toPdfSpace.a) * toPdfSpace.d - double(toPdfSpace.b) * toPdfSpace.c)));
    brush.setCapStyle(Qt::RoundCap);
    brush.setJoinStyle(Qt::RoundJoin);
    const QPainterPath rub = brush.createStroke(line).simplified();
    // each: its places rubbed and this, as one - none of it left (its lines all under it): it goes
    struct Done {
        int id;
        bool gone;
        QByteArray erased;
    };
    std::vector<Done> done;
    for (const Plan &pl : plans) {
        if (!rub.intersects(pl.box))
            continue;
        QPainterPath before;
        {  // (its places rubbed before, read back - moved as it was since)
            const QList<QByteArray> toks = pl.erased.simplified().split(' ');
            std::vector<double> nums;
            for (const QByteArray &t : toks) {
                if (t == "m" && nums.size() >= 2)
                    before.moveTo(nums[nums.size() - 2], nums[nums.size() - 1]);
                else if (t == "l" && nums.size() >= 2)
                    before.lineTo(nums[nums.size() - 2], nums[nums.size() - 1]);
                else if (t == "h")
                    before.closeSubpath();
                else {
                    bool ok = false;
                    const double v = t.toDouble(&ok);
                    if (ok)
                        nums.push_back(v);
                    continue;
                }
                nums.clear();
            }
            before.translate(pl.shift);
        }
        const QPainterPath now = before.isEmpty() ? rub : before.united(rub).simplified();
        if (!pl.ink.isEmpty() && pl.ink.subtracted(now).isEmpty() && !now.isEmpty()) {
            done.push_back({pl.id, true, {}});
            continue;
        }
        QByteArray text;
        for (const QPolygonF &poly : now.toSubpathPolygons()) {
            for (int k = 0; k < poly.size(); ++k)
                text += QByteArray::number(poly[k].x(), 'f', 2) + ' ' + QByteArray::number(poly[k].y(), 'f', 2) + (k == 0 ? " m " : " l ");
            text += "h\n";
        }
        done.push_back({pl.id, false, text});
    }
    if (done.empty())
        return false;
    std::lock_guard guard(docMutex_);
    return onPage(page, "erase", [&](pdf_page *pg) {
        for (const Done &d : done) {
            pdf_annot *a = findAnnot(ctx_, pg, d.id);
            if (d.gone) {  // (none of it left: it goes)
                pdf_delete_annot(ctx_, pg, a);
                continue;
            }
            pdf_obj *obj = pdf_annot_obj(ctx_, a);
            fz_buffer *text = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(d.erased.constData()),
                                                            size_t(d.erased.size()));
            fz_try(ctx_) {
                pdf_obj *old = pdf_dict_gets(ctx_, obj, "MPDFErased");
                if (pdf_is_stream(ctx_, old)) {
                    pdf_update_stream(ctx_, pdf_, old, text, 0);
                } else {
                    pdf_obj *dict = pdf_new_dict(ctx_, pdf_, 1);
                    pdf_obj *ref = pdf_add_stream(ctx_, pdf_, text, dict, 0);
                    pdf_drop_obj(ctx_, dict);
                    pdf_dict_puts_drop(ctx_, obj, "MPDFErased", ref);
                }
                pdf_obj *arr = pdf_new_array(ctx_, pdf_, 2);  // (where it is now: not moved since)
                pdf_array_push_real(ctx_, arr, 0);
                pdf_array_push_real(ctx_, arr, 0);
                pdf_dict_puts_drop(ctx_, obj, "MPDFShift", arr);
                applyErased(ctx_, pdf_, a);
            }
            fz_always(ctx_) {
                fz_drop_buffer(ctx_, text);
            }
            fz_catch(ctx_) {
                fz_rethrow(ctx_);
            }
        }
    }, error);
}

QImage Document::renderWithoutDrawings(int page, double scale, const QRect &area)
{
    QImage out;
    if (area.isEmpty() || page < 0)
        return out;
    const QPointF o = geometry()->origins.at(size_t(page));
    std::lock_guard guard(docMutex_);
    pdf_page *pg = nullptr;
    fz_pixmap *pix = nullptr;
    fz_device *dev = nullptr;
    fz_var(pg);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        const fz_irect box = {area.left(), area.top(), area.left() + area.width(), area.top() + area.height()};
        pix = fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), box, nullptr, 0);
        fz_clear_pixmap_with_value(ctx_, pix, 255);
        const fz_matrix ctm = fz_concat(fz_translate(float(-o.x()), float(-o.y())), fz_scale(float(scale), float(scale)));
        dev = fz_new_draw_device(ctx_, fz_identity, pix);
        pdf_run_page_contents(ctx_, pg, dev, ctm, nullptr);
        for (pdf_annot *a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a))
            if (!erasable(pdf_annot_type(ctx_, a)))
                pdf_run_annot(ctx_, a, dev, ctm, nullptr);
        pdf_run_page_widgets(ctx_, pg, dev, ctm, nullptr);
        fz_close_device(ctx_, dev);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
        fz_drop_pixmap(ctx_, pix);
        pix = nullptr;
    }
    if (pix) {
        out = QImage(fz_pixmap_samples(ctx_, pix), fz_pixmap_width(ctx_, pix), fz_pixmap_height(ctx_, pix), int(fz_pixmap_stride(ctx_, pix)),
                     QImage::Format_RGB888)
                  .copy();
        fz_drop_pixmap(ctx_, pix);
    }
    return out;
}
