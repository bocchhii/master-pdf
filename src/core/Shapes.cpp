// Picture shapes: see Shapes.h. Ported from the Python app (_shape_own, _shape_under, shape_in,
// own_fits, picture_clip) - same thresholds.

#include "core/Shapes.h"

#include <QPolygonF>

#include <algorithm>
#include <cmath>

#include <map>
#include <tuple>

namespace shapes {

namespace {

QImage blank(int w, int h, uchar v = 0)
{
    QImage m(w, h, QImage::Format_Grayscale8);
    m.fill(v);
    return m;
}

inline uchar &at(QImage &m, int x, int y)
{
    return m.scanLine(y)[x];
}
inline uchar get(const QImage &m, int x, int y)
{
    return m.constScanLine(y)[x];
}

// fill the part joined to (x, y) that has `from` with `to` (4 ways); its size
int flood(QImage &m, int x, int y, uchar from, uchar to)
{
    if (from == to || get(m, x, y) != from)
        return 0;
    const int w = m.width(), h = m.height();
    std::vector<std::pair<int, int>> stack{{x, y}};
    at(m, x, y) = to;
    int n = 0;
    while (!stack.empty()) {
        auto [px, py] = stack.back();
        stack.pop_back();
        ++n;
        const int nx[4] = {px - 1, px + 1, px, px};
        const int ny[4] = {py, py, py - 1, py + 1};
        for (int k = 0; k < 4; ++k)
            if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < w && ny[k] < h && get(m, nx[k], ny[k]) == from) {
                at(m, nx[k], ny[k]) = to;
                stack.push_back({nx[k], ny[k]});
            }
    }
    return n;
}

std::vector<std::pair<int, int>> edgeSpots(int w, int h)
{
    std::vector<std::pair<int, int>> e;
    for (int x = 0; x < w; ++x) {
        e.push_back({x, 0});
        e.push_back({x, h - 1});
    }
    for (int y = 0; y < h; ++y) {
        e.push_back({0, y});
        e.push_back({w - 1, y});
    }
    return e;
}

QImage rgb32(const QImage &im)
{
    return im.format() == QImage::Format_RGB32 ? im : im.convertToFormat(QImage::Format_RGB32);
}

// the colour most of the picture's edges are (the median of the biggest group of near colours)
// and how many of the edge's pixels are in that group
std::pair<QRgb, double> edgeColour(const QImage &im)
{
    std::map<std::tuple<int, int, int>, std::vector<QRgb>> groups;
    const auto spots = edgeSpots(im.width(), im.height());
    for (auto [x, y] : spots) {
        const QRgb c = im.pixel(x, y);
        groups[{qRed(c) / 24, qGreen(c) / 24, qBlue(c) / 24}].push_back(c);
    }
    const std::vector<QRgb> *most = nullptr;
    for (const auto &[k, v] : groups)
        if (!most || v.size() > most->size())
            most = &v;
    if (!most)
        return {0, 0};
    auto median = [&](int (*ch)(QRgb)) {
        std::vector<int> v;
        for (QRgb c : *most)
            v.push_back(ch(c));
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    return {qRgb(median(qRed), median(qGreen), median(qBlue)), double(most->size()) / double(spots.size())};
}

inline int diff(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

int count(const QImage &m, uchar v)
{
    int n = 0;
    for (int y = 0; y < m.height(); ++y) {
        const uchar *row = m.constScanLine(y);
        for (int x = 0; x < m.width(); ++x)
            n += row[x] == v;
    }
    return n;
}

std::vector<QPointF> convexHull(std::vector<QPointF> pts)
{
    std::sort(pts.begin(), pts.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y();
    });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3)
        return pts;
    auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> lo, hi;
    for (const QPointF &q : pts) {
        while (lo.size() >= 2 && cross(lo[lo.size() - 2], lo.back(), q) <= 0)
            lo.pop_back();
        lo.push_back(q);
    }
    for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
        while (hi.size() >= 2 && cross(hi[hi.size() - 2], hi.back(), *it) <= 0)
            hi.pop_back();
        hi.push_back(*it);
    }
    lo.pop_back();
    hi.pop_back();
    lo.insert(lo.end(), hi.begin(), hi.end());
    return lo;
}

}  // namespace

