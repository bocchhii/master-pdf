// Document's text wrapping round pictures (Word's Layout Options; the Python app's
// set_picture_layout, wrap_obstacles, wrap_band, flow_change, layout_frame's avoid).
//
// A picture's layout, and each paragraph laid out round pictures (where it was, as wide as it
// was), are kept in the page itself - /MPWrap in the page's dictionary - so they follow Undo
// and Redo and are saved with the PDF:
//     /MPWrap << /Pics [<< /X (the picture) /R [x0 y0 x1 y1] /M /square >> ...]
//                /Frames [<< /H [left right top bottom baseline pitch align] /S [spots...] >> ...] >>
// (rects in points from the page's top left, as shown).
// Text keeps WRAP_SIDE beside a picture and WRAP_TOP over and under it. A paragraph in the
// way is laid out again beside or under the pictures; the paragraphs under it pushed down as
// it grows; one no longer in any picture's way goes back to its place and width.
// (No C++ objects are made inside fz_try blocks.)

#include "core/Document.h"
#include "core/PageText.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr double kSide = 9.0, kTop = 4.0;

QRectF zoneOf(const QRectF &r)
{
    return r.adjusted(-kSide, -kTop, kSide, kTop);
}

struct Obstacle {
    QRectF rect;
    QString mode;
    std::vector<std::pair<double, double>> outline;  // (tight / through: where it's solid, row by row)
};

// where a line from top to bottom can go between x0 and x1 with the pictures in the way: the
// widest room left beside them - none: a picture text goes over and under (no room)
std::optional<std::pair<double, double>> room(const std::vector<Obstacle> &obstacles, double top, double bottom, double x0,
                                              double x1)
{
    std::vector<std::pair<double, double>> blocked;
    for (const Obstacle &o : obstacles) {
        const QRectF &r = o.rect;
        if (bottom <= r.top() - kTop || top >= r.bottom() + kTop || r.right() <= x0 || r.left() >= x1)
            continue;
        if (o.mode == QLatin1String("inline") || o.mode == QLatin1String("top_bottom"))
            return std::nullopt;
        double a = r.left(), b = r.right();
        if (!o.outline.empty() && (o.mode == QLatin1String("tight") || o.mode == QLatin1String("through"))) {
            const int n = int(o.outline.size());
            const int k0 = std::max(0, int((top - kTop - r.top()) / std::max(1e-6, r.height()) * n));
            const int k1 = std::min(n, int((bottom + kTop - r.top()) / std::max(1e-6, r.height()) * n) + 1);
            double lo = 2, hi = -1;
            for (int k = k0; k < k1; ++k)
                if (o.outline[size_t(k)].second > o.outline[size_t(k)].first) {
                    lo = std::min(lo, o.outline[size_t(k)].first);
                    hi = std::max(hi, o.outline[size_t(k)].second);
                }
            if (hi < lo)
                continue;  // (nothing solid there: the text goes through)
            a = r.left() + lo * r.width();
            b = r.left() + hi * r.width();
        }
        blocked.push_back({a - kSide, b + kSide});
    }
    std::sort(blocked.begin(), blocked.end());
    std::vector<std::pair<double, double>> free;
    double at = x0;
    for (auto [a, b] : blocked) {
        if (a > at)
            free.push_back({at, std::min(a, x1)});
        at = std::max(at, b);
    }
    if (at < x1)
        free.push_back({at, x1});
    std::optional<std::pair<double, double>> best;
    for (auto q : free)
        if (q.second > q.first && (!best || q.second - q.first > best->second - best->first))
            best = q;
    return best;
}

bool sameRect(const QRectF &a, const QRectF &b)
{
    return std::abs(a.left() - b.left()) < 1.5 && std::abs(a.top() - b.top()) < 1.5 && std::abs(a.right() - b.right()) < 1.5 &&
           std::abs(a.bottom() - b.bottom()) < 1.5;
}

enum Align { Left, Center, Right, Justify };

}  // namespace

bool Document::flowing(const QString &mode)
{
    return mode == QLatin1String("inline") || mode == QLatin1String("square") || mode == QLatin1String("tight") ||
           mode == QLatin1String("through") || mode == QLatin1String("top_bottom");
}

