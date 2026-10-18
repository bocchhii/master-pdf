// Document's picture shapes (see Shapes.h): the pictures looked at small, their own see-through
// parts, and what the page draws under each - drawn from a copy of the page (in a document of its
// own: the open PDF and its Undo steps aren't touched). Kept once found, by picture and place.
// (No C++ objects are made inside fz_try blocks.)

#include "core/Content.h"
#include "core/Document.h"
#include "core/Shapes.h"

#include <QTransform>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>

using namespace content;

namespace {

QImage fromPixmap(fz_context *ctx, fz_pixmap *pix)
{
    const int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix), n = fz_pixmap_components(ctx, pix);
    const int stride = int(fz_pixmap_stride(ctx, pix));
    const QImage::Format f = n == 1 ? QImage::Format_Grayscale8 : n == 3 ? QImage::Format_RGB888 : QImage::Format_RGBA8888;
    if (n != 1 && n != 3 && n != 4)
        return {};
    return QImage(fz_pixmap_samples(ctx, pix), w, h, stride, f).copy();
}

QImage shrunk(const QImage &im, int size)
{
    return im.width() > size || im.height() > size ? im.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation) : im;
}

bool upright(const Picture &p)
{
    // as shown: its top left first, its top level, its left side straight down (not turned or flipped)
    if (p.corners.size() != 4)
        return false;
    const QPointF &tl = p.corners[0], &tr = p.corners[1], &bl = p.corners[3];
    return std::abs(tl.y() - tr.y()) < 0.01 && std::abs(tl.x() - bl.x()) < 0.01 && tr.x() > tl.x() && bl.y() > tl.y();
}

}  // namespace

// (docMutex_ held) a picture's own pixels as RGB (no see-through), no bigger than size across
QImage Document::pictureRgb(int xref, int size)
{
    QImage out;
    pdf_obj *obj = pdf_new_indirect(ctx_, pdf_, xref, 0);
    fz_image *img = nullptr;
    fz_pixmap *pix = nullptr, *rgb = nullptr;
    fz_var(img);
    fz_var(pix);
    fz_var(rgb);
    fz_try(ctx_) {
        img = pdf_load_image(ctx_, pdf_, obj);
        pix = fz_get_pixmap_from_image(ctx_, img, nullptr, nullptr, nullptr, nullptr);
        rgb = fz_convert_pixmap(ctx_, pix, fz_device_rgb(ctx_), nullptr, nullptr, fz_default_color_params, 0);
    }
    fz_always(ctx_) {
        fz_drop_image(ctx_, img);
        fz_drop_pixmap(ctx_, pix);
        pdf_drop_obj(ctx_, obj);
    }
    fz_catch(ctx_) {
        rgb = nullptr;
    }
    if (rgb) {
        out = fromPixmap(ctx_, rgb);
        fz_drop_pixmap(ctx_, rgb);
    }
    return shrunk(out, size);
}

// (docMutex_ held) its own see-through parts (its SMask) as grey - null if it has none
QImage Document::pictureAlpha(int xref, int size)
{
    QImage out;
    pdf_obj *obj = pdf_new_indirect(ctx_, pdf_, xref, 0);
    fz_image *img = nullptr;
    fz_pixmap *pix = nullptr, *gray = nullptr;
    fz_var(img);
    fz_var(pix);
    fz_var(gray);
    fz_try(ctx_) {
        pdf_obj *smask = pdf_dict_get(ctx_, obj, PDF_NAME(SMask));
        if (pdf_is_indirect(ctx_, smask)) {
            img = pdf_load_image(ctx_, pdf_, smask);
            pix = fz_get_pixmap_from_image(ctx_, img, nullptr, nullptr, nullptr, nullptr);
            gray = fz_pixmap_colorspace(ctx_, pix) && fz_colorspace_n(ctx_, fz_pixmap_colorspace(ctx_, pix)) != 1
                       ? fz_convert_pixmap(ctx_, pix, fz_device_gray(ctx_), nullptr, nullptr, fz_default_color_params, 0)
                       : fz_keep_pixmap(ctx_, pix);
        }
    }
    fz_always(ctx_) {
        fz_drop_image(ctx_, img);
        fz_drop_pixmap(ctx_, pix);
        pdf_drop_obj(ctx_, obj);
    }
    fz_catch(ctx_) {
        gray = nullptr;
    }
    if (gray) {
        out = fromPixmap(ctx_, gray);
        fz_drop_pixmap(ctx_, gray);
        if (out.format() != QImage::Format_Grayscale8)
            out = out.convertToFormat(QImage::Format_Grayscale8);
    }
    return shrunk(out, size);
}

