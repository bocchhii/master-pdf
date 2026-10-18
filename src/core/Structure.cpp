// The PDF's own structure (a tagged PDF - Word saves one): which paragraph element each letter is
// drawn in, and whether it's in a table cell - read with a device that notes MuPDF's
// begin_structure / end_structure calls while the page is run. Acrobat boxes text by it.

#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <vector>

namespace {

struct Level {
    fz_structure kind;
    int id;
};

struct StructDevice {
    fz_device super;
    std::vector<Level> *stack;
    std::vector<Document::StructMark> *out;
    int *next;
    std::vector<QLineF> *rules;  // (drawn horizontal lines: null - not wanted)
};

// a path's horizontal pieces (lines, thin boxes) as drawn
struct Walk {
    fz_matrix ctm;
    fz_point at, start;
    std::vector<QLineF> *rules;
};

void addRule(Walk *w, fz_point a, fz_point b)
{
    a = fz_transform_point(a, w->ctm);
    b = fz_transform_point(b, w->ctm);
    if (std::abs(a.y - b.y) < 1.5 && std::abs(a.x - b.x) > 8)
        w->rules->push_back(QLineF(std::min(a.x, b.x), (a.y + b.y) / 2, std::max(a.x, b.x), (a.y + b.y) / 2));
}

void walkMove(fz_context *, void *arg, float x, float y)
{
    auto *w = static_cast<Walk *>(arg);
    w->at = w->start = fz_make_point(x, y);
}

void walkLine(fz_context *, void *arg, float x, float y)
{
    auto *w = static_cast<Walk *>(arg);
    addRule(w, w->at, fz_make_point(x, y));
    w->at = fz_make_point(x, y);
}

void walkCurve(fz_context *, void *arg, float, float, float, float, float x3, float y3)
{
    static_cast<Walk *>(arg)->at = fz_make_point(x3, y3);
}

void walkClose(fz_context *, void *arg)
{
    auto *w = static_cast<Walk *>(arg);
    addRule(w, w->at, w->start);
    w->at = w->start;
}

void walkRect(fz_context *, void *arg, float x0, float y0, float x1, float y1)
{
    auto *w = static_cast<Walk *>(arg);
    if (std::abs(y1 - y0) < 2.5 || std::abs(x1 - x0) < 2.5) {  // (a thin box: a line drawn as a box)
        addRule(w, fz_make_point(x0, (y0 + y1) / 2), fz_make_point(x1, (y0 + y1) / 2));
    } else {
        addRule(w, fz_make_point(x0, y0), fz_make_point(x1, y0));
        addRule(w, fz_make_point(x0, y1), fz_make_point(x1, y1));
    }
    w->at = w->start = fz_make_point(x0, y0);
}

void rulesOf(fz_context *ctx, fz_device *dev, const fz_path *path, fz_matrix ctm)
{
    auto *d = reinterpret_cast<StructDevice *>(dev);
    if (!d->rules)
        return;
    static const fz_path_walker walker = {walkMove, walkLine, walkCurve, walkClose, nullptr, nullptr, nullptr, walkRect};
    Walk w{ctm, {0, 0}, {0, 0}, d->rules};
    fz_walk_path(ctx, path, &walker, &w);
}

void fillPath(fz_context *ctx, fz_device *dev, const fz_path *path, int, fz_matrix ctm, fz_colorspace *, const float *, float alpha,
              fz_color_params)
{
    if (alpha > 0.1f)
        rulesOf(ctx, dev, path, ctm);
}

void strokePath(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *, fz_matrix ctm, fz_colorspace *,
                const float *, float alpha, fz_color_params)
{
    if (alpha > 0.1f)
        rulesOf(ctx, dev, path, ctm);
}

bool paragraphLike(fz_structure k)
{
    return k == FZ_STRUCTURE_P || k == FZ_STRUCTURE_H || k == FZ_STRUCTURE_H1 || k == FZ_STRUCTURE_H2 || k == FZ_STRUCTURE_H3 ||
           k == FZ_STRUCTURE_H4 || k == FZ_STRUCTURE_H5 || k == FZ_STRUCTURE_H6 || k == FZ_STRUCTURE_LISTITEM || k == FZ_STRUCTURE_LABEL ||
           k == FZ_STRUCTURE_LISTBODY || k == FZ_STRUCTURE_CAPTION || k == FZ_STRUCTURE_TOCI;
}

void note(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm)
{
    auto *d = reinterpret_cast<StructDevice *>(dev);
    int para = -1;
    bool cell = false;
    for (auto it = d->stack->rbegin(); it != d->stack->rend(); ++it) {
        if (para < 0 && paragraphLike(it->kind))
            para = it->id;
        if (it->kind == FZ_STRUCTURE_TD || it->kind == FZ_STRUCTURE_TH)
            cell = true;
    }
    if (para < 0 && !d->stack->empty())
        para = d->stack->back().id;
    for (const fz_text_span *span = text->head; span; span = span->next)
        for (int i = 0; i < span->len; ++i) {
            const fz_matrix trm = fz_concat(fz_make_matrix(span->trm.a, span->trm.b, span->trm.c, span->trm.d, span->items[i].x,
                                                           span->items[i].y),
                                            ctm);
            d->out->push_back({QPointF(trm.e, trm.f), para, cell});
        }
    (void)ctx;
}

void fillText(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_colorspace *, const float *, float, fz_color_params)
{
    note(ctx, dev, text, ctm);
}

void strokeText(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *, fz_matrix ctm, fz_colorspace *,
                const float *, float, fz_color_params)
{
    note(ctx, dev, text, ctm);
}

void clipText(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_rect)
{
    note(ctx, dev, text, ctm);
}

void ignoreText(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm)
{
    note(ctx, dev, text, ctm);
}

void beginStructure(fz_context *, fz_device *dev, fz_structure standard, const char *, int)
{
    auto *d = reinterpret_cast<StructDevice *>(dev);
    d->stack->push_back({standard, (*d->next)++});
}

void endStructure(fz_context *, fz_device *dev)
{
    auto *d = reinterpret_cast<StructDevice *>(dev);
    if (!d->stack->empty())
        d->stack->pop_back();
}

}  // namespace