// ---- the state kept in the page ----

Document::WrapState Document::readWrap(pdf_obj *pageobj)
{
    // (docMutex_ held)
    WrapState s;
    pdf_obj *w = pdf_dict_gets(ctx_, pageobj, "MPWrap");
    if (!pdf_is_dict(ctx_, w))
        return s;
    auto rectOf = [&](pdf_obj *a, int from) {
        return QRectF(QPointF(pdf_array_get_real(ctx_, a, from), pdf_array_get_real(ctx_, a, from + 1)),
                      QPointF(pdf_array_get_real(ctx_, a, from + 2), pdf_array_get_real(ctx_, a, from + 3)));
    };
    pdf_obj *pics = pdf_dict_gets(ctx_, w, "Pics");
    for (int i = 0; i < pdf_array_len(ctx_, pics); ++i) {
        pdf_obj *e = pdf_array_get(ctx_, pics, i);
        WrapPic p;
        p.xref = pdf_to_num(ctx_, pdf_dict_gets(ctx_, e, "X"));  // (a reference: saving renumbers, and follows it)
        p.rect = rectOf(pdf_dict_get(ctx_, e, PDF_NAME(R)), 0);
        p.mode = QString::fromLatin1(pdf_to_name(ctx_, pdf_dict_get(ctx_, e, PDF_NAME(M))));
        s.pics.push_back(p);
    }
    pdf_obj *frames = pdf_dict_gets(ctx_, w, "Frames");
    for (int i = 0; i < pdf_array_len(ctx_, frames); ++i) {
        pdf_obj *e = pdf_array_get(ctx_, frames, i);
        pdf_obj *h = pdf_dict_get(ctx_, e, PDF_NAME(H));
        WrapFrame f;
        f.left = pdf_array_get_real(ctx_, h, 0);
        f.right = pdf_array_get_real(ctx_, h, 1);
        f.top = pdf_array_get_real(ctx_, h, 2);
        f.bottom = pdf_array_get_real(ctx_, h, 3);
        f.baseline = pdf_array_get_real(ctx_, h, 4);
        f.pitch = pdf_array_get_real(ctx_, h, 5);
        f.align = pdf_array_get_int(ctx_, h, 6);
        pdf_obj *spots = pdf_dict_get(ctx_, e, PDF_NAME(S));
        for (int k = 0; k + 3 < pdf_array_len(ctx_, spots); k += 4)
            f.spots.push_back(rectOf(spots, k));
        f.text = QString::fromUtf8(pdf_to_text_string(ctx_, pdf_dict_get(ctx_, e, PDF_NAME(T))));
        f.family = QString::fromUtf8(pdf_to_text_string(ctx_, pdf_dict_get(ctx_, e, PDF_NAME(F))));
        pdf_obj *look = pdf_dict_get(ctx_, e, PDF_NAME(L));
        f.size = pdf_array_get_real(ctx_, look, 0);
        f.color = QRgb(pdf_array_get_int(ctx_, look, 1));
        f.bold = pdf_array_get_int(ctx_, look, 2) != 0;
        f.italic = pdf_array_get_int(ctx_, look, 3) != 0;
        f.rtl = pdf_array_get_int(ctx_, look, 4) != 0;
        s.frames.push_back(f);
    }
    return s;
}

