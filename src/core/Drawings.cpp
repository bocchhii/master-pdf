// Document's drawings (step 7): what the page draws with lines and fills, read from its
// content streams - and letters saved as drawn shapes (a label PowerPoint or Word saved as
// outlines) found among them, to be read and made real text (Convert to text).
// (No C++ objects are made inside fz_try blocks or the functions run in them.)

#include "core/Content.h"
#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QRegularExpression>

#include <algorithm>
#include <functional>
#include <map>
#include <cmath>
#include <numeric>

using namespace content;

namespace {

struct Path {
    int stream;
    qint64 start, end;
    QRectF rect;  // (as shown, points from the page's top left)
    QColor color;
    bool filled;
};

}  // namespace

std::vector<Drawing> Document::drawings(int page)
{
    std::vector<Path> paths;
    {
        std::lock_guard guard(docMutex_);
        if (!pdf_ || page < 0 || page >= int(lists_.size()))
            return {};
        const QPointF o = geometry()->origins[size_t(page)];
        pdf_page *pg = nullptr;
        fz_var(pg);
        fz_try(ctx_) {
            pg = pdf_load_page(ctx_, pdf_, page);
            fz_rect mediabox;
            fz_matrix pagectm;
            pdf_page_transform(ctx_, pg, &mediabox, &pagectm);
            const std::vector<pdf_obj *> streams = contentStreams(ctx_, pg->obj);
            std::vector<std::pair<fz_matrix, QColor>> stack;
            fz_matrix ctm = fz_identity;
            QColor fill(Qt::black);
            for (int s = 0; s < int(streams.size()); ++s) {
                const QByteArray data = streamData(ctx_, streams[size_t(s)]);
                qint64 pathStart = -1;
                fz_rect box = fz_empty_rect;
                auto add = [&](double x, double y) {
                    const fz_point p = fz_transform_point(fz_make_point(float(x), float(y)), fz_concat(ctm, pagectm));
                    box = fz_include_point_in_rect(box, p);
                };
                for (const Op &op : readOps(data)) {
                    const auto &n = op.numbers;
                    const QByteArray &k = op.op;
                    if (k == "q") {
                        stack.emplace_back(ctm, fill);
                    } else if (k == "Q") {
                        if (!stack.empty()) {
                            ctm = stack.back().first;
                            fill = stack.back().second;
                            stack.pop_back();
                        }
                    } else if (k == "cm" && n.size() >= 6) {
                        const size_t b = n.size() - 6;
                        ctm = fz_concat({float(n[b]), float(n[b + 1]), float(n[b + 2]), float(n[b + 3]), float(n[b + 4]), float(n[b + 5])}, ctm);
                    } else if ((k == "rg" || k == "sc" || k == "scn") && n.size() >= 3) {
                        const size_t b = n.size() - 3;
                        fill = QColor::fromRgbF(float(n[b]), float(n[b + 1]), float(n[b + 2]));
                    } else if ((k == "g" || ((k == "sc" || k == "scn") && n.size() == 1)) && !n.empty()) {
                        fill = QColor::fromRgbF(float(n.back()), float(n.back()), float(n.back()));
                    } else if (k == "k" && n.size() >= 4) {
                        const size_t b = n.size() - 4;
                        fill = QColor::fromCmykF(float(n[b]), float(n[b + 1]), float(n[b + 2]), float(n[b + 3])).toRgb();
                    } else if (k == "m" || k == "l" || k == "c" || k == "v" || k == "y" || k == "re" || k == "h") {
                        if (pathStart < 0) {
                            pathStart = op.start;
                            box = fz_empty_rect;
                        }
                        if (k == "re" && n.size() >= 4) {
                            const size_t b = n.size() - 4;
                            add(n[b], n[b + 1]);
                            add(n[b] + n[b + 2], n[b + 1] + n[b + 3]);
                            add(n[b], n[b + 1] + n[b + 3]);
                            add(n[b] + n[b + 2], n[b + 1]);
                        } else {
                            for (size_t i = 0; i + 1 < n.size(); i += 2)
                                add(n[i], n[i + 1]);
                        }
                    } else if (k == "f" || k == "F" || k == "f*" || k == "B" || k == "B*" || k == "b" || k == "b*" ||
                               k == "S" || k == "s" || k == "n") {
                        if (pathStart >= 0 && k != "n" && !fz_is_empty_rect(box)) {
                            Path p;
                            p.stream = s;
                            p.start = pathStart;
                            p.end = op.end;
                            p.rect = QRectF(QPointF(box.x0 - o.x(), box.y0 - o.y()), QPointF(box.x1 - o.x(), box.y1 - o.y()));
                            p.color = fill;
                            p.filled = k != "S" && k != "s";
                            paths.push_back(p);
                        }
                        pathStart = -1;
                    } else if (k == "W" || k == "W*") {
                        // (a clip: its path is ended by n)
                    } else {
                        pathStart = pathStart >= 0 ? pathStart : -1;
                    }
                }
            }
        }
        fz_always(ctx_) {
            fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
        }
        fz_catch(ctx_) {
            paths.clear();
        }
    }
    // letters: small filled shapes, side by side on a line, in one colour - grouped
    std::vector<int> letters;
    for (int i = 0; i < int(paths.size()); ++i) {
        const QRectF &r = paths[size_t(i)].rect;
        if (paths[size_t(i)].filled && r.height() >= 2 && r.height() <= 72 && r.width() <= 150)
            letters.push_back(i);
    }
    std::vector<int> parent(paths.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int x) { return parent[size_t(x)] == x ? x : parent[size_t(x)] = root(parent[size_t(x)]); };
    for (size_t a = 0; a < letters.size(); ++a)
        for (size_t b = a + 1; b < letters.size(); ++b) {
            const Path &p = paths[size_t(letters[a])], &q = paths[size_t(letters[b])];
            const double h = std::max(p.rect.height(), q.rect.height());
            const double hs = std::min(p.rect.height(), q.rect.height());
            const double gap = std::max(p.rect.left(), q.rect.left()) - std::min(p.rect.right(), q.rect.right());
            const double overlap = std::min(p.rect.bottom(), q.rect.bottom()) - std::max(p.rect.top(), q.rect.top());
            const bool sameColour = std::abs(p.color.red() - q.color.red()) + std::abs(p.color.green() - q.color.green()) +
                                        std::abs(p.color.blue() - q.color.blue()) < 60;
            if (sameColour && h < hs * 4 && gap < 0.6 * h && overlap > 0.25 * hs)
                parent[size_t(root(letters[b]))] = root(letters[a]);
        }
    std::map<int, std::vector<int>> groups;
    for (int i : letters)
        groups[root(i)].push_back(i);
    std::vector<Drawing> out;
    for (const auto &[r, members] : groups) {
        QRectF rect = paths[size_t(members[0])].rect;
        for (int m : members)
            rect |= paths[size_t(m)].rect;
        if (members.size() < 2 && rect.width() < rect.height() * 1.5)
            continue;  // (one shape alone: a dot, an icon - not letters)
        Drawing d;
        d.index = int(out.size());
        d.rect = rect;
        d.color = paths[size_t(members[0])].color;
        for (int m : members)
            d.parts.push_back({paths[size_t(m)].stream, paths[size_t(m)].start, paths[size_t(m)].end});
        out.push_back(d);
    }
    return out;
}

