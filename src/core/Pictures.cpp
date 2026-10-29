// Document's pictures (step 6): found in the page's own drawing instructions, and moved,
// resized, turned, layered, replaced or deleted right there - so what's drawn over a picture
// (text on it) stays over it (the Python app's moved_in_place).
// (No C++ objects are made inside fz_try blocks or the functions run in them.)

#include <cstring>
#include "core/Content.h"
#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QRegularExpression>
#include <QBuffer>
#include <QFile>
#include <QImage>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <tuple>

using namespace content;

namespace {

fz_matrix concat(fz_matrix a, fz_matrix b)
{
    return fz_concat(a, b);
}

QByteArray matrixOp(const fz_matrix &m)
{
    char buf[256];
    fz_snprintf(buf, sizeof buf, "%g %g %g %g %g %g cm ", m.a, m.b, m.c, m.d, m.e, m.f);  // (not the locale's commas)
    return QByteArray(buf);
}

// a picture drawn clipped to its shape - "q <boxes> W n /MPclip MP /Im Do Q": its place widened
// to all of it (so it's moved, deleted, replaced with its clip). (A marked-content point after
// the clip, not a BMC ... EMC round it: MuPDF's tidying of a page's drawing - redaction, when
// text is taken out - put the clip's path out of order round a BMC. The spaces may go.)
QByteArray squeezed(const QByteArray &text)
{
    QByteArray out;
    for (char c : text)
        if (!QChar::isSpace(uchar(c)))
            out += c;
    return out;
}

void widenToClip(const QByteArray &data, Picture &p)
{
    const qint64 k = data.lastIndexOf("/MPclip", p.start);
    if (k < 0 || squeezed(data.mid(k, p.start - k)) != "/MPclipMP")
        return;
    // back from it: "W n", the boxes (numbers and re), and the q they start with
    qint64 at = k;
    bool sawClip = false;
    while (at > 0) {
        qint64 end = at;
        while (end > 0 && QChar::isSpace(uchar(data[end - 1])))
            --end;
        qint64 begin = end;
        while (begin > 0 && !QChar::isSpace(uchar(data[begin - 1])) && data[begin - 1] != '/')
            --begin;
        const QByteArray token = data.mid(begin, end - begin);
        if (token.isEmpty())
            return;
        if (token == "q" && sawClip) {
            at = begin;
            break;
        }
        if (token == "Wn" || token == "W" || token == "n")
            sawClip = true;
        else if (token != "re" && token != "cm" && token != "m" && token != "l" && token != "h" && token.toDouble() == 0 && token != "0" && !token.startsWith("0") && !token.startsWith("-0") &&
                 !token.startsWith(".") && !token.startsWith("-."))
            return;  // (something else: not our clip)
        at = begin;
    }
    const qint64 e = data.indexOf('Q', p.end);
    if (e < 0 || !squeezed(data.mid(p.end, e - p.end)).isEmpty())
        return;
    p.start = at;
    p.end = e + 1;
    p.clipped = true;
}

// (the Python app's moved_in_place) where a picture's move goes so its own q ... Q - its place,
// the crop PowerPoint sets round it - moves with it: just inside the outermost q round its "Do"
// that holds nothing else to see. at -1: none (it isn't in a q of its own, or a frame round it -
// a table cell's - would hide it at its new place). before: the page's streams before this one.
struct InPlace {
    qint64 at = -1;
    fz_matrix x = fz_identity;
};

bool quiet(const QByteArray &op)
{
    static const char *ops[] = {"m", "l", "c", "v", "y", "h", "re", "q", "Q", "cm", "W", "W*", "n", "gs", "w", "J",
                                "j", "M", "d", "ri", "i", "rg", "RG", "g", "G", "k", "K", "cs", "CS", "sc", "scn",
                                "SC", "SCN", "BDC", "BMC", "EMC", "MP", "DP"};
    for (const char *o : ops)
        if (op == o)
            return true;
    return false;
}

InPlace movedInPlace(const std::vector<QByteArray> &before, const QByteArray &data, const Picture &pic, fz_matrix move,
                     fz_rect newBox)
{
    std::vector<Op> all;
    for (const QByteArray &b : before)
        for (Op &op : readOps(b))
            all.push_back(std::move(op));
    const size_t first = all.size();
    for (Op &op : readOps(data))
        all.push_back(std::move(op));
    size_t k = all.size();
    for (size_t n = first; n < all.size() && k == all.size(); ++n)
        if (all[n].op == "Do" && all[n].name == pic.name && all[n].start >= pic.start && all[n].start < pic.end)
            k = n;
    if (k == all.size())
        return {};
    // the q's open at it (their matrix, the clips set inside them), and the clips outside any q
    struct Level {
        size_t q;
        fz_matrix ctm;
        std::vector<fz_rect> clips;
    };
    std::vector<Level> stack;
    std::vector<fz_matrix> saved;
    std::vector<fz_rect> top;
    fz_matrix ctm = fz_identity;
    fz_rect path = fz_empty_rect, clipNext = fz_empty_rect;
    bool clipping = false;
    for (size_t n = 0; n < k; ++n) {
        const Op &op = all[n];
        const auto &v = op.numbers;
        if (op.op == "q") {
            stack.push_back({n, ctm, {}});
            saved.push_back(ctm);
        } else if (op.op == "Q") {
            if (!stack.empty())
                stack.pop_back();
            if (!saved.empty()) {
                ctm = saved.back();
                saved.pop_back();
            }
        } else if (op.op == "cm" && v.size() >= 6) {
            const size_t b = v.size() - 6;
            ctm = fz_concat({float(v[b]), float(v[b + 1]), float(v[b + 2]), float(v[b + 3]), float(v[b + 4]), float(v[b + 5])}, ctm);
        } else if (op.op == "re" && v.size() >= 4) {
            const size_t b = v.size() - 4;
            for (auto [x, y] : {std::pair(v[b], v[b + 1]), std::pair(v[b] + v[b + 2], v[b + 1] + v[b + 3])})
                path = fz_include_point_in_rect(path, fz_transform_point_xy(float(x), float(y), ctm));
        } else if (op.op == "m" || op.op == "l" || op.op == "c" || op.op == "v" || op.op == "y") {
            for (size_t i = 0; i + 1 < v.size(); i += 2)
                path = fz_include_point_in_rect(path, fz_transform_point_xy(float(v[i]), float(v[i + 1]), ctm));
        } else if (op.op == "W" || op.op == "W*") {
            clipNext = path;
            clipping = true;
        } else if (op.op == "n" || op.op == "f" || op.op == "F" || op.op == "f*" || op.op == "S" || op.op == "s" ||
                   op.op == "B" || op.op == "B*" || op.op == "b" || op.op == "b*") {
            if (clipping)
                (stack.empty() ? top : stack.back().clips).push_back(clipNext);
            path = fz_empty_rect;
            clipping = false;
        }
    }
    std::vector<size_t> closes;  // (where each q still open is closed, innermost first)
    int depth = 0;
    for (size_t n = k + 1; n < all.size() && closes.size() < stack.size(); ++n) {
        if (all[n].op == "q")
            ++depth;
        else if (all[n].op == "Q") {
            if (depth)
                --depth;
            else
                closes.push_back(n);
        }
    }
    int best = -1;
    for (int level = int(stack.size()) - 1; level >= 0; --level) {  // (from the innermost q out)
        const size_t j = stack.size() - 1 - size_t(level);
        if (j >= closes.size() || stack[size_t(level)].q < first)
            break;
        bool still = true;
        for (size_t n = stack[size_t(level)].q + 1; n < closes[j] && still; ++n)
            still = n == k || quiet(all[n].op);
        if (!still)
            break;
        best = level;
    }
    if (best < 0)
        return {};
    std::vector<fz_rect> outer = top;
    for (int level = 0; level < best; ++level)
        outer.insert(outer.end(), stack[size_t(level)].clips.begin(), stack[size_t(level)].clips.end());
    for (const fz_rect &c : outer)
        if (!fz_contains_rect(fz_make_rect(c.x0 - 1, c.y0 - 1, c.x1 + 1, c.y1 + 1), newBox))
            return {};
    const fz_matrix cq = stack[size_t(best)].ctm;
    return {all[stack[size_t(best)].q].end, fz_concat(fz_concat(cq, move), fz_invert_matrix(cq))};
}

// a crop (the Crop tool) round a picture's "Do": q x y w h re W n /MPcrop MP ... Q - its box in
// the picture's own square. Found: the picture's crop, start .. end widened round it
void widenToCrop(const QByteArray &data, Picture &p)
{
    const qint64 k = data.lastIndexOf("/MPcrop", p.start);
    if (k < 0 || squeezed(data.mid(k, p.start - k)) != "/MPcropMP")
        return;
    const qint64 q = data.lastIndexOf('q', k);
    if (q < 0)
        return;
    // (a move puts its matrix just inside the q: "q a b c d e f cm x y w h re W n")
    static const QRegularExpression form(QStringLiteral(
        "^q\\s*((?:(?:[-0-9.eE]+\\s+){6}cm\\s*)*)([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+re\\s*W\\s*n\\s*$"));
    const QRegularExpressionMatch m = form.match(QString::fromLatin1(data.mid(q, k - q)));
    if (!m.hasMatch())
        return;
    p.cropMoves = m.captured(1).toLatin1();
    const qint64 e = data.indexOf('Q', p.end);
    if (e < 0 || !squeezed(data.mid(p.end, e - p.end)).isEmpty())
        return;
    p.crop = QRectF(m.captured(2).toDouble(), m.captured(3).toDouble(), m.captured(4).toDouble(), m.captured(5).toDouble());
    p.cropStart = q;
    p.cropEnd = e + 1;
    p.start = q;
    p.end = e + 1;
}

// the graphics state's matrix at a place in the page's drawing (stream, byte)
fz_matrix ctmAt(const std::vector<QByteArray> &streams, int stream, qint64 offset)
{
    std::vector<fz_matrix> stack;
    fz_matrix ctm = fz_identity;
    for (int s = 0; s <= stream && s < int(streams.size()); ++s)
        for (const Op &op : readOps(streams[size_t(s)])) {
            if (s == stream && op.start >= offset)
                return ctm;
            if (op.op == "q") {
                stack.push_back(ctm);
            } else if (op.op == "Q") {
                if (!stack.empty()) {
                    ctm = stack.back();
                    stack.pop_back();
                }
            } else if (op.op == "cm" && op.numbers.size() >= 6) {
                const size_t k = op.numbers.size() - 6;
                const fz_matrix m = {float(op.numbers[k]), float(op.numbers[k + 1]), float(op.numbers[k + 2]),
                                     float(op.numbers[k + 3]), float(op.numbers[k + 4]), float(op.numbers[k + 5])};
                ctm = concat(m, ctm);
            }
        }
    return ctm;
}

// (the clip's boxes - in the picture's own square - set in the page's own space: out of the picture's matrix,
// the boxes there, then back in it. MuPDF's tidying of a page - when turned text is taken out - threw away a
// picture whose clip was set inside its square: the tiles moved, gone when a word near them was moved)
QByteArray clipped(const QByteArray &path, const QByteArray &draw, const fz_matrix &c)
{
    QByteArray user;
    std::vector<double> nums;
    for (const QByteArray &w : path.simplified().split(' ')) {
        if (w == "re" && nums.size() >= 4) {
            const size_t b = nums.size() - 4;
            const double x = nums[b], y = nums[b + 1], ww = nums[b + 2], hh = nums[b + 3];
            const fz_point q[4] = {fz_transform_point_xy(float(x), float(y), c), fz_transform_point_xy(float(x + ww), float(y), c),
                                   fz_transform_point_xy(float(x + ww), float(y + hh), c), fz_transform_point_xy(float(x), float(y + hh), c)};
            char buf[256];
            fz_snprintf(buf, sizeof buf, "%g %g m %g %g l %g %g l %g %g l h ", q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y, q[3].x, q[3].y);
            user += buf;
            nums.clear();
            continue;
        }
        bool ok = false;
        const double v = w.toDouble(&ok);
        if (ok)
            nums.push_back(v);
        else
            nums.clear();
    }
    // (spaced: what's beside it may have none)
    return " q " + matrixOp(fz_invert_matrix(c)) + user + "W n " + matrixOp(c) + "/MPclip MP " + draw + " Q ";
}

// a see-through picture's unseen pixels given the colour of the nearest seen one (from each seen pixel outwards, a
// step at a time): its colours and its see-through mask are written apart, and drawn small they're each blended -
// the unseen pixels' black (as an edited picture leaves them) bled into its edge, a grey line round it
QImage bled(QImage argb)
{
    const int w = argb.width(), h = argb.height();
    if (w == 0 || h == 0)
        return argb;
    std::vector<char> done(size_t(w) * size_t(h), 0);
    std::vector<int> front;
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (qAlpha(row[x]) > 0) {
                done[size_t(y) * size_t(w) + size_t(x)] = 1;
                front.push_back(y * w + x);
            }
    }
    if (front.empty() || front.size() == size_t(w) * size_t(h))
        return argb;
    for (size_t at = 0; at < front.size(); ++at) {
        const int x = front[at] % w, y = front[at] / w;
        const QRgb from = reinterpret_cast<const QRgb *>(argb.constScanLine(y))[x];
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || done[size_t(ny) * size_t(w) + size_t(nx)])
                continue;
            done[size_t(ny) * size_t(w) + size_t(nx)] = 1;
            reinterpret_cast<QRgb *>(argb.scanLine(ny))[nx] = qRgba(qRed(from), qGreen(from), qBlue(from), 0);
            front.push_back(ny * w + nx);
        }
    }
    return argb;
}

}  // namespace