void Document::writeWrap(pdf_obj *pageobj, const WrapState &s)
{
    // (inside an operation)
    if (s.pics.empty() && s.frames.empty()) {
        pdf_dict_dels(ctx_, pageobj, "MPWrap");
        return;
    }
    pdf_obj *w = pdf_dict_puts_dict(ctx_, pageobj, "MPWrap", 2);
    auto putRect = [&](pdf_obj *arr, const QRectF &r) {
        pdf_array_push_real(ctx_, arr, r.left());
        pdf_array_push_real(ctx_, arr, r.top());
        pdf_array_push_real(ctx_, arr, r.right());
        pdf_array_push_real(ctx_, arr, r.bottom());
    };
    auto putsArray = [&](pdf_obj *dict, const char *name, int n) {
        pdf_obj *key = pdf_new_name(ctx_, name);
        pdf_obj *a = pdf_dict_put_array(ctx_, dict, key, n);
        pdf_drop_obj(ctx_, key);
        return a;
    };
    pdf_obj *pics = putsArray(w, "Pics", int(s.pics.size()));
    for (const WrapPic &p : s.pics) {
        pdf_obj *e = pdf_array_push_dict(ctx_, pics, 3);
        pdf_dict_puts_drop(ctx_, e, "X", pdf_new_indirect(ctx_, pdf_, p.xref, 0));
        putRect(pdf_dict_put_array(ctx_, e, PDF_NAME(R), 4), p.rect);
        const QByteArray m = p.mode.toLatin1();
        pdf_dict_put_drop(ctx_, e, PDF_NAME(M), pdf_new_name(ctx_, m.constData()));
    }
    pdf_obj *frames = putsArray(w, "Frames", int(s.frames.size()));
    for (const WrapFrame &f : s.frames) {
        pdf_obj *e = pdf_array_push_dict(ctx_, frames, 2);
        pdf_obj *h = pdf_dict_put_array(ctx_, e, PDF_NAME(H), 7);
        for (double v : {f.left, f.right, f.top, f.bottom, f.baseline, f.pitch})
            pdf_array_push_real(ctx_, h, v);
        pdf_array_push_int(ctx_, h, f.align);
        pdf_obj *spots = pdf_dict_put_array(ctx_, e, PDF_NAME(S), int(f.spots.size()) * 4);
        for (const QRectF &r : f.spots)
            putRect(spots, r);
        const QByteArray text = f.text.toUtf8(), family = f.family.toUtf8();
        pdf_dict_put_text_string(ctx_, e, PDF_NAME(T), text.constData());
        pdf_dict_put_text_string(ctx_, e, PDF_NAME(F), family.constData());
        pdf_obj *look = pdf_dict_put_array(ctx_, e, PDF_NAME(L), 5);
        pdf_array_push_real(ctx_, look, f.size);
        pdf_array_push_int(ctx_, look, int64_t(f.color & 0xFFFFFF));
        pdf_array_push_int(ctx_, look, f.bold);
        pdf_array_push_int(ctx_, look, f.italic);
        pdf_array_push_int(ctx_, look, f.rtl);
    }
}

Document::WrapState Document::wrapState(int page)
{
    std::lock_guard guard(docMutex_);
    WrapState s;
    if (!pdf_ || page < 0 || page >= int(lists_.size()))
        return s;
    pdf_obj *pageobj = nullptr;
    fz_try(ctx_) {
        pageobj = pdf_lookup_page_obj(ctx_, pdf_, page);
    }
    fz_catch(ctx_) {
        pageobj = nullptr;
    }
    if (pageobj)
        s = readWrap(pageobj);
    return s;
}

QString Document::pictureLayout(int page, const Picture &pic)
{
    if (pic.vector)  // (a drawing: over the page, as it is)
        return QStringLiteral("front");
    const QRectF full = pic.full.isNull() ? pic.rect : pic.full;
    for (const WrapPic &p : wrapState(page).pics)
        if (p.xref == pic.xref && sameRect(p.rect, full))
            return p.mode;
    return QStringLiteral("front");
}

// where a picture with see-through parts is solid, row by row from its top: (left, right) as
// parts of its width - none for a solid picture
std::vector<std::pair<double, double>> Document::solidRows(int xref)
{
    std::lock_guard guard(docMutex_);
    const QImage a = pictureAlpha(xref, 200);
    std::vector<std::pair<double, double>> out;
    if (a.isNull())
        return out;
    const QImage small = a.scaled(std::min(200, a.width()), std::max(1, std::min(120, a.height())), Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < small.height(); ++y) {
        const uchar *row = small.constScanLine(y);
        int first = -1, last = -1;
        for (int x = 0; x < small.width(); ++x)
            if (row[x] > 40) {
                if (first < 0)
                    first = x;
                last = x;
            }
        out.push_back(first < 0 ? std::make_pair(0.0, 0.0)
                                : std::make_pair(double(first) / small.width(), double(last + 1) / small.width()));
    }
    return out;
}

// ---- the plan: worked out before the change (outside the lock), written in it ----