// (docMutex_ held) what the page draws under a picture - everything before it - where it is,
// size pixels big: drawn from a copy of the page in a document of its own
QImage Document::underPicture(int page, const Picture &pic, QSize size)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    // the page's drawing up to the picture (its q's closed)
    QByteArray before;
    {
        pdf_obj *pageobj = nullptr;
        fz_try(ctx_) {
            pageobj = pdf_lookup_page_obj(ctx_, pdf_, page);
        }
        fz_catch(ctx_) {
            pageobj = nullptr;
        }
        if (!pageobj)
            return {};
        const std::vector<pdf_obj *> streams = contentStreams(ctx_, pageobj);
        if (pic.stream >= int(streams.size()))
            return {};
        for (int s = 0; s < pic.stream; ++s)
            before += streamData(ctx_, streams[size_t(s)]) + ' ';
        before += streamData(ctx_, streams[size_t(pic.stream)]).left(pic.start);
        int depth = 0;
        for (const Op &op : readOps(before))
            depth += op.op == "q" ? 1 : op.op == "Q" ? -1 : 0;
        for (int k = 0; k < depth; ++k)
            before += " Q";
    }
    const QRectF r = pic.full.isNull() ? pic.rect : pic.full;
    const int w = std::max(1, size.width()), h = std::max(1, size.height());
    QImage out;
    pdf_document *tmp = nullptr;
    pdf_graft_map *map = nullptr;
    fz_buffer *buf = nullptr;
    fz_page *tp = nullptr;
    fz_pixmap *pix = nullptr;
    fz_device *dev = nullptr;
    fz_var(tmp);
    fz_var(map);
    fz_var(buf);
    fz_var(tp);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx_) {
        tmp = pdf_create_document(ctx_);
        map = pdf_new_graft_map(ctx_, tmp);
        pdf_graft_mapped_page(ctx_, map, -1, pdf_, page);
        pdf_obj *tpo = pdf_lookup_page_obj(ctx_, tmp, 0);
        buf = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(before.constData()), size_t(before.size()));
        pdf_dict_put_drop(ctx_, tpo, PDF_NAME(Contents), pdf_add_stream(ctx_, tmp, buf, nullptr, 0));
        pdf_dict_del(ctx_, tpo, PDF_NAME(Annots));
        tp = fz_load_page(ctx_, reinterpret_cast<fz_document *>(tmp), 0);
        const fz_matrix m = fz_concat(fz_translate(float(-(r.left() + o.x())), float(-(r.top() + o.y()))),
                                      fz_scale(float(w / r.width()), float(h / r.height())));
        pix = fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), fz_make_irect(0, 0, w, h), nullptr, 0);
        fz_clear_pixmap_with_value(ctx_, pix, 255);
        dev = fz_new_draw_device(ctx_, fz_identity, pix);
        fz_run_page_contents(ctx_, tp, dev, m, nullptr);
        fz_close_device(ctx_, dev);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        fz_drop_page(ctx_, tp);
        fz_drop_buffer(ctx_, buf);
        pdf_drop_graft_map(ctx_, map);
        pdf_drop_document(ctx_, tmp);
    }
    fz_catch(ctx_) {
        fz_drop_pixmap(ctx_, pix);
        pix = nullptr;
    }
    if (pix) {
        out = fromPixmap(ctx_, pix);
        fz_drop_pixmap(ctx_, pix);
    }
    return out;
}

// (docMutex_ held) the shape a picture shows (kept once found): judged against what's under it
// first - the surest - else from the picture alone, if cut it looks the same
Document::ShapeKey Document::shapeKey(int page, int xref, const QRectF &r)
{
    return std::make_tuple(page, xref, int(std::lround(r.left())), int(std::lround(r.top())), int(std::lround(r.width())),
                           int(std::lround(r.height())));
}

void Document::shapeMoved(int page, const Picture &pic, const QRectF &to)
{
    // (moved or resized: the same shape where it goes - not judged again against what's there)
    const QRectF r = pic.full.isNull() ? pic.rect : pic.full;
    auto it = shapes_.find(shapeKey(page, pic.xref, r));
    if (it != shapes_.end())
        shapes_[shapeKey(page, pic.xref, to)] = ShapeInfo(it->second);
}