// ---- the pictures on a page ----

std::vector<Picture> Document::pictures(int page)
{
    std::lock_guard guard(docMutex_);
    return picturesLocked(page);
}

std::vector<Picture> Document::picturesLocked(int page)
{
    std::vector<Picture> out;
    if (!pdf_ || page < 0 || page >= int(lists_.size()))
        return out;
    const QPointF o = geometry()->origins[size_t(page)];
    pdf_page *pg = nullptr;
    fz_var(pg);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        fz_rect mediabox;
        fz_matrix pagectm;
        pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
        pdf_obj *res = pdf_dict_get_inheritable(ctx_, pg->obj, PDF_NAME(Resources));
        pdf_obj *xobjs = pdf_dict_get(ctx_, res, PDF_NAME(XObject));
        const std::vector<pdf_obj *> streams = contentStreams(ctx_, pg->obj);
        // the graphics state's matrix, through q / Q / cm, across the streams
        std::vector<fz_matrix> stack;
        fz_matrix ctm = fz_identity;
        int count = 0;
        long opNo = 0, lastText = -1;  // (to tell the pictures text is drawn after)
        std::vector<long> drawnAt;
        // the drawings (lines, boxes, frames - as Acrobat picks them): each painted path, from its first
        // point to its painting, and where it shows; the line width through q / Q; a new "segment" each
        // time text or a picture is drawn - a drawing is the paths painted one after another, touching,
        // with nothing else drawn between them
        struct Painted {
            Picture::Piece piece;
            int segment;
            long opNo;
            bool white = false;  // (filled white)
        };
        std::vector<Painted> painted;
        std::vector<float> widths;
        float lineWidth = 1;
        // (the fill's colour - a path just filled white is a text box's background, not seen: left out,
        // unless it's letters - a label in white, on a coloured shape)
        bool white = false;
        QColor fillColor = Qt::black;
        std::vector<bool> whites;
        std::vector<QColor> fills;
        qint64 pathStart = -1;
        fz_rect pathBox = fz_empty_rect;
        QPainterPath path;
        int curves = 0;
        int segment = 0;
        // (the path's operators as given, and the matrix then: painted under another (a "cm" between - as a
        // cleaned page writes "re q ... cm f Q"), it's shown as the matrix at its painting has it, as MuPDF
        // draws it - so read again so)
        std::vector<Op> pathOps;
        fz_matrix pathCtm = fz_identity;
        // (a picture clipped to its shape - moved, turned: the boxes of its clip, on the page - its shape's
        // outline read from them, as it was found in its pixels before)
        std::vector<QPointF> boxPts, clipPts, markedClip;
        for (int s = 0; s < int(streams.size()); ++s) {
            const QByteArray data = streamData(ctx_, streams[size_t(s)]);
            for (const Op &op : readOps(data)) {
                ++opNo;
                if (op.op == "Tj" || op.op == "TJ" || op.op == "'" || op.op == "\"") {
                    lastText = opNo;
                    ++segment;
                    continue;
                }
                const QByteArray &k = op.op;
                if (k == "m" || k == "l" || k == "c" || k == "v" || k == "y" || k == "re" || k == "h") {
                    if (pathStart < 0) {
                        pathStart = op.start;
                        pathBox = fz_empty_rect;
                        path = QPainterPath();
                        curves = 0;
                        pathOps.clear();
                        pathCtm = ctm;
                    }
                    pathOps.push_back(op);
                    const fz_matrix toPage = concat(ctm, pagectm);
                    const std::vector<double> &n = op.numbers;
                    auto add = [&](double x, double y) { pathBox = fz_include_point_in_rect(pathBox, fz_transform_point_xy(float(x), float(y), toPage)); };
                    auto pt = [&](double x, double y) {
                        const fz_point q = fz_transform_point_xy(float(x), float(y), toPage);
                        return QPointF(q.x - o.x(), q.y - o.y());
                    };
                    if (k == "re" && n.size() >= 4) {
                        add(n[0], n[1]), add(n[0] + n[2], n[1]), add(n[0], n[1] + n[3]), add(n[0] + n[2], n[1] + n[3]);
                        for (const QPointF &q : {pt(n[0], n[1]), pt(n[0] + n[2], n[1]), pt(n[0], n[1] + n[3]), pt(n[0] + n[2], n[1] + n[3])})
                            boxPts.push_back(q);
                        path.moveTo(pt(n[0], n[1]));
                        path.lineTo(pt(n[0] + n[2], n[1]));
                        path.lineTo(pt(n[0] + n[2], n[1] + n[3]));
                        path.lineTo(pt(n[0], n[1] + n[3]));
                        path.closeSubpath();
                    } else {
                        for (size_t i = 0; i + 1 < n.size(); i += 2)
                            add(n[i], n[i + 1]);
                        if ((k == "m" || k == "l") && n.size() >= 2)
                            boxPts.push_back(pt(n[0], n[1]));  // (a clip's outline: as its boxes')
                        if (k == "m" && n.size() >= 2)
                            path.moveTo(pt(n[0], n[1]));
                        else if (k == "l" && n.size() >= 2)
                            path.lineTo(pt(n[0], n[1]));
                        else if (k == "c" && n.size() >= 6)
                            path.cubicTo(pt(n[0], n[1]), pt(n[2], n[3]), pt(n[4], n[5])), ++curves;
                        else if (k == "v" && n.size() >= 4)
                            path.cubicTo(path.currentPosition(), pt(n[0], n[1]), pt(n[2], n[3])), ++curves;
                        else if (k == "y" && n.size() >= 4)
                            path.cubicTo(pt(n[0], n[1]), pt(n[2], n[3]), pt(n[2], n[3])), ++curves;
                        else if (k == "h")
                            path.closeSubpath();
                    }
                    continue;
                }
                if (k == "W" || k == "W*") {  // (the boxes so far: a clip)
                    clipPts = boxPts;
                    continue;
                }
                if (k == "MP" && op.name == "MPclip") {  // (the app's own: a picture's shape clip)
                    markedClip = clipPts;
                    continue;
                }
                if (k == "n") {
                    pathStart = -1;
                    boxPts.clear();
                    continue;
                }
                if (k == "g" || k == "rg" || k == "k" || k == "sc" || k == "scn") {
                    const std::vector<double> &n = op.numbers;
                    auto unit = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
                    if (n.size() == 1) {
                        white = n[0] > 0.99;
                        fillColor = QColor::fromRgbF(unit(n[0]), unit(n[0]), unit(n[0]));
                    } else if (n.size() == 3) {
                        white = n[0] > 0.99 && n[1] > 0.99 && n[2] > 0.99;
                        fillColor = QColor::fromRgbF(unit(n[0]), unit(n[1]), unit(n[2]));
                    } else if (n.size() == 4) {
                        white = n[0] < 0.01 && n[1] < 0.01 && n[2] < 0.01 && n[3] < 0.01;
                        fillColor = QColor::fromCmykF(unit(n[0]), unit(n[1]), unit(n[2]), unit(n[3]));
                    }
                    continue;
                }
                if (k == "cs") {
                    white = false;
                    fillColor = Qt::black;
                    continue;
                }
                if (k == "f" || k == "F" || k == "f*" || k == "S" || k == "s" || k == "B" || k == "B*" || k == "b" || k == "b*") {
                    const bool filledWhite = white && (k == "f" || k == "F" || k == "f*");
                    if (pathStart >= 0 && std::memcmp(&pathCtm, &ctm, sizeof ctm) != 0) {
                        const fz_matrix toPage = concat(ctm, pagectm);
                        auto pt = [&](double x, double y) {
                            const fz_point q = fz_transform_point_xy(float(x), float(y), toPage);
                            pathBox = fz_include_point_in_rect(pathBox, q);
                            return QPointF(q.x - o.x(), q.y - o.y());
                        };
                        pathBox = fz_empty_rect;
                        path = QPainterPath();
                        curves = 0;
                        for (const Op &po : pathOps) {
                            const std::vector<double> &n = po.numbers;
                            const QByteArray &pk = po.op;
                            if (pk == "re" && n.size() >= 4) {
                                path.moveTo(pt(n[0], n[1]));
                                path.lineTo(pt(n[0] + n[2], n[1]));
                                path.lineTo(pt(n[0] + n[2], n[1] + n[3]));
                                path.lineTo(pt(n[0], n[1] + n[3]));
                                path.closeSubpath();
                            } else if (pk == "m" && n.size() >= 2) {
                                path.moveTo(pt(n[0], n[1]));
                            } else if (pk == "l" && n.size() >= 2) {
                                path.lineTo(pt(n[0], n[1]));
                            } else if (pk == "c" && n.size() >= 6) {
                                const QPointF a = pt(n[0], n[1]), b = pt(n[2], n[3]), c = pt(n[4], n[5]);
                                path.cubicTo(a, b, c), ++curves;
                            } else if (pk == "v" && n.size() >= 4) {
                                const QPointF a = pt(n[0], n[1]), b = pt(n[2], n[3]);
                                path.cubicTo(path.currentPosition(), a, b), ++curves;
                            } else if (pk == "y" && n.size() >= 4) {
                                const QPointF a = pt(n[0], n[1]), b = pt(n[2], n[3]);
                                path.cubicTo(a, b, b), ++curves;
                            } else if (pk == "h") {
                                path.closeSubpath();
                            }
                        }
                    }
                    if (pathStart >= 0 && !fz_is_empty_rect(pathBox)) {
                        Painted d;
                        d.white = filledWhite;
                        d.piece.fill = fillColor;
                        d.piece.curves = curves;
                        path.setFillRule(k == "f*" || k == "B*" || k == "b*" ? Qt::OddEvenFill : Qt::WindingFill);
                        d.piece.path = path;
                        d.piece.stream = s;
                        d.piece.start = pathStart;
                        d.piece.end = op.end;
                        // (its matrix where it starts - where a change goes in; and as painted - how it's turned)
                        d.piece.ctm[0] = pathCtm.a, d.piece.ctm[1] = pathCtm.b, d.piece.ctm[2] = pathCtm.c, d.piece.ctm[3] = pathCtm.d,
                        d.piece.ctm[4] = pathCtm.e, d.piece.ctm[5] = pathCtm.f;
                        d.piece.painted[0] = ctm.a, d.piece.painted[1] = ctm.b, d.piece.painted[2] = ctm.c, d.piece.painted[3] = ctm.d;
                        // (stroked: its line's half width round it)
                        const bool stroked = k != "f" && k != "F" && k != "f*";
                        const double half = stroked ? lineWidth * std::sqrt(std::abs(double(ctm.a) * ctm.d - double(ctm.b) * ctm.c)) / 2 : 0;
                        d.piece.box = QRectF(QPointF(pathBox.x0 - o.x(), pathBox.y0 - o.y()), QPointF(pathBox.x1 - o.x(), pathBox.y1 - o.y()))
                                          .adjusted(-half, -half, half, half);
                        d.segment = segment;
                        d.opNo = opNo;
                        painted.push_back(d);
                    }
                    pathStart = -1;
                    boxPts.clear();
                    continue;
                }
                if (k == "w" && !op.numbers.empty()) {
                    lineWidth = float(op.numbers.back());
                    continue;
                }
                if (k == "BI" || k == "sh") {
                    ++segment;
                    continue;
                }
                if (op.op == "q") {
                    stack.push_back(ctm);
                    widths.push_back(lineWidth);
                    whites.push_back(white);
                    fills.push_back(fillColor);
                } else if (op.op == "Q") {
                    if (!whites.empty()) {
                        white = whites.back();
                        whites.pop_back();
                    }
                    if (!fills.empty()) {
                        fillColor = fills.back();
                        fills.pop_back();
                    }
                    if (!stack.empty()) {
                        ctm = stack.back();
                        stack.pop_back();
                    }
                    if (!widths.empty()) {
                        lineWidth = widths.back();
                        widths.pop_back();
                    }
                } else if (op.op == "cm" && op.numbers.size() >= 6) {
                    const size_t k = op.numbers.size() - 6;
                    const fz_matrix m = {float(op.numbers[k]), float(op.numbers[k + 1]), float(op.numbers[k + 2]),
                                         float(op.numbers[k + 3]), float(op.numbers[k + 4]), float(op.numbers[k + 5])};
                    ctm = concat(m, ctm);
                } else if (op.op == "Do" && !op.name.isEmpty()) {
                    ++segment;  // (something drawn: drawings before it and after it apart)
                    pdf_obj *x = pdf_dict_gets(ctx_, xobjs, op.name.constData());
                    if (!pdf_name_eq(ctx_, pdf_dict_get(ctx_, x, PDF_NAME(Subtype)), PDF_NAME(Image)))
                        continue;
                    Picture p;
                    p.index = count++;
                    p.stream = s;
                    p.start = op.start;
                    p.end = op.end;
                    p.doStart = op.start;
                    p.doEnd = op.end;
                    widenToCrop(data, p);
                    widenToClip(data, p);
                    p.name = op.name;
                    p.xref = pdf_to_num(ctx_, x);
                    p.ctm[0] = ctm.a, p.ctm[1] = ctm.b, p.ctm[2] = ctm.c, p.ctm[3] = ctm.d, p.ctm[4] = ctm.e, p.ctm[5] = ctm.f;
                    const fz_matrix shown = concat(ctm, pagectm);  // (the unit square, as shown)
                    for (const fz_point &c : {fz_transform_point_xy(0, 1, shown), fz_transform_point_xy(1, 1, shown),
                                              fz_transform_point_xy(1, 0, shown), fz_transform_point_xy(0, 0, shown)})
                        p.whole.push_back(QPointF(c.x - o.x(), c.y - o.y()));
                    // (cropped: the part shown)
                    const QRectF u = p.crop.isNull() ? QRectF(0, 0, 1, 1) : p.crop;
                    const float ux0 = float(u.left()), uy0 = float(u.top()), ux1 = float(u.right()), uy1 = float(u.bottom());
                    const fz_point corners[4] = {fz_transform_point_xy(ux0, uy1, shown), fz_transform_point_xy(ux1, uy1, shown),
                                                 fz_transform_point_xy(ux1, uy0, shown), fz_transform_point_xy(ux0, uy0, shown)};
                    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                    for (const fz_point &c : corners) {
                        const QPointF q(c.x - o.x(), c.y - o.y());
                        p.corners.push_back(q);
                        x0 = std::min(x0, q.x()), y0 = std::min(y0, q.y());
                        x1 = std::max(x1, q.x()), y1 = std::max(y1, q.y());
                    }
                    p.rect = QRectF(QPointF(x0, y0), QPointF(x1, y1));
                    p.width = pdf_dict_get_int(ctx_, x, PDF_NAME(Width));
                    p.height = pdf_dict_get_int(ctx_, x, PDF_NAME(Height));
                    p.order = opNo;
                    if (p.clipped && outlines_.count(p.xref)) {  // (the shape found in it before: the same frame)
                        p.full = p.rect;
                        frameFromHull(p, outlines_[p.xref]);
                    } else if (p.clipped && markedClip.size() >= 3) {
                        shapeFromClip(p, markedClip);
                    }
                    out.push_back(p);
                    drawnAt.push_back(opNo);
                }
            }
        }
        for (size_t k = 0; k < out.size(); ++k)
            out[k].textAfter = lastText > drawnAt[k];
        // the drawings: paths painted one after another, touching (a frame's sides, a rule's lines), as one
        std::vector<Picture> drawings;
        int lastSegment = -1;
        for (const Painted &d : painted) {
            const QRectF box = d.piece.box.adjusted(-0.25, -0.25, 0.25, 0.25);  // (a hairline: some width)
            if (drawings.empty() || d.segment != lastSegment || !drawings.back().rect.adjusted(-2, -2, 2, 2).intersects(box)) {
                Picture g;
                g.vector = true;
                g.stream = d.piece.stream;
                g.start = d.piece.start;
                g.order = d.opNo;
                g.rect = box;
                drawings.push_back(g);
            }
            Picture &g = drawings.back();
            g.pieces.push_back(d.piece);
            g.whites.push_back(d.white);
            g.rect |= box;
            if (d.piece.stream == g.stream)
                g.end = d.piece.end;
            g.textAfter = lastText > d.opNo;
            lastSegment = d.segment;
        }
        const QSizeF size = geometry()->sizes.at(size_t(page));
        for (Picture &g : drawings) {
            // (not the page's background, nor a speck)
            if ((g.rect.width() >= 0.98 * size.width() && g.rect.height() >= 0.98 * size.height()) ||
                (g.rect.width() < 1.5 && g.rect.height() < 1.5))
                continue;
            // letters drawn as shapes (as the Python app tells them): shapes of many curves - not boxes and
            // lines; a label saved a line a shape, or a letter a shape
            int most = 0, all = 0;
            for (const Picture::Piece &pc : g.pieces)
                most = std::max(most, pc.curves), all += pc.curves;
            const int n = int(g.pieces.size());
            g.letters = (most >= 10 && all >= 4 * n) || (n == 1 && most >= 8);
            // (white: a text box's background, not seen - kept only as letters, a label in white)
            if (!g.letters && std::all_of(g.whites.begin(), g.whites.end(), [](bool w) { return w; }))
                continue;
            g.corners = {g.rect.topLeft(), g.rect.topRight(), g.rect.bottomRight(), g.rect.bottomLeft()};
            // a drawing turned (its paths' matrix turned - by the rotate handle): its frame its own box, turned with
            // it - measured across its own way, then turned back; its top side where its top went
            double turn = 0;
            bool same = true;
            for (size_t i = 0; i < g.pieces.size(); ++i) {
                const Picture::Piece &pc = g.pieces[i];
                const fz_matrix m = concat({float(pc.painted[0]), float(pc.painted[1]), float(pc.painted[2]), float(pc.painted[3]), 0, 0}, pagectm);
                const double a = std::atan2(double(m.b), double(m.a));
                if (i == 0)
                    turn = a;
                else if (std::abs(std::remainder(a - turn, 2 * M_PI)) > 1e-3)
                    same = false;
            }
            if (same && std::abs(std::remainder(turn, 2 * M_PI)) > 1e-3) {
                const QTransform back = QTransform().rotateRadians(-turn), there = QTransform().rotateRadians(turn);
                double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                for (const Picture::Piece &pc : g.pieces) {
                    const double half = std::max(0.0, (pc.box.width() - pc.path.controlPointRect().width()) / 2);
                    for (int e = 0; e < pc.path.elementCount(); ++e) {
                        const QPointF q = back.map(QPointF(pc.path.elementAt(e)));
                        x0 = std::min(x0, q.x() - half), y0 = std::min(y0, q.y() - half);
                        x1 = std::max(x1, q.x() + half), y1 = std::max(y1, q.y() + half);
                    }
                }
                if (x1 > x0 && y1 > y0) {
                    const QRectF own = QRectF(QPointF(x0, y0), QPointF(x1, y1)).adjusted(-0.25, -0.25, 0.25, 0.25);
                    g.corners = {there.map(own.topLeft()), there.map(own.topRight()), there.map(own.bottomRight()), there.map(own.bottomLeft())};
                }
            }
            g.whole = g.corners;
            out.push_back(g);
        }
        // (all in the order the page draws them - the topmost last - and numbered so)
        std::stable_sort(out.begin(), out.end(), [](const Picture &a, const Picture &b) { return a.order < b.order; });
        for (size_t k = 0; k < out.size(); ++k)
            out[k].index = int(k);
    }
    fz_always(ctx_) {
        fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
        out.clear();
    }
    for (Picture &p : out)  // (a shape on a background saved into it: picked by its shape - not a cropped one)
        if (p.vector)
            continue;
        else if (p.clipped && !p.shape.empty())
            continue;  // (its shape from its clip already)
        else if (p.crop.isNull())
            applyShape(page, p);
        else
            p.full = p.rect;
    return out;
}

