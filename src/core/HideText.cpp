// Document::renderHiding: part of a page drawn as render() does, but without the letters inside
// some boxes - what's under the text being typed (its background, a table's shading, a picture)
// shown while it's edited in place, as Acrobat does: no white box, the text where it is.
// A device that passes everything on to the draw device but the hidden letters.
// (No C++ objects are made inside fz_try blocks or in the device's calls.)

#include "core/Document.h"
#include "core/PageText.h"

#include <QPainter>

#include <mupdf/fitz.h>

namespace {

struct HideDevice {
    fz_device super;
    fz_device *target;
    const fz_rect *hide;  // (in the device's pixels)
    int count;
    char *unseen;  // (each box: has a scan's text read - not drawn, the scan shows its letters: covered with its paper)
};

HideDevice *me(fz_device *dev)
{
    return reinterpret_cast<HideDevice *>(dev);
}

bool hidden(const HideDevice *d, fz_point p)
{
    for (int k = 0; k < d->count; ++k)
        if (fz_is_point_inside_rect(p, d->hide[k]))
            return true;
    return false;
}

// the text without the hidden letters (null: none of it hidden - the text as it is)
fz_text *without(fz_context *ctx, const HideDevice *d, const fz_text *text, fz_matrix ctm, bool *all)
{
    bool any = false;
    *all = true;
    for (const fz_text_span *s = text->head; s; s = s->next)
        for (int i = 0; i < s->len; ++i) {
            const bool h = hidden(d, fz_transform_point(fz_make_point(s->items[i].x, s->items[i].y), ctm));
            any = any || h;
            *all = *all && h;
        }
    if (!any || *all)
        return nullptr;
    fz_text *out = fz_new_text(ctx);
    fz_try(ctx) {
        for (const fz_text_span *s = text->head; s; s = s->next)
            for (int i = 0; i < s->len; ++i) {
                const fz_text_item &it = s->items[i];
                if (hidden(d, fz_transform_point(fz_make_point(it.x, it.y), ctm)))
                    continue;
                fz_matrix trm = s->trm;
                trm.e = it.x;
                trm.f = it.y;
                fz_show_glyph(ctx, out, s->font, trm, it.gid, it.ucs, int(s->wmode), int(s->bidi_level),
                              fz_bidi_direction(s->markup_dir), fz_text_language(s->language));
            }
    }
    fz_catch(ctx) {
        fz_drop_text(ctx, out);
        fz_rethrow(ctx);
    }
    return out;
}

void fillText(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_colorspace *cs, const float *color,
              float alpha, fz_color_params cp)
{
    bool all = false;
    fz_text *t = without(ctx, me(dev), text, ctm, &all);
    if (all)
        return;
    fz_try(ctx) {
        fz_fill_text(ctx, me(dev)->target, t ? t : text, ctm, cs, color, alpha, cp);
    }
    fz_always(ctx) {
        fz_drop_text(ctx, t);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

void strokeText(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *st, fz_matrix ctm,
                fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
    bool all = false;
    fz_text *t = without(ctx, me(dev), text, ctm, &all);
    if (all)
        return;
    fz_try(ctx) {
        fz_stroke_text(ctx, me(dev)->target, t ? t : text, st, ctm, cs, color, alpha, cp);
    }
    fz_always(ctx) {
        fz_drop_text(ctx, t);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// the rest: passed on as it is
void fillPath(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix ctm, fz_colorspace *cs, const float *c,
              float a, fz_color_params cp)
{
    fz_fill_path(ctx, me(dev)->target, p, eo, ctm, cs, c, a, cp);
}
void strokePath(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix ctm, fz_colorspace *cs,
                const float *c, float a, fz_color_params cp)
{
    fz_stroke_path(ctx, me(dev)->target, p, st, ctm, cs, c, a, cp);
}
void clipPath(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix ctm, fz_rect sc)
{
    fz_clip_path(ctx, me(dev)->target, p, eo, ctm, sc);
}
void clipStrokePath(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix ctm, fz_rect sc)
{
    fz_clip_stroke_path(ctx, me(dev)->target, p, st, ctm, sc);
}
void clipText(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix ctm, fz_rect sc)
{
    fz_clip_text(ctx, me(dev)->target, t, ctm, sc);
}
void clipStrokeText(fz_context *ctx, fz_device *dev, const fz_text *t, const fz_stroke_state *st, fz_matrix ctm, fz_rect sc)
{
    fz_clip_stroke_text(ctx, me(dev)->target, t, st, ctm, sc);
}
void ignoreText(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix ctm)
{
    HideDevice *d = me(dev);
    for (const fz_text_span *s = t->head; s; s = s->next)
        for (int i = 0; i < s->len; ++i) {
            const fz_point p = fz_transform_point(fz_make_point(s->items[i].x, s->items[i].y), ctm);
            for (int k = 0; k < d->count; ++k)
                if (fz_is_point_inside_rect(p, d->hide[k]))
                    d->unseen[k] = 1;
        }
    fz_ignore_text(ctx, d->target, t, ctm);
}
void fillShade(fz_context *ctx, fz_device *dev, fz_shade *s, fz_matrix ctm, float a, fz_color_params cp)
{
    fz_fill_shade(ctx, me(dev)->target, s, ctm, a, cp);
}
void fillImage(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm, float a, fz_color_params cp)
{
    fz_fill_image(ctx, me(dev)->target, img, ctm, a, cp);
}
void fillImageMask(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm, fz_colorspace *cs, const float *c, float a,
                   fz_color_params cp)
{
    fz_fill_image_mask(ctx, me(dev)->target, img, ctm, cs, c, a, cp);
}
void clipImageMask(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm, fz_rect sc)
{
    fz_clip_image_mask(ctx, me(dev)->target, img, ctm, sc);
}
void popClip(fz_context *ctx, fz_device *dev)
{
    fz_pop_clip(ctx, me(dev)->target);
}
void beginMask(fz_context *ctx, fz_device *dev, fz_rect area, int lum, fz_colorspace *cs, const float *bc, fz_color_params cp)
{
    fz_begin_mask(ctx, me(dev)->target, area, lum, cs, bc, cp);
}
void endMask(fz_context *ctx, fz_device *dev, fz_function *fn)
{
    fz_end_mask_tr(ctx, me(dev)->target, fn);
}
void beginGroup(fz_context *ctx, fz_device *dev, fz_rect area, fz_colorspace *cs, int iso, int ko, int bm, float a)
{
    fz_begin_group(ctx, me(dev)->target, area, cs, iso, ko, bm, a);
}
void endGroup(fz_context *ctx, fz_device *dev)
{
    fz_end_group(ctx, me(dev)->target);
}
int beginTile(fz_context *ctx, fz_device *dev, fz_rect area, fz_rect view, float xs, float ys, fz_matrix ctm, int id)
{
    return fz_begin_tile_id(ctx, me(dev)->target, area, view, xs, ys, ctm, id);
}
void endTile(fz_context *ctx, fz_device *dev)
{
    fz_end_tile(ctx, me(dev)->target);
}
void renderFlags(fz_context *ctx, fz_device *dev, int set, int clear)
{
    fz_render_flags(ctx, me(dev)->target, set, clear);
}
void defaultColorspaces(fz_context *ctx, fz_device *dev, fz_default_colorspaces *dcs)
{
    fz_set_default_colorspaces(ctx, me(dev)->target, dcs);
}
void beginLayer(fz_context *ctx, fz_device *dev, const char *name)
{
    fz_begin_layer(ctx, me(dev)->target, name);
}
void endLayer(fz_context *ctx, fz_device *dev)
{
    fz_end_layer(ctx, me(dev)->target);
}

}  // namespace

QImage Document::renderHiding(int page, double scale, const QRect &area, const std::vector<QRectF> &hide)
{
    if (area.isEmpty())
        return {};
    QPointF o;
    fz_display_list *list = keepDisplayList(page, &o);
    if (!list)
        return {};
    fz_context *ctx = threadContext();
    // the boxes in the drawing's pixels (a little bigger: a letter's start on its edge is hidden too)
    std::vector<fz_rect> boxes;
    for (const QRectF &r : hide)
        boxes.push_back(fz_make_rect(float((r.left() - 1) * scale), float((r.top() - 1) * scale), float((r.right() + 1) * scale),
                                     float((r.bottom() + 1) * scale)));
    std::vector<char> unseen(boxes.size(), 0);
    QImage out;
    fz_pixmap *pix = nullptr;
    fz_device *draw = nullptr;
    fz_device *dev = nullptr;
    fz_var(pix);
    fz_var(draw);
    fz_var(dev);
    fz_try(ctx) {
        const fz_irect box = {area.left(), area.top(), area.left() + area.width(), area.top() + area.height()};
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        const fz_matrix ctm = fz_concat(fz_translate(float(-o.x()), float(-o.y())), fz_scale(float(scale), float(scale)));
        draw = fz_new_draw_device(ctx, fz_identity, pix);
        HideDevice *h = fz_new_derived_device(ctx, HideDevice);
        dev = &h->super;
        h->target = draw;
        h->hide = boxes.data();
        h->count = int(boxes.size());
        h->unseen = unseen.data();
        dev->fill_path = fillPath;
        dev->stroke_path = strokePath;
        dev->clip_path = clipPath;
        dev->clip_stroke_path = clipStrokePath;
        dev->fill_text = fillText;
        dev->stroke_text = strokeText;
        dev->clip_text = clipText;
        dev->clip_stroke_text = clipStrokeText;
        dev->ignore_text = ignoreText;
        dev->fill_shade = fillShade;
        dev->fill_image = fillImage;
        dev->fill_image_mask = fillImageMask;
        dev->clip_image_mask = clipImageMask;
        dev->pop_clip = popClip;
        dev->begin_mask = beginMask;
        dev->end_mask = endMask;
        dev->begin_group = beginGroup;
        dev->end_group = endGroup;
        dev->begin_tile = beginTile;
        dev->end_tile = endTile;
        dev->render_flags = renderFlags;
        dev->set_default_colorspaces = defaultColorspaces;
        dev->begin_layer = beginLayer;
        dev->end_layer = endLayer;
        fz_run_display_list(ctx, list, dev, ctm, fz_rect_from_irect(box), nullptr);
        fz_close_device(ctx, dev);
        fz_close_device(ctx, draw);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_device(ctx, draw);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }
    if (pix) {
        out = QImage(fz_pixmap_samples(ctx, pix), fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix),
                     int(fz_pixmap_stride(ctx, pix)), QImage::Format_RGB888)
                  .copy();
        fz_drop_pixmap(ctx, pix);
        // a scan's text hidden: the scan's letters under it covered with the paper round them (as an edit of it does)
        std::vector<QRectF> covered;
        QRectF all;
        for (size_t k = 0; k < boxes.size(); ++k)
            if (unseen[k]) {
                const QRectF b = QRectF(QPointF(boxes[k].x0, boxes[k].y0), QPointF(boxes[k].x1, boxes[k].y1)).translated(-area.topLeft());
                const double h = b.height();
                covered.push_back(b.adjusted(-0.25 * h, -0.08 * h, 0.25 * h, 0.08 * h));
                all |= b;
            }
        if (!covered.empty()) {
            const QColor paper = QColor::fromRgb(pagetext::paperIn(out, all.adjusted(-2, -1, 2, 1).toAlignedRect()));
            QPainter p(&out);
            for (const QRectF &b : covered)
                p.fillRect(b, paper);
        }
    }
    fz_drop_display_list(ctx, list);
    return out;
}