const Document::ShapeInfo &Document::shapeOf(int page, const Picture &pic)
{
    const QRectF r = pic.full.isNull() ? pic.rect : pic.full;
    const ShapeKey key = shapeKey(page, pic.xref, r);
    auto it = shapes_.find(key);
    if (it != shapes_.end())
        return it->second;
    ShapeInfo &info = shapes_[key];
    const QSizeF ps = geometry()->sizes.at(size_t(page));
    // (turned or flipped, a page's background, or tiny: not looked at - left whole)
    if (!upright(pic) || r.width() < 8 || r.height() < 8 || r.width() * r.height() > 0.5 * ps.width() * ps.height())
        return info;
    const QImage rgb = pictureRgb(pic.xref, 160);
    if (rgb.width() < 8 || rgb.height() < 8)
        return info;
    const QImage alpha = pictureAlpha(pic.xref, 160);
    const QImage under = underPicture(page, pic, rgb.size());
    if (under.isNull())
        return info;
    if (alpha.isNull()) {
        if (auto fg = shapes::onPage(rgb, under))
            if (auto found = shapes::shapeIn(*fg)) {
                info.kind = 1;
                info.hull = found->hull;
                info.pieces = found->pieces;
                return info;
            }
    }
    if (alpha.isNull() && !shapes::ownFits(rgb, under))
        return info;
    if (auto fg = shapes::own(rgb, alpha))
        if (auto found = shapes::shapeIn(*fg)) {
            info.kind = 2;
            info.hull = found->hull;
            info.pieces = found->pieces;
        }
    return info;
}

// (docMutex_ held) the picture's shape as a clip path in its own square, looked at finely
// (400 across) - empty if it has no shape (all picture)
QByteArray Document::shapeClip(int page, const Picture &pic)
{
    const ShapeInfo info = shapeOf(page, pic);
    if (!info.kind)
        return {};
    const QImage rgb = pictureRgb(pic.xref, 400);
    std::optional<QImage> fine;
    if (info.kind == 1)
        fine = shapes::onPage(rgb, underPicture(page, pic, rgb.size()), false);
    else
        fine = shapes::own(rgb, pictureAlpha(pic.xref, 400));
    return fine ? shapes::clipPath(*fine, info.pieces) : QByteArray();
}

namespace {

// the smallest box round points, turned as they are (the Python turned_box): its corners - top
// left, top right, bottom right, bottom left, as shown (its top: the side nearest to across);
// near straight (under a degree), or not box-like: straight
std::vector<QPointF> turnedBox(const std::vector<QPointF> &pts)
{
    double bestArea = -1, bestAngle = 0;
    QRectF best;
    for (size_t k = 0; k < pts.size(); ++k) {
        const QPointF a = pts[k], b = pts[(k + 1) % pts.size()];
        if (a == b)
            continue;
        double angle = std::atan2(b.y() - a.y(), b.x() - a.x()) * 180 / M_PI;
        angle = std::fmod(std::fmod(angle + 45, 90) + 90, 90) - 45;
        QTransform back;
        back.rotate(-angle);
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const QPointF &q : pts) {
            const QPointF r = back.map(q);
            x0 = std::min(x0, r.x()), y0 = std::min(y0, r.y());
            x1 = std::max(x1, r.x()), y1 = std::max(y1, r.y());
        }
        const double area = (x1 - x0) * (y1 - y0);
        if (bestArea < 0 || area < bestArea - 1e-6) {
            bestArea = area;
            bestAngle = angle;
            best = QRectF(QPointF(x0, y0), QPointF(x1, y1));
        }
    }
    if (bestArea < 0)
        return {};
    // (turned only if it's box-like - a tile filling its turned box; a shape of any other outline
    // - clip-art, a logo - gets its plain box: the smallest box round it may lean any way)
    double area = 0;
    for (size_t k = 0; k < pts.size(); ++k) {
        const QPointF &a = pts[k], &b = pts[(k + 1) % pts.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    area = std::abs(area) / 2;
    if (std::abs(bestAngle) < 1 || area < 0.85 * bestArea) {  // (straight: its plain box)
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const QPointF &q : pts)
            x0 = std::min(x0, q.x()), y0 = std::min(y0, q.y()), x1 = std::max(x1, q.x()), y1 = std::max(y1, q.y());
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    QTransform turn;
    turn.rotate(bestAngle);
    return {turn.map(best.topLeft()), turn.map(best.topRight()), turn.map(best.bottomRight()), turn.map(best.bottomLeft())};
}

}  // namespace

// (docMutex_ held) a picture's shape put in: picked by it, outlined round it (turned as it's
// turned in the picture: a tile drawn askew in an upright picture)
void Document::applyShape(int page, Picture &pic)
{
    pic.full = pic.rect;
    const ShapeInfo &info = shapeOf(page, pic);
    if (!info.kind)
        return;
    outlines_[pic.xref] = info.hull;
    frameFromHull(pic, info.hull);
}

// a picture's own square on the page (pic.whole: its corners as (0, 1), (1, 1), (1, 0), (0, 0) are shown) - a
// point of its pixels (parts of its width and height from its top left) where it's shown, and back
namespace {
QPointF shownAt(const Picture &pic, QPointF u)
{
    const QPointF &tl = pic.whole[0], &tr = pic.whole[1], &bl = pic.whole[3];
    return tl + u.x() * (tr - tl) + u.y() * (bl - tl);
}
QPointF inPicture(const Picture &pic, QPointF p)
{
    const QPointF &tl = pic.whole[0], a = pic.whole[1] - tl, b = pic.whole[3] - tl, d = p - tl;
    const double det = a.x() * b.y() - a.y() * b.x();
    if (std::abs(det) < 1e-9)
        return {};
    return QPointF((d.x() * b.y() - d.y() * b.x()) / det, (a.x() * d.y() - a.y() * d.x()) / det);
}
}  // namespace

void Document::frameFromHull(Picture &pic, const std::vector<QPointF> &hull)
{
    // its shape's frame worked out in the picture's own pixels (the smallest box round it, turned as it is
    // there - its top: its side nearest across, in the picture), then put where the picture's shown: turned,
    // moved, flipped with the picture as one - its rotate handle on the same side wherever that goes
    if (pic.whole.size() != 4 || hull.size() < 3)
        return;
    const double w = std::max(1, pic.width), h = std::max(1, pic.height);
    std::vector<QPointF> px;
    for (const QPointF &u : hull)
        px.push_back(QPointF(u.x() * w, u.y() * h));
    std::vector<QPointF> box = turnedBox(px);
    pic.shape.clear();
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const QPointF &u : hull) {
        const QPointF p = shownAt(pic, u);
        pic.shape.push_back(p);
        x0 = std::min(x0, p.x()), y0 = std::min(y0, p.y()), x1 = std::max(x1, p.x()), y1 = std::max(y1, p.y());
    }
    pic.rect = QRectF(QPointF(x0, y0), QPointF(x1, y1));
    pic.corners.clear();
    if (box.size() == 4)
        for (const QPointF &q : box)
            pic.corners.push_back(shownAt(pic, QPointF(q.x() / w, q.y() / h)));
    else
        pic.corners = {pic.rect.topLeft(), pic.rect.topRight(), pic.rect.bottomRight(), pic.rect.bottomLeft()};
}