// ---- changing one ----

namespace {

fz_matrix pictureCtm(const Picture &p)
{
    return {float(p.ctm[0]), float(p.ctm[1]), float(p.ctm[2]), float(p.ctm[3]), float(p.ctm[4]), float(p.ctm[5])};
}

}  // namespace

// a drawing's paths, each changed where it is (from the last back, so the places before stay where they are)
void Document::editPieces(pdf_page *pg, const Picture &pic, const std::function<QByteArray(const Picture::Piece &, const QByteArray &)> &change)
{
    const std::vector<pdf_obj *> streams = contentStreams(ctx_, pg->obj);
    std::map<int, std::vector<const Picture::Piece *>> byStream;
    for (const Picture::Piece &pc : pic.pieces)
        byStream[pc.stream].push_back(&pc);
    for (auto &[s, pieces] : byStream) {
        pdf_obj *stm = streams.at(size_t(s));
        QByteArray data = streamData(ctx_, stm);
        std::sort(pieces.begin(), pieces.end(), [](const Picture::Piece *a, const Picture::Piece *b) { return a->start > b->start; });
        for (const Picture::Piece *pc : pieces) {
            if (pc->end > data.size())
                fz_throw(ctx_, FZ_ERROR_GENERIC, "the drawing isn't where it was");
            data = data.left(pc->start) + change(*pc, data.mid(pc->start, pc->end - pc->start)) + data.mid(pc->end);
        }
        setStreamData(ctx_, pdf_, stm, data);
    }
}

