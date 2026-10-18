#include "core/ScanFont.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QPointF>
#include <QRect>
#include <QtEndian>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb-ot.h>
#include <hb.h>
#include <potracelib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>

namespace scanfont {

namespace {

constexpr int kEm = 192;           // (a glyph's copies are fitted, averaged and traced this many pixels to the em)
constexpr size_t kKeep = 8;        // (copies kept of each glyph: the most alike)
constexpr double kMargin = 0.08;   // (room round a glyph's own box: the scan's ink can be heavier, a little off)
constexpr double kMarginUp = 0.25; // (... and above and below it: a dot, an accent higher / lower than the font's)
constexpr double kReach = 0.06;    // (a glyph's ink: this near its own shape - not its neighbours')
constexpr double kBandUp = 1.1, kBandDown = 0.6;  // (a word cut into glyphs: from this far above its baseline to this far below, ems)

struct Sample {
    std::vector<uchar> px;  // (its darkness, 0..255, where the glyph's ink can be)
    double score;
};

// a glyph of the base font, in its frame of kEm pixels to the em (row 0 at its top)
struct Glyph {
    bool tried = false, empty = true;
    bool rtl = false;          // (seen right to left - an Arabic letter's form, joined to its neighbours)
    int w = 0, h = 0;
    int left = 0, bottom = 0;  // (font units: the frame's bottom left corner)
    std::vector<float> base;   // (the font's own glyph: how much of each pixel it covers)
    std::vector<char> near;    // (near its own ink: where it's fitted to the scan)
    std::vector<char> band;    // (above and below its ink: an "i"'s dot, an accent - wherever the scan has it)
    std::vector<char> cut;     // (where its ink in the scan can be: either)
    std::vector<double> advances;  // (how far the scan's next letter was from it, font units: its width as printed)
    struct Free {                   // (a copy cut from a word laid over the scan elastically: kEm to the em, from
        std::vector<uchar> px;      // kBandUp above the baseline to kBandDown below, as wide as it was)
        int w = 0;
        double cost = 0;
    };
    std::vector<Free> free;
    std::vector<Sample> samples;
    double area = 0, edge = 0;  // (the font's glyph: pixels inked, pixels on its edge)
};

struct Point {
    double x, y;
    bool on;
};
using Contour = std::vector<Point>;

quint16 be16(const char *p)
{
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

quint32 be32(const char *p)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

void put16(QByteArray &b, int v)
{
    const quint16 x = quint16(v);
    b.append(char(x >> 8));
    b.append(char(x & 0xFF));
}

void put32(QByteArray &b, quint32 v)
{
    put16(b, int(v >> 16));
    put16(b, int(v & 0xFFFF));
}

void set16(QByteArray &b, int at, int v)
{
    if (at + 2 <= b.size()) {
        b[at] = char(quint16(v) >> 8);
        b[at + 1] = char(quint16(v) & 0xFF);
    }
}

void set32(QByteArray &b, int at, quint32 v)
{
    set16(b, at, int(v >> 16));
    set16(b, at + 2, int(v & 0xFFFF));
}

quint32 checksum(const QByteArray &b)
{
    quint32 sum = 0;
    QByteArray p = b;
    while (p.size() % 4)
        p.append('\0');
    for (int i = 0; i < p.size(); i += 4)
        sum += be32(p.constData() + i);
    return sum;
}

// (a cubic curve as quadratic ones - TrueType's - near enough: within tol)
void cubicToQuads(Contour &c, QPointF p0, QPointF c1, QPointF c2, QPointF p3, double tol)
{
    const QPointF dd = p3 - 3 * c2 + 3 * c1 - p0;
    const double e = std::hypot(dd.x(), dd.y());
    const int n = std::clamp(int(std::ceil(std::cbrt(e * std::sqrt(3.0) / 36 / tol))), 1, 8);
    auto at = [&](double t) {
        const double u = 1 - t;
        return u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p3;
    };
    auto slope = [&](double t) {
        const double u = 1 - t;
        return 3 * u * u * (c1 - p0) + 6 * u * t * (c2 - c1) + 3 * t * t * (p3 - c2);
    };
    for (int k = 0; k < n; ++k) {
        const double t0 = double(k) / n, t1 = double(k + 1) / n, dt = t1 - t0;
        const QPointF q0 = at(t0), q3 = at(t1);
        const QPointF q1 = q0 + slope(t0) * dt / 3, q2 = q3 - slope(t1) * dt / 3;
        const QPointF ctl = (3 * (q1 + q2) - (q0 + q3)) / 4;
        c.push_back({ctl.x(), ctl.y(), false});
        c.push_back({q3.x(), q3.y(), true});
    }
}

double signedArea(const Contour &c)
{
    double a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const Point &p = c[i], &q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

// (the ink traced smooth - potrace - in the font's units; outer contours clockwise, holes the other way:
// TrueType's)
std::vector<Contour> trace(const std::vector<char> &ink, const Glyph &g, int upem, int em = kEm, double alphamax = 1.1, double tolerance = 0.3)
{
    std::vector<Contour> out;
    constexpr int N = int(sizeof(potrace_word) * 8);
    potrace_bitmap_t bm;
    bm.w = g.w;
    bm.h = g.h;
    bm.dy = (g.w + N - 1) / N;
    std::vector<potrace_word> map(size_t(bm.dy) * size_t(g.h), 0);
    bm.map = map.data();
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u)
            if (ink[size_t(v) * size_t(g.w) + size_t(u)]) {
                const int y = g.h - 1 - v;  // (potrace's rows: from the bottom up)
                map[size_t(y) * size_t(bm.dy) + size_t(u / N)] |= potrace_word(1) << (N - 1 - u % N);
            }
    potrace_param_t *param = potrace_param_default();
    if (!param)
        return out;
    param->turdsize = std::max(2, int(12.0 * em * em / (kEm * kEm)));  // (specks left out)
    param->alphamax = alphamax;
    param->opticurve = 1;
    param->opttolerance = tolerance;
    potrace_state_t *st = potrace_trace(param, &bm);
    const double k = double(upem) / em, tol = upem / 2000.0;
    auto P = [&](const potrace_dpoint_t &p) { return QPointF(g.left + p.x * k, g.bottom + p.y * k); };
    if (st && st->status == POTRACE_STATUS_OK)
        for (potrace_path_t *path = st->plist; path; path = path->next) {
            const potrace_curve_t &cv = path->curve;
            if (cv.n < 2)
                continue;
            Contour c;
            QPointF cur = P(cv.c[cv.n - 1][2]);
            c.push_back({cur.x(), cur.y(), true});
            for (int i = 0; i < cv.n; ++i) {
                const QPointF end = P(cv.c[i][2]);
                if (cv.tag[i] == POTRACE_CORNER) {
                    const QPointF corner = P(cv.c[i][1]);
                    c.push_back({corner.x(), corner.y(), true});
                    c.push_back({end.x(), end.y(), true});
                } else {
                    cubicToQuads(c, cur, P(cv.c[i][0]), P(cv.c[i][1]), end, tol);
                }
                cur = end;
            }
            c.pop_back();  // (its end: its start again)
            if (c.size() < 3)
                continue;
            if ((path->sign == '+') != (signedArea(c) < 0))
                std::reverse(c.begin(), c.end());
            while (!c.front().on)  // (starting on the curve)
                std::rotate(c.begin(), c.begin() + 1, c.end());
            out.push_back(std::move(c));
        }
    if (st)
        potrace_state_free(st);
    potrace_param_free(param);
    return out;
}

// a simple TrueType glyph: its contours (font units), no instructions; its box
QByteArray simpleGlyph(const std::vector<Contour> &contours, int *xMin, int *yMin, int *xMax, int *yMax, int *points)
{
    QByteArray b;
    std::vector<std::array<int, 2>> pts;
    std::vector<bool> on;
    std::vector<int> ends;
    for (const Contour &c : contours) {
        for (const Point &p : c) {
            pts.push_back({int(std::lround(p.x)), int(std::lround(p.y))});
            on.push_back(p.on);
        }
        if (!c.empty())
            ends.push_back(int(pts.size()) - 1);
    }
    *points = int(pts.size());
    if (pts.empty())
        return b;  // (no outline: an empty glyph)
    *xMin = *yMin = 32767;
    *xMax = *yMax = -32768;
    for (const auto &p : pts) {
        *xMin = std::min(*xMin, p[0]), *xMax = std::max(*xMax, p[0]);
        *yMin = std::min(*yMin, p[1]), *yMax = std::max(*yMax, p[1]);
    }
    put16(b, int(ends.size()));
    put16(b, *xMin);
    put16(b, *yMin);
    put16(b, *xMax);
    put16(b, *yMax);
    for (int e : ends)
        put16(b, e);
    put16(b, 0);  // (no instructions)
    for (bool o : on)
        b.append(char(o ? 1 : 0));
    int last = 0;
    for (const auto &p : pts)
        put16(b, p[0] - last), last = p[0];
    last = 0;
    for (const auto &p : pts)
        put16(b, p[1] - last), last = p[1];
    while (b.size() % 4)
        b.append('\0');
    return b;
}

// (an outline - font units - drawn in a frame of em pixels to the em from left / bottom: how much of each
// pixel it covers, row 0 at the top)
std::vector<float> coverage(FT_Library lib, const FT_Outline &src, int left, int bottom, int w, int h, int upem, int em)
{
    std::vector<float> out;
    FT_Outline o;
    if (FT_Outline_New(lib, FT_UInt(src.n_points), src.n_contours, &o))
        return out;
    FT_Outline_Copy(&src, &o);
    FT_Outline_Translate(&o, -left, -bottom);
    FT_Matrix scale = {FT_Fixed(em * 64.0 / upem * 65536), 0, 0, FT_Fixed(em * 64.0 / upem * 65536)};
    FT_Outline_Transform(&o, &scale);
    std::vector<uchar> px(size_t(w) * size_t(h), 0);
    FT_Bitmap bm = {};
    bm.rows = unsigned(h);
    bm.width = unsigned(w);
    bm.pitch = w;
    bm.buffer = px.data();
    bm.num_grays = 256;
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    FT_Outline_Get_Bitmap(lib, &o, &bm);
    FT_Outline_Done(lib, &o);
    out.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i)
        out[i] = px[i] / 255.0f;
    return out;
}

// (blurred as a gaussian of sigma pixels is - three box blurs each way, quick at any size)
void boxBlur(std::vector<float> &px, int w, int h, double sigma)
{
    if (sigma < 0.3)
        return;
    const int r = std::max(1, int(std::lround((std::sqrt(12 * sigma * sigma / 3 + 1) - 1) / 2)));
    std::vector<float> line;
    auto pass = [&](int n, auto get, auto set) {
        line.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            line[size_t(i)] = get(i);
        double sum = 0;
        for (int i = -r; i <= r; ++i)
            sum += line[size_t(std::clamp(i, 0, n - 1))];
        for (int i = 0; i < n; ++i) {
            set(i, float(sum / (2 * r + 1)));
            sum += line[size_t(std::clamp(i + r + 1, 0, n - 1))] - line[size_t(std::clamp(i - r, 0, n - 1))];
        }
    };
    for (int k = 0; k < 3; ++k) {
        for (int v = 0; v < h; ++v)
            pass(w, [&](int i) { return px[size_t(v) * size_t(w) + size_t(i)]; }, [&](int i, float x) { px[size_t(v) * size_t(w) + size_t(i)] = x; });
        for (int u = 0; u < w; ++u)
            pass(h, [&](int i) { return px[size_t(i) * size_t(w) + size_t(u)]; }, [&](int i, float x) { px[size_t(i) * size_t(w) + size_t(u)] = x; });
    }
}

// (the font's glyph's small pieces apart from its body - an "i"'s dot, an accent, Arabic's dots: their boxes)
std::vector<QRect> marksOf(const Glyph &g)
{
    std::vector<QRect> out;
    const size_t size = g.base.size();
    std::vector<char> seen(size, 0);
    std::vector<std::pair<size_t, QRect>> pieces;
    for (size_t start = 0; start < size; ++start) {
        if (g.base[start] <= 0.5f || seen[start])
            continue;
        size_t n = 0;
        int x0 = g.w, y0 = g.h, x1 = -1, y1 = -1;
        std::deque<size_t> todo{start};
        seen[start] = 1;
        while (!todo.empty()) {
            const size_t i = todo.front();
            todo.pop_front();
            ++n;
            const int u = int(i % size_t(g.w)), v = int(i / size_t(g.w));
            x0 = std::min(x0, u), x1 = std::max(x1, u), y0 = std::min(y0, v), y1 = std::max(y1, v);
            const int nu[4] = {u - 1, u + 1, u, u}, nv[4] = {v, v, v - 1, v + 1};
            for (int q = 0; q < 4; ++q)
                if (nu[q] >= 0 && nv[q] >= 0 && nu[q] < g.w && nv[q] < g.h) {
                    const size_t j = size_t(nv[q]) * size_t(g.w) + size_t(nu[q]);
                    if (g.base[j] > 0.5f && !seen[j])
                        seen[j] = 1, todo.push_back(j);
                }
        }
        pieces.emplace_back(n, QRect(QPoint(x0, y0), QPoint(x1, y1)));
    }
    if (pieces.size() < 2)
        return out;
    const size_t body = std::max_element(pieces.begin(), pieces.end(), [](const auto &a, const auto &b) { return a.first < b.first; })->first;
    for (const auto &[n, box] : pieces)
        if (n < body && n <= size_t(0.03 * kEm * kEm))
            out.push_back(box);
    return out;
}

// (a glyph's copies cut from words: made as wide as they mostly were (W), averaged - false: none to average)
double likeOwn(const std::vector<char> &ink, int W, int Hcan, const Glyph &g);

bool freeMean(const Glyph &g, int *W, std::vector<float> *mean)
{
    if (g.free.empty())
        return false;
    std::vector<int> ws;
    for (const Glyph::Free &f : g.free)
        ws.push_back(f.w);
    std::nth_element(ws.begin(), ws.begin() + long(ws.size() / 2), ws.end());
    *W = ws[ws.size() / 2];
    const int Hcan = int(std::lround((kBandUp + kBandDown) * kEm));
    const int w = *W;
    const size_t size = size_t(w) * size_t(Hcan);
    std::vector<std::vector<float>> copies;
    for (const Glyph::Free &f : g.free) {
        if (std::abs(f.w - w) > 0.35 * w)
            continue;  // (cut much wider / narrower than it mostly was: laid over wrong)
        std::vector<float> c(size, 0);
        for (int v = 0; v < Hcan; ++v)
            for (int u = 0; u < w; ++u) {
                const double x = (u + 0.5) * f.w / w - 0.5;
                const int a = std::clamp(int(std::floor(x)), 0, f.w - 1), b = std::clamp(a + 1, 0, f.w - 1);
                const double t = std::clamp(x - a, 0.0, 1.0);
                c[size_t(v) * size_t(w) + size_t(u)] =
                    float((f.px[size_t(v) * size_t(f.w) + size_t(a)] * (1 - t) + f.px[size_t(v) * size_t(f.w) + size_t(b)] * t) / 255.0);
            }
        copies.push_back(std::move(c));
    }
    if (copies.empty())
        return false;
    // (each copy laid over the others where it matches best - the cuts between letters fall a little differently each
    // time: unaligned, a thin upright - a lam's - averages away)
    const int most = std::max(1, int(0.25 * w));
    auto match = [&](const std::vector<float> &a, const std::vector<float> &b, int shift) {
        double ab = 0, aa = 0, bb = 0;
        for (int v = 0; v < Hcan; ++v)
            for (int u = std::max(0, shift); u < std::min(w, w + shift); ++u) {
                const double x = a[size_t(v) * size_t(w) + size_t(u)], y = b[size_t(v) * size_t(w) + size_t(u - shift)];
                ab += x * y, aa += x * x, bb += y * y;
            }
        return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
    };
    auto best = [&](const std::vector<float> &ref, const std::vector<float> &c, double *score) {
        int at = 0;
        double top = -1;
        for (int sh = -most; sh <= most; ++sh) {
            const double m = match(ref, c, sh) - 0.002 * std::abs(sh);
            if (m > top)
                top = m, at = sh;
        }
        if (score)
            *score = top;
        return at;
    };
    size_t refIndex = 0;
    if (copies.size() <= 2) {  // (two, one: the one most like the font's own letter - the other may be a misread's)
        double top = -2;
        for (size_t i = 0; i < copies.size(); ++i) {
            std::vector<char> ink(size);
            for (size_t k = 0; k < size; ++k)
                ink[k] = copies[i][k] >= 0.5f;
            const double like = likeOwn(ink, w, Hcan, g);
            if (like > top)
                top = like, refIndex = i;
        }
    } else {  // (the copy most like the rest: the one the others are laid over)
        double topSum = -1;
        for (size_t i = 0; i < copies.size(); ++i) {
            double sum = 0;
            for (size_t j = 0; j < copies.size(); ++j)
                if (j != i) {
                    double sc = 0;
                    best(copies[i], copies[j], &sc);
                    sum += sc;
                }
            if (sum > topSum)
                topSum = sum, refIndex = i;
        }
    }
    mean->assign(size, 0);
    int used = 0;
    for (size_t i = 0; i < copies.size(); ++i) {
        double sc = 1;
        const int sh = i == refIndex ? 0 : best(copies[refIndex], copies[i], &sc);
        if (sc < 0.6)
            continue;  // (not the same letter: a misread's, laid over wrong)
        ++used;
        for (int v = 0; v < Hcan; ++v)
            for (int u = std::max(0, sh); u < std::min(w, w + sh); ++u)
                (*mean)[size_t(v) * size_t(w) + size_t(u)] += copies[i][size_t(v) * size_t(w) + size_t(u - sh)];
    }
    for (float &v : *mean)
        v /= float(used);
    return true;
}

// (how like the font's own glyph a glyph cut from words is: their inks' boxes fitted over each other, the ink they
// have in common - -1: nothing to tell by)
double likeOwn(const std::vector<char> &ink, int W, int Hcan, const Glyph &g)
{
    auto boxOf = [](const std::vector<char> &m, int w, int h) {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (m[size_t(y) * size_t(w) + size_t(x)])
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        return x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
    };
    std::vector<char> own(g.base.size());
    for (size_t i = 0; i < own.size(); ++i)
        own[i] = g.base[i] > 0.5f;
    const QRect a = boxOf(ink, W, Hcan), b = boxOf(own, g.w, g.h);
    if (a.isEmpty() || b.isEmpty())
        return -1;
    long both = 0, either = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const bool p = ink[size_t(a.top() + y) * size_t(W) + size_t(a.left() + x)];
            const int bx = b.left() + int((x + 0.5) * b.width() / a.width()), by = b.top() + int((y + 0.5) * b.height() / a.height());
            const bool q = own[size_t(by) * size_t(g.w) + size_t(bx)];
            both += p && q, either += p || q;
        }
    return either > 0 ? double(both) / either : -1;
}

}  // namespace