void Document::shapeFromClip(Picture &pic, const std::vector<QPointF> &points)
{
    // its outline: round the clip's boxes (their hull - as the shape's found in its pixels), in the picture's
    // own square; then framed as a shape found in its pixels is
    if (pic.whole.size() != 4)
        return;
    const double w = std::max(1, pic.width), h = std::max(1, pic.height);
    std::vector<QPointF> pts;
    for (const QPointF &p : points) {
        const QPointF u = inPicture(pic, p);
        pts.push_back(QPointF(u.x() * w, u.y() * h));
    }
    std::sort(pts.begin(), pts.end(), [](const QPointF &a, const QPointF &b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3)
        return;
    auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> hull(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0)
            --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k > 1 ? k - 1 : k);
    if (hull.size() < 3)
        return;
    std::vector<QPointF> unit;
    for (const QPointF &q : hull)
        unit.push_back(QPointF(q.x() / w, q.y() / h));
    pic.full = pic.rect;  // (the whole picture's box)
    frameFromHull(pic, unit);
}

bool Document::seeThroughAt(const Picture &pic, QPointF at)
{
    if (pic.vector)
        return false;
    // a picture's own see-through parts there (a logo's background taken out): clicks go through
    if (!upright(pic))
        return false;
    std::lock_guard guard(docMutex_);
    auto it = alphas_.find(pic.xref);
    if (it == alphas_.end())
        it = alphas_.emplace(pic.xref, pictureAlpha(pic.xref, 256)).first;
    const QImage &a = it->second;
    const QRectF r = pic.full.isNull() ? pic.rect : pic.full;
    if (a.isNull() || r.isEmpty())
        return false;
    const double u = (at.x() - r.left()) / r.width(), v = (at.y() - r.top()) / r.height();
    if (u < 0 || u > 1 || v < 0 || v > 1)
        return false;
    const int x = std::min(a.width() - 1, int(u * a.width())), y = std::min(a.height() - 1, int(v * a.height()));
    int most = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int px = std::clamp(x + dx, 0, a.width() - 1), py = std::clamp(y + dy, 0, a.height() - 1);
            most = std::max(most, int(a.constScanLine(py)[px]));
        }
    return most < 40;
}