bool Document::transformDrawing(int page, const Picture &pic, const QTransform &move, const char *what, QString *error)
{
    // each of its paths in a q ... Q of its own with the move - in its own matrix there (C' = C x move)
    const QPointF o = geometry()->origins.at(size_t(page));
    std::lock_guard guard(docMutex_);
    return onPage(page, what, [&](pdf_page *pg) {
        fz_rect mediabox;
        fz_matrix pagectm;
        pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
        const fz_matrix shown = {float(move.m11()), float(move.m12()), float(move.m21()), float(move.m22()),
                                 float(move.dx() + o.x() - (move.m11() * o.x() + move.m21() * o.y())),
                                 float(move.dy() + o.y() - (move.m12() * o.x() + move.m22() * o.y()))};
        const fz_matrix inPdf = concat(concat(pagectm, shown), fz_invert_matrix(pagectm));
        editPieces(pg, pic, [&](const Picture::Piece &pc, const QByteArray &text) {
            const fz_matrix c = {float(pc.ctm[0]), float(pc.ctm[1]), float(pc.ctm[2]), float(pc.ctm[3]), float(pc.ctm[4]), float(pc.ctm[5])};
            const fz_matrix m = concat(concat(c, inPdf), fz_invert_matrix(c));
            return QByteArray(" q ") + matrixOp(m) + " " + text + " Q ";
        });
    }, error);
}

bool Document::transformPicture(int page, const Picture &pic, const QTransform &move, const char *what, QString *error)
{
    if (pic.vector)
        return transformDrawing(page, pic, move, what, error);
    // move: in the page's own space, as shown (points from its top left)
    const QPointF o = geometry()->origins.at(size_t(page));
    // a picture text flows round (or behind the text): its layout goes with it, the text laid out
    // round it where it goes - in the same change
    const QRectF whole = pic.full.isNull() ? pic.rect : pic.full;
    const FlowPlan flow = pictureLayout(page, pic) != QLatin1String("front") ? planFlow(page, &pic, move.mapRect(whole), QString())
                                                                            : FlowPlan();
    std::lock_guard guard(docMutex_);
    // a shape saved on a background (a slide's tile saved with the slide behind it, a logo on a
    // band): it - and the ones drawn after it over where it was or goes - clipped to its shape, so
    // the background saved round it doesn't go with it and cover what's beside it (the Python app's
    // clip_shapes). A picture with no shape found in it: moved whole, as it's saved
    const QRectF was = pic.full.isNull() ? pic.rect : pic.full, goes = move.mapRect(was);
    std::vector<std::pair<Picture, QByteArray>> clips;
    for (const Picture &q : picturesLocked(page)) {
        if (q.vector)
            continue;
        const QRectF r = q.full.isNull() ? q.rect : q.full;
        const bool it = q.index == pic.index;
        if (q.clipped || q.stream != pic.stream || (!it && (q.index < pic.index || !(r.intersects(was) || r.intersects(goes)))))
            continue;
        const QByteArray path = shapeClip(page, q);
        if (!path.isEmpty())
            clips.emplace_back(q, path);
    }
    const bool ok = onPage(page, what, [&](pdf_page *pg) {
        fz_rect mediabox;
        fz_matrix pagectm;
        pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
        // the move in PDF space: to the page as shown, moved, and back
        const fz_matrix shown = {float(move.m11()), float(move.m12()), float(move.m21()), float(move.m22()),
                                 float(move.dx() + o.x() - (move.m11() * o.x() + move.m21() * o.y())),
                                 float(move.dy() + o.y() - (move.m12() * o.x() + move.m22() * o.y()))};
        const fz_matrix inPdf = concat(concat(pagectm, shown), fz_invert_matrix(pagectm));
        // put just before the picture's own drawing, in a q ... Q of its own: the picture
        // ends up moved where the page draws it (C' = C x move: M = C x move x C^-1)
        const fz_matrix c = pictureCtm(pic);
        const fz_matrix m = concat(concat(c, inPdf), fz_invert_matrix(c));
        const std::vector<pdf_obj *> streams = contentStreams(ctx_, pg->obj);
        pdf_obj *stm = streams.at(size_t(pic.stream));
        const QByteArray data = streamData(ctx_, stm);
        if (pic.end > data.size())
            fz_throw(ctx_, FZ_ERROR_GENERIC, "the picture isn't where it was");
        // moved inside its own q ... Q (with the crop round it) if it has one - else its drawing
        // wrapped in a q ... Q with the move
        std::vector<QByteArray> before;
        for (int s = 0; s < pic.stream; ++s)
            before.push_back(streamData(ctx_, streams[size_t(s)]));
        const InPlace in = movedInPlace(before, data, pic, inPdf, fz_transform_rect(fz_unit_rect, concat(c, inPdf)));
        QByteArray now = data;
        qint64 added = 0;
        if (in.at >= 0) {
            const QByteArray cm = " " + matrixOp(in.x);
            now.insert(in.at, cm);
            added = cm.size();
        }
        auto after = [&](qint64 v) { return in.at >= 0 && v >= in.at ? v + added : v; };
        // (from the end of the drawing back, so the places before stay where they are)
        std::vector<std::tuple<qint64, qint64, QByteArray>> edits;
        // (its drawing as it is now - the move may have gone inside it, just inside its crop's q)
        QByteArray draw = now.mid(after(pic.start), after(pic.end) - after(pic.start));
        for (const auto &[q, path] : clips)
            if (q.index == pic.index)
                draw = clipped(path, draw, pictureCtm(q));
            else if (q.end <= data.size())
                edits.emplace_back(after(q.start), after(q.end), clipped(path, now.mid(after(q.start), q.end - q.start), pictureCtm(q)));
        edits.emplace_back(after(pic.start), after(pic.end), in.at >= 0 ? draw : " q " + matrixOp(m) + draw + " Q ");
        std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
        for (const auto &[a, b, text] : edits)
            now = now.left(a) + text + now.mid(b);
        setStreamData(ctx_, pdf_, stm, now);
        applyFlow(pg, page, flow);
    }, error);
    if (ok && std::abs(move.m12()) < 1e-6 && std::abs(move.m21()) < 1e-6 && move.m11() > 0 && move.m22() > 0)
        shapeMoved(page, pic, goes);
    return ok;
}

namespace {

// what the page draws, in its order - to arrange a picture among: each piece of text (BT ... ET, or a drawing of its own
// with text in it), each thing painted (a filled or stroked shape, a picture, a shading) and where it shows (PDF space).
// "cover": painted over most of the page - its background (a white sheet, a slide's backdrop), never put over a picture
struct Layer {
    int stream = 0;
    qint64 start = 0, end = 0;
    bool text = false, cover = false;
    fz_rect box = fz_empty_rect;
};

bool before(int s1, qint64 at1, int s2, qint64 at2)
{
    return s1 < s2 || (s1 == s2 && at1 < at2);
}

// a path's points (m l c v y re), in the page's PDF space
void pathPoints(fz_rect &path, const Op &op, const fz_matrix &ctm)
{
    const QByteArray &o = op.op;
    const std::vector<double> &n = op.numbers;
    auto point = [&](double x, double y) { path = fz_include_point_in_rect(path, fz_transform_point(fz_make_point(float(x), float(y)), ctm)); };
    const size_t want = o == "c" ? 6 : (o == "v" || o == "y" || o == "re") ? 4 : 2;
    if (n.size() < want)
        return;
    const size_t k = n.size() - want;
    if (o == "re") {
        point(n[k], n[k + 1]);
        point(n[k] + n[k + 2], n[k + 1] + n[k + 3]);
        point(n[k] + n[k + 2], n[k + 1]);
        point(n[k], n[k + 1] + n[k + 3]);
    } else {
        for (size_t i = k; i + 1 < n.size(); i += 2)
            point(n[i], n[i + 1]);
    }
}

bool isPath(const QByteArray &o)
{
    return o == "m" || o == "l" || o == "c" || o == "v" || o == "y" || o == "re";
}

bool isPaint(const QByteArray &o)
{
    return o == "f" || o == "F" || o == "f*" || o == "B" || o == "B*" || o == "b" || o == "b*" || o == "S" || o == "s" || o == "n";
}

fz_matrix opMatrix(const std::vector<double> &n)
{
    const size_t k = n.size() - 6;
    return {float(n[k]), float(n[k + 1]), float(n[k + 2]), float(n[k + 3]), float(n[k + 4]), float(n[k + 5])};
}

// the bytes a text operator writes ((...) and <...> strings): its letters, one or two bytes each
int textBytes(const QByteArray &raw)
{
    int n = 0;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw[i] == '(') {
            int depth = 1;
            for (++i; i < raw.size(); ++i) {
                const char d = raw[i];
                if (d == '\\') {
                    ++i;
                    if (i < raw.size() && raw[i] >= '0' && raw[i] <= '7')
                        for (int k = 1; k < 3 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7'; ++k)
                            ++i;
                } else if (d == '(') {
                    ++depth;
                } else if (d == ')' && --depth == 0) {
                    break;
                }
                ++n;
            }
        } else if (raw[i] == '<') {
            int digits = 0;
            for (++i; i < raw.size() && raw[i] != '>'; ++i)
                digits += std::isxdigit(static_cast<unsigned char>(raw[i])) ? 1 : 0;
            n += (digits + 1) / 2;
        }
    }
    return n;
}