struct Builder::Impl {
    fonts::Found base;
    QByteArray file;
    quint32 faceAt = 0;  // (its table directory - a collection's font: not at the start)
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    int upem = 1000;
    std::map<unsigned, Glyph> glyphs;
    double likeness = -1;
    bool copy = true;

    Glyph &frame(unsigned gid);
    QHash<QByteArray, QByteArray> tables() const;
};

Glyph &Builder::Impl::frame(unsigned gid)
{
    Glyph &g = glyphs[gid];
    if (g.tried)
        return g;
    g.tried = true;
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points == 0)
        return g;
    FT_BBox box;
    FT_Outline_Get_CBox(&face->glyph->outline, &box);
    const int m = int(std::lround(kMargin * upem)), mu = int(std::lround(kMarginUp * upem));
    g.left = int(box.xMin) - m;
    g.bottom = int(box.yMin) - mu;
    g.w = int(std::ceil(double(box.xMax - box.xMin + 2 * m) * kEm / upem));
    g.h = int(std::ceil(double(box.yMax - box.yMin + 2 * mu) * kEm / upem));
    if (g.w < 3 || g.h < 3 || g.w > 5 * kEm || g.h > 5 * kEm)
        return g;
    // (the font's glyph drawn in its frame)
    FT_Outline o;
    if (FT_Outline_New(lib, FT_UInt(face->glyph->outline.n_points), face->glyph->outline.n_contours, &o))
        return g;
    FT_Outline_Copy(&face->glyph->outline, &o);
    FT_Outline_Translate(&o, -g.left, -g.bottom);
    FT_Matrix scale = {FT_Fixed(kEm * 64.0 / upem * 65536), 0, 0, FT_Fixed(kEm * 64.0 / upem * 65536)};
    FT_Outline_Transform(&o, &scale);
    std::vector<uchar> px(size_t(g.w) * size_t(g.h), 0);
    FT_Bitmap bm = {};
    bm.rows = unsigned(g.h);
    bm.width = unsigned(g.w);
    bm.pitch = g.w;
    bm.buffer = px.data();
    bm.num_grays = 256;
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    FT_Outline_Get_Bitmap(lib, &o, &bm);
    FT_Outline_Done(lib, &o);
    g.base.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i)
        g.base[i] = px[i] / 255.0f;
    // (its ink and edge; where the scan's ink of it can be: near its own)
    std::vector<char> inked(px.size(), 0);
    for (size_t i = 0; i < px.size(); ++i)
        inked[i] = g.base[i] > 0.25f;
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u) {
            const size_t i = size_t(v) * size_t(g.w) + size_t(u);
            if (g.base[i] <= 0.5f)
                continue;
            ++g.area;
            const bool rim = u == 0 || v == 0 || u == g.w - 1 || v == g.h - 1 || g.base[i - 1] <= 0.5f || g.base[i + 1] <= 0.5f ||
                             g.base[i - size_t(g.w)] <= 0.5f || g.base[i + size_t(g.w)] <= 0.5f;
            g.edge += rim ? 1 : 0;
        }
    const int r = int(std::lround(kReach * kEm));
    std::vector<char> rows(px.size(), 0);
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u) {
            bool any = false;
            for (int k = std::max(0, u - r); k <= std::min(g.w - 1, u + r) && !any; ++k)
                any = inked[size_t(v) * size_t(g.w) + size_t(k)];
            rows[size_t(v) * size_t(g.w) + size_t(u)] = any;
        }
    g.near.assign(px.size(), 0);
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u) {
            bool any = false;
            for (int k = std::max(0, v - r); k <= std::min(g.h - 1, v + r) && !any; ++k)
                any = rows[size_t(k) * size_t(g.w) + size_t(u)];
            g.near[size_t(v) * size_t(g.w) + size_t(u)] = any;
        }
    std::vector<char> column(size_t(g.w), 0);
    for (int u = 0; u < g.w; ++u)
        for (int v = 0; v < g.h && !column[size_t(u)]; ++v)
            column[size_t(u)] = inked[size_t(v) * size_t(g.w) + size_t(u)];
    // (a glyph with dots, an accent of its own: looked for further to either side - another typeface puts them elsewhere)
    const int widen = int(std::lround((marksOf(g).empty() ? 0.02 : 0.12) * kEm));
    g.band.assign(px.size(), 0);
    g.cut.assign(px.size(), 0);
    for (int u = 0; u < g.w; ++u) {
        bool in = false;
        for (int k = std::max(0, u - widen); k <= std::min(g.w - 1, u + widen) && !in; ++k)
            in = column[size_t(k)];
        for (int v = 0; v < g.h; ++v) {
            const size_t i = size_t(v) * size_t(g.w) + size_t(u);
            g.band[i] = in;
            g.cut[i] = in || g.near[i];
        }
    }
    g.empty = g.area < 4;
    return g;
}