Document::FlowPlan Document::planFlow(int page, const Picture *pic, const QRectF &newFull, const QString &mode)
{
    FlowPlan plan;
    plan.state = wrapState(page);
    const std::vector<Picture> pics = pictures(page);
    auto fullOf = [](const Picture &p) { return p.full.isNull() ? p.rect : p.full; };
    // the picture changed: its layout, where it goes
    if (pic) {
        const QRectF was = fullOf(*pic);
        auto it = std::find_if(plan.state.pics.begin(), plan.state.pics.end(),
                               [&](const WrapPic &p) { return p.xref == pic->xref && sameRect(p.rect, was); });
        if (it != plan.state.pics.end()) {
            it->rect = newFull;
            if (!mode.isEmpty())
                it->mode = mode;
            if (it->mode == QLatin1String("front"))
                plan.state.pics.erase(it);
        } else if (!mode.isEmpty() && mode != QLatin1String("front")) {
            plan.state.pics.push_back({pic->xref, newFull, mode});
        }
    }
    // the pictures text flows round, where they show (round their shape, if they have one)
    std::vector<Obstacle> obstacles;
    for (const WrapPic &w : plan.state.pics) {
        if (!flowing(w.mode))
            continue;
        Obstacle o{w.rect, w.mode, {}};
        const Picture *now = nullptr;
        for (const Picture &p : pics)
            if (p.xref == w.xref && (sameRect(fullOf(p), w.rect) || (pic && p.index == pic->index)))
                now = &p;
        if (now && !now->shape.empty()) {  // (its shape's box, moved and sized as the whole one)
            const QRectF f = fullOf(*now), s = now->rect;
            const double kx = w.rect.width() / std::max(1e-6, f.width()), ky = w.rect.height() / std::max(1e-6, f.height());
            o.rect = QRectF(w.rect.left() + (s.left() - f.left()) * kx, w.rect.top() + (s.top() - f.top()) * ky,
                            s.width() * kx, s.height() * ky);
        }
        if (w.mode == QLatin1String("tight") || w.mode == QLatin1String("through"))
            o.outline = solidRows(w.xref);
        obstacles.push_back(o);
    }
    // the paragraphs: each in the way of a picture, or laid out round one before, or pushed
    const std::vector<TextLine> lines = this->lines(page);
    std::vector<bool> seen(lines.size(), false);
    struct Entry {
        std::vector<int> lines;
        QRectF rect;
        int rec = -1;  // (its record in the state)
        WrapFrame home;
        bool inWay = false;
        double grow = 0;
    };
    std::vector<Entry> entries;
    // a paragraph laid out round a picture before: all the lines where it put them, as one (its
    // lines no longer line up - left of a picture, beside it, under it - so they're not found
    // as one paragraph by their edges)
    for (int r = 0; r < int(plan.state.frames.size()); ++r) {
        Entry e;
        e.rec = r;
        for (int i = 0; i < int(lines.size()); ++i)
            for (const QRectF &spot : plan.state.frames[size_t(r)].spots)
                if (!seen[size_t(i)] && spot.adjusted(1, 1, -1, -1).intersects(lines[size_t(i)].rect)) {
                    seen[size_t(i)] = true;
                    e.lines.push_back(i);
                    e.rect |= lines[size_t(i)].rect;
                }
        entries.push_back(e);  // (even with none of its lines on the page: they went past its bottom)
    }
    for (int i = 0; i < int(lines.size()); ++i) {
        if (seen[size_t(i)] || lines[size_t(i)].text.trimmed().isEmpty() || std::abs(lines[size_t(i)].angle) > 0.01)
            continue;  // (turned text isn't laid out round pictures)
        Entry e;
        for (int k : pagetext::paragraph(lines, i))
            if (!seen[size_t(k)])
                e.lines.push_back(k);
        if (e.lines.empty())
            e.lines = {i};
        for (int k : e.lines) {
            seen[size_t(k)] = true;
            e.rect |= lines[size_t(k)].rect;
        }
        entries.push_back(e);
    }
    for (Entry &e : entries) {
        for (const Obstacle &o : obstacles)
            e.inWay = e.inWay || zoneOf(o.rect).intersects(e.rect);
        if (e.rec >= 0) {
            e.home = plan.state.frames[size_t(e.rec)];
        } else {
            const TextLine &first = lines[size_t(e.lines.front())], &last = lines[size_t(e.lines.back())];
            e.home.left = e.rect.left();
            e.home.right = e.rect.right();
            e.home.top = e.rect.top();
            e.home.bottom = e.rect.bottom();
            e.home.baseline = first.baseline;
            e.home.pitch = e.lines.size() > 1 ? (last.baseline - first.baseline) / double(e.lines.size() - 1)
                                              : first.mainRun().size * 1.2;
            // how its lines lined up: justified, centred, right or left
            bool justified = e.lines.size() > 1, centered = true, rightAligned = true, leftAligned = true;
            for (size_t n = 0; n < e.lines.size(); ++n) {
                const QRectF &r = lines[size_t(e.lines[n])].rect;
                if (n + 1 < e.lines.size() && (std::abs(r.right() - e.rect.right()) > 2 || std::abs(r.left() - e.rect.left()) > 2))
                    justified = false;
                centered = centered && std::abs(r.center().x() - e.rect.center().x()) <= 2;
                rightAligned = rightAligned && std::abs(r.right() - e.rect.right()) <= 2;
                leftAligned = leftAligned && std::abs(r.left() - e.rect.left()) <= 2;
            }
            e.home.align = justified ? Justify
                           : (centered && !leftAligned && !rightAligned && e.lines.size() > 1) ? Center
                           : (first.rtl || (rightAligned && !leftAligned)) ? Right : Left;
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.home.top < b.home.top; });
    // (a record none of whose lines are seen, and that's out of every picture's way: as its text says)
    std::vector<WrapFrame> frames;  // (the records kept after it)
    std::vector<bool> recUsed(plan.state.frames.size(), false);
    std::vector<const Entry *> moved;
    for (Entry &e : entries) {
        double push = 0;
        for (const Entry *m : moved)
            if (std::min(e.home.right, m->home.right) - std::max(e.home.left, m->home.left) > 0 && m->home.top < e.home.top)
                push = std::max(push, m->grow);
        if (e.rec >= 0)
            recUsed[size_t(e.rec)] = true;
        if (e.rec < 0 && !e.inWay && push < 0.5)
            continue;  // (not in the way, nothing over it grew: as it is)
        const QRectF home(QPointF(e.home.left, e.home.top), QPointF(e.home.right, e.home.bottom));
        bool clear = true;  // (out of every picture's way now: back in its place, as wide as it was)
        for (const Obstacle &o : obstacles)
            clear = clear && !zoneOf(o.rect).intersects(home);
        // its words and look: kept with it once laid out (its lines may have gone past the page's
        // bottom), else its own now - its main run's
        FlowPlan::Frame out;
        for (int k : e.lines)
            out.remove.push_back(lines[size_t(k)].rect);
        if (e.rec < 0) {
            QStringList all;
            for (int k : e.lines)
                all << lines[size_t(k)].text;
            const TextLine &first = lines[size_t(e.lines.front())];
            const TextRun &main = first.mainRun();
            e.home.text = all.join(QLatin1Char(' '));
            const fonts::Found f = fontOf(main, LineLook{}, e.home.text);
            e.home.family = f.family;
            e.home.bold = f.bold;
            e.home.italic = f.italic;
            e.home.size = main.size;
            e.home.color = main.color;
            e.home.rtl = first.rtl;
        }
        QStringList words = e.home.text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        out.font = fonts::forText(fonts::findFamily(e.home.family, e.home.bold, e.home.italic), e.home.text);
        out.size = e.home.size;
        out.color = QColor::fromRgb(e.home.color);
        const bool rtl = e.home.rtl;
        // the lines, one at a time: where each goes tells how wide it can be
        const double base = out.size, leading = std::max(1.0, e.home.pitch), x0 = e.home.left, x1 = e.home.right;
        const double space = textWidth(QStringLiteral(" "), out.font, out.size);
        const double widest = std::max(10.0, x1 - x0);
        double y = e.home.baseline + push;
        int k = 0;
        double bottom = y;
        while (!words.isEmpty()) {
            if (k)
                y += leading;
            double lx0 = x0, lx1 = x1;
            if (!clear) {  // (beside the pictures text flows round - or under them)
                for (int tries = 0; tries < 2000; ++tries) {
                    const auto free = room(obstacles, y - 0.8 * base, y + 0.25 * base, x0, x1);
                    if (free && free->second - free->first >= std::min(widest, 4 * base)) {
                        lx0 = free->first;
                        lx1 = free->second;
                        break;
                    }
                    y += leading / 4;
                }
            }
            const double roomNow = std::max(10.0, lx1 - lx0);
            QStringList row;
            double used = 0;
            while (!words.isEmpty()) {
                const double ww = textWidth(words.front(), out.font, out.size);
                if (!row.isEmpty() && used + space + ww > roomNow + 0.01)
                    break;
                used += (row.isEmpty() ? 0 : space) + ww;
                row << words.takeFirst();
            }
            const bool last = words.isEmpty();
            if (e.home.align == Justify && !last && row.size() > 1 && !rtl) {  // (word by word, the gaps shared)
                double total = 0;
                for (const QString &w : row)
                    total += textWidth(w, out.font, out.size);
                const double gap = (roomNow - total) / double(row.size() - 1);
                double x = lx0;
                for (const QString &w : row) {
                    out.rows.push_back({w, QPointF(x, y)});
                    x += textWidth(w, out.font, out.size) + gap;
                }
            } else {
                const QString text = row.join(QLatin1Char(' '));
                const double w = textWidth(text, out.font, out.size);
                const double x = e.home.align == Center ? lx0 + (roomNow - w) / 2
                                 : (e.home.align == Right || (e.home.align == Justify && rtl)) ? lx1 - w : lx0;
                out.rows.push_back({text, QPointF(x, y)});
            }
            bottom = y + 0.25 * base;
            ++k;
        }
        e.grow = bottom - e.home.bottom;
        if (std::abs(e.grow) < 0.5 && clear)
            e.grow = 0;
        moved.push_back(&e);
        plan.frames.push_back(out);
        if (!clear || push >= 0.5) {  // (laid round a picture, or pushed down: kept, to come back as it was)
            WrapFrame f = e.home;
            f.spots.clear();
            for (const auto &r : out.rows)
                f.spots.push_back(QRectF(r.at.x(), r.at.y() - 0.8 * base,
                                         std::max(1.0, textWidth(r.text, out.font, out.size)), 1.05 * base));
            frames.push_back(f);
        }
        if (bottom > geometry()->sizes.at(size_t(page)).height())
            plan.pastBottom = true;
    }
    // records of paragraphs not seen now (gone): dropped
    plan.state.frames = frames;
    plan.any = true;
    return plan;
}