std::vector<Layer> pageLayers(fz_context *ctx, pdf_obj *pageobj, const std::vector<QByteArray> &streams, const fz_rect &page)
{
    pdf_obj *res = pdf_dict_get_inheritable(ctx, pageobj, PDF_NAME(Resources));
    pdf_obj *xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    pdf_obj *fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    const float area = (page.x1 - page.x0) * (page.y1 - page.y0);
    auto covers = [&](const fz_rect &r) {
        const fz_rect in = fz_intersect_rect(r, page);
        return !fz_is_empty_rect(in) && (in.x1 - in.x0) * (in.y1 - in.y0) >= 0.85f * area;
    };
    static const QRegularExpression textOp(QStringLiteral("(^|\\s)BT(\\s|$)"));
    std::vector<Layer> out;
    std::vector<std::pair<fz_matrix, fz_rect>> stack;
    fz_matrix ctm = fz_identity;
    fz_rect clip = fz_infinite_rect;  // (where what's drawn can show: text clipped to its table cell shows only there)
    bool clipping = false;
    size_t shown = 0;  // (the layers cut to the clip they're drawn in, so far)
    // (the text's state: where it's written - its box worked out without the font's widths, half its size a letter)
    fz_matrix tm = fz_identity, tlm = fz_identity;
    double size = 0, leading = 0, hscale = 1, rise = 0;
    bool twoByte = false;
    for (int s = 0; s < int(streams.size()); ++s) {
        qint64 textStart = -1, pathStart = -1;
        fz_rect path = fz_empty_rect, written = fz_empty_rect;
        for (const Op &op : readOps(streams[size_t(s)])) {
            const QByteArray &o = op.op;
            const std::vector<double> &n = op.numbers;
            for (; shown < out.size(); ++shown)  // (what was just drawn: only where it can show)
                out[shown].box = fz_intersect_rect(out[shown].box, clip);
            if (o == "q") {
                stack.emplace_back(ctm, clip);
            } else if (o == "Q") {
                if (!stack.empty()) {
                    ctm = stack.back().first;
                    clip = stack.back().second;
                    stack.pop_back();
                }
            } else if (o == "cm" && n.size() >= 6) {
                ctm = concat(opMatrix(n), ctm);
            } else if (o == "W" || o == "W*") {
                clipping = true;
            } else if (o == "Tf" && !n.empty()) {
                size = n.back();
                pdf_obj *font = pdf_dict_gets(ctx, fonts, op.name.constData());
                twoByte = pdf_name_eq(ctx, pdf_dict_get(ctx, font, PDF_NAME(Subtype)), PDF_NAME(Type0));
            } else if (o == "TL" && !n.empty()) {
                leading = n.back();
            } else if (o == "Tz" && !n.empty()) {
                hscale = n.back() / 100;
            } else if (o == "Ts" && !n.empty()) {
                rise = n.back();
            } else if (o == "BT") {
                textStart = op.start;
                tm = tlm = fz_identity;
                written = fz_empty_rect;
            } else if (o == "ET") {
                if (textStart >= 0) {
                    Layer l;
                    l.stream = s;
                    l.start = textStart;
                    l.end = op.end;
                    l.text = true;
                    l.box = fz_is_empty_rect(written) ? page : written;  // (not worked out: anywhere)
                    out.push_back(l);
                }
                textStart = -1;
            } else if (textStart >= 0) {  // (inside the text: where its letters go)
                if ((o == "Td" || o == "TD") && n.size() >= 2) {
                    if (o == "TD")
                        leading = -n.back();
                    tlm = concat(fz_translate(float(n[n.size() - 2]), float(n.back())), tlm);
                    tm = tlm;
                } else if (o == "Tm" && n.size() >= 6) {
                    tm = tlm = opMatrix(n);
                } else if (o == "T*" || o == "'" || o == "\"") {
                    tlm = concat(fz_translate(0, float(-leading)), tlm);
                    tm = tlm;
                }
                if (o == "Tj" || o == "TJ" || o == "'" || o == "\"") {
                    const int bytes = textBytes(streams[size_t(s)].mid(op.start, op.end - op.start));
                    const double w = (twoByte ? bytes / 2.0 : bytes) * 0.5 * size * hscale;
                    const fz_rect r = {0, float(rise - 0.25 * size), float(w), float(rise + 0.9 * size)};
                    written = fz_union_rect(written, fz_transform_rect(r, concat(tm, ctm)));
                    tm = concat(fz_translate(float(w), 0), tm);
                }
            } else if (isPath(o)) {
                if (pathStart < 0)
                    pathStart = op.start;
                pathPoints(path, op, ctm);
            } else if (o == "n") {
                if (clipping)
                    clip = fz_intersect_rect(clip, path);
                clipping = false;
                pathStart = -1;
                path = fz_empty_rect;
            } else if (isPaint(o)) {
                if (clipping)
                    clip = fz_intersect_rect(clip, path);
                clipping = false;
                if (pathStart >= 0 && !fz_is_empty_rect(path)) {
                    Layer l;
                    l.stream = s;
                    l.start = pathStart;
                    l.end = op.end;
                    l.box = path;
                    l.cover = o != "S" && o != "s" && covers(path);
                    out.push_back(l);
                }
                pathStart = -1;
                path = fz_empty_rect;
            } else if (o == "sh") {
                Layer l;
                l.stream = s;
                l.start = op.start;
                l.end = op.end;
                l.box = page;
                l.cover = true;
                out.push_back(l);
            } else if (o == "BI" || (o == "Do" && !op.name.isEmpty())) {
                Layer l;
                l.stream = s;
                l.start = op.start;
                l.end = op.end;
                pdf_obj *x = o == "Do" ? pdf_dict_gets(ctx, xobjs, op.name.constData()) : nullptr;
                if (x && pdf_name_eq(ctx, pdf_dict_get(ctx, x, PDF_NAME(Subtype)), PDF_NAME(Form))) {
                    fz_buffer *buf = pdf_load_stream(ctx, x);
                    unsigned char *bytes = nullptr;
                    const size_t len = fz_buffer_storage(ctx, buf, &bytes);
                    l.text = QString::fromLatin1(reinterpret_cast<const char *>(bytes), qsizetype(len)).contains(textOp);
                    fz_drop_buffer(ctx, buf);
                    l.box = fz_transform_rect(pdf_dict_get_rect(ctx, x, PDF_NAME(BBox)),
                                              concat(pdf_dict_get_matrix(ctx, x, PDF_NAME(Matrix)), ctm));
                } else {
                    l.box = fz_transform_rect(fz_unit_rect, ctm);
                }
                l.cover = !l.text && covers(l.box);
                out.push_back(l);
            }
        }
    }
    for (; shown < out.size(); ++shown)
        out[shown].box = fz_intersect_rect(out[shown].box, clip);
    return out;
}

// where a picture goes, among the page's layers: (stream, byte) - none: stream < 0
struct Spot {
    int stream = -1;
    qint64 at = 0;
};

// a place moved out of the clips round it that would cut the picture away (a table cell's text is drawn clipped to its
// cell: a picture put in there showed only inside the cell - or not at all): before the first of them (after: past it)
Spot unclipped(const std::vector<QByteArray> &streams, Spot spot, const fz_rect &pic, bool after)
{
    struct Frame {
        int stream;
        qint64 start;
        fz_rect clip;  // (the clip before its q)
        fz_matrix ctm;
    };
    std::vector<Frame> frames;
    fz_matrix ctm = fz_identity;
    fz_rect clip = fz_infinite_rect, path = fz_empty_rect;
    bool clipping = false;
    bool reached = false;
    for (int s = 0; s < int(streams.size()) && !reached; ++s)
        for (const Op &op : readOps(streams[size_t(s)])) {
            if (s == spot.stream && op.start >= spot.at) {
                reached = true;
                break;
            }
            const QByteArray &o = op.op;
            if (o == "q") {
                frames.push_back({s, op.start, clip, ctm});
            } else if (o == "Q") {
                if (!frames.empty()) {
                    clip = frames.back().clip;
                    ctm = frames.back().ctm;
                    frames.pop_back();
                }
            } else if (o == "cm" && op.numbers.size() >= 6) {
                ctm = concat(opMatrix(op.numbers), ctm);
            } else if (isPath(o)) {
                pathPoints(path, op, ctm);
            } else if (o == "W" || o == "W*") {
                clipping = true;
            } else if (isPaint(o)) {
                if (clipping)
                    clip = fz_intersect_rect(clip, path);
                clipping = false;
                path = fz_empty_rect;
            }
        }
    const float a = (pic.x1 - pic.x0) * (pic.y1 - pic.y0);
    auto holds = [&](const fz_rect &c) {
        const fz_rect in = fz_intersect_rect(c, pic);
        return a <= 0 || (!fz_is_empty_rect(in) && (in.x1 - in.x0) * (in.y1 - in.y0) >= 0.98f * a);
    };
    int bad = -1;
    for (int i = 0; i < int(frames.size()) && bad < 0; ++i)
        if (!holds(i + 1 < int(frames.size()) ? frames[size_t(i) + 1].clip : clip))
            bad = i;
    if (bad < 0)
        return spot;
    if (!after)
        return {frames[size_t(bad)].stream, frames[size_t(bad)].start};
    int depth = int(frames.size());  // (past the Q that ends it)
    for (int s = spot.stream; s < int(streams.size()); ++s)
        for (const Op &op : readOps(streams[size_t(s)])) {
            if (s == spot.stream && op.start < spot.at)
                continue;
            if (op.op == "q") {
                ++depth;
            } else if (op.op == "Q") {
                if (depth == bad + 1)
                    return {s, op.end};
                --depth;
            }
        }
    return spot;
}

struct Arrangement {
    std::vector<Layer> layers;  // (the picture's own drawing not among them)
    std::vector<QByteArray> streams;
    fz_rect box = fz_empty_rect;  // (the picture's, PDF space)
    int firstText = -1;           // (the first piece of text)
    int firstOver = -1;           // (the first piece of text it covers)
    int background = -1;          // (the last of the backgrounds: the covers drawn before any text)
    int own = 0;                  // (where the picture is: how many layers are drawn before it)
    // (what covers just a bit of it - a table's line across it, its edge - passed by, not a step)
    bool over(const Layer &l) const
    {
        const fz_rect in = fz_intersect_rect(l.box, box);
        return !fz_is_empty_rect(in) && in.x1 - in.x0 >= 1.5f && in.y1 - in.y0 >= 1.5f &&
               (in.x1 - in.x0) * (in.y1 - in.y0) >= 0.02f * (box.x1 - box.x0) * (box.y1 - box.y0);
    }
};