QHash<QByteArray, QByteArray> Builder::Impl::tables() const
{
    QHash<QByteArray, QByteArray> out;
    if (file.size() < int(faceAt) + 12)
        return out;
    const char *d = file.constData();
    const int n = be16(d + faceAt + 4);
    for (int i = 0; i < n; ++i) {
        const char *rec = d + faceAt + 12 + 16 * i;
        if (rec + 16 > d + file.size())
            break;
        const quint32 at = be32(rec + 8), len = be32(rec + 12);
        if (qint64(at) + len <= file.size())
            out.insert(QByteArray(rec, 4), file.mid(int(at), int(len)));
    }
    return out;
}

Builder::Builder(const fonts::Found &base) : d(std::make_unique<Impl>())
{
    d->base = base;
    QFile f(base.path);
    if (base.path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return;
    d->file = f.readAll();
    if (d->file.size() < 12)
        return;
    if (d->file.startsWith("ttcf")) {  // (a collection: its font's directory)
        const int n = int(be32(d->file.constData() + 8));
        if (base.index >= n || d->file.size() < 12 + 4 * (base.index + 1))
            return;
        d->faceAt = be32(d->file.constData() + 12 + 4 * base.index);
    }
    if (be32(d->file.constData() + d->faceAt) != 0x00010000 && d->file.mid(int(d->faceAt), 4) != "true")
        return;  // (TrueType outlines only: a CFF font's aren't rewritten)
    if (FT_Init_FreeType(&d->lib))
        return;
    if (FT_New_Memory_Face(d->lib, reinterpret_cast<const FT_Byte *>(d->file.constData()), FT_Long(d->file.size()), base.index, &d->face)) {
        d->face = nullptr;
        return;
    }
    d->upem = std::max<int>(16, d->face->units_per_EM);
}

Builder::~Builder()
{
    if (d->face)
        FT_Done_Face(d->face);
    if (d->lib)
        FT_Done_FreeType(d->lib);
}

double Builder::likeness() const
{
    return d->likeness;
}

double Builder::measureLikeness()
{
    double sum = 0;
    int count = 0;
    const int Hcan = int(std::lround((kBandUp + kBandDown) * kEm));
    for (auto &[gid, g] : d->glyphs) {
        int W = 0;
        std::vector<float> mean;
        if (g.empty || !freeMean(g, &W, &mean))
            continue;
        std::vector<char> ink(mean.size(), 0);
        for (size_t i = 0; i < mean.size(); ++i)
            ink[i] = mean[i] >= 0.5f;
        const double like = likeOwn(ink, W, Hcan, g);
        if (like >= 0)
            sum += like, ++count;
    }
    d->likeness = count ? sum / count : -1;
    return d->likeness;
}

void Builder::setCopyShapes(bool copy)
{
    d->copy = copy;
}

bool Builder::ok() const
{
    return d->face != nullptr;
}

const fonts::Found &Builder::base() const
{
    return d->base;
}

double Builder::addLine(const QImage &grey, const QString &text, QPointF origin, double em, double advance, int paper, int ink)
{
    if (!ok() || em < 6 || grey.format() != QImage::Format_Grayscale8 || paper - ink < 40)
        return 0;
    double width = 0;
    const std::vector<ShapedGlyph> shaped = shapeText(text, d->base, em, &width);
    if (shaped.empty() || width <= 0)
        return 0;
    const double s = advance > 0 ? advance / width : 1;  // (the scan's line a little wider / narrower)
    if (s < 0.6 || s > 1.6)
        return 0;
    const int W = grey.width(), H = grey.height();
    const double range = paper - ink;
    // (the scan's darkness - its paper 0, its ink 1 - between its pixels)
    auto dark = [&](double X, double Y) -> float {
        X -= 0.5, Y -= 0.5;
        const int x = int(std::floor(X)), y = int(std::floor(Y));
        if (x < 0 || y < 0 || x + 1 >= W || y + 1 >= H)
            return 0;
        const double fx = X - x, fy = Y - y;
        const uchar *r0 = grey.constScanLine(y), *r1 = grey.constScanLine(y + 1);
        const double g = (r0[x] * (1 - fx) + r0[x + 1] * fx) * (1 - fy) + (r1[x] * (1 - fx) + r1[x + 1] * fx) * fy;
        return float(std::clamp((paper - g) / range, 0.0, 1.0));
    };
    const double k = em / kEm;  // (scan pixels to a frame pixel)
    const int R = std::max(2, int(std::lround(0.06 * em)));
    const int step = std::max(1, int(std::lround(kEm / em)));
    std::vector<double> pens(shaped.size(), std::nan(""));
    int tried = 0, found = 0;
    double alike = 0;  // (how like the font's glyphs its glyphs are, on the whole)
    for (size_t gi = 0; gi < shaped.size(); ++gi) {
        const ShapedGlyph &sg = shaped[gi];
        Glyph &g = d->frame(sg.gid);
        if (g.empty)
            continue;
        g.rtl = g.rtl || sg.rtl;
        const double x0 = origin.x() + s * (sg.x + g.left * em / d->upem), y0 = origin.y() - (sg.y + g.bottom * em / d->upem);
        auto X = [&](int u) { return x0 + s * (u + 0.5) * k; };
        auto Y = [&](int v) { return y0 - (g.h - v - 0.5) * k; };
        if (X(0) - R < 1 || Y(0) - R < 1 || X(g.w) + R > W - 2 || Y(g.h) + R > H - 2)
            continue;
        ++tried;
        // (laid where it fits the scan's ink best - a pixel or two off, with the font's own spacing)
        auto score = [&](double dx, double dy) {
            double bd = 0, bb = 0, dd = 0;
            for (int v = 0; v < g.h; v += step)
                for (int u = 0; u < g.w; u += step) {
                    const size_t i = size_t(v) * size_t(g.w) + size_t(u);
                    if (!g.near[i])
                        continue;
                    const double b = g.base[i], dk = dark(X(u) + dx, Y(v) + dy);
                    bd += b * dk, bb += b * b, dd += dk * dk;
                }
            return bb > 0 && dd > 0 ? bd / std::sqrt(bb * dd) : 0.0;
        };
        double best = -1;
        int bx = 0, by = 0;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                const double sc = score(dx, dy);
                if (sc > best)
                    best = sc, bx = dx, by = dy;
            }
        alike += std::max(0.0, best);
        if (best < (sg.rtl ? 0.72 : 0.65))  // (Arabic's joined letters: closer - a heavy display letter is somewhat like any bold one)
            continue;  // (not that glyph after all: a letter read wrong, a smudge)
        ++found;
        // (to a part of a pixel: the top of a parabola through the best and its neighbours)
        auto peak = [&](double l, double m, double r) {
            const double den = l - 2 * m + r;
            return den < 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
        };
        const double fx = bx + peak(score(bx - 1, by), best, score(bx + 1, by));
        const double fy = by + peak(score(bx, by - 1), best, score(bx, by + 1));
        pens[gi] = origin.x() + s * sg.x + fx;
        // (a right-to-left glyph - Arabic, its letters joined to their neighbours: just its own stretch of the line,
        // from its pen to the next one's - not a neighbour joined to it)
        double from = -1e9, to = 1e9;
        if (sg.rtl) {
            const double next = gi + 1 < shaped.size() ? shaped[gi + 1].x : width;
            from = origin.x() + s * sg.x + fx - 0.03 * em;
            to = origin.x() + s * next + fx + 0.03 * em;
        }
        Sample smp;
        smp.score = best;
        smp.px.assign(size_t(g.w) * size_t(g.h), 0);
        for (int v = 0; v < g.h; ++v)
            for (int u = 0; u < g.w; ++u) {
                const size_t i = size_t(v) * size_t(g.w) + size_t(u);
                const double x = X(u) + fx;
                if (g.cut[i] && x >= from && x <= to)
                    smp.px[i] = uchar(std::lround(dark(x, Y(v) + fy) * 255));
            }
        g.samples.push_back(std::move(smp));
        if (g.samples.size() > kKeep) {
            auto worst = std::min_element(g.samples.begin(), g.samples.end(), [](const Sample &a, const Sample &b) { return a.score < b.score; });
            g.samples.erase(worst);
        }
    }
    // (each glyph's width as printed: from its pen to the next one's, both fitted - not a mark's, of no width)
    for (size_t gi = 0; gi + 1 < shaped.size(); ++gi) {
        const double planned = shaped[gi + 1].x - shaped[gi].x;
        if (std::isnan(pens[gi]) || std::isnan(pens[gi + 1]) || shaped[gi].rtl != shaped[gi + 1].rtl || planned < 0.15 * em)
            continue;
        const double printed = (pens[gi + 1] - pens[gi]) / s;
        if (printed > 0.5 * planned && printed < 1.6 * planned)
            d->glyphs[shaped[gi].gid].advances.push_back(printed * d->upem / em);
    }
    return tried ? alike / tried : 0;
}