std::optional<QImage> own(const QImage &rgbIn, const QImage &alpha)
{
    const QImage im = rgb32(rgbIn);
    const int w = im.width(), h = im.height();
    if (w < 8 || h < 8)
        return std::nullopt;
    if (!alpha.isNull()) {  // (its own see-through parts: what's solid is the shape)
        const QImage a = alpha.convertToFormat(QImage::Format_Grayscale8).scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QImage fg = blank(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                at(fg, x, y) = get(a, x, y) > 40 ? 255 : 0;
        return fg;
    }
    // a background of one colour round it, from its edges in
    const auto [mode, share] = edgeColour(im);
    if (share < 0.4)
        return std::nullopt;  // (no background round it: all picture)
    QImage alike = blank(w, h), ink = blank(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int d = diff(im.pixel(x, y), mode);
            at(alike, x, y) = d <= 36 ? 255 : 0;
            at(ink, x, y) = d > 36 ? 255 : 0;
        }
    for (auto [x, y] : edgeSpots(w, h))
        flood(alike, x, y, 255, 128);
    QImage fg = blank(w, h);
    int fgCount = 0, inkCount = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool on = get(alike, x, y) != 128;
            at(fg, x, y) = on ? 255 : 0;
            fgCount += on;
            inkCount += on && get(ink, x, y);
        }
    // lines on the background - a signature, a sketch: inside its loops it's the background
    // still - not a filled shape: the whole picture
    if (inkCount < 0.75 * fgCount)
        return std::nullopt;
    return fg;
}

std::optional<QImage> onPage(const QImage &rgbIn, const QImage &underIn, bool judging)
{
    const QImage im = rgb32(rgbIn);
    const int w = im.width(), h = im.height();
    if (w < 8 || h < 8)
        return std::nullopt;
    const QImage under = rgb32(underIn.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    QImage alike = blank(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            at(alike, x, y) = diff(im.pixel(x, y), under.pixel(x, y)) < 30 ? 255 : 0;
    const auto edge = edgeSpots(w, h);
    int same = 0;
    for (auto [x, y] : edge)
        same += get(alike, x, y) == 255;
    if (same < 0.7 * double(edge.size()))
        return std::nullopt;  // (it isn't sitting on what's under it: a picture of its own)
    for (auto [x, y] : edge)  // (the background: what's alike, from its edges in...)
        flood(alike, x, y, 255, 128);
    // ...and the coloured patches of it it encloses (a slide's band between a logo's rings) -
    // not white or grey ones: they may be the picture's own (a shape's shine, a letter's inside)
    const int least = std::max(4, int(0.001 * w * h));
    int label = 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (get(alike, x, y) != 255 || label >= 120)
                continue;
            const uchar tag = uchar(label);
            // its pixels (and their colour under it)
            std::vector<std::pair<int, int>> part;
            {
                std::vector<std::pair<int, int>> stack{{x, y}};
                at(alike, x, y) = tag;
                while (!stack.empty()) {
                    auto [px, py] = stack.back();
                    stack.pop_back();
                    part.push_back({px, py});
                    const int nx[4] = {px - 1, px + 1, px, px}, ny[4] = {py, py, py - 1, py + 1};
                    for (int k = 0; k < 4; ++k)
                        if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < w && ny[k] < h && get(alike, nx[k], ny[k]) == 255) {
                            at(alike, nx[k], ny[k]) = tag;
                            stack.push_back({nx[k], ny[k]});
                        }
                }
            }
            double r = 0, g = 0, b = 0;
            for (auto [px, py] : part) {
                const QRgb c = under.pixel(px, py);
                r += qRed(c), g += qGreen(c), b += qBlue(c);
            }
            const double n = double(part.size());
            r /= n, g /= n, b /= n;
            const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
            const bool coloured = hi > 40 && (hi - lo) / hi > 0.18;
            const uchar to = coloured && int(part.size()) >= least ? 128 : 0;
            for (auto [px, py] : part)
                at(alike, px, py) = to;
            ++label;
        }
    // the background found must be plain - a colour or a few (a slide's bands); busy artwork
    // saved round a logo matches what's under it only roughly: cut to it, the cut is ragged -
    // the picture is left whole
    std::map<std::tuple<int, int, int>, int> counts;
    int total = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2)
            if (get(alike, x, y) == 128) {
                const QRgb c = under.pixel(x, y);
                ++counts[{qRed(c) / 24, qGreen(c) / 24, qBlue(c) / 24}];
                ++total;
            }
    std::vector<int> sizes;
    for (const auto &[k, v] : counts)
        sizes.push_back(v);
    std::sort(sizes.rbegin(), sizes.rend());
    int top3 = 0;
    for (size_t k = 0; k < sizes.size() && k < 3; ++k)
        top3 += sizes[k];
    if (judging && (!total || top3 < 0.9 * total))
        return std::nullopt;
    QImage fg = blank(w, h);
    int fgCount = 0, inkCount = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool on = get(alike, x, y) != 128;
            at(fg, x, y) = on ? 255 : 0;
            fgCount += on;
            inkCount += on && diff(im.pixel(x, y), under.pixel(x, y)) >= 30;
        }
    // lines on what's under it - a signature, a sketch: inside its loops it's the page still -
    // not a filled shape: the whole picture (as from the picture alone)
    if (judging && inkCount < 0.75 * fgCount)
        return std::nullopt;
    return fg;
}