Arrangement arrange(fz_context *ctx, pdf_page *pg, const Picture &pic)
{
    fz_rect mediabox;
    fz_matrix pagectm;
    pdf_page_transform(ctx, pg, &mediabox, &pagectm);
    Arrangement a;
    for (pdf_obj *o : contentStreams(ctx, pg->obj))
        a.streams.push_back(streamData(ctx, o));
    a.box = fz_transform_rect(fz_unit_rect, pictureCtm(pic));
    for (const Layer &l : pageLayers(ctx, pg->obj, a.streams, mediabox))
        if (!(l.stream == pic.stream && l.start >= pic.start && l.end <= pic.end))
            a.layers.push_back(l);
    for (int i = 0; i < int(a.layers.size()); ++i) {
        const Layer &l = a.layers[size_t(i)];
        if (a.firstText < 0 && l.text)
            a.firstText = i;
        if (a.firstOver < 0 && l.text && a.over(l))
            a.firstOver = i;
        if (a.firstText < 0 && l.cover)
            a.background = i;
        if (before(l.stream, l.start, pic.stream, pic.start))
            a.own = i + 1;
    }
    return a;
}

}  // namespace

void Document::drawPictureAt(pdf_page *pg, const Picture &pic, int stream, qint64 at)
{
    // its drawing taken from where it is and put there, in the page's matrix at that place
    const std::vector<pdf_obj *> objs = contentStreams(ctx_, pg->obj);
    std::vector<QByteArray> streams;
    for (pdf_obj *o : objs)
        streams.push_back(streamData(ctx_, o));
    const QByteArray from = streams.at(size_t(pic.stream));
    if (pic.end > from.size() || at > streams.at(size_t(stream)).size())
        fz_throw(ctx_, FZ_ERROR_GENERIC, "the picture isn't where it was");
    const fz_matrix here = ctmAt(streams, stream, at);
    const QByteArray draw = " q " + matrixOp(concat(pictureCtm(pic), fz_invert_matrix(here))) + from.mid(pic.start, pic.end - pic.start) + " Q ";
    const QByteArray without = from.left(pic.start) + " " + from.mid(pic.end);
    if (stream == pic.stream) {  // (the later place changed first: the earlier one stays where it is)
        QByteArray data = from;
        if (at >= pic.end) {
            data.insert(at, draw);
            data = data.left(pic.start) + " " + data.mid(pic.end);
        } else {
            data = without;
            data.insert(at, draw);
        }
        setStreamData(ctx_, pdf_, objs.at(size_t(stream)), data);
    } else {
        QByteArray to = streams.at(size_t(stream));
        to.insert(at, draw);
        setStreamData(ctx_, pdf_, objs.at(size_t(stream)), to);
        setStreamData(ctx_, pdf_, objs.at(size_t(pic.stream)), without);
    }
}

bool Document::stepPicture(int page, const Picture &pic, bool forward, QString *error)
{
    if (pic.vector) {
        if (error)
            *error = QStringLiteral("only pictures can be arranged");
        return false;
    }
    // one step under / over what it covers: the next thing painted over it - or all the text over it drawn before that,
    // as one step. What doesn't touch it is passed by, and the page's
    // background stays under it. Nothing to pass: false, no error (it's at the back / front already)
    std::lock_guard guard(docMutex_);
    Spot spot;
    pdf_page *pg = nullptr;
    fz_var(pg);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        const Arrangement a = arrange(ctx_, pg, pic);
        bool text = false;  // (the text over it: one step, however many pieces it's written in)
        if (forward) {
            for (int i = a.own; i < int(a.layers.size()); ++i) {
                const Layer &l = a.layers[size_t(i)];
                if (!a.over(l))
                    continue;
                if (l.text) {
                    text = true;
                    spot = {l.stream, l.end};
                    continue;
                }
                if (!text)
                    spot = {l.stream, l.end};
                break;
            }
        } else {
            for (int i = a.own - 1; i >= 0; --i) {
                const Layer &l = a.layers[size_t(i)];
                if (i <= a.background && l.cover)
                    break;  // (the page's background: not under it)
                if (!a.over(l))
                    continue;
                if (l.text) {
                    text = true;
                    spot = {l.stream, l.start};
                    continue;
                }
                if (!text)
                    spot = {l.stream, l.start};
                break;
            }
        }
        if (spot.stream >= 0)
            spot = unclipped(a.streams, spot, a.box, forward);
    }
    fz_always(ctx_) {
        fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
        if (error)
            *error = QString::fromUtf8(fz_caught_message(ctx_));
        return false;
    }
    if (spot.stream < 0)
        return false;
    return onPage(page, forward ? "bring forward" : "send backward", [&](pdf_page *p) { drawPictureAt(p, pic, spot.stream, spot.at); },
                  error);
}

bool Document::cropPicture(int page, const Picture &pic, const QRectF &unitIn, QString *error)
{
    if (pic.vector) {
        if (error)
            *error = QStringLiteral("only pictures can be cropped");
        return false;
    }
    // its "Do" in a q ... Q of its own with the box (replacing a crop it had); all of it: none
    const QRectF unit = unitIn.normalized() & QRectF(0, 0, 1, 1);
    if (unit.width() < 0.005 || unit.height() < 0.005)
        return false;
    const bool all = unit.left() < 0.001 && unit.top() < 0.001 && unit.right() > 0.999 && unit.bottom() > 0.999;
    std::lock_guard guard(docMutex_);
    return onPage(page, all ? "uncrop picture" : "crop picture", [&](pdf_page *pg) {
        pdf_obj *stm = contentStreams(ctx_, pg->obj).at(size_t(pic.stream));
        const QByteArray data = streamData(ctx_, stm);
        if (pic.doEnd > data.size() || pic.end > data.size())
            fz_throw(ctx_, FZ_ERROR_GENERIC, "the picture isn't where it was");
        const QByteArray doOp = data.mid(pic.doStart, pic.doEnd - pic.doStart);
        const QByteArray moves = pic.cropStart >= 0 ? pic.cropMoves : QByteArray();
        const QByteArray now = all ? (moves.isEmpty() ? " " + doOp + " " : " q " + moves + " " + doOp + " Q ")
                                   : " q " + moves + " " + QByteArray::number(unit.left(), 'f', 5) + " " + QByteArray::number(unit.top(), 'f', 5) + " " +
                                         QByteArray::number(unit.width(), 'f', 5) + " " + QByteArray::number(unit.height(), 'f', 5) +
                                         " re W n /MPcrop MP " + doOp + " Q ";
        const qint64 a = pic.cropStart >= 0 ? pic.cropStart : pic.doStart, b = pic.cropStart >= 0 ? pic.cropEnd : pic.doEnd;
        setStreamData(ctx_, pdf_, stm, data.left(a) + now + data.mid(b));
    }, error);
}

bool Document::deletePicture(int page, const Picture &pic, QString *error)
{
    std::lock_guard guard(docMutex_);
    if (pic.vector)  // (a drawing: its paths taken out)
        return onPage(page, "delete drawing", [&](pdf_page *pg) { editPieces(pg, pic, [](const Picture::Piece &, const QByteArray &) { return QByteArray(" "); }); },
                      error);
    return onPage(page, "delete picture", [&](pdf_page *pg) {
        pdf_obj *stm = contentStreams(ctx_, pg->obj).at(size_t(pic.stream));
        const QByteArray data = streamData(ctx_, stm);
        setStreamData(ctx_, pdf_, stm, data.left(pic.start) + " " + data.mid(pic.end));
    }, error);
}

bool Document::layerPicture(int page, const Picture &pic, bool front, QString *error)
{
    if (pic.vector) {
        if (error)
            *error = QStringLiteral("only pictures can be arranged");
        return false;
    }
    // Bring to Front: drawn last, over everything; Send to Back: under everything but the page's background (a white
    // sheet, a slide's backdrop - under that it would be hidden)
    std::lock_guard guard(docMutex_);
    return onPage(page, front ? "bring to front" : "send to back", [&](pdf_page *pg) { layerIn(pg, pic, front); }, error);
}

void Document::behindText(pdf_page *pg, const Picture &pic)
{
    // just under the text it covers: still over the page's background and what's drawn before that text (a picture
    // under it already stays - unless it's under the background, where it's hidden: put over that)
    const Arrangement a = arrange(ctx_, pg, pic);
    const int under = a.firstOver >= 0 ? a.firstOver : a.firstText;  // (the text it covers - none: the page's text)
    Spot spot;
    if (under >= 0 && a.own > under) {
        const Layer &l = a.layers[size_t(under)];
        spot = unclipped(a.streams, {l.stream, l.start}, a.box, false);
    } else if (a.background >= 0 && a.own <= a.background) {
        const Layer &l = a.layers[size_t(a.background)];
        spot = unclipped(a.streams, {l.stream, l.end}, a.box, true);
    }
    if (spot.stream >= 0)
        drawPictureAt(pg, pic, spot.stream, spot.at);
}

void Document::layerIn(pdf_page *pg, const Picture &pic, bool front)
{
    if (!front) {  // (to the back: just over the page's background, if it has one)
        const Arrangement a = arrange(ctx_, pg, pic);
        if (a.background >= 0) {
            const Layer &l = a.layers[size_t(a.background)];
            const Spot spot = unclipped(a.streams, {l.stream, l.end}, a.box, true);
            drawPictureAt(pg, pic, spot.stream, spot.at);
            return;
        }
    }
    pdf_obj *stm = contentStreams(ctx_, pg->obj).at(size_t(pic.stream));
    const QByteArray data = streamData(ctx_, stm);
    setStreamData(ctx_, pdf_, stm, data.left(pic.start) + " " + data.mid(pic.end));
    const QByteArray draw = "\nq " + matrixOp(pictureCtm(pic)) + data.mid(pic.start, pic.end - pic.start) + " Q\n";
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(draw.constData()),
                                                    size_t(draw.size()));
    pdf_obj *arr = nullptr;
    fz_var(arr);
    fz_try(ctx_) {
        pdf_obj *old = pdf_dict_get(ctx_, pg->obj, PDF_NAME(Contents));
        arr = pdf_new_array(ctx_, pdf_, 4);
        if (front) {  // (what was there in q ... Q, the picture after it)
            fz_buffer *q = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>("q\n"), 2);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, q, nullptr, 0));
            fz_drop_buffer(ctx_, q);
        } else {
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, buf, nullptr, 0));
        }
        if (pdf_is_array(ctx_, old))
            for (int i = 0; i < pdf_array_len(ctx_, old); ++i)
                pdf_array_push(ctx_, arr, pdf_array_get(ctx_, old, i));
        else if (old)
            pdf_array_push(ctx_, arr, old);
        if (front) {
            fz_buffer *end = fz_new_buffer(ctx_, size_t(draw.size()) + 4);
            fz_append_string(ctx_, end, "\nQ");
            fz_append_buffer(ctx_, end, buf);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, end, nullptr, 0));
            fz_drop_buffer(ctx_, end);
        }
        pdf_dict_put(ctx_, pg->obj, PDF_NAME(Contents), arr);
    }
    fz_always(ctx_) {
        fz_drop_buffer(ctx_, buf);
        pdf_drop_obj(ctx_, arr);
    }
    fz_catch(ctx_) {
        fz_rethrow(ctx_);
    }
}