double Builder::wordScale(const QImage &grey, const QString &word, const QRect &box, double em, int paper, int ink)
{
    double scale = 0;
    addWord(grey, word, box, 0, em, paper, ink, &scale, false);
    return scale;
}

void Builder::addWord(const QImage &grey, const QString &word, const QRect &box, double baseline, double em, int paper, int ink,
                      double *measured, bool resize)
{
    if (!ok() || em < 8 || grey.format() != QImage::Format_Grayscale8 || paper - ink < 40)
        return;
    const int upem = d->upem, pad = 2;
    // the word's own ink: the marks whose middle is in its box - not the line above's or below's reaching in, not a
    // rule running behind it (a little round each mark kept: its soft edge)
    const QRect area = box.adjusted(-int(0.3 * em), -int(0.8 * em), int(0.3 * em), int(0.8 * em)).intersected(grey.rect());
    std::vector<char> own(size_t(std::max(0, area.width())) * size_t(std::max(0, area.height())), 0);
    {
        const int aw = area.width(), ah = area.height();
        std::vector<char> on(own.size(), 0), seen(own.size(), 0);
        for (int y = 0; y < ah; ++y) {
            const uchar *row = grey.constScanLine(area.top() + y);
            for (int x = 0; x < aw; ++x)
                on[size_t(y) * size_t(aw) + size_t(x)] = (paper - row[area.left() + x]) > 0.5 * (paper - ink);
        }
        const QRect keep = box.adjusted(-int(0.05 * em), -int(0.1 * em), int(0.05 * em), int(0.1 * em));
        std::vector<int> todo, piece;
        std::vector<char> kept(own.size(), 0);
        for (int start = 0; start < aw * ah; ++start) {
            if (!on[size_t(start)] || seen[size_t(start)])
                continue;
            piece.clear();
            todo.assign(1, start);
            seen[size_t(start)] = 1;
            int x0 = aw, y0 = ah, x1 = -1, y1 = -1;
            while (!todo.empty()) {
                const int i = todo.back();
                todo.pop_back();
                piece.push_back(i);
                const int x = i % aw, y = i / aw;
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < aw && ny < ah && on[size_t(ny) * size_t(aw) + size_t(nx)] && !seen[size_t(ny) * size_t(aw) + size_t(nx)])
                            seen[size_t(ny) * size_t(aw) + size_t(nx)] = 1, todo.push_back(ny * aw + nx);
                    }
            }
            const QPoint middle(area.left() + (x0 + x1) / 2, area.top() + (y0 + y1) / 2);
            const bool rule = (x1 - x0) > 1.5 * em && (y1 - y0) < 0.2 * em;
            if (keep.contains(middle) && !rule)
                for (int i : piece)
                    kept[size_t(i)] = 1;
        }
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x)
                if (kept[size_t(y) * size_t(aw) + size_t(x)])
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < aw && ny < ah)
                                own[size_t(ny) * size_t(aw) + size_t(nx)] = 1;
                        }
    }
    // (the scan's darkness there - its paper 0, its ink 1 - just the word's)
    auto darkAt = [&](int gx, int gy) -> float {
        if (!area.contains(gx, gy) || !own[size_t(gy - area.top()) * size_t(area.width()) + size_t(gx - area.left())])
            return 0;
        return float(std::clamp((paper - grey.constScanLine(gy)[gx]) / double(paper - ink), 0.0, 1.0));
    };
    // the word set in the font (its glyphs' ink, the band's height) - at em pixels to the em
    std::vector<ShapedGlyph> shaped;
    double width = 0;
    int Hc = 0, br = 0, Wr = 0;
    std::vector<float> drawn;
    auto setWord = [&](double size) {
        shaped = shapeText(word, d->base, size, &width);
        Hc = int(std::ceil((kBandUp + kBandDown) * size)), br = int(std::lround(kBandUp * size));
        Wr = int(std::ceil(width)) + 2 * pad;
        drawn.assign(size_t(std::max(1, Wr)) * size_t(std::max(1, Hc)), 0);
        for (const ShapedGlyph &sg : shaped) {
            if (FT_Load_Glyph(d->face, sg.gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
                d->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || d->face->glyph->outline.n_points == 0)
                continue;
            const int left = int(std::lround(-(sg.x + pad) * upem / size)), bottom = int(std::lround(-((Hc - br) + sg.y) * upem / size));
            const std::vector<float> cov = coverage(d->lib, d->face->glyph->outline, left, bottom, Wr, Hc, upem, int(std::lround(size)));
            for (size_t i = 0; i < cov.size() && i < drawn.size(); ++i)
                drawn[i] = std::max(drawn[i], cov[i]);
        }
    };
    setWord(em);
    if (shaped.empty() || width <= 0)
        return;
    // its baseline and size on the scan, from the scan itself (Tesseract's baseline wanders in a display font):
    // the baseline where its joining strokes' ink is thickest - their bottom - and the size that makes the word as
    // tall above it as the font's is
    {
        const int yTop = std::max(0, box.top() - int(0.2 * em)), yBot = std::min(grey.height() - 1, box.bottom() + int(0.2 * em));
        std::vector<double> rows(size_t(std::max(0, yBot - yTop + 1)), 0);
        for (int y = yTop; y <= yBot; ++y)
            for (int x = std::max(0, box.left()); x <= std::min(grey.width() - 1, box.right()); ++x)
                rows[size_t(y - yTop)] += darkAt(x, y);
        int peak = -1;
        for (int y = box.top() + box.height() / 3; y <= std::min(yBot, box.bottom()); ++y)
            if (y >= yTop && (peak < 0 || rows[size_t(y - yTop)] > rows[size_t(peak - yTop)]))
                peak = y;
        if (peak >= 0 && rows[size_t(peak - yTop)] > 0) {
            const double most = rows[size_t(peak - yTop)];
            int base = peak;
            while (base + 1 <= yBot && rows[size_t(base + 1 - yTop)] >= 0.5 * most)
                ++base;
            int top = yTop;
            while (top < base && rows[size_t(top - yTop)] < 0.03 * most)
                ++top;
            // (the font's word: how tall above its baseline)
            double rmost = 0;
            for (int y = 0; y < Hc; ++y) {
                double s = 0;
                for (int x = 0; x < Wr; ++x)
                    s += drawn[size_t(y) * size_t(Wr) + size_t(x)];
                rmost = std::max(rmost, s);
            }
            int rtop = 0;
            for (; rtop < br; ++rtop) {
                double s = 0;
                for (int x = 0; x < Wr; ++x)
                    s += drawn[size_t(rtop) * size_t(Wr) + size_t(x)];
                if (s >= 0.03 * rmost)
                    break;
            }
            if (base - top > 4 && br - rtop > 4) {
                const double scale = std::clamp(double(base - top) / (br - rtop), 0.5, 2.0);
                if (measured) {  // (just its size asked for: the line's sizes made one)
                    *measured = scale;
                    return;
                }
                baseline = base + 0.5;
                if (resize && std::abs(scale - 1) > 0.03) {
                    em *= scale;
                    setWord(em);
                    if (shaped.empty() || width <= 0)
                        return;
                }
            }
        }
    }
    if (measured)
        return;
    // ... and the scan's (its darkness - its paper 0, its ink 1 - over the same band)
    const int x0 = box.left() - int(0.1 * em), Ws = box.width() + 2 * int(0.1 * em), y0 = int(std::lround(baseline)) - br;
    std::vector<float> scan(size_t(Ws) * size_t(Hc), 0);
    for (int y = 0; y < Hc; ++y)
        for (int x = 0; x < Ws; ++x)
            scan[size_t(y) * size_t(Ws) + size_t(x)] = darkAt(x0 + x, y0 + y);
    // each column: how much ink, its top, its bottom, its middle (empty: -1)
    struct Col {
        double ink = 0, top = -1, bottom = -1, middle = -1;
        bool join = false;  // (a joining stroke's: thin ink along the baseline - where letters meet, in any Arabic font)
    };
    auto columns = [&](const std::vector<float> &px, int w) {
        std::vector<Col> cols(static_cast<size_t>(w));
        for (int x = 0; x < w; ++x) {
            double sum = 0, wy = 0;
            int top = -1, bottom = -1;
            for (int y = 0; y < Hc; ++y) {
                const double v = px[size_t(y) * size_t(w) + size_t(x)];
                sum += v, wy += v * y;
                if (v > 0.5) {
                    if (top < 0)
                        top = y;
                    bottom = y;
                }
            }
            Col &c = cols[size_t(x)];
            c.ink = sum / em;
            if (top >= 0) {
                c.top = double(top) / Hc, c.bottom = double(bottom) / Hc, c.middle = wy / sum / Hc;
                c.join = (bottom - top) <= 0.2 * em && std::abs((top + bottom) / 2.0 - br) <= 0.25 * em;
            }
        }
        return cols;
    };
    std::vector<Col> rc = columns(drawn, Wr), sc = columns(scan, Ws);
    auto extent = [](const std::vector<Col> &c, int *a, int *b) {
        *a = -1, *b = -1;
        for (int x = 0; x < int(c.size()); ++x)
            if (c[size_t(x)].top >= 0) {
                if (*a < 0)
                    *a = x;
                *b = x;
            }
    };
    int ra, rb, sa, sb;
    extent(rc, &ra, &rb);
    extent(sc, &sa, &sb);
    if (ra < 0 || sa < 0 || rb - ra < 3 || sb - sa < 3)
        return;
    // the word's pieces (an Arabic word breaks after a letter that doesn't join - its pieces apart: the dots, marks left
    // out) - as many on the scan as in the font's: each laid over its own, a letter's ink only its own piece's
    auto piecesOf = [&](const std::vector<float> &px, int w, std::vector<int> *label) {
        std::vector<QRect> bodies;
        std::vector<std::vector<int>> members;
        std::vector<char> seen(px.size(), 0);
        label->assign(px.size(), -1);
        std::vector<int> todo, piece;
        for (int start = 0; start < w * Hc; ++start) {
            if (px[size_t(start)] <= 0.5f || seen[size_t(start)])
                continue;
            piece.clear();
            todo.assign(1, start);
            seen[size_t(start)] = 1;
            int bx0 = w, by0 = Hc, bx1 = -1, by1 = -1;
            while (!todo.empty()) {
                const int i = todo.back();
                todo.pop_back();
                piece.push_back(i);
                const int x = i % w, y = i / w;
                bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < w && ny < Hc && px[size_t(ny) * size_t(w) + size_t(nx)] > 0.5f &&
                            !seen[size_t(ny) * size_t(w) + size_t(nx)])
                            seen[size_t(ny) * size_t(w) + size_t(nx)] = 1, todo.push_back(ny * w + nx);
                    }
            }
            if (by0 < br && by1 > br - 0.25 * em && double(piece.size()) >= 0.01 * em * em) {
                bodies.push_back(QRect(QPoint(bx0, by0), QPoint(bx1, by1)));
                members.push_back(piece);
            }
        }
        std::vector<size_t> order(bodies.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bodies[a].center().x() < bodies[b].center().x(); });
        std::vector<QRect> sorted;
        for (size_t k = 0; k < order.size(); ++k) {
            sorted.push_back(bodies[order[k]]);
            for (int i : members[order[k]])
                (*label)[size_t(i)] = int(k);
        }
        // (a piece's soft edge its own too)
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<int> grown = *label;
            for (int y = 0; y < Hc; ++y)
                for (int x = 0; x < w; ++x)
                    if ((*label)[size_t(y) * size_t(w) + size_t(x)] < 0)
                        for (int q = 0; q < 4 && grown[size_t(y) * size_t(w) + size_t(x)] < 0; ++q) {
                            const int nx = x + (q == 0) - (q == 1), ny = y + (q == 2) - (q == 3);
                            if (nx >= 0 && ny >= 0 && nx < w && ny < Hc)
                                grown[size_t(y) * size_t(w) + size_t(x)] = (*label)[size_t(ny) * size_t(w) + size_t(nx)];
                        }
            *label = std::move(grown);
        }
        return sorted;
    };
    std::vector<int> rLabel, sLabel;
    const std::vector<QRect> rBodies = piecesOf(drawn, Wr, &rLabel), sBodies = piecesOf(scan, Ws, &sLabel);
    const bool anchored = rBodies.size() >= 2 && rBodies.size() == sBodies.size();
    // (each column of the font's word: where on the scan it may fall - in its piece's span, or between the pieces it's
    // between)
    std::vector<int> lo(size_t(Wr), -1000000), hi(size_t(Wr), 1000000);
    if (anchored)
        for (int x = 0; x < Wr; ++x) {
            int l = 1000000, h = -1000000, prev = -1, next = -1;
            for (size_t k = 0; k < rBodies.size(); ++k) {
                if (x >= rBodies[k].left() && x <= rBodies[k].right())
                    l = std::min(l, sBodies[k].left() - 3), h = std::max(h, sBodies[k].right() + 3);
                if (rBodies[k].right() < x && (prev < 0 || rBodies[k].right() > rBodies[size_t(prev)].right()))
                    prev = int(k);
                if (rBodies[k].left() > x && (next < 0 || rBodies[k].left() < rBodies[size_t(next)].left()))
                    next = int(k);
            }
            if (h < l) {
                l = prev >= 0 ? sBodies[size_t(prev)].right() - 3 : -1000000;
                h = next >= 0 ? sBodies[size_t(next)].left() + 3 : 1000000;
            }
            lo[size_t(x)] = l, hi[size_t(x)] = h;
        }
    // (each one's ink as a share of its mean: a heavier print matched as well as a lighter one)
    for (std::vector<Col> *cs : {&rc, &sc}) {
        double mean = 0;
        int n = 0;
        for (const Col &c : *cs)
            if (c.top >= 0)
                mean += c.ink, ++n;
        mean = n ? mean / n : 1;
        for (Col &c : *cs)
            c.ink /= std::max(1e-6, mean);
    }
    // the font's columns laid over the scan's, stretched where they need to be (DTW, near the even stretch)
    const int n = rb - ra + 1, m = sb - sa + 1;
    auto cost = [&](int i, int j) {
        const Col &a = rc[size_t(ra + i)], &b = sc[size_t(sa + j)];
        // (what any Arabic font shares: a gap where a letter doesn't join, a thin stroke where letters join - and,
        // less, how much ink, how high: the typeface's own)
        double c = std::abs(a.ink - b.ink) * 0.25;
        if ((a.top < 0) != (b.top < 0))
            c += 1.0;
        else if (a.top >= 0)
            c += (a.join != b.join ? 0.8 : 0.0) + 0.6 * (std::abs(a.top - b.top) + std::abs(a.bottom - b.bottom) + std::abs(a.middle - b.middle));
        return c;
    };
    const double inf = 1e18;
    std::vector<double> D(size_t(n) * size_t(m), inf);
    const double band = 0.35;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            if (anchored ? (sa + j < lo[size_t(ra + i)] || sa + j > hi[size_t(ra + i)]) : std::abs(double(j) / m - double(i) / n) > band)
                continue;
            const double c = cost(i, j);
            double best = (i == 0 && j == 0) ? 0 : inf;
            if (i > 0 && j > 0)
                best = std::min(best, D[size_t(i - 1) * size_t(m) + size_t(j - 1)]);
            if (i > 0)
                best = std::min(best, D[size_t(i - 1) * size_t(m) + size_t(j)] + 0.1);
            if (j > 0)
                best = std::min(best, D[size_t(i) * size_t(m) + size_t(j - 1)] + 0.1);
            if (best < inf)
                D[size_t(i) * size_t(m) + size_t(j)] = best + c;
        }
    if (D.back() >= inf)
        return;
    std::vector<double> sumJ(size_t(n), 0), countJ(size_t(n), 0);
    int steps = 0;
    for (int i = n - 1, j = m - 1;;) {
        sumJ[size_t(i)] += j, countJ[size_t(i)] += 1, ++steps;
        if (i == 0 && j == 0)
            break;
        double diag = inf, up = inf, leftC = inf;
        if (i > 0 && j > 0)
            diag = D[size_t(i - 1) * size_t(m) + size_t(j - 1)];
        if (i > 0)
            up = D[size_t(i - 1) * size_t(m) + size_t(j)];
        if (j > 0)
            leftC = D[size_t(i) * size_t(m) + size_t(j - 1)];
        if (diag <= up && diag <= leftC)
            --i, --j;
        else if (up <= leftC)
            --i;
        else
            --j;
    }
    const double wordCost = D.back() / steps;
    if (wordCost > 0.9)
        return;  // (laid over badly: another word than read, a smudge)
    // (a column of the word as set -> where it fell on the scan)
    auto onScan = [&](double x) {
        const double i = x - ra;
        if (i <= 0)
            return sa + i * double(m) / n;
        if (i >= n - 1)
            return sa + (m - 1) + (i - (n - 1)) * double(m) / n;
        const int k = int(i);
        const double f = i - k;
        const double a = sumJ[size_t(k)] / countJ[size_t(k)], b = sumJ[size_t(k + 1)] / countJ[size_t(k + 1)];
        return sa + a + (b - a) * f;
    };
    // each glyph: from its pen to the next one's, on the scan - its piece of the band, kEm to the em
    const int Hcan = int(std::lround((kBandUp + kBandDown) * kEm));
    for (size_t gi = 0; gi < shaped.size(); ++gi) {
        const double from = onScan(shaped[gi].x + pad), to = onScan((gi + 1 < shaped.size() ? shaped[gi + 1].x : width) + pad);
        const int Wc = int(std::lround((to - from) * kEm / em));
        if (Wc < 3 || Wc > 4 * kEm)
            continue;
        Glyph &g = d->frame(shaped[gi].gid);
        // (its piece: the one most of its columns' ink is in)
        int own = -1;
        if (anchored) {
            std::vector<int> votes(rBodies.size(), 0);
            const int c0 = int(shaped[gi].x) + pad, c1 = int((gi + 1 < shaped.size() ? shaped[gi + 1].x : width)) + pad;
            for (int y = 0; y < Hc; ++y)
                for (int x = std::max(0, c0); x < std::min(Wr, c1); ++x) {
                    const int k = rLabel[size_t(y) * size_t(Wr) + size_t(x)];
                    if (k >= 0 && drawn[size_t(y) * size_t(Wr) + size_t(x)] > 0.5f)
                        ++votes[size_t(k)];
                }
            const auto top = std::max_element(votes.begin(), votes.end());
            if (top != votes.end() && *top > 0)
                own = int(top - votes.begin());
        }
        Glyph::Free f;
        f.w = Wc;
        f.cost = wordCost;
        f.px.assign(size_t(Wc) * size_t(Hcan), 0);
        for (int v = 0; v < Hcan; ++v)
            for (int u = 0; u < Wc; ++u) {
                const double X = from + (u + 0.5) * em / kEm, Y = (v + 0.5) * em / kEm;
                const int xi = int(std::floor(X - 0.5)), yi = int(std::floor(Y - 0.5));
                const double fx = X - 0.5 - xi, fy = Y - 0.5 - yi;
                auto at = [&](int x, int y) -> double {
                    if (x < 0 || y < 0 || x >= Ws || y >= Hc)
                        return 0.0;
                    const int k = sLabel[size_t(y) * size_t(Ws) + size_t(x)];
                    return own >= 0 && k >= 0 && k != own ? 0.0 : scan[size_t(y) * size_t(Ws) + size_t(x)];
                };
                const double val = (at(xi, yi) * (1 - fx) + at(xi + 1, yi) * fx) * (1 - fy) + (at(xi, yi + 1) * (1 - fx) + at(xi + 1, yi + 1) * fx) * fy;
                f.px[size_t(v) * size_t(Wc) + size_t(u)] = uchar(std::lround(val * 255));
            }
        g.free.push_back(std::move(f));
        if (g.free.size() > kKeep) {
            auto worst = std::max_element(g.free.begin(), g.free.end(), [](const Glyph::Free &a, const Glyph::Free &b) { return a.cost < b.cost; });
            g.free.erase(worst);
        }
    }
}