bool ownFits(const QImage &rgbIn, const QImage &underIn)
{
    const QImage im = rgb32(rgbIn);
    const int w = im.width(), h = im.height();
    if (w < 2 || h < 2)
        return false;
    const QImage under = rgb32(underIn.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    const QRgb mode = edgeColour(im).first;
    const auto spots = edgeSpots(w, h);
    int alike = 0;
    for (auto [x, y] : spots)
        alike += diff(under.pixel(x, y), mode) <= 40;
    return alike >= 0.6 * double(spots.size());  // (its background's edges: the page's colour)
}

std::optional<Found> shapeIn(QImage fg)
{
    fg = fg.convertToFormat(QImage::Format_Grayscale8);
    const int w = fg.width(), h = fg.height();
    // its pieces: each a value of its own (1..253), its size, at an edge?
    struct Part {
        int size = 0;
        bool edge = false;
    };
    std::map<int, Part> parts;
    int label = 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (get(fg, x, y) == 255 && label < 254) {
                parts[label].size = flood(fg, x, y, 255, uchar(label));
                ++label;
            }
    if (parts.empty())
        return std::nullopt;
    for (auto [x, y] : edgeSpots(w, h)) {
        auto it = parts.find(get(fg, x, y));
        if (it != parts.end())
            it->second.edge = true;
    }
    int big = parts.begin()->first;
    for (const auto &[k, p] : parts)
        if (p.size > parts[big].size)
            big = k;
    const int most = parts[big].size;
    if (most < 0.12 * w * h)
        return std::nullopt;  // (hardly anything there: not a shape)
    // kept: it, what's inside its edges (letters...), and anything at its edges nearly as big -
    // not specks, nor the bits of the shapes beside it, cut off at its edges
    // (inside its edges: within the outline of the shape itself - its letters, its shine; bits
    // beside it, apart from its edges (strokes of the next tile's border, saved into it), aren't)
    std::vector<QPointF> bigPts;
    for (int y = 0; y < h; ++y) {
        int first = -1, last = -1;
        for (int x = 0; x < w; ++x)
            if (get(fg, x, y) == big) {
                first = first < 0 ? x : first;
                last = x;
            }
        if (first >= 0)
            bigPts.insert(bigPts.end(), {QPointF(first - 1, y - 1), QPointF(last + 2, y - 1), QPointF(first - 1, y + 2), QPointF(last + 2, y + 2)});
    }
    const std::vector<QPointF> bigHull = convexHull(bigPts);
    const QPolygonF outline(QList<QPointF>(bigHull.begin(), bigHull.end()));
    std::map<int, int> inside;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = get(fg, x, y);
            if (v && v != big && parts.count(v) && outline.containsPoint(QPointF(x + 0.5, y + 0.5), Qt::OddEvenFill))
                ++inside[v];
        }
    // (pieces apart from it, outside it: a logo's rings and letters - together a real part of
    // it, kept - or a few specks of the next tile's border saved into it - left out)
    int apart = 0;
    for (const auto &[k, p] : parts)
        if (k != big && p.size >= 3 && !p.edge && inside[k] < 0.8 * p.size)
            apart += p.size;
    const bool apartKept = apart >= 0.05 * most;
    bool keep[256] = {};
    for (const auto &[k, p] : parts)
        keep[k] = k == big || (p.size >= 3 && (p.edge ? p.size >= 0.35 * most : apartKept || inside[k] >= 0.8 * p.size));
    Found out;
    out.pieces = blank(w, h);
    std::vector<QPointF> pts;
    int kept = 0;
    for (int y = 0; y < h; ++y) {
        int first = -1, last = -1;
        for (int x = 0; x < w; ++x)
            if (keep[get(fg, x, y)] && get(fg, x, y)) {
                at(out.pieces, x, y) = 255;
                ++kept;
                if (first < 0)
                    first = x;
                last = x;
            }
        if (first >= 0)
            for (QPointF q : {QPointF(first - 1, y - 1), QPointF(last + 2, y - 1), QPointF(first - 1, y + 2), QPointF(last + 2, y + 2)})
                pts.push_back(QPointF(std::clamp(q.x(), 0.0, double(w)), std::clamp(q.y(), 0.0, double(h))));
    }
    const std::vector<QPointF> hull = convexHull(pts);
    double area = 0;
    for (size_t k = 0; k < hull.size(); ++k) {
        const QPointF &a = hull[k], &b = hull[(k + 1) % hull.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    area = std::abs(area) / 2;
    if (hull.size() < 3 || area > 0.92 * w * h)
        return std::nullopt;  // (it fills its box)
    if (kept < 0.4 * area)
        return std::nullopt;  // (strokes - a signature, a sketch - not a filled shape: the whole picture)
    for (const QPointF &q : hull)
        out.hull.push_back(QPointF(q.x() / w, q.y() / h));
    return out;
}

QByteArray clipPath(const QImage &fineIn, const QImage &piecesIn)
{
    const QImage fine = fineIn.convertToFormat(QImage::Format_Grayscale8);
    const int w = fine.width(), h = fine.height();
    // its own pieces, a little bigger (5 x 5 at the size looked at finely), and the fine shape inside them
    const QImage small = piecesIn.convertToFormat(QImage::Format_Grayscale8).scaled(w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QImage grown = blank(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar v = 0;
            for (int dy = -2; dy <= 2 && !v; ++dy)
                for (int dx = -2; dx <= 2 && !v; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px >= 0 && py >= 0 && px < w && py < h && get(small, px, py) > 127)
                        v = 255;
                }
            at(grown, x, y) = v;
        }
    // each row's runs of the shape: (from, to)
    std::vector<std::vector<std::pair<int, int>>> rows(size_t(std::max(0, h)));
    for (int y = 0; y < h; ++y) {
        int start = -1;
        for (int x = 0; x <= w; ++x) {
            const bool on = x < w && get(fine, x, y) > 127 && get(grown, x, y);
            if (on && start < 0)
                start = x;
            else if (!on && start >= 0) {
                rows[size_t(y)].push_back({start, x});
                start = -1;
            }
        }
    }
    QByteArray out;
    for (int y = 0; y < h;) {  // (rows with the same runs: one box each, as tall as they are)
        int y1 = y + 1;
        while (y1 < h && rows[size_t(y1)] == rows[size_t(y)])
            ++y1;
        for (auto [x0, x1] : rows[size_t(y)]) {
            // (each box half a pixel bigger all round: boxes side by side overlap - drawn edge
            // to edge, a hairline of what's under shows between them)
            for (double v : {(x0 - 0.5) / w, 1 - (y1 + 0.5) / h, double(x1 - x0 + 1) / w, double(y1 - y + 1) / h})
                out += QByteArray::number(v, 'f', 5) + ' ';  // (never the locale's commas)
            out += "re ";
        }
        y = y1;
    }
    return out.trimmed();
}

}  // namespace shapes
