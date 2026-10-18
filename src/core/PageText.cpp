#include "core/PageText.h"

#include <QChar>
#include <QImage>
#include <QList>
#include <QStringList>
#include <QRegularExpression>
#include <QTransform>

#include <algorithm>
#include <array>
#include <set>
#include <map>
#include <cmath>

namespace pagetext {

namespace {

bool isSpace(char32_t c) { return QChar::isSpace(c); }

QString str(char32_t c) { return QString::fromUcs4(&c, 1); }

QString nfkc(const QString &s) { return s.normalized(QString::NormalizationForm_KC); }

bool hasRtl(const QString &s)
{
    for (char32_t c : s.toUcs4())
        if (isRtl(c))
            return true;
    return false;
}

// Letters drawn as one joined shape kept as "presentation forms" (text laid out as HTML
// reads back: ﺗﻘﺭﻳﺭ) - as the letters they are (تقرير); "ﻻ" two of them, side by side
void plainLetters(std::vector<TextChar> &line)
{
    std::vector<TextChar> out;
    out.reserve(line.size());
    for (const TextChar &ch : line) {
        const char32_t c = ch.c;
        if ((c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) {
            const QList<uint> plain = nfkc(str(c)).toUcs4();
            bool arabic = false;
            for (uint p : plain)
                arabic = arabic || isArabic(p);
            if (plain.size() > 1 && arabic) {
                const double w = ch.box.width() / double(plain.size());
                for (qsizetype n = 0; n < plain.size(); ++n) {
                    TextChar one = ch;
                    one.c = char32_t(plain[n]);
                    one.box.setLeft(ch.box.right() - double(n + 1) * w);
                    one.box.setRight(ch.box.right() - double(n) * w);
                    out.push_back(one);
                }
                continue;
            }
            if (plain.size() == 1) {
                TextChar one = ch;
                one.c = char32_t(plain[0]);
                out.push_back(one);
                continue;
            }
        }
        out.push_back(ch);
    }
    line = std::move(out);
}

// Letters drawn as one joined shape (lam-alif "لأ", "الله") are listed backwards: the
// shape's first letters have no width, its last carries it all - turned round
void joinedShapes(std::vector<TextChar> &line, const std::vector<double> &advances)
{
    std::vector<TextChar> out;
    out.reserve(line.size());
    size_t k = 0;
    while (k < line.size()) {
        size_t j = k;
        while (j < line.size() && advances[j] < 0.01 && isArabic(line[j].c))
            ++j;
        if (j > k && j < line.size() && isArabic(line[j].c)) {
            for (size_t n = j + 1; n-- > k;)
                out.push_back(line[n]);
            k = j + 1;
        } else {
            out.push_back(line[k++]);
        }
    }
    line = std::move(out);
}

// A full stop, comma... ending a line is listed first (drawn at the line's far left): put
// at the end. A sign or space listed first but drawn inside the line goes where it's drawn:
// before the first letter (not a space) found left of it - right to left, that's the one it
// comes before.
void leadingSigns(std::vector<TextChar> &line)
{
    size_t first = 0;
    while (first < line.size() && !isArabic(line[first].c))
        ++first;
    if (first == line.size())
        first = 0;
    if (first == 0)
        return;
    for (size_t n = 0; n < first; ++n)
        if (QChar::isLetterOrNumber(line[n].c))
            return;
    std::vector<size_t> rest;  // (the letters after them that aren't spaces)
    for (size_t n = first; n < line.size(); ++n)
        if (!isSpace(line[n].c))
            rest.push_back(n);
    std::vector<std::vector<TextChar>> before(line.size());
    std::vector<TextChar> atEnd;
    for (size_t n = 0; n < first; ++n) {
        const TextChar &ch = line[n];
        auto to = std::find_if(rest.begin(), rest.end(), [&](size_t r) {
            return line[r].box.right() <= ch.box.left() + 0.5;
        });
        if (to == rest.end())
            atEnd.push_back(ch);
        else
            before[*to].push_back(ch);
    }
    std::vector<TextChar> out;
    out.reserve(line.size());
    for (size_t n = first; n < line.size(); ++n) {
        out.insert(out.end(), before[n].begin(), before[n].end());
        out.push_back(line[n]);
    }
    size_t end = out.size();  // (before the spaces the line ends with; read right to left)
    while (end > 0 && isSpace(out[end - 1].c))
        --end;
    out.insert(out.begin() + std::ptrdiff_t(end), atEnd.rbegin(), atEnd.rend());
    line = std::move(out);
}

// The spaces a PDF leaves out: many write each word on its own, spaced by position with no
// space letter between - a gap clearly wider than the line's usual gap between letters gets
// a space put in, as if it had been written
void gapSpaces(std::vector<TextChar> &line)
{
    std::vector<double> gaps;
    for (size_t n = 1; n < line.size(); ++n)
        if (!isSpace(line[n - 1].c) && !isSpace(line[n].c))
            gaps.push_back(line[n].box.left() - line[n - 1].box.right());
    if (gaps.empty())
        return;
    std::sort(gaps.begin(), gaps.end());
    const double usual = gaps[gaps.size() / 2];
    std::vector<TextChar> out;
    out.reserve(line.size() + 8);
    for (size_t n = 0; n < line.size(); ++n) {
        if (n > 0 && !isSpace(line[n - 1].c) && !isSpace(line[n].c)) {
            const TextChar &prev = line[n - 1];
            const TextChar &ch = line[n];
            const double gap = ch.box.left() - prev.box.right();
            const double size = ch.size > 0 ? ch.size : 10;
            // (at least an eighth of its size: a word's letters spaced out - its letter spacing set, 1/10 of
            // its size - aren't words apart; on a line of usual letters they read "F i r s t")
            if (gap > std::max(0.12 * size, usual + 0.06 * size) && gap < 3 * size) {
                TextChar space = ch;
                space.c = U' ';
                space.box = QRectF(QPointF(prev.box.right(), ch.box.top()),
                                   QPointF(ch.box.left(), ch.box.bottom()));
                out.push_back(space);
            }
        }
        out.push_back(line[n]);
    }
    line = std::move(out);
}

QString textOf(const std::vector<TextChar> &chars, size_t lo, size_t hi)
{
    QString s;
    for (size_t n = lo; n < hi; ++n)
        s += str(chars[n].c);
    return nfkc(s);
}

}  // namespace

bool isRtl(char32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

bool isArabic(char32_t c)
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0x08A0 && c <= 0x08FF) || (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

Bidi bidiType(char32_t c)
{
    if ((c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9) || c == 0x66B || c == 0x66C)
        return c == 0x66B || c == 0x66C ? Bidi::CS : Bidi::AN;
    if (c >= U'0' && c <= U'9')
        return Bidi::EN;
    if (c == U'/' || c == U'.' || c == U',' || c == U':' || c == U'-' || c == U'+')
        return Bidi::CS;
    if (isRtl(c))
        return QChar::isMark(c) ? Bidi::NI : Bidi::R;
    if (QChar::isLetter(c) || QChar::isDigit(c))
        return Bidi::L;
    return Bidi::NI;
}

std::vector<int> bidiLevels(std::vector<Bidi> t, bool rtl)
{
    const size_t n = t.size();
    // (a sign between two numbers of a kind: theirs - "١٤٤٧/٤/٢١", "3.5", "12-3")
    for (size_t k = 1; k + 1 < n; ++k)
        if (t[k] == Bidi::CS && t[k - 1] == t[k + 1] && (t[k - 1] == Bidi::AN || t[k - 1] == Bidi::EN))
            t[k] = t[k - 1];
    for (Bidi &b : t)
        if (b == Bidi::CS)
            b = Bidi::NI;
    // (0-9 after a Latin word: its - "Room 12")
    for (size_t k = 0, last = 0; k < n; ++k) {
        if (t[k] == Bidi::L || t[k] == Bidi::R)
            last = t[k] == Bidi::L ? 1 : 2;
        else if (t[k] == Bidi::EN && (last == 1 || (last == 0 && !rtl)))
            t[k] = Bidi::L;
    }
    // (the signs, spaces between: their neighbours' way if both sides go it - numbers as right-to-left letters - else the line's)
    auto way = [](Bidi b) { return b == Bidi::L ? 0 : 1; };
    std::vector<int> level(n);
    for (size_t k = 0; k < n; ++k) {
        if (t[k] != Bidi::NI)
            continue;
        int before = rtl ? 1 : 0, after = rtl ? 1 : 0;
        for (size_t j = k; j-- > 0;)
            if (t[j] != Bidi::NI) {
                before = way(t[j]);
                break;
            }
        for (size_t j = k + 1; j < n; ++j)
            if (t[j] != Bidi::NI) {
                after = way(t[j]);
                break;
            }
        level[k] = before == after ? before : (rtl ? 1 : 0);
    }
    for (size_t k = 0; k < n; ++k)
        if (t[k] == Bidi::L)
            level[k] = rtl ? 2 : 0;
        else if (t[k] == Bidi::R)
            level[k] = 1;
        else if (t[k] == Bidi::AN || t[k] == Bidi::EN)
            level[k] = 2;
        else if (rtl && level[k] == 0)  // (a sign between two Latin words in a right-to-left line: theirs)
            level[k] = 2;
    return level;
}

std::vector<size_t> bidiOrder(const std::vector<int> &levels)
{
    std::vector<size_t> order(levels.size());
    for (size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    const int top = levels.empty() ? 0 : *std::max_element(levels.begin(), levels.end());
    for (int lev = top; lev >= 1; --lev)  // (each run at this level or above turned round, the highest first)
        for (size_t k = 0; k < order.size();) {
            if (levels[order[k]] < lev) {
                ++k;
                continue;
            }
            size_t e = k;
            while (e < order.size() && levels[order[e]] >= lev)
                ++e;
            std::reverse(order.begin() + long(k), order.begin() + long(e));
            k = e;
        }
    return order;
}

namespace {

// Arabic letters lifted off their line - a font that joins its letters end to end (Calibri's, Sakkal Majalla's cursive
// attachment) draws some higher or lower, and a glyph of two letters ("في") is read with no width: MuPDF reads them as
// lines of their own, and the rest of their line in pieces. Each put back in the line it's on (of its font and size,
// within 0.6 of its size up or down, beside it or across it), that line's letters then in order by where they are: right
// to left, a number or a Latin word one piece left to right inside - and a space MuPDF put where a lifted letter was left out
void liftedBack(std::vector<std::vector<TextChar>> &lines)
{
    struct Info {
        double base = 0, x0 = 1e9, x1 = -1e9, size = 0;
        QString font;
        int arabic = 0, latin = 0, n = 0;
        bool ok = false;
    };
    auto median = [](std::vector<double> v) {
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    std::vector<Info> info(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty() || !lines[i].front().straight)
            continue;
        Info &f = info[i];
        std::vector<double> ys, ss;
        QHash<QString, int> fonts;
        for (const TextChar &ch : lines[i]) {
            if (QChar::isSpace(ch.c))
                continue;
            ys.push_back(ch.origin.y());
            ss.push_back(ch.size);
            f.x0 = std::min({f.x0, ch.box.left(), ch.origin.x()});
            f.x1 = std::max({f.x1, ch.box.right(), ch.origin.x()});
            ++fonts[ch.font];
            f.arabic += isArabic(ch.c) ? 1 : 0;
            f.latin += ch.c < 0x250 && QChar::isLetter(ch.c) ? 1 : 0;
            ++f.n;
        }
        if (ys.empty())
            continue;
        f.base = median(ys);
        f.size = std::max(1.0, median(ss));
        int most = 0;
        for (auto it = fonts.begin(); it != fonts.end(); ++it)
            if (it.value() > most)
                most = it.value(), f.font = it.key();
        f.ok = true;
    }
    std::vector<int> into(lines.size(), -1);
    for (size_t i = 0; i < lines.size(); ++i) {
        const Info &a = info[i];
        if (!a.ok || a.arabic == 0 || a.latin > 0)
            continue;
        int best = -1;
        double bestDy = 1e9;
        for (size_t j = 0; j < lines.size(); ++j) {
            const Info &b = info[j];
            if (j == i || !b.ok || b.arabic == 0 || b.font != a.font || std::abs(b.size - a.size) > 0.15 * b.size)
                continue;
            if (b.n < a.n || (b.n == a.n && j > i))  // (into the longer one - of two alike, the first)
                continue;
            const double dy = std::abs(a.base - b.base);
            if (dy > 0.6 * b.size || a.x1 < b.x0 - 1.5 * b.size || a.x0 > b.x1 + 1.5 * b.size)
                continue;
            if (dy < bestDy)
                bestDy = dy, best = int(j);
        }
        into[i] = best;
    }
    std::vector<std::vector<size_t>> gets(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (into[i] < 0)
            continue;
        size_t host = size_t(into[i]);
        for (int hops = 0; into[host] >= 0 && hops < 64; ++hops)
            host = size_t(into[host]);
        gets[host].push_back(i);
    }
    // (where a right-to-left letter starts: its right edge - a letter kerned under the one before it starts after it still)
    auto start = [](const TextChar &ch) { return std::max(ch.box.right(), ch.origin.x() + ch.advance); };
    for (size_t h = 0; h < lines.size(); ++h) {
        if (gets[h].empty())
            continue;
        std::vector<TextChar> &line = lines[h];
        const TextChar like = line.front();
        std::vector<double> moved;  // (where the letters put back are)
        std::vector<TextChar> back;  // (the letters put back: right to left, by where they start - a piece MuPDF read
                                     // the wrong way round in order again)
        std::vector<TextChar> marks;  // (a mark - a tanween, a fatha - read apart: after the letter it's over)
        for (size_t i : gets[h]) {
            for (TextChar ch : lines[i]) {
                const QChar::Category cat = QChar::category(ch.c);
                const bool mark = cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_Enclosing;
                if (!mark && (std::abs(ch.origin.y() - info[h].base) > 0.05 * info[h].size || ch.advance < 0.05f * ch.size))
                    moved.push_back(ch.origin.x());
                ch.line = like.line;
                ch.block = like.block;
                ch.origin.setY(info[h].base);
                ch.box.setTop(like.box.top());
                ch.box.setBottom(like.box.bottom());
                (mark ? marks : back).push_back(ch);
            }
            lines[i].clear();
        }
        std::stable_sort(back.begin(), back.end(), [&](const TextChar &a, const TextChar &b) { return start(a) > start(b) + 0.01; });
        // (a number, a Latin word in them: left to right inside)
        auto across = [](char32_t c) {
            return (c >= U'0' && c <= U'9') || (c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9) || (c < 0x250 && QChar::isLetter(c));
        };
        for (size_t a = 0; a < back.size();) {
            size_t b = a;
            while (b < back.size() && across(back[b].c))
                ++b;
            std::reverse(back.begin() + long(a), back.begin() + long(b));
            a = b > a ? b : a + 1;
        }
        // (the line's own letters in the order they were read; each put back before the first that starts left of it)
        std::vector<TextChar> merged;
        size_t at = 0;
        for (const TextChar &ch : line) {
            while (at < back.size() && start(back[at]) > start(ch) + 0.01)
                merged.push_back(back[at++]);
            merged.push_back(ch);
        }
        merged.insert(merged.end(), back.begin() + long(at), back.end());
        // (a space MuPDF put where a letter was taken off: none - and two spaces one)
        line.clear();
        for (const TextChar &ch : merged) {
            bool over = false;
            if (QChar::isSpace(ch.c))
                for (double x : moved)
                    over = over || (x > ch.box.left() + 0.01 && x < ch.box.right() + 0.3);
            if (over || (QChar::isSpace(ch.c) && (line.empty() || QChar::isSpace(line.back().c))))
                continue;
            line.push_back(ch);
        }
        while (!line.empty() && QChar::isSpace(line.back().c))
            line.pop_back();
        for (const TextChar &m : marks) {  // (each after the letter under its middle - the nearest - and its marks)
            const double mid = m.box.center().x();
            size_t best = line.size();
            double near = 1e9;
            for (size_t n = 0; n < line.size(); ++n) {
                if (QChar::isSpace(line[n].c))
                    continue;
                const double d = mid < line[n].box.left() ? line[n].box.left() - mid : mid > line[n].box.right() ? mid - line[n].box.right() : 0;
                if (d < near)
                    near = d, best = n;
            }
            size_t at = best < line.size() ? best + 1 : line.size();
            while (at < line.size()) {
                const QChar::Category cat = QChar::category(line[at].c);
                if (cat != QChar::Mark_NonSpacing && cat != QChar::Mark_SpacingCombining && cat != QChar::Mark_Enclosing)
                    break;
                ++at;
            }
            line.insert(line.begin() + long(at), m);
        }
    }
}

}  // namespace

std::vector<TextChar> mend(std::vector<std::vector<TextChar>> lines)
{
    liftedBack(lines);
    std::vector<TextChar> out;
    for (auto &line : lines) {
        if (line.empty())
            continue;
        const bool straight = line.front().straight;
        if (std::any_of(line.begin(), line.end(), [](const TextChar &ch) { return isArabic(ch.c); })) {
            plainLetters(line);
            std::vector<double> advances;  // (a letter's width along its line)
            advances.reserve(line.size());
            for (const TextChar &ch : line)
                advances.push_back(ch.advance);
            joinedShapes(line, advances);
            if (straight)
                leadingSigns(line);
        }
        if (straight)
            gapSpaces(line);
        out.insert(out.end(), line.begin(), line.end());
    }
    return out;
}

std::vector<FindHit> find(const std::vector<TextChar> &chars, int page, const QString &query)
{
    // (Arabic's tatweel - a word drawn stretched, "المملكـة" - not counted: found as "المملكة")
    const QString key = nfkc(query).toCaseFolded().remove(QChar(0x0640)).simplified();
    std::vector<FindHit> found;
    if (key.isEmpty())
        return found;
    size_t start = 0;
    while (start < chars.size()) {
        size_t end = start;
        while (end < chars.size() && chars[end].line == chars[start].line)
            ++end;
        // the line read out normalized, and which letter each piece of it came from
        QString flat;
        std::vector<size_t> where;
        for (size_t n = start; n < end; ++n) {
            if (chars[n].c == 0x0640)
                continue;
            QString piece = nfkc(str(chars[n].c)).toCaseFolded();
            if (piece.isEmpty() || piece.trimmed().isEmpty()) {
                if (!flat.isEmpty() && flat.back() == QLatin1Char(' '))
                    continue;
                piece = QStringLiteral(" ");
            }
            for (QChar q : piece) {
                flat += q;
                where.push_back(n);
            }
        }
        qsizetype at = flat.indexOf(key);
        while (at >= 0) {
            const size_t a = where[size_t(at)];
            const size_t b = where[size_t(at + key.size() - 1)];
            FindHit hit;
            hit.page = page;
            hit.rect = chars[a].box;
            for (size_t n = a; n <= b; ++n)
                hit.rect |= chars[n].box;
            hit.before = textOf(chars, start, a);
            hit.words = textOf(chars, a, b + 1);
            hit.after = textOf(chars, b + 1, end);
            if (hit.before.size() > 40)
                hit.before = QStringLiteral("...") + hit.before.right(37).trimmed();
            if (hit.after.size() > 70)
                hit.after = hit.after.left(67).trimmed() + QStringLiteral("...");
            found.push_back(hit);
            at = flat.indexOf(key, at + std::max<qsizetype>(1, key.size()));
        }
        start = end;
    }
    // as you'd read them: line by line from the top (results on one line: their middles
    // within half a line of each other) - on a line left to right, or right to left
    std::stable_sort(found.begin(), found.end(), [](const FindHit &x, const FindHit &y) {
        return x.rect.center().y() < y.rect.center().y();
    });
    std::vector<FindHit> out;
    out.reserve(found.size());
    size_t r = 0;
    while (r < found.size()) {
        const double mid = found[r].rect.center().y();
        size_t e = r + 1;
        while (e < found.size() &&
               std::abs(found[e].rect.center().y() - mid) < 0.5 * std::max(found[e].rect.height(), 1.0))
            ++e;
        bool rtl = false;
        for (size_t n = r; n < e; ++n)
            rtl = rtl || hasRtl(found[n].before + found[n].words + found[n].after);
        std::stable_sort(found.begin() + std::ptrdiff_t(r), found.begin() + std::ptrdiff_t(e),
                         [rtl](const FindHit &x, const FindHit &y) {
                             return rtl ? x.rect.right() > y.rect.right() : x.rect.left() < y.rect.left();
                         });
        out.insert(out.end(), found.begin() + std::ptrdiff_t(r), found.begin() + std::ptrdiff_t(e));
        r = e;
    }
    return out;
}

int caret(const std::vector<TextChar> &chars, QPointF p, double near)
{
    int best = -1;
    double dist = 1e18;
    for (size_t k = 0; k < chars.size(); ++k) {
        // (by the letters: a space has no ink, and Word can put an Arabic line's last space
        // at its right end, where the line starts)
        if (isSpace(chars[k].c))
            continue;
        const QRectF &b = chars[k].box;
        const double dx = std::max({b.left() - p.x(), 0.0, p.x() - b.right()});
        const double dy = std::max({b.top() - p.y(), 0.0, p.y() - b.bottom()});
        const double d = dx * dx + 9 * dy * dy;  // (away from a line counts more than along it)
        if (d < dist) {
            best = int(k);
            dist = d;
        }
    }
    if (best < 0 || (near >= 0 && dist > near * near))
        return -1;
    const TextChar &ch = chars[size_t(best)];
    const bool rightHalf = p.x() > ch.box.center().x();
    if (isRtl(ch.c))  // (right to left: its right half is before it)
        return rightHalf ? best : best + 1;
    return rightHalf ? best + 1 : best;
}

TextPick letters(const std::vector<TextChar> &chars, int lo, int hi)
{
    TextPick out;
    QStringList lines;
    std::vector<int> keys;
    std::vector<QRectF> boxes;
    for (int n = std::max(0, lo); n < std::min(hi, int(chars.size())); ++n) {
        const TextChar &ch = chars[size_t(n)];
        QString c = str(ch.c);
        if (ch.c >= 0xFB00 && ch.c <= 0xFB06)  // (a ligature, "fi": its letters)
            c = nfkc(c);
        if (keys.empty() || keys.back() != ch.line) {
            keys.push_back(ch.line);
            lines.append(QString());
            boxes.emplace_back();
        }
        lines.back() += c;
        if (!isSpace(ch.c))
            boxes.back() = boxes.back().isNull() ? ch.box : (boxes.back() | ch.box);
    }
    for (const QRectF &r : boxes)
        if (!r.isNull())
            out.boxes.push_back(r);
    QStringList kept;
    for (const QString &line : lines)
        if (!line.trimmed().isEmpty())
            kept.append(line.trimmed());
    out.text = kept.join(QLatin1Char('\n'));
    return out;
}

TextPick pick(const std::vector<TextChar> &chars, QPointF a, QPointF b)
{
    const int ka = caret(chars, a, 15);
    const int kb = caret(chars, b);
    if (ka < 0 || kb < 0 || ka == kb)
        return {};
    return letters(chars, std::min(ka, kb), std::max(ka, kb));
}

int charAt(const std::vector<TextChar> &chars, QPointF p)
{
    for (size_t n = 0; n < chars.size(); ++n)
        if (!isSpace(chars[n].c) && chars[n].box.contains(p))
            return int(n);
    return -1;
}

TextPick around(const std::vector<TextChar> &chars, QPointF p, bool line)
{
    const int k = charAt(chars, p);
    if (k < 0)
        return {};
    const int key = chars[size_t(k)].line;
    auto same = [&](int n) {
        return chars[size_t(n)].line == key && (line || !isSpace(chars[size_t(n)].c));
    };
    int lo = k, hi = k + 1;
    while (lo > 0 && same(lo - 1))
        --lo;
    while (hi < int(chars.size()) && same(hi))
        ++hi;
    return letters(chars, lo, hi);
}

}  // namespace pagetext

// ---- the lines the Edit tool can retype ----

QPolygonF TextLine::shown() const
{
    QPolygonF corners({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});
    if (std::abs(angle) < 0.01)
        return corners;
    QTransform t;
    t.translate(pivot.x(), pivot.y());
    t.rotate(angle);
    t.translate(-pivot.x(), -pivot.y());
    return t.map(corners);
}

const TextRun &TextLine::mainRun() const
{
    size_t best = 0;
    int most = -1;
    for (size_t k = 0; k < runs.size(); ++k) {
        int n = 0;
        for (QChar c : runs[k].text)
            n += c.isSpace() ? 0 : 1;
        if (n > most) {
            most = n;
            best = k;
        }
    }
    return runs[best];
}

namespace pagetext {

namespace {

// pieces of one line read back as several (MuPDF does that with Arabic: a bold label, the English
// in it, the rest), on one baseline, side by side: one line again - its pieces in reading order,
// from their right edges for right to left (the Python app's same_baseline / visual_order). Not
// across a gap far wider than a space (a tab, a table's columns: kept apart, as lines() cut them)
// a spacing accent (a PDF's "g" + "˘" drawn apart) -> its combining mark, or 0
char16_t combiningOf(QChar c)
{
    switch (c.unicode()) {
    case 0x02D8: return 0x0306;  // ˘
    case 0x02C6: return 0x0302;  // ˆ
    case 0x02DC: return 0x0303;  // ˜
    case 0x00A8: return 0x0308;  // ¨
    case 0x00B4: return 0x0301;  // ´
    case 0x0060: return 0x0300;  // `
    case 0x02D9: return 0x0307;  // ˙
    case 0x02DA: return 0x030A;  // ˚
    case 0x02DB: return 0x0328;  // ˛
    case 0x02DD: return 0x030B;  // ˝
    case 0x02C7: return 0x030C;  // ˇ
    case 0x00B8: return 0x0327;  // ¸
    case 0x00AF: return 0x0304;  // ¯
    default: return 0;
    }
}

std::vector<TextLine> joinPieces(std::vector<TextLine> lines)
{
    auto size = [](const TextLine &l) {
        double s = 0;
        for (const TextRun &r : l.runs)
            s = std::max(s, r.size);
        return s > 0 ? s : 10;
    };
    bool joined = true;
    while (joined) {
        joined = false;
        for (size_t a = 0; a < lines.size() && !joined; ++a)
            for (size_t b = 0; b < lines.size() && !joined; ++b) {
                if (a == b)
                    continue;
                TextLine &x = lines[a];
                const TextLine &y = lines[b];
                const double s = std::max(size(x), size(y));
                if (std::abs(x.baseline - y.baseline) > 0.15 * s || std::abs(x.angle - y.angle) > 0.01 ||
                    (std::abs(x.angle) > 0.01 && QLineF(x.pivot, y.pivot).length() > 0.5) || std::abs(size(x) - size(y)) > 0.3 * s)
                    continue;  // (turned pieces: only round one point - their straight boxes side by side)
                // y just after x, reading the way the line reads: right to left - y to x's left
                const bool rtl = x.rtl || y.rtl;
                const double gap = rtl ? x.rect.left() - y.rect.right() : y.rect.left() - x.rect.right();
                // (x ends in an accent drawn on its own over y's first letter - "Elazı ˘" + "g, Turkey": one
                // line, the letter made whole: "Elazığ, Turkey")
                const QString xt = x.text.trimmed();
                const char16_t mark = !rtl && !xt.isEmpty() && !x.runs.empty() && !y.runs.empty() && !y.text.isEmpty() && y.text[0].isLetter()
                                          ? combiningOf(xt.back()) : 0;
                if (mark && gap >= -1.0 * s && gap <= 0.3 * s) {
                    TextLine m = x;
                    TextLine yy = y;
                    QString &last = m.runs.back().text;  // (the accent - and a space before it - off x's end)
                    while (!last.isEmpty() && last.back().isSpace())
                        last.chop(1);
                    if (!last.isEmpty() && combiningOf(last.back()))
                        last.chop(1);
                    while (!last.isEmpty() && last.back().isSpace())
                        last.chop(1);
                    if (last.isEmpty())
                        m.runs.pop_back();
                    QString &first = yy.runs.front().text;
                    const int at = int(first.indexOf(y.text[0]));
                    if (at >= 0)
                        first.replace(at, 1, (QString(first[at]) + QChar(mark)).normalized(QString::NormalizationForm_C));
                    m.text = QString();
                    for (const TextRun &r : m.runs)
                        m.text += r.text;
                    const QString yText = (QString(y.text[0]) + QChar(mark)).normalized(QString::NormalizationForm_C) + y.text.mid(1);
                    m.rect = x.rect | y.rect;
                    m.runs.insert(m.runs.end(), yy.runs.begin(), yy.runs.end());
                    m.text += yText;
                    m.quads.insert(m.quads.end(), y.quads.begin(), y.quads.end());
                    m.block = std::min(x.block, y.block);
                    lines[a] = m;
                    lines.erase(lines.begin() + long(b));
                    joined = true;
                    continue;
                }
                // (right to left: a letter tucked under the one before it - Arabic's kerning of "ز" + "ا" in Simplified
                // Arabic - its piece overlaps more)
                if (gap < (rtl ? -0.45 : -0.2) * s || gap > 1.3 * s)
                    continue;
                // (one of them mostly English inside right-to-left text: still the line's direction)
                const int ax = int(x.text.size()), ay = int(y.text.size());
                TextLine m = x;
                m.rect = x.rect | y.rect;
                const bool space = gap > 0.12 * s && !x.text.endsWith(QLatin1Char(' ')) && !y.text.startsWith(QLatin1Char(' '));
                if (space && !m.runs.empty())
                    m.runs.back().text += QLatin1Char(' ');
                m.runs.insert(m.runs.end(), y.runs.begin(), y.runs.end());
                m.text = x.text + (space ? QStringLiteral(" ") : QString()) + y.text;
                m.quads.insert(m.quads.end(), y.quads.begin(), y.quads.end());
                m.rtl = (x.rtl ? ax : 0) + (y.rtl ? ay : 0) >= (x.rtl ? 0 : ax) + (y.rtl ? 0 : ay);
                m.block = std::min(x.block, y.block);
                m.baseline = x.baseline;
                lines[a] = m;
                lines.erase(lines.begin() + long(b));
                joined = true;
            }
    }
    return lines;
}

}  // namespace

std::vector<TextLine> lines(const std::vector<TextChar> &chars)
{
    std::vector<TextLine> out;
    size_t start = 0;
    while (start < chars.size()) {
        size_t end = start;
        while (end < chars.size() && chars[end].line == chars[start].line)
            ++end;
        // (text going straight across - or turned text, read straightened)
        const bool turned = !chars[start].straight;
        if (turned && chars[start].quad.isEmpty()) {
            start = end;
            continue;
        }
        auto boxOf = [turned](const TextChar &c) { return turned ? c.sbox : c.box; };
        // cut where there's a gap far wider than a space (over 1.3 times the text's size)
        std::vector<std::pair<size_t, size_t>> parts;
        size_t partStart = start;
        // (a line with numbers, put in reading order when it was read - right to left with a number left to right in it, a
        // left-to-right one with Arabic's numbers right to left: its letters jump across each other in that order. Its
        // pieces: the stretches of it with nothing across between them - each of its letters in one)
        bool digits = false;
        for (size_t n = start; n < end && !digits; ++n)
            digits = !turned && ((chars[n].c >= U'0' && chars[n].c <= U'9') || (chars[n].c >= 0x660 && chars[n].c <= 0x669));
        std::vector<int> stretch(end - start, -1);
        if (digits) {
            std::vector<size_t> byX;
            for (size_t n = start; n < end; ++n)
                if (!isSpace(chars[n].c))
                    byX.push_back(n);
            std::sort(byX.begin(), byX.end(), [&](size_t a, size_t b) { return boxOf(chars[a]).left() < boxOf(chars[b]).left(); });
            int k = 0;
            double reach = -1e9;
            for (size_t i = 0; i < byX.size(); ++i) {
                const QRectF b = boxOf(chars[byX[i]]);
                if (i > 0 && b.left() - reach > 1.3 * std::max(1.0f, chars[byX[i]].size))
                    ++k;
                reach = std::max(reach, b.right());
                stretch[byX[i] - start] = k;
            }
        }
        QRectF last;
        bool any = false;
        int lastStretch = -1;
        for (size_t n = start; n < end; ++n) {
            const TextChar &c = chars[n];
            if (isSpace(c.c))
                continue;
            // (either way: right to left, the next letter is to the left)
            const QRectF b = boxOf(c);
            const double gap = std::max(b.left() - last.right(), last.left() - b.right());
            const bool cut = digits ? stretch[n - start] != lastStretch : gap > 1.3 * std::max(1.0f, c.size);
            if (any && cut) {
                parts.emplace_back(partStart, n);
                partStart = n;
            }
            last = b;
            lastStretch = stretch[n - start];
            any = true;
        }
        parts.emplace_back(partStart, end);
        for (const auto &[a, b] : parts) {
            size_t lo = a, hi = b;  // (the spaces at its ends left out)
            while (lo < hi && isSpace(chars[lo].c))
                ++lo;
            while (hi > lo && isSpace(chars[hi - 1].c))
                --hi;
            if (lo >= hi)
                continue;
            TextLine line;
            line.block = chars[lo].block;
            if (turned) {
                line.angle = chars[lo].angle;
                line.pivot = chars[lo].pivot;
            }
            int arabic = 0, latin = 0;
            // each word's letter spacing (1/1000 of its size: the room from a letter's end to the next
            // one's pen) - words spaced their own way (character spacing set on a few) are runs of their
            // own, so it's read back; a word too short to tell: as the one before it
            std::vector<double> wordTrack(hi - lo, 0);
            if (!turned) {
                double was = 0;
                for (size_t n = lo; n < hi;) {
                    if (isSpace(chars[n].c)) {
                        wordTrack[n - lo] = was;
                        ++n;
                        continue;
                    }
                    size_t e = n;
                    while (e < hi && !isSpace(chars[e].c))
                        ++e;
                    std::vector<double> g;
                    for (size_t m = n; m + 1 < e; ++m) {
                        const TextChar &a = chars[m], &b = chars[m + 1];
                        if (a.font == b.font && std::abs(a.size - b.size) < 0.05 && b.origin.x() > a.origin.x())
                            g.push_back((b.origin.x() - a.origin.x() - a.advance) / std::max(0.1f, a.size) * 1000);
                    }
                    if (g.size() >= 3) {
                        std::sort(g.begin(), g.end());
                        was = std::abs(g[g.size() / 2]) < 8 ? 0 : g[g.size() / 2];
                    }
                    for (size_t m = n; m < e; ++m)
                        wordTrack[m - lo] = was;
                    n = e;
                }
            }
            double runTrack = 0;
            std::vector<size_t> runStarts;  // (where each run's letters start)
            for (size_t n = lo; n < hi; ++n) {
                const TextChar &c = chars[n];
                char32_t ch = c.c == 0xA0 || c.c == 0xFFFD ? U' ' : c.c;
                const bool space = isSpace(ch);
                if (!space)
                    line.rect = line.rect.isNull() ? boxOf(c) : (line.rect | boxOf(c));
                if (!space && turned)
                    line.quads.push_back(c.quad);
                arabic += isArabic(ch) ? 1 : 0;
                latin += (ch < 128 && QChar::isLetter(ch)) ? 1 : 0;
                // a new run where the look changes (its letter spacing too)
                if (line.runs.empty() || (!space && (line.runs.back().font != c.font || std::abs(line.runs.back().size - c.size) > 0.05 ||
                                                     line.runs.back().color != c.color ||
                                                     std::abs(line.runs.back().scale - c.scale * 100) > 1 ||
                                                     std::abs(wordTrack[n - lo] - runTrack) >= 40))) {
                    runTrack = wordTrack[n - lo];
                    if (!line.runs.empty() && space) {
                        line.runs.back().text += QLatin1Char(' ');
                        continue;
                    }
                    TextRun r;
                    r.font = c.font;
                    r.flags = c.flags;
                    r.outline = c.outline;
                    r.paper = c.paper;
                    r.size = c.size;
                    r.scale = c.scale * 100;
                    r.color = c.color;
                    r.origin = turned ? c.sorigin : c.origin;
                    r.box = boxOf(c);
                    line.runs.push_back(r);
                    runStarts.push_back(n);
                }
                TextRun &r = line.runs.back();
                r.text += space ? QStringLiteral(" ") : str(ch);
                if (!space)
                    r.box |= boxOf(c);
            }
            // each run's letter spacing: the room from a letter's end to the next one's pen, past its
            // own width - most of its letters' (a pair kerned closer or wider here and there aside)
            if (!turned) {
                for (size_t i = 0; i < line.runs.size() && i < runStarts.size(); ++i) {
                    TextRun &r = line.runs[i];
                    const size_t runEnd = i + 1 < runStarts.size() ? runStarts[i + 1] : hi;  // (its own letters)
                    std::vector<double> gaps;
                    for (size_t n = runStarts[i]; n + 1 < runEnd; ++n) {
                        const TextChar &a = chars[n], &b = chars[n + 1];
                        if (a.font != r.font || std::abs(a.size - r.size) > 0.05)
                            break;
                        if (isSpace(a.c) || isSpace(b.c) || b.origin.x() <= a.origin.x())
                            continue;
                        gaps.push_back((b.origin.x() - a.origin.x() - a.advance) / std::max(0.1, r.size) * 1000);
                    }
                    if (gaps.size() >= 3) {
                        std::sort(gaps.begin(), gaps.end());
                        const double t = gaps[gaps.size() / 2];
                        r.track = std::abs(t) < 8 ? 0 : std::round(t);
                    }
                }
            }
            for (TextRun &r : line.runs) {
                static const QRegularExpression spaces(QStringLiteral(" {2,}"));
                r.text.replace(spaces, QStringLiteral(" "));
                line.text += r.text;
            }
            static const QRegularExpression spaces(QStringLiteral(" {2,}"));
            line.text = line.text.replace(spaces, QStringLiteral(" ")).trimmed();
            line.rtl = arabic > 0 && arabic >= latin;
            line.baseline = line.runs.front().origin.y();
            out.push_back(std::move(line));
        }
        start = end;
    }
    return joinPieces(std::move(out));
}

namespace {

double lineSize(const TextLine &l)
{
    double s = 0;
    for (const TextRun &r : l.runs)
        s = std::max(s, r.size);
    return s > 0 ? s : 10;
}

bool runBold(const TextRun &r)
{
    return (r.flags & 1) || r.font.contains(QStringLiteral("bold"), Qt::CaseInsensitive);
}

// a list's item: its line starts with a bullet (or the Symbol font's) and a space - each one its own
// paragraph, two of them never run together
bool listItem(const QString &text)
{
    const QString t = text.trimmed();
    if (t.size() < 2 || !t[1].isSpace())
        return false;
    static const QString signs = QStringLiteral("•●▪■◦‣⁃➢✓○");
    return signs.contains(t[0]);
}

// how wide a line's first word is (its share of the line's width)
double firstWordWidth(const TextLine &l)
{
    const QString t = l.text.trimmed();
    if (t.isEmpty())
        return 0;
    const int end = int(t.indexOf(QLatin1Char(' ')));
    const int n = end < 0 ? int(t.size()) : end;
    return l.rect.width() * n / double(t.size());
}

std::vector<std::vector<int>> splitParagraphs(const std::vector<TextLine> &all, const std::vector<int> &ls, const QRectF &rect)
{
    const double x0 = rect.left(), x1 = rect.right(), mid = (x0 + x1) / 2;
    std::vector<double> gaps;
    for (size_t k = 1; k < ls.size(); ++k)
        gaps.push_back(all[size_t(ls[k])].rect.top() - all[size_t(ls[k - 1])].rect.bottom());
    std::vector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    const double normal = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    // (centred / right-aligned lines: one starting further in is how they look)
    bool anyIn = false, allMid = true, allRight = true;
    for (int i : ls) {
        const TextLine &o = all[size_t(i)];
        anyIn = anyIn || o.rect.left() > x0 + lineSize(o);
        allMid = allMid && std::abs(o.rect.center().x() - mid) < 0.6 * lineSize(o);
        allRight = allRight && std::abs(o.rect.right() - x1) < 0.6 * lineSize(o);
    }
    const bool ragged = ls.size() > 1 && anyIn && (allMid || allRight);
    std::vector<std::vector<int>> paras{{ls.front()}};
    for (size_t k = 1; k < ls.size(); ++k) {
        const TextLine &a = all[size_t(ls[k - 1])], &b = all[size_t(ls[k])];
        const double sa = lineSize(a), sb = lineSize(b), g = gaps[k - 1];
        const bool fits = a.rect.width() + 0.3 * sb + firstWordWidth(b) <= (x1 - x0) + 0.5;
        bool indented = false;
        if (!ragged)
            indented = b.rtl ? (b.rect.right() < x1 - 1.2 * sb && a.rect.right() >= x1 - 1.2 * sa)
                             : (b.rect.left() > x0 + 1.2 * sb && a.rect.left() <= x0 + 1.2 * sa);
        const bool fresh = g > normal + 0.35 * std::max(sa, sb)            // (a bigger gap)
                           || std::abs(sa - sb) > 0.15 * std::max(sa, sb)  // (another size)
                           || fits                                         // (ended, though the next word would have fitted)
                           || (runBold(b.runs.front()) && !runBold(a.runs.back()))  // (a bold label starting it)
                           || indented || listItem(b.text);                 // (a list's next item: its sign first)
        if (fresh)
            paras.push_back({ls[k]});
        else
            paras.back().push_back(ls[k]);
    }
    return paras;
}

}  // namespace

std::vector<TextFrame> frames(const std::vector<TextLine> &lines)
{
    std::vector<int> order(lines.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const QRectF &ra = lines[size_t(a)].rect, &rb = lines[size_t(b)].rect;
        return ra.top() != rb.top() ? ra.top() < rb.top() : ra.left() < rb.left();
    });
    struct Building {
        std::vector<int> lines;
        QRectF rect;
        std::vector<double> gaps;
    };
    std::vector<Building> out;
    for (int i : order) {
        const TextLine &o = lines[size_t(i)];
        if (o.text.trimmed().isEmpty() || o.runs.empty())
            continue;
        const QRectF &r = o.rect;
        const double h = r.height();
        int best = -1;
        double bestGap = 0;
        for (int f = 0; f < int(out.size()); ++f) {
            const Building &fr = out[size_t(f)];
            const TextLine &lastLine = lines[size_t(fr.lines.back())];
            const QRectF &last = lastLine.rect;
            if (std::abs(lastLine.angle - o.angle) > 0.01 || lastLine.rtl != o.rtl)
                continue;  // (turned alike, the same way across)
            const double gap = r.top() - last.bottom();
            const double over = std::min(r.right(), fr.rect.right()) - std::max(r.left(), fr.rect.left());
            const double size = std::max(lineSize(o), lineSize(lastLine));
            bool linedUp = false;
            for (const QRectF &q : {fr.rect, last})
                linedUp = linedUp || std::abs(r.left() - q.left()) < 2.2 * size || std::abs(r.right() - q.right()) < 2.2 * size ||
                          std::abs((r.left() + r.right() - q.left() - q.right()) / 2) < 2.2 * size;
            const bool sameSize = std::abs(lineSize(o) - lineSize(lastLine)) <= 0.15 * size;
            bool close;
            if (!fr.gaps.empty()) {  // (spaced like the frame's lines)
                std::vector<double> g = fr.gaps;
                std::sort(g.begin(), g.end());
                close = gap <= std::max(g[g.size() / 2], 0.0) + std::max(1.5, 0.25 * size);
            } else {  // (the first two: like lines of text - up to an airy design's spacing)
                close = gap <= 0.65 * size;
                // (... or a paragraph's lines set far apart - its line spacing 2 or more: the same letters,
                // from the same left, the one above a long line of text, as wide as this one or wider - a
                // line wrapped; not a form's labels, a column of marks)
                if (!close && gap <= 2.0 * size && std::abs(r.left() - last.left()) < 0.5 && last.width() >= 0.9 * r.width() &&
                    last.width() >= 15 * size && r.width() >= 0.75 * last.width() &&  // (a third follows: this isn't its short last line)
                    !lastLine.runs.empty() && !o.runs.empty() && lastLine.mainRun().font == o.mainRun().font &&
                    std::abs(lastLine.mainRun().size - o.mainRun().size) < 0.05) {
                    // (and a third line under it the same step down, from the same left, in the same letters -
                    // a paragraph going on; two lines alone - a signature's line and its date - stay apart)
                    const double step = r.top() - last.top();
                    for (const TextLine &n : lines)
                        if (std::abs(n.rect.top() - r.top() - step) < 0.5 && std::abs(n.rect.left() - r.left()) < 0.5 && !n.runs.empty() &&
                            n.mainRun().font == o.mainRun().font && std::abs(n.mainRun().size - o.mainRun().size) < 0.05) {
                            close = true;
                            break;
                        }
                    // (... or a paragraph above it, in the same letters from the same left, its lines that same
                    // step apart: a paragraph of two lines among others set as far apart)
                    for (int g = 0; g < int(out.size()) && !close; ++g) {
                        const std::vector<int> &ls = out[size_t(g)].lines;
                        for (size_t m = 1; m < ls.size() && !close; ++m) {
                            const TextLine &a = lines[size_t(ls[m - 1])], &b = lines[size_t(ls[m])];
                            close = std::abs((b.rect.top() - a.rect.top()) - step) < 0.5 && std::abs(b.rect.left() - r.left()) < 0.5 &&
                                    !b.runs.empty() && b.mainRun().font == o.mainRun().font && std::abs(b.mainRun().size - o.mainRun().size) < 0.05;
                        }
                    }
                }
            }
            if (-0.3 * h < gap && close && sameSize && over >= 0.4 * std::min(r.width(), fr.rect.width()) && linedUp &&
                (best < 0 || gap < bestGap)) {
                best = f;
                bestGap = gap;
            }
        }
        if (best >= 0) {
            Building &fr = out[size_t(best)];
            fr.gaps.push_back(bestGap);
            fr.lines.push_back(i);
            fr.rect |= r;
        } else {
            out.push_back({{i}, r, {}});
        }
    }
    std::vector<TextFrame> result;
    for (const Building &bf : out)
        result.push_back({bf.lines, bf.rect, splitParagraphs(lines, bf.lines, bf.rect)});
    return result;
}

QString alignOf(const std::vector<TextLine> &lines, const std::vector<int> &para, double left, double right)
{
    if (para.empty())
        return QStringLiteral("left");
    // (its own left: past a first-line indent - a line between its first and last; two lines: the
    // block's, else its last line was measured against itself, and a centred title read as justified)
    const double own = para.size() > 2 ? lines[size_t(para[1])].rect.left() : para.size() == 2 ? left : lines[size_t(para[0])].rect.left();
    bool just = para.size() > 1, cen = true, rig = true, lef = true, same = true;
    for (size_t m = 0; m < para.size(); ++m) {
        const QRectF &r = lines[size_t(para[m])].rect;
        if (m + 1 < para.size())
            just = just && std::abs(r.right() - right) <= 2 && (m == 0 || std::abs(r.left() - own) <= 2);
        else if (para.size() > 1)  // (a justified paragraph's last line starts where its lines start)
            just = just && (lines[size_t(para[m])].rtl ? std::abs(r.right() - right) <= 2 : std::abs(r.left() - own) <= 2);
        cen = cen && std::abs(r.center().x() - (left + right) / 2) <= 2;
        rig = rig && std::abs(r.right() - right) <= 2;
        lef = lef && std::abs(r.left() - left) <= 2;
        same = same && (m == 0 || std::abs(r.left() - own) <= 2);
    }
    if (just && para.size() == 2) {
        // (two lines: its first is what the box's right edge is - a left-aligned one looks just the
        // same. Justified only if other lines of the column reach that edge too, as body text's do)
        bool others = false;
        for (size_t i = 0; i < lines.size() && !others; ++i)
            others = int(i) != para[0] && int(i) != para[1] && std::abs(lines[i].rect.right() - right) <= 2 &&
                     std::abs(lines[i].rect.left() - left) <= 2;
        just = others;
    }
    const QRectF &first = lines[size_t(para.front())].rect;
    const bool ragged = para.size() > 1 ? !same : first.width() < 0.6 * (right - left);
    return just ? QStringLiteral("justify") : (cen && !lef && !rig) ? QStringLiteral("center")
           : (lines[size_t(para.front())].rtl || (rig && !lef && ragged)) ? QStringLiteral("right") : QStringLiteral("left");
}

std::vector<TextBox> boxes(const std::vector<TextLine> &lines, const std::vector<TextFrame> &frames, const std::vector<StructMark> &marks,
                           const std::vector<QLineF> &rules)
{
    // (a tagged PDF: each line's paragraph element - most of its letters' - and whether it's in a
    // table cell. In a cell, paragraphs apart stay apart - a label and its value; one paragraph
    // element stays one box)
    std::vector<int> paraOf(lines.size(), -1);
    std::vector<bool> cellOf(lines.size(), false);
    bool tagged = false;
    for (const StructMark &m : marks)
        tagged = tagged || m.para >= 0;
    for (size_t n = 0; n < lines.size() && !marks.empty(); ++n) {
        std::map<int, int> count;
        const QRectF r = lines[n].rect.adjusted(-1, -1, 1, 1);
        for (const StructMark &m : marks)
            if (r.contains(m.at)) {
                ++count[m.para];
                cellOf[n] = cellOf[n] || m.cell;
            }
        int best = -1, most = 0;
        for (const auto &[k, v] : count)
            if (v > most) {
                most = v;
                best = k;
            }
        paraOf[n] = best;
    }
    // (not tagged: the PDF's own text blocks as MuPDF reads them stand in for its paragraphs - a
    // label and its value apart, a label and its text together; blocks apart join only if they
    // look alike: the same weight and size)
    if (!tagged)
        for (size_t n = 0; n < lines.size(); ++n) {
            paraOf[n] = lines[n].block;
            cellOf[n] = false;
        }
    auto styleOf = [&](int k) {
        const TextRun &r = lines[size_t(k)].mainRun();
        const bool bold = r.font.contains(QLatin1String("Bold"), Qt::CaseInsensitive) || (r.flags & 1);
        return std::make_pair(bold, int(std::lround(r.size * 2)));
    };
    // the text as Acrobat boxes it: paragraphs (a first line's indent kept inside one) put together
    // when their edges line up - left, right or middle, within 2 pt - up to 2.2 times the smaller
    // size apart (or, in a column spaced its own way, a skipped row); a label and the list indented
    // under it, a set-in contents list, columns, table cells: apart. A piece of a letter or two:
    // on its own. (Each box: the blocks it touches - editing keeps to the blocks.)
    struct Unit {
        std::vector<int> lines;
        QRectF rect;
        int frame;
        std::set<int> paras;  // (its lines' paragraph elements)
        bool cell = false;
    };
    std::vector<Unit> units;
    std::vector<bool> inFrame(lines.size(), false);
    for (int f = 0; f < int(frames.size()); ++f)
        for (const std::vector<int> &para : frames[size_t(f)].paras) {
            Unit u{para, {}, f, {}, false};
            for (int k : para) {
                u.rect |= lines[size_t(k)].rect;
                inFrame[size_t(k)] = true;
                if (paraOf[size_t(k)] >= 0)
                    u.paras.insert(paraOf[size_t(k)]);
                u.cell = u.cell || cellOf[size_t(k)];
            }
            units.push_back(u);
        }
    auto sizeOf = [&](const Unit &u) {
        double s0 = 0;
        for (int k : u.lines)
            s0 = std::max(s0, lineSize(lines[size_t(k)]));
        return s0;
    };
    auto letters = [&](const Unit &u) {
        int n = 0;
        for (int k : u.lines)
            n += int(QString(lines[size_t(k)].text).remove(QLatin1Char(' ')).size());
        return n;
    };
    auto turned = [&](const Unit &u) {
        for (int k : u.lines)
            if (std::abs(lines[size_t(k)].angle) > 0.01)
                return true;
        return false;
    };
    std::sort(units.begin(), units.end(), [](const Unit &a, const Unit &b) {
        return a.rect.top() != b.rect.top() ? a.rect.top() < b.rect.top() : a.rect.left() < b.rect.left();
    });
    std::vector<TextBox> out;
    std::vector<int> lastOf;  // (each box's lowest paragraph)
    std::vector<std::vector<double>> gapsOf;  // (each box's gaps between its lines)
    auto ownGaps = [&](const Unit &u) {
        std::vector<double> g;
        for (size_t k = 1; k < u.lines.size(); ++k)
            g.push_back(lines[size_t(u.lines[k])].rect.top() - lines[size_t(u.lines[k - 1])].rect.bottom());
        return g;
    };
    auto together = [](const QRectF &a, const QRectF &c) {
        return std::abs(a.left() - c.left()) < 2 || std::abs(a.right() - c.right()) < 2 || std::abs(a.center().x() - c.center().x()) < 2;
    };
    std::vector<int> boxOfUnit(units.size(), -1);
    for (size_t i = 0; i < units.size(); ++i) {
        const Unit &u = units[i];
        int best = -1;
        // (just the box of the text right above it, over the same place across - never past another)
        int above = -1;
        for (size_t j = 0; j < i; ++j) {
            const QRectF &a = units[j].rect;
            const bool across = std::min(a.right(), u.rect.right()) > std::max(a.left(), u.rect.left());
            if (across && a.bottom() <= u.rect.top() + 0.3 * u.rect.height() && (above < 0 || a.bottom() > units[size_t(above)].rect.bottom()))
                above = int(j);
        }
        if (letters(u) >= 3 && !turned(u) && above >= 0)
            for (int b = boxOfUnit[size_t(above)]; b >= 0 && b < int(out.size()); b = -1) {
                const Unit &last = units[size_t(above)];
                if (letters(last) < 3 || turned(last))
                    continue;
                const QRectF &r = u.rect, &q = out[size_t(b)].rect;
                const double size = std::min(sizeOf(u), sizeOf(last));  // (a title's size doesn't stretch the gap)
                const double gap = r.top() - last.rect.bottom();
                const double over = std::min(r.right(), q.right()) - std::max(r.left(), q.left());
                std::vector<double> g = gapsOf[size_t(b)];
                std::sort(g.begin(), g.end());
                const bool spaced = g.size() >= 2 && gap <= 2.5 * std::max(1.0, g[g.size() / 2]) && gap <= 3 * size;
                bool sameElement = false;  // (the PDF says it's one paragraph)
                for (int k : u.paras)
                    sameElement = sameElement || last.paras.count(k);
                const bool apartInCell = (u.cell || last.cell) && !u.paras.empty() && !last.paras.empty() && !sameElement;
                if (apartInCell)
                    continue;
                bool ruled = false;  // (a line drawn between them, across both: a table's row, a rule)
                for (const QLineF &l : rules)
                    ruled = ruled || (l.y1() > last.rect.bottom() - 0.5 && l.y1() < u.rect.top() + 0.5 &&
                                      l.x1() < std::min(u.rect.right(), last.rect.right()) && l.x2() > std::max(u.rect.left(), last.rect.left()));
                if (ruled)
                    continue;
                // (a list, and a paragraph after it starting left of its bullets: apart - Acrobat's way)
                auto bulleted = [&](const Unit &x) {
                    const QString t = lines[size_t(x.lines.front())].text.trimmed();
                    if (t.isEmpty())
                        return false;
                    const char16_t c = t[0].unicode();
                    return c == 0x2022 || c == 0x25CF || c == 0x25AA || c == 0x25A0 || c == 0x25E6 || c == 0x2023 || c == 0x2043 ||
                           c == 0x2013 || c == 0xF0B7 || c == 0xF0A7 || c == 0xF076 || c == 0xF0D8 || c == 0x27A2 || c == 0x2713;
                };
                if (bulleted(last) && !bulleted(u) && u.rect.left() < last.rect.left() - 2)
                    continue;
                // (text of another size close to it - a title over its author line, a heading set tight
                // over its paragraph: apart. A heading with room round it joins its text)
                const double big = std::max(sizeOf(u), sizeOf(last));
                if ((!tagged || !sameElement) && big >= 1.15 * size && gap < 0.6 * big)
                    continue;
                // (text set solid - no room between its paragraphs, its lines evenly spaced: a bigger gap
                // than a line's size is a break - a heading, the keywords under an abstract)
                // (and it starts another way: bold - a heading, a "Keywords:" label - or in another size)
                const TextRun &first = lines[size_t(u.lines.front())].runs.empty() ? lines[size_t(u.lines.front())].mainRun()
                                                                                   : lines[size_t(u.lines.front())].runs.front();
                const bool firstBold = first.font.contains(QLatin1String("Bold"), Qt::CaseInsensitive) || (first.flags & 1);
                if (g.size() >= 2 && g.back() <= 0.35 * size && gap > 1.0 * size &&
                    (firstBold || std::abs(sizeOf(u) - sizeOf(last)) > 0.1 * size))
                    continue;
                // (blocks apart, the same size but one bold and one not: a label and its value - a heading
                // in its own size over its text joins it)
                if (!tagged && !sameElement && styleOf(u.lines.front()).second == styleOf(last.lines.back()).second &&
                    styleOf(u.lines.front()).first != styleOf(last.lines.back()).first)
                    continue;
                if (gap > -0.3 * size && (gap <= 2.2 * size || spaced) &&
                    ((over >= 0.4 * std::min(r.width(), q.width()) && (together(r, q) || together(r, last.rect))) || (sameElement && gap <= 2.2 * size))) {
                    best = b;
                    break;
                }
            }
        if (best >= 0) {
            gapsOf[size_t(best)].push_back(u.rect.top() - units[size_t(lastOf[size_t(best)])].rect.bottom());
            for (double g : ownGaps(u))
                gapsOf[size_t(best)].push_back(g);
            TextBox &bx = out[size_t(best)];
            if (std::find(bx.frames.begin(), bx.frames.end(), u.frame) == bx.frames.end())
                bx.frames.push_back(u.frame);
            bx.rect |= u.rect;
            bx.paras.push_back(u.lines);
            lastOf[size_t(best)] = int(i);
            boxOfUnit[i] = best;
        } else {
            out.push_back({{u.frame}, u.rect, {u.lines}});
            lastOf.push_back(int(i));
            gapsOf.push_back(ownGaps(u));
            boxOfUnit[i] = int(out.size()) - 1;
        }
    }
    // a marker of a letter or two ("1", "2", "*" before an affiliation, a bullet set apart) right beside a
    // box's line, on its row: shown inside that box's outline, as Acrobat boxes it (still edited alone)
    for (TextBox &m : out) {
        if (m.paras.size() != 1 || m.paras[0].size() != 1)
            continue;
        const TextLine &ml = lines[size_t(m.paras[0][0])];
        if (QString(ml.text).remove(QLatin1Char(' ')).size() > 2 || std::abs(ml.angle) > 0.01)
            continue;
        for (TextBox &b : out) {
            if (&b == &m || b.hidden || (b.paras.size() == 1 && b.paras[0].size() == 1))
                continue;
            bool beside = false;
            for (const std::vector<int> &para : b.paras)
                for (int k : para) {
                    const TextLine &l = lines[size_t(k)];
                    const double overlap = std::min(l.rect.bottom(), ml.rect.bottom()) - std::max(l.rect.top(), ml.rect.top());
                    const double room = std::max(l.rect.left() - ml.rect.right(), ml.rect.left() - l.rect.right());
                    beside = beside || (overlap >= 0.5 * std::min(l.rect.height(), ml.rect.height()) && room >= -1 && room <= 2 * lineSize(l));
                }
            if (beside) {
                b.shown = (b.shown.isNull() ? b.rect : b.shown) | ml.rect;
                m.hidden = true;
                break;
            }
        }
    }
    return out;
}

int frameOf(const std::vector<TextFrame> &frames, int line)
{
    for (int f = 0; f < int(frames.size()); ++f)
        for (int l : frames[size_t(f)].lines)
            if (l == line)
                return f;
    return -1;
}

std::vector<int> paragraph(const std::vector<TextLine> &lines, int index)
{
    // the paragraph it's in, in its frame
    if (index < 0 || index >= int(lines.size()))
        return {};
    const std::vector<TextFrame> fs = frames(lines);
    const int f = frameOf(fs, index);
    if (f < 0)
        return {index};
    for (const std::vector<int> &para : fs[size_t(f)].paras)
        if (std::find(para.begin(), para.end(), index) != para.end())
            return para;
    return {index};
}

unsigned paperIn(const QImage &image, const QRect &area)
{
    const QImage im = image.format() == QImage::Format_RGB888 ? image : image.convertToFormat(QImage::Format_RGB888);
    const QRect r = area.intersected(im.rect());
    if (r.isEmpty())
        return 0xFFFFFF;
    // (a step so a big part isn't counted pixel by pixel)
    const int step = std::max(1, int(std::sqrt(double(r.width()) * r.height() / 40000.0)));
    std::vector<std::array<int, 4>> px;  // (lightness, r, g, b)
    for (int y = r.top(); y <= r.bottom(); y += step) {
        const uchar *row = im.constScanLine(y);
        for (int x = r.left(); x <= r.right(); x += step) {
            const uchar *p = row + 3 * x;
            px.push_back({p[0] * 299 + p[1] * 587 + p[2] * 114, p[0], p[1], p[2]});
        }
    }
    std::sort(px.begin(), px.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    // (the lighter half - letters are less of a line than its paper - less its lightest tenth: specks)
    const size_t from = px.size() / 2, to = std::max(from + 1, px.size() - px.size() / 10);
    double sr = 0, sg = 0, sb = 0;
    for (size_t k = from; k < to; ++k)
        sr += px[k][1], sg += px[k][2], sb += px[k][3];
    const double n = double(to - from);
    return (unsigned(std::lround(sr / n)) << 16) | (unsigned(std::lround(sg / n)) << 8) | unsigned(std::lround(sb / n));
}

unsigned inkIn(const QImage &image, const QRect &area)
{
    const QImage im = image.format() == QImage::Format_RGB888 ? image : image.convertToFormat(QImage::Format_RGB888);
    const QRect r = area.intersected(im.rect());
    if (r.isEmpty())
        return 0;
    std::vector<std::array<int, 4>> px;  // (lightness, r, g, b)
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const uchar *row = im.constScanLine(y);
        for (int x = r.left(); x <= r.right(); ++x) {
            const uchar *p = row + 3 * x;
            px.push_back({p[0] * 299 + p[1] * 587 + p[2] * 114, p[0], p[1], p[2]});
        }
    }
    std::sort(px.begin(), px.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    // (its strokes' middles: darker than a quarter of the way from its darkest to its paper - their middle one's colour)
    const int dark = px[px.size() / 50][0], light = px[px.size() * 7 / 10][0];
    size_t n = 0;
    while (n < px.size() && px[n][0] <= dark + (light - dark) / 4)
        ++n;
    if (n < 8 || light - dark < 30000)
        return 0;
    std::array<std::vector<int>, 3> ch;
    for (size_t k = 0; k < n; ++k)
        for (int c = 0; c < 3; ++c)
            ch[size_t(c)].push_back(px[k][size_t(c) + 1]);
    unsigned out = 0;
    for (auto &v : ch) {
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        out = (out << 8) | unsigned(v[v.size() / 2]);
    }
    return out;
}

}  // namespace pagetext