QByteArray Builder::build(const QString &name, bool bold, int *tracedCount, QString *psName)
{
    *tracedCount = 0;
    if (!ok())
        return {};
    const int upem = d->upem;
    // each glyph seen: its copies averaged (those unlike the rest left out), the ink there traced
    std::map<unsigned, std::vector<Contour>> traced;
    double grown = 0, edges = 0;
    std::vector<double> softs;  // (each glyph traced: its edges' blur, pixels)
    size_t measured = 0;        // (glyphs whose copies told how heavy, how soft the scan's letters are)
    for (auto &[gid, g] : d->glyphs) {
        if (g.empty || g.samples.empty() || (g.samples.size() == 1 && g.samples[0].score < 0.78))
            continue;
        const size_t size = size_t(g.w) * size_t(g.h);
        std::vector<char> use(g.samples.size(), 1);
        std::vector<float> mean(size, 0);
        auto average = [&] {
            std::fill(mean.begin(), mean.end(), 0.0f);
            int count = 0;
            for (size_t s = 0; s < g.samples.size(); ++s)
                if (use[s]) {
                    ++count;
                    for (size_t i = 0; i < size; ++i)
                        mean[i] += g.samples[s].px[i];
                }
            for (float &m : mean)
                m /= std::max(1, count) * 255.0f;
        };
        average();
        if (g.samples.size() == 2) {  // (two unlike each other: just the surer one)
            double ab = 0, aa = 0, bb = 0;
            for (size_t i = 0; i < size; ++i) {
                const double a = g.samples[0].px[i], b = g.samples[1].px[i];
                ab += a * b, aa += a * a, bb += b * b;
            }
            if (aa <= 0 || bb <= 0 || ab / std::sqrt(aa * bb) < 0.8) {
                use[g.samples[0].score < g.samples[1].score ? 0 : 1] = 0;
                average();
            }
        }
        if (g.samples.size() >= 3) {  // (a copy unlike the others - a wrong letter, a stain: left out)
            for (size_t s = 0; s < g.samples.size(); ++s) {
                double pm = 0, pp = 0, mm = 0;
                for (size_t i = 0; i < size; ++i) {
                    const double p = g.samples[s].px[i] / 255.0;
                    pm += p * mean[i], pp += p * p, mm += double(mean[i]) * mean[i];
                }
                use[s] = pp > 0 && mm > 0 && pm / std::sqrt(pp * mm) >= 0.8;
            }
            if (std::count(use.begin(), use.end(), 1) == 0)
                continue;
            average();
        }
        // (smoothed a little: the scan's grain and the pixels' steps evened out - an even edge to trace)
        {
            const double sigma = 0.010 * kEm;
            const int rad = int(std::ceil(3 * sigma));
            std::vector<float> kernel(size_t(2 * rad + 1));
            float total = 0;
            for (int k = -rad; k <= rad; ++k)
                total += kernel[size_t(k + rad)] = float(std::exp(-k * k / (2 * sigma * sigma)));
            for (float &k : kernel)
                k /= total;
            std::vector<float> tmp(size, 0);
            for (int v = 0; v < g.h; ++v)
                for (int u = 0; u < g.w; ++u) {
                    float a = 0;
                    for (int k = -rad; k <= rad; ++k)
                        a += kernel[size_t(k + rad)] * mean[size_t(v) * size_t(g.w) + size_t(std::clamp(u + k, 0, g.w - 1))];
                    tmp[size_t(v) * size_t(g.w) + size_t(u)] = a;
                }
            for (int v = 0; v < g.h; ++v)
                for (int u = 0; u < g.w; ++u) {
                    float a = 0;
                    for (int k = -rad; k <= rad; ++k)
                        a += kernel[size_t(k + rad)] * tmp[size_t(std::clamp(v + k, 0, g.h - 1)) * size_t(g.w) + size_t(u)];
                    mean[size_t(v) * size_t(g.w) + size_t(u)] = a;
                }
        }
        // (its ink: darker than half - the edge where the scan's ink is - in pieces of the glyph itself, not a
        // neighbour's reaching in; a dot, an accent above or below it kept)
        std::vector<char> ink(size, 0);
        for (size_t i = 0; i < size; ++i)
            ink[i] = mean[i] >= 0.5f;
        std::vector<char> seen(size, 0);
        double area = 0;
        for (size_t start = 0; start < size; ++start) {
            if (!ink[start] || seen[start])
                continue;
            std::vector<size_t> piece;
            std::deque<size_t> todo{start};
            seen[start] = 1;
            int inside = 0;
            while (!todo.empty()) {
                const size_t i = todo.front();
                todo.pop_front();
                piece.push_back(i);
                inside += g.base[i] > 0.1f ? 1 : 0;
                const int u = int(i % size_t(g.w)), v = int(i / size_t(g.w));
                const int nu[4] = {u - 1, u + 1, u, u}, nv[4] = {v, v, v - 1, v + 1};
                for (int q = 0; q < 4; ++q)
                    if (nu[q] >= 0 && nv[q] >= 0 && nu[q] < g.w && nv[q] < g.h) {
                        const size_t j = size_t(nv[q]) * size_t(g.w) + size_t(nu[q]);
                        if (ink[j] && !seen[j])
                            seen[j] = 1, todo.push_back(j);
                    }
            }
            bool mark = false;  // (a small piece over a small piece of the font's own glyph: an "i"'s dot, an accent -
                                // not a joined neighbour's dot reaching over it)
            if (piece.size() <= size_t(0.03 * kEm * kEm)) {
                size_t sumU = 0;
                for (size_t i : piece)
                    sumU += i % size_t(g.w);
                const int mid = int(sumU / piece.size());
                const int reach = int(std::lround(0.15 * kEm));
                for (const QRect &m : marksOf(g))
                    mark = mark || (mid >= m.left() - reach && mid <= m.right() + reach);
            }
            if (inside < 0.25 * double(piece.size()) && !mark)
                for (size_t i : piece)
                    ink[i] = 0;
            else
                area += double(piece.size());
        }
        // (a piece of the glyph - an "i"'s dot, an accent - the scan printed faint: taken where its ink is fainter,
        // else the font's own piece kept - not a dotless "i")
        {
            std::vector<char> done(size, 0);
            for (size_t start = 0; start < size; ++start) {
                if (g.base[start] <= 0.5f || done[start])
                    continue;
                std::vector<size_t> part;
                std::deque<size_t> todo{start};
                done[start] = 1;
                int x0 = g.w, y0 = g.h, x1 = -1, y1 = -1;
                while (!todo.empty()) {
                    const size_t i = todo.front();
                    todo.pop_front();
                    part.push_back(i);
                    const int u = int(i % size_t(g.w)), v = int(i / size_t(g.w));
                    x0 = std::min(x0, u), x1 = std::max(x1, u), y0 = std::min(y0, v), y1 = std::max(y1, v);
                    const int nu[4] = {u - 1, u + 1, u, u}, nv[4] = {v, v, v - 1, v + 1};
                    for (int q = 0; q < 4; ++q)
                        if (nu[q] >= 0 && nv[q] >= 0 && nu[q] < g.w && nv[q] < g.h) {
                            const size_t j = size_t(nv[q]) * size_t(g.w) + size_t(nu[q]);
                            if (g.base[j] > 0.5f && !done[j])
                                done[j] = 1, todo.push_back(j);
                        }
                }
                const int pad = int(std::lround(0.05 * kEm));
                x0 = std::max(0, x0 - pad), y0 = std::max(0, y0 - pad), x1 = std::min(g.w - 1, x1 + pad), y1 = std::min(g.h - 1, y1 + pad);
                auto inkIn = [&] {
                    int n = 0;
                    for (int v = y0; v <= y1; ++v)
                        for (int u = x0; u <= x1; ++u)
                            n += ink[size_t(v) * size_t(g.w) + size_t(u)];
                    return n;
                };
                if (inkIn() >= 0.15 * double(part.size()))
                    continue;
                for (int v = y0; v <= y1; ++v)
                    for (int u = x0; u <= x1; ++u)
                        if (mean[size_t(v) * size_t(g.w) + size_t(u)] >= 0.25f)
                            ink[size_t(v) * size_t(g.w) + size_t(u)] = 1;
                if (inkIn() < 0.3 * double(part.size()))
                    for (size_t i : part)
                        ink[i] = 1;
            }
            area = double(std::count(ink.begin(), ink.end(), 1));
        }
        // (pieces the font keeps apart - an "i"'s dot and its stem - run together by the scan's blur: parted again,
        // the ink kept to the font's glyph grown as much as the scan's strokes are heavier)
        {
            auto pieces = [&](auto inside) {
                std::vector<char> seenP(size, 0);
                int count = 0;
                for (size_t start = 0; start < size; ++start) {
                    if (!inside(start) || seenP[start])
                        continue;
                    size_t n = 0;
                    std::deque<size_t> todo{start};
                    seenP[start] = 1;
                    while (!todo.empty()) {
                        const size_t i = todo.front();
                        todo.pop_front();
                        ++n;
                        const int u = int(i % size_t(g.w)), v = int(i / size_t(g.w));
                        const int nu[4] = {u - 1, u + 1, u, u}, nv[4] = {v, v, v - 1, v + 1};
                        for (int q = 0; q < 4; ++q)
                            if (nu[q] >= 0 && nv[q] >= 0 && nu[q] < g.w && nv[q] < g.h) {
                                const size_t j = size_t(nv[q]) * size_t(g.w) + size_t(nu[q]);
                                if (inside(j) && !seenP[j])
                                    seenP[j] = 1, todo.push_back(j);
                            }
                    }
                    count += n >= size_t(0.002 * kEm * kEm) ? 1 : 0;
                }
                return count;
            };
            const int own = pieces([&](size_t i) { return g.base[i] > 0.5f; });
            const int now = pieces([&](size_t i) { return ink[i] != 0; });
            if (now < own && g.edge > 0) {
                const int grow = std::max(1, int(std::lround(std::max(0.0, area - g.area) / g.edge))) + int(std::lround(0.01 * kEm));
                std::vector<char> rows(size, 0), allowed(size, 0);
                for (int v = 0; v < g.h; ++v)
                    for (int u = 0; u < g.w; ++u) {
                        bool any = false;
                        for (int k = std::max(0, u - grow); k <= std::min(g.w - 1, u + grow) && !any; ++k)
                            any = g.base[size_t(v) * size_t(g.w) + size_t(k)] > 0.5f;
                        rows[size_t(v) * size_t(g.w) + size_t(u)] = any;
                    }
                for (int v = 0; v < g.h; ++v)
                    for (int u = 0; u < g.w; ++u) {
                        bool any = false;
                        for (int k = std::max(0, v - grow); k <= std::min(g.h - 1, v + grow) && !any; ++k)
                            any = rows[size_t(k) * size_t(g.w) + size_t(u)];
                        allowed[size_t(v) * size_t(g.w) + size_t(u)] = any;
                    }
                std::vector<char> parted(size, 0);
                for (size_t i = 0; i < size; ++i)
                    parted[i] = ink[i] && allowed[i];
                // (just a bridge the blur made taken away - not a shape of its own the font draws elsewhere: a display
                // font's dots)
                if (pieces([&](size_t i) { return parted[i] != 0; }) >= own && double(std::count(parted.begin(), parted.end(), 1)) >= 0.9 * area) {
                    ink = parted;
                    area = double(std::count(ink.begin(), ink.end(), 1));
                }
            }
        }
        if (area < 0.5 * g.area || area > 2.2 * g.area)
            continue;  // (nothing like the glyph: misread)
        {  // (its shape still the font's glyph's - heavier or lighter, not another letter: else the font's own kept)
            double ib = 0, ii = 0, bb = 0;
            for (size_t i = 0; i < size; ++i) {
                const double a = ink[i] ? 1.0 : 0.0, b = g.base[i];
                ib += a * b, ii += a * a, bb += b * b;
            }
            if (ii <= 0 || bb <= 0 || ib / std::sqrt(ii * bb) < 0.72)
                continue;
        }
        // (copied from the scan - or, the font matched, its own shape, as heavy as the scan's: an Arabic letter's, its joins
        // to its neighbours the font's; a Latin letter, standing alone, copied as the scan has it)
        const bool copy = d->copy || !g.rtl;
        std::vector<Contour> contours = copy ? trace(ink, g, upem) : std::vector<Contour>();
        if (copy && contours.empty())
            continue;
        ++measured;
        {  // (how much heavier: its ink in all - the scan's darkness over the glyph and its blur round it, against the
           // font's glyph's - a blur spreads ink, doesn't add to it: drawn sharp in the scan's ink, as dark as the scan's)
            const int rad = int(std::lround(0.03 * kEm));
            std::vector<char> rows(size, 0);
            for (int v = 0; v < g.h; ++v)
                for (int u = 0; u < g.w; ++u) {
                    bool any = false;
                    for (int k = std::max(0, u - rad); k <= std::min(g.w - 1, u + rad) && !any; ++k)
                        any = ink[size_t(v) * size_t(g.w) + size_t(k)] != 0;
                    rows[size_t(v) * size_t(g.w) + size_t(u)] = any;
                }
            double dark = 0, own = 0;
            for (int v = 0; v < g.h; ++v)
                for (int u = 0; u < g.w; ++u) {
                    bool any = false;
                    for (int k = std::max(0, v - rad); k <= std::min(g.h - 1, v + rad) && !any; ++k)
                        any = rows[size_t(k) * size_t(g.w) + size_t(u)] != 0;
                    const size_t i = size_t(v) * size_t(g.w) + size_t(u);
                    dark += any ? mean[i] : 0;
                    own += g.base[i];
                }
            area = g.area + (dark - own);
        }
        grown += area - g.area;
        edges += g.edge;
        {  // (how soft its edges are: how wide the ink takes to go from paper to ink - a step blurred by sigma: 1.68 sigma)
            double soft = 0;
            for (size_t i = 0; i < size; ++i)
                soft += mean[i] > 0.2f && mean[i] < 0.8f ? 1 : 0;
            if (g.edge > 0)
                softs.push_back(soft / g.edge / 1.68);
        }
        if (copy)
            traced[gid] = std::move(contours);
    }
    // the glyphs cut from words laid over the scan elastically (a scan not in the font's shapes): their copies made as
    // wide as they mostly were, averaged, traced - and that width theirs
    std::map<unsigned, double> freeWidth;  // (font units)
    double likeSum = 0;
    int likeCount = 0;
    for (auto &[gid, g] : d->glyphs) {
        if (g.free.empty() || traced.count(gid))
            continue;
        int W = 0;
        std::vector<float> mean;
        if (!freeMean(g, &W, &mean))
            continue;
        const int Hcan = int(std::lround((kBandUp + kBandDown) * kEm));
        const size_t size = size_t(W) * size_t(Hcan);
        // (seen once or twice: its scan's grain and blur smoothed out - an even edge - and a little thinner: the blur
        // spread its ink)
        boxBlur(mean, W, Hcan, 0.018 * kEm);
        std::vector<char> ink(size, 0);
        int inked = 0;
        for (size_t i = 0; i < size; ++i)
            inked += (ink[i] = mean[i] >= 0.56f) ? 1 : 0;
        if (inked < 0.002 * kEm * kEm)
            continue;
        {  // (a small piece apart - a dot - kept only where the font's glyph has one, above or below: not a piece of the
           // line above or below cut with it)
            bool marksUp = false, marksDown = false;
            for (const QRect &m : marksOf(g)) {
                const double y = g.bottom + (g.h - (m.top() + m.bottom()) / 2.0) * upem / kEm;  // (font units, up from the baseline)
                if (y > 0.3 * upem)
                    marksUp = true;
                else if (y < 0.1 * upem)
                    marksDown = true;
            }
            std::vector<char> seen(size, 0);
            std::vector<std::vector<size_t>> parts;
            for (size_t start = 0; start < size; ++start) {
                if (!ink[start] || seen[start])
                    continue;
                parts.emplace_back();
                std::deque<size_t> todo{start};
                seen[start] = 1;
                while (!todo.empty()) {
                    const size_t i = todo.front();
                    todo.pop_front();
                    parts.back().push_back(i);
                    const int u = int(i % size_t(W)), v = int(i / size_t(W));
                    const int nu[4] = {u - 1, u + 1, u, u}, nv[4] = {v, v, v - 1, v + 1};
                    for (int q = 0; q < 4; ++q)
                        if (nu[q] >= 0 && nv[q] >= 0 && nu[q] < W && nv[q] < Hcan) {
                            const size_t j = size_t(nv[q]) * size_t(W) + size_t(nu[q]);
                            if (ink[j] && !seen[j])
                                seen[j] = 1, todo.push_back(j);
                        }
                }
            }
            size_t biggest = 0;
            for (const auto &p : parts)
                biggest = std::max(biggest, p.size());
            for (const auto &p : parts) {
                if (p.size() >= 0.3 * biggest)
                    continue;
                double v = 0;
                int vTop = Hcan, vBottom = 0;
                for (size_t i : p) {
                    const int row = int(i / size_t(W));
                    v += row, vTop = std::min(vTop, row), vBottom = std::max(vBottom, row);
                }
                v /= double(p.size());
                const double y = (Hcan - v) * upem / kEm - kBandDown * upem;  // (font units, up from the baseline)
                // (an upright - a lam's, cut off from the rest of it - tall, or down to the baseline: not a mark)
                const double tall = double(vBottom - vTop) * upem / kEm;
                const double low = (Hcan - vBottom) * double(upem) / kEm - kBandDown * upem;
                const bool upright = tall > 0.3 * upem && low < 0.25 * upem;
                const bool keep = upright || (y > 0.3 * upem ? marksUp : y < 0.1 * upem ? marksDown : true);
                if (!keep)
                    for (size_t i : p)
                        ink[i] = 0;
            }
        }
        Glyph frame;
        frame.left = 0;
        frame.bottom = -int(std::lround(kBandDown * upem));
        frame.w = W;
        frame.h = Hcan;
        std::vector<Contour> contours = trace(ink, frame, upem, kEm, 1.25, 0.5);
        if (contours.empty())
            continue;
        // (how like the font's own glyph - too unlike: another letter's ink cut with it, laid over wrong - the font's
        // own made as heavy as the scan's, below, not a wrong shape)
        const double like = likeOwn(ink, W, Hcan, g);
        if (like >= 0)
            likeSum += like, ++likeCount;
        if (like >= 0 && like < 0.45)
            continue;
        traced[gid] = std::move(contours);
        freeWidth[gid] = double(W) * upem / kEm;
    }
    if (likeCount > 0)
        d->likeness = likeSum / likeCount;
    if (std::max(traced.size(), measured) < 6)
        return {};  // (too few letters seen to make a font of them)
    // the glyphs not seen: the font's own, as much heavier / lighter as the scan's ink is than the font's
    // (its edge moved out by what the glyphs seen grew by, on the whole)
    const double each = edges > 0 ? grown / edges * upem / kEm : 0;
    // ... and as soft: drawn with the scan's blur and traced as the glyphs seen were - one look, every glyph
    double blur = 0.01 * kEm;
    if (!softs.empty()) {
        std::nth_element(softs.begin(), softs.begin() + long(softs.size() / 2), softs.end());
        blur = std::clamp(softs[softs.size() / 2], 0.004 * kEm, 0.04 * kEm);
    }
    constexpr int kSynth = 128;  // (pixels to the em the glyphs not seen are drawn at: as smooth, quicker)
    const double synthBlur = blur * kSynth / kEm;
    QHash<QByteArray, QByteArray> t = d->tables();
    const QByteArray head0 = t.value("head"), maxp0 = t.value("maxp"), loca0 = t.value("loca"), glyf0 = t.value("glyf"),
                     hhea0 = t.value("hhea");
    if (head0.size() < 54 || maxp0.size() < 6 || hhea0.size() < 36 || glyf0.isEmpty())
        return {};
    const int numGlyphs = be16(maxp0.constData() + 4);
    const bool longLoca = be16(head0.constData() + 50) == 1;
    auto oldGlyph = [&](int gid) -> QByteArray {
        const int stride = longLoca ? 4 : 2;
        if ((gid + 2) * stride > loca0.size())
            return {};
        const quint32 a = longLoca ? be32(loca0.constData() + gid * 4) : 2u * be16(loca0.constData() + gid * 2);
        const quint32 b = longLoca ? be32(loca0.constData() + gid * 4 + 4) : 2u * be16(loca0.constData() + gid * 2 + 2);
        if (b <= a || qint64(b) > glyf0.size())
            return {};
        QByteArray g = glyf0.mid(int(a), int(b - a));
        while (g.size() % 4)
            g.append('\0');
        return g;
    };
    // (the glyphs the letters of Latin, Arabic, digits and signs can be shaped into - redrawn as the scan's; the rest of
    // the font - Hebrew, Cyrillic... - left as it is)
    std::vector<char> reachable(size_t(numGlyphs), 0);
    {
        hb_blob_t *blob = hb_blob_create(d->file.constData(), unsigned(d->file.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t *face = hb_face_create(blob, unsigned(d->base.index));
        hb_font_t *font = hb_font_create(face);
        hb_set_t *set = hb_set_create();
        auto close = [&](const std::vector<uint32_t> &chars, hb_direction_t dir, const char *script) {
            hb_buffer_t *buf = hb_buffer_create();
            hb_buffer_add_utf32(buf, chars.data(), int(chars.size()), 0, int(chars.size()));
            hb_buffer_set_direction(buf, dir);
            hb_buffer_set_script(buf, hb_script_from_string(script, -1));
            hb_ot_shape_glyphs_closure(font, buf, nullptr, 0, set);
            hb_buffer_destroy(buf);
        };
        std::vector<uint32_t> latin, arabic;
        for (uint32_t c = 0x20; c < 0x250; ++c)
            latin.push_back(c);
        for (uint32_t c = 0x2010; c < 0x2050; ++c)
            latin.push_back(c);
        for (uint32_t c = 0x600; c < 0x700; ++c)
            arabic.push_back(c);
        arabic.push_back(0x200C), arabic.push_back(0x200D);
        close(latin, HB_DIRECTION_LTR, "Latn");
        close(arabic, HB_DIRECTION_RTL, "Arab");
        for (hb_codepoint_t gid = HB_SET_VALUE_INVALID; hb_set_next(set, &gid);)
            if (gid < hb_codepoint_t(numGlyphs))
                reachable[gid] = 1;
        hb_set_destroy(set);
        hb_font_destroy(font);
        hb_face_destroy(face);
        hb_blob_destroy(blob);
    }
    QByteArray glyf, loca;
    QByteArray hmtx = t.value("hmtx");
    const int metrics = be16(hhea0.constData() + 34);
    int maxPoints = maxp0.size() >= 8 ? be16(maxp0.constData() + 6) : 0, maxContours = maxp0.size() >= 10 ? be16(maxp0.constData() + 8) : 0;
    int fxMin = 32767, fyMin = 32767, fxMax = -32768, fyMax = -32768;
    for (int gid = 0; gid < numGlyphs; ++gid) {
        put32(loca, quint32(glyf.size()));
        std::vector<Contour> contours;
        bool rewrite = false;
        auto it = traced.find(unsigned(gid));
        if (it != traced.end()) {
            contours = it->second;
            rewrite = true;
        } else if (reachable[size_t(gid)] && !FT_Load_Glyph(d->face, FT_UInt(gid), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) &&
                   d->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && d->face->glyph->outline.n_points > 0) {
            FT_Outline &o = d->face->glyph->outline;
            if (std::abs(each) >= 0.5)
                FT_Outline_EmboldenXY(&o, FT_Pos(std::lround(2 * each)), FT_Pos(std::lround(2 * each)));
            FT_BBox box;
            FT_Outline_Get_CBox(&o, &box);
            const int m = int(std::ceil((3 * synthBlur + 2) * upem / kSynth));
            Glyph frame;
            frame.left = int(box.xMin) - m;
            frame.bottom = int(box.yMin) - m;
            frame.w = int(std::ceil(double(box.xMax - box.xMin + 2 * m) * kSynth / upem));
            frame.h = int(std::ceil(double(box.yMax - box.yMin + 2 * m) * kSynth / upem));
            if (frame.w >= 3 && frame.h >= 3 && frame.w <= 8 * kSynth && frame.h <= 8 * kSynth) {
                std::vector<float> cov = coverage(d->lib, o, frame.left, frame.bottom, frame.w, frame.h, upem, kSynth);
                // (blurred as the scan is - its corners rounded - over its own shape a little blurred: its dots, its thin
                // parts not blurred away)
                std::vector<float> core = cov;
                boxBlur(core, frame.w, frame.h, synthBlur / 3);
                boxBlur(cov, frame.w, frame.h, synthBlur);
                std::vector<char> ink(cov.size(), 0);
                for (size_t i = 0; i < cov.size(); ++i)
                    ink[i] = cov[i] >= 0.5f || core[i] >= 0.5f;
                contours = trace(ink, frame, upem, kSynth);
            }
            if (contours.empty()) {  // (too small, too big to draw: its outline as it is)
                int first = 0;
                for (int c = 0; c < o.n_contours; ++c) {
                    Contour ct;
                    for (int p = first; p <= o.contours[c]; ++p)
                        ct.push_back({double(o.points[p].x), double(o.points[p].y), (o.tags[p] & 1) != 0});
                    first = o.contours[c] + 1;
                    if (!ct.empty())
                        contours.push_back(std::move(ct));
                }
            }
            rewrite = true;
        }
        if (!rewrite) {
            const QByteArray g = oldGlyph(gid);
            if (g.size() >= 10) {
                fxMin = std::min<int>(fxMin, qint16(be16(g.constData() + 2))), fyMin = std::min<int>(fyMin, qint16(be16(g.constData() + 4)));
                fxMax = std::max<int>(fxMax, qint16(be16(g.constData() + 6))), fyMax = std::max<int>(fyMax, qint16(be16(g.constData() + 8)));
            }
            glyf += g;
            continue;
        }
        int xMin = 0, yMin = 0, xMax = 0, yMax = 0, points = 0;
        const QByteArray g = simpleGlyph(contours, &xMin, &yMin, &xMax, &yMax, &points);
        glyf += g;
        if (g.isEmpty())
            continue;
        maxPoints = std::max(maxPoints, points);
        maxContours = std::max(maxContours, int(contours.size()));
        fxMin = std::min(fxMin, xMin), fyMin = std::min(fyMin, yMin), fxMax = std::max(fxMax, xMax), fyMax = std::max(fyMax, yMax);
        // (its left side bearing: where its outline starts now)
        const int at = gid < metrics ? gid * 4 + 2 : metrics * 4 + (gid - metrics) * 2;
        set16(hmtx, at, xMin);
        // (its width as the scan printed it - cut from words laid over it: as wide as its piece; else the middle of
        // what was seen, if seen twice or more)
        if (freeWidth.count(unsigned(gid)) && gid < metrics && hmtx.size() >= gid * 4 + 2)
            set16(hmtx, gid * 4, int(std::lround(freeWidth[unsigned(gid)])));
        auto gl = d->glyphs.find(unsigned(gid));
        if (it != traced.end() && !freeWidth.count(unsigned(gid)) && gl != d->glyphs.end() && gl->second.advances.size() >= 2 && gid < metrics &&
            hmtx.size() >= gid * 4 + 2) {
            std::vector<double> a = gl->second.advances;
            std::nth_element(a.begin(), a.begin() + long(a.size() / 2), a.end());
            const double was = be16(hmtx.constData() + gid * 4), now = a[a.size() / 2];
            if (now > 0.7 * was && now < 1.4 * was)
                set16(hmtx, gid * 4, int(std::lround(now)));
        }
    }
    put32(loca, quint32(glyf.size()));
    *tracedCount = int(std::max(traced.size(), measured));
    // its tables: glyphs, where they are, its sizes, its names - the rest as the font has them (but what
    // was worked out for the old glyphs: hinted widths, a signature)
    QByteArray head = head0, maxp = maxp0;
    set32(head, 8, 0);
    set16(head, 50, 1);
    set16(head, 36, fxMin), set16(head, 38, fyMin), set16(head, 40, fxMax), set16(head, 42, fyMax);
    set16(maxp, 6, maxPoints);
    set16(maxp, 8, maxContours);
    const QByteArray hash = QCryptographicHash::hash(glyf, QCryptographicHash::Sha1).toHex().left(6);
    QString clean;
    for (const QChar c : name)
        if (c.isLetterOrNumber() && c.unicode() < 128)
            clean += c;
    clean.truncate(18);  // (its name 31 letters at most: MuPDF reads no more of it back - a longer one isn't found again)
    const QString family = QStringLiteral("MPScan %1 %2").arg(QString::fromLatin1(hash), clean);
    const QString style = bold ? QStringLiteral("Bold") : QStringLiteral("Regular");
    const QString ps = QStringLiteral("MPScan%1-%2").arg(QString::fromLatin1(hash), clean);
    *psName = ps;
    QByteArray nameTable;
    {
        const std::vector<std::pair<int, QString>> names = {{1, family}, {2, style}, {3, ps}, {4, family + QLatin1Char(' ') + style},
                                                            {5, QStringLiteral("Version 1.0")}, {6, ps}};
        QByteArray strings;
        put16(nameTable, 0);
        put16(nameTable, int(names.size()));
        put16(nameTable, 6 + 12 * int(names.size()));
        for (const auto &[id, text] : names) {
            QByteArray s;
            for (const QChar c : text)
                put16(s, c.unicode());
            put16(nameTable, 3), put16(nameTable, 1), put16(nameTable, 0x409), put16(nameTable, id);
            put16(nameTable, int(s.size())), put16(nameTable, int(strings.size()));
            strings += s;
        }
        nameTable += strings;
    }
    if (bold) {  // (as heavy as the scan's: bold - not made bold again where it's written)
        QByteArray os2 = t.value("OS/2");
        if (os2.size() >= 64) {
            set16(os2, 4, 700);
            set16(os2, 62, (be16(os2.constData() + 62) | 0x20) & ~0x40);
            t.insert("OS/2", os2);
        }
        set16(head, 44, be16(head.constData() + 44) | 1);
    }
    t.insert("head", head);
    t.insert("maxp", maxp);
    t.insert("glyf", glyf);
    t.insert("loca", loca);
    t.insert("hmtx", hmtx);
    t.insert("name", nameTable);
    for (const char *drop : {"DSIG", "hdmx", "LTSH", "VDMX"})
        t.remove(drop);
    // the file: its tables A to Z, each on 4 bytes; its checksum made right
    QList<QByteArray> tags = t.keys();
    std::sort(tags.begin(), tags.end());
    const int n = int(tags.size());
    int power = 1, log2 = 0;
    while (power * 2 <= n)
        power *= 2, ++log2;
    QByteArray out;
    put32(out, 0x00010000);
    put16(out, n);
    put16(out, power * 16);
    put16(out, log2);
    put16(out, n * 16 - power * 16);
    quint32 at = quint32(12 + 16 * n);
    QByteArray body;
    int headAt = -1;
    for (const QByteArray &tag : tags) {
        QByteArray data = t.value(tag);
        out += tag;
        put32(out, checksum(data));
        put32(out, at);
        put32(out, quint32(data.size()));
        if (tag == "head")
            headAt = int(at);
        while (data.size() % 4)
            data.append('\0');
        body += data;
        at += quint32(data.size());
    }
    out += body;
    if (headAt >= 0)
        set32(out, headAt + 8, 0xB1B0AFBAu - checksum(out));
    return out;
}

}  // namespace scanfont