namespace {

// a picture file read for the PDF: as it is (JPEG, PNG...) when MuPDF reads it, else made a
// PNG by Qt first
QByteArray pictureBytes(const QString &path, const QImage &image, QString *error)
{
    if (!image.isNull()) {
        QByteArray png;
        QBuffer out(&png);
        out.open(QIODevice::WriteOnly);
        (image.hasAlphaChannel() ? bled(image.convertToFormat(QImage::Format_ARGB32)) : image).save(&out, "PNG");  // (see-through: no grey edge, bled above)
        return png;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return {};
    }
    return f.readAll();
}

}  // namespace

bool Document::placePicture(int page, const QString &path, const QImage &image, const QRectF &box, bool fit,
                            const Picture *replacing, QString *error)
{
    // box: where it goes (page points); fit: the picture as big as fits in it, its shape kept
    // (else it fills the box). replacing: drawn in that picture's place, instead of it.
    QByteArray bytes = pictureBytes(path, image, error);
    if (bytes.isEmpty())
        return false;
    QImage probe;
    if (!probe.loadFromData(bytes) && image.isNull()) {
        if (error)
            *error = QStringLiteral("It can't be read as a picture.");
        return false;
    }
    const QPointF o = geometry()->origins.at(size_t(page));
    QRectF r = box;
    if (fit && !probe.isNull()) {
        const double k = std::min(box.width() / probe.width(), box.height() / probe.height());
        const QSizeF s(probe.width() * k, probe.height() * k);
        r = QRectF(box.center() - QPointF(s.width() / 2, s.height() / 2), s);
    }
    // a format MuPDF doesn't read (WebP...): as PNG
    {
        fz_buffer *test = fz_new_buffer_from_shared_data(ctx_, reinterpret_cast<const unsigned char *>(bytes.constData()),
                                                         size_t(bytes.size()));
        bool readable = true;
        fz_image *img = nullptr;
        fz_var(img);
        fz_try(ctx_) {
            img = fz_new_image_from_buffer(ctx_, test);
        }
        fz_always(ctx_) {
            fz_drop_image(ctx_, img);
            fz_drop_buffer(ctx_, test);
        }
        fz_catch(ctx_) {
            readable = false;
        }
        if (!readable && !probe.isNull())
            bytes = pictureBytes(QString(), probe, error);
    }
    // (see-through - an edited cut-out, a PNG with its own see-through parts: its colours and its mask written apart, as
    // they are - MuPDF reading the PNG would make its unseen pixels black, a grey line round it when drawn small)
    const bool seeThrough = !probe.isNull() && probe.hasAlphaChannel();
    std::lock_guard guard(docMutex_);
    return onPage(page, replacing ? "replace picture" : "add picture", [&](pdf_page *pg) {
        placeIn(pg, page, bytes, r, replacing, seeThrough ? &probe : nullptr);
    }, error);
}

// (docMutex_ held, in a change) the picture's bytes added, drawn at r (page points) - in replacing's
// place in the page's drawing, or over the page
void Document::placeIn(pdf_page *pg, int page, const QByteArray &bytes, const QRectF &r, const Picture *replacing, const QImage *seeThrough)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    const Picture old = replacing ? *replacing : Picture();
    fz_rect mediabox;
    fz_matrix pagectm;
    pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(bytes.constData()),
                                                    size_t(bytes.size()));
    fz_image *img = nullptr;
    pdf_obj *ref = nullptr;
    fz_pixmap *colours = nullptr, *alpha = nullptr;
    fz_image *mask = nullptr;
    fz_var(img);
    fz_var(ref);
    fz_var(colours);
    fz_var(alpha);
    fz_var(mask);
    const QImage argb = seeThrough ? bled(seeThrough->convertToFormat(QImage::Format_ARGB32)) : QImage();
    fz_try(ctx_) {
        if (seeThrough) {
            const int w = argb.width(), h = argb.height();
            colours = fz_new_pixmap(ctx_, fz_device_rgb(ctx_), w, h, nullptr, 0);
            alpha = fz_new_pixmap(ctx_, fz_device_gray(ctx_), w, h, nullptr, 0);
            unsigned char *c = fz_pixmap_samples(ctx_, colours), *a = fz_pixmap_samples(ctx_, alpha);
            const ptrdiff_t cs = fz_pixmap_stride(ctx_, colours), as = fz_pixmap_stride(ctx_, alpha);
            for (int y = 0; y < h; ++y) {
                const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    c[y * cs + 3 * x] = uchar(qRed(row[x]));
                    c[y * cs + 3 * x + 1] = uchar(qGreen(row[x]));
                    c[y * cs + 3 * x + 2] = uchar(qBlue(row[x]));
                    a[y * as + x] = uchar(qAlpha(row[x]));
                }
            }
            mask = fz_new_image_from_pixmap(ctx_, alpha, nullptr);
            img = fz_new_image_from_pixmap(ctx_, colours, mask);
        } else {
            img = fz_new_image_from_buffer(ctx_, buf);
        }
        ref = pdf_add_image(ctx_, pdf_, img);
        // its name in the page's resources
        pdf_obj *res = pdf_dict_get(ctx_, pg->obj, PDF_NAME(Resources));
        if (!res) {
            pdf_obj *inherited = pdf_dict_get_inheritable(ctx_, pg->obj, PDF_NAME(Resources));
            pdf_dict_put_drop(ctx_, pg->obj, PDF_NAME(Resources),
                              inherited ? pdf_deep_copy_obj(ctx_, inherited) : pdf_new_dict(ctx_, pdf_, 2));
            res = pdf_dict_get(ctx_, pg->obj, PDF_NAME(Resources));
        }
        pdf_obj *xobjs = pdf_dict_get(ctx_, res, PDF_NAME(XObject));
        if (!xobjs)
            xobjs = pdf_dict_put_dict(ctx_, res, PDF_NAME(XObject), 2);
        char name[32];
        int k = 1;
        do
            fz_snprintf(name, sizeof name, "MPimg%d", k++);
        while (pdf_dict_gets(ctx_, xobjs, name));
        pdf_dict_puts(ctx_, xobjs, name, ref);
        // its square to the box, as shown (the picture's top row at the box's top)
        const fz_matrix shown = {float(r.width()), 0, 0, -float(r.height()), float(r.left() + o.x()), float(r.bottom() + o.y())};
        const fz_matrix inPdf = concat(shown, fz_invert_matrix(pagectm));
        if (replacing) {  // drawn in the old one's place in the page's drawing
            pdf_obj *stm = contentStreams(ctx_, pg->obj).at(size_t(old.stream));
            fz_buffer *now = pdf_load_stream(ctx_, stm);
            unsigned char *data = nullptr;
            const size_t len = fz_buffer_storage(ctx_, now, &data);
            fz_buffer *out = fz_new_buffer(ctx_, len + 128);
            fz_append_data(ctx_, out, data, size_t(old.start));
            // (the matrix there is the old picture's: the new one's from it)
            const fz_matrix m = concat(inPdf, fz_invert_matrix(pictureCtm(old)));
            fz_append_printf(ctx_, out, " q %g %g %g %g %g %g cm /%s Do Q ", m.a, m.b, m.c, m.d, m.e, m.f, name);
            fz_append_data(ctx_, out, data + old.end, len - size_t(old.end));
            pdf_update_stream(ctx_, pdf_, stm, out, 0);
            fz_drop_buffer(ctx_, out);
            fz_drop_buffer(ctx_, now);
        } else {  // drawn on top of the page
            fz_buffer *draw = fz_new_buffer(ctx_, 128);
            fz_append_printf(ctx_, draw, "\nQ\nq %g %g %g %g %g %g cm /%s Do Q\n", inPdf.a, inPdf.b, inPdf.c, inPdf.d,
                             inPdf.e, inPdf.f, name);
            fz_buffer *q = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>("q\n"), 2);
            pdf_obj *old = pdf_dict_get(ctx_, pg->obj, PDF_NAME(Contents));
            pdf_obj *arr = pdf_new_array(ctx_, pdf_, 4);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, q, nullptr, 0));
            if (pdf_is_array(ctx_, old))
                for (int i = 0; i < pdf_array_len(ctx_, old); ++i)
                    pdf_array_push(ctx_, arr, pdf_array_get(ctx_, old, i));
            else if (old)
                pdf_array_push(ctx_, arr, old);
            pdf_array_push_drop(ctx_, arr, pdf_add_stream(ctx_, pdf_, draw, nullptr, 0));
            pdf_dict_put_drop(ctx_, pg->obj, PDF_NAME(Contents), arr);
            fz_drop_buffer(ctx_, q);
            fz_drop_buffer(ctx_, draw);
        }
    }
    fz_always(ctx_) {
        pdf_drop_obj(ctx_, ref);
        fz_drop_image(ctx_, img);
        fz_drop_image(ctx_, mask);
        fz_drop_pixmap(ctx_, colours);
        fz_drop_pixmap(ctx_, alpha);
        fz_drop_buffer(ctx_, buf);
    }
    fz_catch(ctx_) {
        fz_rethrow(ctx_);
    }
}

bool Document::pictureToText(int page, const Picture &pic, const QImage &cleaned, const QRectF &box, const std::vector<OcrLine> &lines,
                             QString *error)
{
    // the picture's letters read (OCR): the picture without them, real text where they were - one change
    QString why;
    const QByteArray bytes = pictureBytes(QString(), cleaned, &why);
    if (bytes.isEmpty()) {
        if (error)
            *error = why;
        return false;
    }
    struct Out {
        QString text;
        QPointF at;
        fonts::Found font;
        double size, scale;
        QColor color;
    };
    std::vector<Out> out;
    for (const OcrLine &l : lines) {
        if (l.text.trimmed().isEmpty() || l.size <= 0)
            continue;
        const QString family = l.mono ? QStringLiteral("Courier New") : l.serif ? QStringLiteral("Times New Roman") : QStringLiteral("Arial");
        const fonts::Found font = fonts::forText(fonts::findFamily(family, l.bold, false), l.text);
        const double natural = textWidth(l.text, font, l.size);
        // (as wide as the letters were in the picture: its letters made a little wider / narrower)
        const double scale = natural > 0 && l.width > 0 ? std::clamp(l.width / natural, 0.6, 1.8) : 1;
        out.push_back({l.text, l.baseline, font, l.size, scale, l.color});
    }
    std::lock_guard guard(docMutex_);
    return onPage(page, "convert to text", [&](pdf_page *pg) {
        placeIn(pg, page, bytes, box, &pic);
        for (const Out &o : out)
            writeText(pg, page, o.at, o.text, o.font, o.size, o.color, o.scale, 0);
    }, error);
}