std::vector<Document::StructMark> Document::structureMarks(int page, std::vector<QLineF> *rules)
{
    std::vector<StructMark> out;
    std::vector<Level> stack;
    int next = 0;
    std::lock_guard guard(docMutex_);
    if (!pdf_ || page < 0 || page >= pageCount())
        return out;
    const QPointF o = geometry()->origins.at(size_t(page));
    fz_page *pg = nullptr;
    StructDevice *dev = nullptr;
    fz_var(pg);
    fz_var(dev);
    fz_try(ctx_) {
        pg = fz_load_page(ctx_, reinterpret_cast<fz_document *>(pdf_), page);
        dev = reinterpret_cast<StructDevice *>(fz_new_device_of_size(ctx_, sizeof(StructDevice)));
        dev->super.fill_text = fillText;
        dev->super.stroke_text = strokeText;
        dev->super.clip_text = clipText;
        dev->super.ignore_text = ignoreText;
        dev->super.begin_structure = beginStructure;
        dev->super.end_structure = endStructure;
        dev->super.fill_path = fillPath;
        dev->super.stroke_path = strokePath;
        dev->rules = rules;
        dev->stack = &stack;
        dev->out = &out;
        dev->next = &next;
        fz_run_page(ctx_, pg, &dev->super, fz_identity, nullptr);
        fz_close_device(ctx_, &dev->super);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, &dev->super);
        fz_drop_page(ctx_, pg);
    }
    fz_catch(ctx_) {
        out.clear();
    }
    for (StructMark &m : out)  // (page points from its top left, as the letters')
        m.at -= o;
    if (rules)
        for (QLineF &l : *rules)
            l.translate(-o);
    return out;
}