void Document::applyFlow(pdf_page *pg, int page, const FlowPlan &plan)
{
    // (inside an operation, docMutex_ held)
    if (!plan.any)
        return;
    for (const FlowPlan::Frame &f : plan.frames)
        for (const QRectF &r : f.remove)
            removeText(pg, page, r);
    for (const FlowPlan::Frame &f : plan.frames)
        for (const auto &r : f.rows)
            writeText(pg, page, r.at, r.text, f.font, f.size, f.color);
    writeWrap(pg->obj, plan.state);
}

bool Document::setPictureLayout(int page, const Picture &pic, const QString &mode, bool *pastBottom, QString *error)
{
    if (pic.vector) {
        if (error)
            *error = QStringLiteral("only pictures can be laid out with the text");
        return false;
    }
    const QRectF full = pic.full.isNull() ? pic.rect : pic.full;
    const FlowPlan plan = planFlow(page, &pic, full, mode);
    if (pastBottom)
        *pastBottom = plan.pastBottom;
    const bool behind = mode == QLatin1String("behind"), front = pictureLayout(page, pic) == QLatin1String("behind");
    std::lock_guard guard(docMutex_);
    return onPage(page, "picture layout", [&](pdf_page *pg) {
        if (behind)  // (behind the text: drawn first, under everything)
            layerIn(pg, pic, false);
        else if (front && mode == QLatin1String("front"))
            layerIn(pg, pic, true);
        applyFlow(pg, page, plan);
    }, error);
}