bool Document::scanText(int page, const std::vector<OcrLine> &lines, QString *error)
{
    struct Out {
        QString text;
        QPointF at;
        fonts::Found font;
        double size, scale;
        QColor color;
    };
    std::vector<Out> out;
    for (const OcrLine &l : lines) {
        if (l.text.trimmed().isEmpty() || l.size <= 0)
            continue;
        const QString family = !l.family.isEmpty() ? l.family
                               : l.mono            ? QStringLiteral("Courier New")
                               : l.serif           ? QStringLiteral("Times New Roman")
                                                   : QStringLiteral("Arial");
        fonts::Found font = fonts::forText(fonts::findFamily(family, l.bold, false), l.text);
        if (!l.fontName.isEmpty()) {  // (the scan's own letters, if it has them all)
            const fonts::Found own = fonts::findFont(l.fontName, l.bold ? 1 : 0);
            if (fonts::isEmbedded(own) && fonts::hasLetters(own, l.text))
                font = own;
        }
        const double natural = textWidth(l.text, font, l.size);
        // (as wide as the scan's line: the letters made a little wider / narrower - found, picked where they're seen)
        const double scale = natural > 0 && l.width > 0 ? std::clamp(l.width / natural, 0.5, 2.0) : 1;
        out.push_back({l.text, l.baseline, font, l.size, scale, l.color});
    }
    if (out.empty())
        return true;
    std::lock_guard guard(docMutex_);
    struct Seen {  // (writeText's text drawn again, however it ends)
        bool &on;
        ~Seen() { on = false; }
    } seen{invisibleText_};
    invisibleText_ = true;
    return onPage(page, "scan", [&](pdf_page *pg) {
        for (const Out &o : out)
            writeText(pg, page, o.at, o.text, o.font, o.size, o.color, o.scale, 0);
    }, error);
}

bool Document::scanPictures(int page, const std::vector<ScanPicture> &pictures, const Picture *background, const QByteArray &cleaned,
                            QString *error)
{
    if (pictures.empty())
        return true;
    std::lock_guard guard(docMutex_);
    const bool clean = background && !cleaned.isEmpty();
    return onPage(page, "scan", [&](pdf_page *pg) {
        if (clean)  // (the scan without the pictures' marks: paper where they were)
            placeIn(pg, page, cleaned, background->rect, background);
        for (size_t n = 0; n < pictures.size(); ++n) {
            if (pictures[n].image.isNull())
                continue;
            if (!clean)
                coverText(pg, page, pictures[n].box, pictures[n].paper, true);  // (moved away: paper where it was)
            placeIn(pg, page, QByteArray(), pictures[n].box, nullptr, &pictures[n].image);
        }
    }, error);
}

bool Document::drawingToText(int page, const Picture &pic, double degrees, QPointF pivot, const std::vector<OcrLine> &lines,
                             const std::vector<std::pair<Picture, QImage>> &cleaned, QString *error)
{
    struct Clean {
        Picture pic;
        QByteArray bytes;
    };
    std::vector<Clean> clean;
    for (const auto &[p, im] : cleaned) {
        QString why;
        const QByteArray bytes = pictureBytes(QString(), im, &why);
        if (bytes.isEmpty()) {
            if (error)
                *error = why;
            return false;
        }
        clean.push_back({p, bytes});
    }
    // (put back from the last drawn: the places before it - the shapes' too - stay where they are)
    std::sort(clean.begin(), clean.end(), [](const Clean &a, const Clean &b) {
        return a.pic.stream != b.pic.stream ? a.pic.stream > b.pic.stream : a.pic.start > b.pic.start;
    });
    // letters drawn as shapes read: the shapes taken out, real text where they were, turned as they
    // were - one change
    struct Out {
        QString text;
        QPointF at;
        fonts::Found font;
        double size, scale;
        QColor color;
    };
    std::vector<Out> out;
    for (const OcrLine &l : lines) {
        if (l.text.trimmed().isEmpty() || l.size <= 0)
            continue;
        const fonts::Found font = fonts::forText(fonts::findFamily(l.serif ? QStringLiteral("Times New Roman") : QStringLiteral("Arial"), l.bold, false), l.text);
        const double natural = textWidth(l.text, font, l.size);
        const double scale = natural > 0 && l.width > 0 ? std::clamp(l.width / natural, 0.6, 1.8) : 1;
        out.push_back({l.text, l.baseline, font, l.size, scale, l.color});
    }
    std::lock_guard guard(docMutex_);
    struct Straight {  // (writeText's turn put back, however it ends)
        Document *d;
        ~Straight() { d->setTurn(0, QPointF()); }
    } straight{this};
    return onPage(page, "convert to text", [&](pdf_page *pg) {
        for (const Clean &c : clean)
            placeIn(pg, page, c.bytes, c.pic.full.isNull() ? c.pic.rect : c.pic.full, &c.pic);
        editPieces(pg, pic, [](const Picture::Piece &, const QByteArray &) { return QByteArray(" "); });
        setTurn(std::abs(degrees) < 0.05 ? 0 : degrees, pivot);
        for (const Out &o : out)
            writeText(pg, page, o.at, o.text, o.font, o.size, o.color, o.scale, 0);
        setTurn(0, QPointF());
    }, error);
}

QImage Document::pictureImage(const Picture &pic)
{
    if (pic.vector)  // (a drawing: no pixels of its own)
        return {};
    // the picture as it's stored (its own pixels, not as big as it's shown)
    QImage out;
    std::lock_guard guard(docMutex_);
    pdf_obj *obj = pdf_new_indirect(ctx_, pdf_, pic.xref, 0);
    fz_image *img = nullptr;
    fz_pixmap *pix = nullptr;
    fz_pixmap *rgb = nullptr;
    fz_pixmap *mask = nullptr;  // (its see-through parts, if it has any: its soft mask)
    fz_var(img);
    fz_var(pix);
    fz_var(rgb);
    fz_var(mask);
    fz_try(ctx_) {
        img = pdf_load_image(ctx_, pdf_, obj);
        pix = fz_get_pixmap_from_image(ctx_, img, nullptr, nullptr, nullptr, nullptr);
        rgb = fz_convert_pixmap(ctx_, pix, fz_device_rgb(ctx_), nullptr, nullptr, fz_default_color_params, 1);
        if (img->mask)
            mask = fz_get_pixmap_from_image(ctx_, img->mask, nullptr, nullptr, nullptr, nullptr);
    }
    fz_always(ctx_) {
        fz_drop_image(ctx_, img);
        pdf_drop_obj(ctx_, obj);
    }
    fz_catch(ctx_) {
    }
    if (rgb) {
        const int w = fz_pixmap_width(ctx_, rgb), h = fz_pixmap_height(ctx_, rgb);
        const int stride = int(fz_pixmap_stride(ctx_, rgb));
        const bool alpha = fz_pixmap_alpha(ctx_, rgb);
        out = QImage(fz_pixmap_samples(ctx_, rgb), w, h, stride, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888)
                  .copy();
        fz_drop_pixmap(ctx_, rgb);
        // (its soft mask: its alpha - as big as it, or stretched to it)
        if (mask && fz_pixmap_components(ctx_, mask) >= 1) {
            const int mw = fz_pixmap_width(ctx_, mask), mh = fz_pixmap_height(ctx_, mask), mn = fz_pixmap_components(ctx_, mask);
            QImage a(fz_pixmap_samples(ctx_, mask), mw, mh, int(fz_pixmap_stride(ctx_, mask)), mn == 1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
            a = a.convertToFormat(QImage::Format_Grayscale8);
            if (a.size() != out.size())
                a = a.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            out = out.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < out.height(); ++y) {
                QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
                const uchar *m = a.constScanLine(y);
                for (int x = 0; x < out.width(); ++x)
                    o[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(o[x]) * m[x] / 255);
            }
        }
    }
    fz_drop_pixmap(ctx_, mask);
    fz_drop_pixmap(ctx_, pix);
    return out;
}

// ---- form fields (a signing app's "SIGN here" box) ----

std::vector<FormField> Document::fields(int page)
{
    std::vector<FormField> out;
    std::lock_guard guard(docMutex_);
    if (!pdf_ || page < 0 || page >= int(lists_.size()))
        return out;
    const QPointF o = geometry()->origins[size_t(page)];
    pdf_page *pg = nullptr;
    fz_var(pg);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        for (pdf_annot *w = pdf_first_widget(ctx_, pg); w; w = pdf_next_widget(ctx_, w)) {
            FormField f;
            f.id = pdf_to_num(ctx_, pdf_annot_obj(ctx_, w));
            const fz_rect r = pdf_bound_annot(ctx_, w);
            f.rect = QRectF(QPointF(r.x0 - o.x(), r.y0 - o.y()), QPointF(r.x1 - o.x(), r.y1 - o.y()));
            f.signature = pdf_widget_type(ctx_, w) == PDF_WIDGET_TYPE_SIGNATURE;
            const char *label = pdf_field_label(ctx_, pdf_annot_obj(ctx_, w));
            f.name = QString::fromUtf8(label ? label : "");
            out.push_back(f);
        }
    }
    fz_always(ctx_) {
        fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
    }
    return out;
}

bool Document::deleteField(int page, int id, QString *error)
{
    // the field taken out of the file: off the page, and out of the form's list of fields
    std::lock_guard guard(docMutex_);
    return onPage(page, "delete form field", [&](pdf_page *pg) {
        pdf_obj *annots = pdf_dict_get(ctx_, pg->obj, PDF_NAME(Annots));
        for (int i = pdf_array_len(ctx_, annots) - 1; i >= 0; --i)
            if (pdf_to_num(ctx_, pdf_array_get(ctx_, annots, i)) == id)
                pdf_array_delete(ctx_, annots, i);
        pdf_obj *fieldsArr = pdf_dict_getp(ctx_, pdf_trailer(ctx_, pdf_), "Root/AcroForm/Fields");
        for (int i = pdf_array_len(ctx_, fieldsArr) - 1; i >= 0; --i)
            if (pdf_to_num(ctx_, pdf_array_get(ctx_, fieldsArr, i)) == id)
                pdf_array_delete(ctx_, fieldsArr, i);
        // (a widget that's a kid of a field: out of its parent's kids)
        pdf_obj *self = pdf_new_indirect(ctx_, pdf_, id, 0);
        pdf_obj *parent = pdf_dict_get(ctx_, self, PDF_NAME(Parent));
        pdf_obj *kids = pdf_dict_get(ctx_, parent, PDF_NAME(Kids));
        for (int i = pdf_array_len(ctx_, kids) - 1; i >= 0; --i)
            if (pdf_to_num(ctx_, pdf_array_get(ctx_, kids, i)) == id)
                pdf_array_delete(ctx_, kids, i);
        pdf_drop_obj(ctx_, self);
    }, error);
}