bool Document::removeDrawing(int page, const Drawing &d, const std::vector<std::pair<QString, QRectF>> &lines, QString *error)
{
    // its shapes out of the page's drawing (each stream from its end, so the places hold);
    // the lines read written in their place: as wide as the letters were, in their colour
    struct Line {
        QString text;
        fonts::Found font;
        double size;
        QPointF at;
    };
    std::vector<Line> write;
    for (const auto &[text, rect] : lines) {
        Line l;
        l.text = text;
        l.font = fonts::forText(fonts::findFamily(QStringLiteral("Arial"), false, false), text);
        const double w1 = std::max(1e-6, textWidth(text, l.font, 1));
        l.size = std::max(4.0, rect.width() / w1);
        const bool low = text.contains(QRegularExpression(QStringLiteral("[gjpqy,;]")));  // (letters below the line)
        l.at = QPointF(rect.left(), rect.bottom() - (low ? 0.21 * l.size : 0.0));
        write.push_back(l);
    }
    std::vector<DrawingPart> parts = d.parts;
    std::sort(parts.begin(), parts.end(), [](const DrawingPart &a, const DrawingPart &b) {
        return a.stream != b.stream ? a.stream < b.stream : a.start > b.start;
    });
    std::lock_guard guard(docMutex_);
    return onPage(page, lines.empty() ? "delete drawing" : "convert to text", [&](pdf_page *pg) {
        const std::vector<pdf_obj *> streams = contentStreams(ctx_, pg->obj);
        size_t k = 0;
        while (k < parts.size()) {
            const int s = parts[k].stream;
            QByteArray data = streamData(ctx_, streams.at(size_t(s)));
            for (; k < parts.size() && parts[k].stream == s; ++k)
                data.remove(parts[k].start, parts[k].end - parts[k].start);
            setStreamData(ctx_, pdf_, streams.at(size_t(s)), data);
        }
        for (const Line &l : write)
            writeText(pg, page, l.at, l.text, l.font, l.size, d.color);
    }, error);
}
