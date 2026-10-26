// MainWindow's Scan (as Acrobat's Scan & OCR): a scanned page's text read and written over it as
// invisible text - the scan stays as it was, its words can be found, picked, copied; and edited: the line
// edited is covered with the paper round it and written in the font matched to it.
// Each line: just the lines of text (not a logo's, a stamp's, a signature's: what isn't text, isn't sure,
// or looks like no font), as wide as on the scan, in the font that draws it most alike (its size, bold),
// one font a paragraph. Arabic read first when the PDF has it.

#include "app/MainWindow.h"

#include "core/Document.h"
#include "core/Jobs.h"
#include "core/Ocr.h"
#include "core/PageText.h"
#include "core/ScanFont.h"
#include "ui/Classic.h"
#include "ui/ClassicWindow.h"
#include "ui/DocTabs.h"
#include "ui/Sounds.h"
#include "ui/ToolButton.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QCloseEvent>
#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QPainter>
#include <QRegularExpression>
#include <QTextLayout>

#include <algorithm>
#include <thread>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>

namespace {

constexpr double kDpi = 300;  // (the page read at - Tesseract's best)

// how far the page's reading is - 0 to 1, and what it's doing (empty: as it was): the scan's box shows it (it never
// throws; stepAt does, where the scan can stop: called off - the scan's tab closed, the window closed)
struct ScanCancelled {};
std::atomic<bool> scanCancel{false};
std::function<void(double, const QString &)> readStep = [](double, const QString &) {};
void stepAt(double f, const QString &what = QString())
{
    if (scanCancel)
        throw ScanCancelled();
    readStep(f, what);
}
// (... a part of it, from .. to of the page's reading: done of all)
void stepIn(double from, double to, double done, double all)
{
    stepAt(from + (to - from) * std::clamp(done / std::max(1.0, all), 0.0, 1.0));
}

// (an Arabic line's numbers: in Arabic-Indic digits when any of the number is - Tesseract reads some as 4, 7, or
// Persian's ۱)
QString arabicDigits(QString text)
{
    auto digit = [](QChar c) { return c.isDigit() && (c.unicode() < 0x80 || (c.unicode() >= 0x660 && c.unicode() <= 0x669) ||
                                                      (c.unicode() >= 0x6F0 && c.unicode() <= 0x6F9)); };
    for (int i = 0; i < text.size();) {
        if (!digit(text[i])) {
            ++i;
            continue;
        }
        int j = i;
        bool indic = false;
        while (j < text.size() && (digit(text[j]) || ((text[j] == u'/' || text[j] == u'.' || text[j] == u'-') && j + 1 < text.size() && digit(text[j + 1])))) {
            indic = indic || text[j].unicode() >= 0x660;
            ++j;
        }
        if (indic)
            for (int k = i; k < j; ++k)
                if (text[k].isDigit())
                    text[k] = QChar(char16_t(0x660 + text[k].digitValue()));
        i = j;
    }
    return text;
}

// (an Arabic line: more Arabic letters than Latin ones - a stray mark read into an English line doesn't make it one)
bool arabic(const QString &text)
{
    int arab = 0, latin = 0;
    for (const QChar c : text)
        if (pagetext::isArabic(c.unicode()))
            ++arab;
        else if (c.isLetter() && c.unicode() < 0x250)
            ++latin;
    return arab > 0 && arab >= latin;
}

// the fonts a line is matched against - the usual ones a printed page is in (those installed)
const QStringList &candidates(bool forArabic)
{
    static QStringList latin, arab;
    static bool made = false;
    if (!made) {
        made = true;
        const QStringList have = QFontDatabase::families();
        for (const char *f : {"Arial", "Times New Roman", "Calibri", "Verdana", "Tahoma", "Courier New", "Segoe UI"})
            if (have.contains(QString::fromLatin1(f)))
                latin << QString::fromLatin1(f);
        for (const char *f : {"Traditional Arabic", "Simplified Arabic", "Sakkal Majalla", "Arabic Typesetting", "Arial", "Times New Roman",
                              "Tahoma", "Segoe UI"})
            if (have.contains(QString::fromLatin1(f)))
                arab << QString::fromLatin1(f);
    }
    return forArabic ? arab : latin;
}

// every font installed that writes the script - looked over quickly for each paragraph, the likeliest few
// matched closely (a letterhead's Century Gothic, a form's Garamond: not just the usual ones)
const QStringList &allFamilies(bool forArabic)
{
    static QStringList latin, arab;
    static bool made = false;
    if (!made) {
        made = true;
        QStringList known;
        for (const QString &f : fonts::families())
            known << f.toLower();
        for (const QString &f : QFontDatabase::families()) {
            if (f.startsWith(QLatin1Char('@')) || QFontDatabase::isPrivateFamily(f) || !known.contains(f.toLower()))
                continue;
            const QList<QFontDatabase::WritingSystem> ws = QFontDatabase::writingSystems(f);
            if (ws.contains(QFontDatabase::Latin))
                latin << f;
            if (ws.contains(QFontDatabase::Arabic))
                arab << f;
        }
    }
    return forArabic ? arab : latin;
}

// a grey picture's ink: darker than the line between its paper and its ink (Otsu's)
std::vector<char> inkOf(const QImage &grey, QRect *box)
{
    const int w = grey.width(), h = grey.height();
    int hist[256] = {};
    for (int y = 0; y < h; ++y) {
        const uchar *row = grey.constScanLine(y);
        for (int x = 0; x < w; ++x)
            ++hist[row[x]];
    }
    const double total = double(w) * h;
    double sum = 0;
    for (int k = 0; k < 256; ++k)
        sum += k * double(hist[k]);
    double sumB = 0, wB = 0, best = -1;
    int cut = 128;
    for (int k = 0; k < 256; ++k) {
        wB += hist[k];
        if (wB == 0)
            continue;
        const double wF = total - wB;
        if (wF == 0)
            break;
        sumB += k * double(hist[k]);
        const double mB = sumB / wB, mF = (sum - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best)
            best = between, cut = k;
    }
    std::vector<char> ink(size_t(w) * size_t(h), 0);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const uchar *row = grey.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (row[x] <= cut) {
                ink[size_t(y) * size_t(w) + size_t(x)] = 1;
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
    }
    *box = x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
    return ink;
}

// a line (a word) drawn in a font, big: its ink's box, its baseline, its width as written
struct Drawn {
    QImage image;
    QRect box;
    double base = 0, advance = 0;
    int pad = 0;
};

Drawn draw(const QString &text, const QString &family, bool bold, int kPx = 48)
{
    Drawn d;
    QFont f(family);
    f.setPixelSize(kPx);
    f.setBold(bold);
    f.setStyleStrategy(QFont::PreferAntialias);
    const QFontMetricsF fm(f);
    d.advance = fm.horizontalAdvance(text);
    if (d.advance <= 0)
        return d;
    d.pad = kPx / 2;
    d.image = QImage(int(std::ceil(d.advance)) + 2 * d.pad, kPx * 2 + 2 * d.pad, QImage::Format_Grayscale8);
    d.image.fill(255);
    d.base = d.pad + fm.ascent();
    {
        QPainter p(&d.image);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(QPointF(d.pad, d.base), text);
    }
    inkOf(d.image, &d.box);
    return d;
}

// (the scan's ink box inside a part of it)
QRect inkIn(const std::vector<char> &ink, QSize size, const QRect &part)
{
    const QRect r = part.intersected(QRect(QPoint(0, 0), size));
    int x0 = r.right() + 1, y0 = r.bottom() + 1, x1 = -1, y1 = -1;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (ink[size_t(y) * size_t(size.width()) + size_t(x)])
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    return x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
}

// how alike: the drawn ink scaled onto the scan's (both ways), the share of ink they have in common - less for
// a font drawn much wider / narrower than the scan's
// (inks: the drawn ink's pixels and the scan's, counted - how heavy its strokes are, against the scan's)
// (inks, dark given: how much ink each has in all - the drawing's coverage, the scan's darkness, paper 0 to ink 1 - a
// blur spreads a scan's ink but doesn't add to it: a regular line blurred isn't taken for a bold one)
double overlap(const Drawn &d, const std::vector<char> &ink, QSize size, const QRect &inkBox, double *inks = nullptr,
               const std::vector<float> *dark = nullptr)
{
    if (d.box.isEmpty() || inkBox.isEmpty())
        return -1;
    const double sx = double(inkBox.width()) / d.box.width(), sy = double(inkBox.height()) / d.box.height();
    const QImage fitted = d.image.copy(d.box).scaled(inkBox.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    long both = 0, either = 0;
    for (int y = 0; y < inkBox.height(); ++y) {
        const uchar *row = fitted.constScanLine(y);
        for (int x = 0; x < inkBox.width(); ++x) {
            const bool a = row[x] < 128;
            const size_t at = size_t(y + inkBox.top()) * size_t(size.width()) + size_t(x + inkBox.left());
            const bool b = ink[at];
            both += a && b;
            either += a || b;
            if (inks) {
                inks[0] += dark ? (255 - row[x]) / 255.0 : double(a);
                inks[1] += dark ? double((*dark)[at]) : double(b);
                // (the drawing's outline: its ink's pixels on its edge)
                if (a && (x == 0 || y == 0 || x + 1 == inkBox.width() || y + 1 == inkBox.height() || row[x - 1] >= 128 || row[x + 1] >= 128 ||
                          fitted.constScanLine(y - 1)[x] >= 128 || fitted.constScanLine(y + 1)[x] >= 128))
                    inks[2] += 1;
            }
        }
    }
    return (either ? double(both) / double(either) : 0) - 0.35 * std::abs(std::log(sx / sy));
}

// (a quick look: a word's scan ink made small - its ink's box kSmall pixels high - and a font's drawing of it
// at that size; how alike, as overlap's)
constexpr int kSmall = 22;

QImage smallInk(const std::vector<char> &ink, QSize size, const QRect &box)
{
    if (box.isEmpty() || box.height() < 4)
        return {};
    QImage im(box.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < box.height(); ++y) {
        uchar *row = im.scanLine(y);
        for (int x = 0; x < box.width(); ++x)
            row[x] = ink[size_t(y + box.top()) * size_t(size.width()) + size_t(x + box.left())] ? 0 : 255;
    }
    const int w = std::max(2, int(std::lround(double(box.width()) * kSmall / box.height())));
    return im.scaled(w, kSmall, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

double quickLook(const QString &word, const QString &family, const QImage &target)
{
    const Drawn d = draw(word, family, false, 28);
    if (d.box.isEmpty() || target.isNull())
        return -1;
    const double sx = double(target.width()) / d.box.width(), sy = double(target.height()) / d.box.height();
    const QImage fitted = d.image.copy(d.box).scaled(target.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    long both = 0, either = 0;
    for (int y = 0; y < target.height(); ++y) {
        const uchar *a = fitted.constScanLine(y), *b = target.constScanLine(y);
        for (int x = 0; x < target.width(); ++x) {
            both += a[x] < 128 && b[x] < 128;
            either += a[x] < 128 || b[x] < 128;
        }
    }
    return (either ? double(both) / double(either) : 0) - 0.35 * std::abs(std::log(sx / sy));
}

// a line in a font fitted over the scan's: where, how big - and how alike, word by word (a line's spaces
// differ from font to font; its words' shapes are the font's)
struct Fit {
    double score = -1;
    double size = 0;      // (points)
    double baseline = 0;  // (the scan's pixels, down from the crop's top)
    double penX = 0;      // (where its pen starts: the scan's pixels, from the crop's left)
    double advance = 0;   // (its width as written: the scan's pixels)
    double weight = 1;    // (its strokes' ink over the scan's: 1 as heavy - bold or not)
    double aspect = 0;    // (its words' shapes against the scan's: how much wider for their height, as a log - 0 alike)
    double drawnInk = 0, scanInk = 0, edge = 0;  // (its words' ink drawn, the scan's, the drawing's outline - pixels)
};

Fit fitFont(const QString &text, const std::vector<std::pair<QString, QRect>> &words, const QString &family, bool bold,
            const std::vector<char> &ink, QSize size, const QRect &inkBox, const std::vector<float> *dark = nullptr)
{
    Fit fit;
    const Drawn d = draw(text, family, bold);
    if (d.box.isEmpty() || inkBox.isEmpty())
        return fit;
    const double sx = double(inkBox.width()) / d.box.width(), sy = double(inkBox.height()) / d.box.height();
    fit.size = 48 * sy / (kDpi / 72);
    fit.baseline = inkBox.top() + (d.base - d.box.top()) * sy;
    fit.penX = inkBox.left() - (d.box.left() - d.pad) * sx;
    fit.advance = d.advance * sx;
    double sum = 0, weight = 0;
    double inks[3] = {0, 0, 0};
    double aspects = 0, aspectWeight = 0;
    for (const auto &[word, box] : words) {
        const QRect wb = inkIn(ink, size, box);
        const Drawn dw = draw(word, family, bold);
        const double s = overlap(dw, ink, size, wb, inks, dark);
        if (s > -1)
            sum += s * wb.width(), weight += wb.width();
        if (s > -1 && !dw.box.isEmpty() && wb.height() > 4 && word.size() >= 3) {
            aspects += std::log((double(dw.box.width()) / dw.box.height()) / (double(wb.width()) / wb.height())) * wb.width();
            aspectWeight += wb.width();
        }
    }
    fit.aspect = aspectWeight > 0 ? aspects / aspectWeight : 0;
    fit.score = weight > 0 ? sum / weight : overlap(d, ink, size, inkBox, inks, dark);
    fit.weight = inks[1] > 0 ? inks[0] / inks[1] : 1;
    fit.drawnInk = inks[0], fit.scanInk = inks[1], fit.edge = inks[2];
    return fit;
}

void levels(const QImage &grey, const QRect &r, int *paper, int *ink);

// (a line's ink, as its crop has it - without the line above's or below's marks reaching into it: a piece cut by its top
// edge that ends above its middle, or by its bottom edge that starts below it; its box again)
void ownInk(std::vector<char> &ink, QSize size, QRect *box)
{
    const int W = size.width(), H = size.height();
    if (W < 2 || H < 4)
        return;
    std::vector<char> seen(ink.size(), 0);
    std::vector<size_t> piece;
    for (size_t start = 0; start < ink.size(); ++start) {
        if (!ink[start] || seen[start])
            continue;
        piece.clear();
        std::vector<size_t> todo{start};
        seen[start] = 1;
        int y0 = H, y1 = -1;
        while (!todo.empty()) {
            const size_t i = todo.back();
            todo.pop_back();
            piece.push_back(i);
            const int x = int(i % size_t(W)), y = int(i / size_t(W));
            y0 = std::min(y0, y), y1 = std::max(y1, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H)
                        continue;
                    const size_t j = size_t(ny) * size_t(W) + size_t(nx);
                    if (ink[j] && !seen[j])
                        seen[j] = 1, todo.push_back(j);
                }
        }
        if ((y0 == 0 && y1 < H / 2) || (y1 == H - 1 && y0 > H / 2))
            for (size_t i : piece)
                ink[i] = 0;
    }
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (ink[size_t(y) * size_t(W) + size_t(x)])
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    *box = x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
}

// a line's size and baseline made exact: the line drawn in its font, as wide as the scan's (its ink's ends where the scan's
// are - that doesn't change with its size), laid over the scan's at sizes and heights round the first guess - the one most
// like it, by the correlation of their ink (in columns a tenth of an em wide: its shapes, a pixel or two off no matter). A
// first guess from the line's ink box is out where its box took in the line above's or below's marks
// (returns how alike they are then: the correlation, -1..1)
double refine(Fit &fit, const QString &text, const QString &family, bool bold, const QImage &grey, const QRect &box)
{
    const double em = fit.size * kDpi / 72;
    if (em < 6 || fit.advance <= 0 || text.trimmed().isEmpty())
        return -1;
    // (the line drawn at its first size: its pen where it's fitted, its advance the scan's - rows round its baseline)
    QFont f(family);
    f.setPointSizeF(em);  // (at 72 dots an inch: a point a pixel)
    f.setBold(bold);
    f.setStyleStrategy(QFont::PreferAntialias);
    const double natural = QFontMetricsF(f).horizontalAdvance(text);
    if (natural <= 0)
        return -1;
    const int up = int(std::ceil(1.4 * em)), down = int(std::ceil(0.8 * em));
    QImage drawn(box.width(), up + down, QImage::Format_Grayscale8);
    drawn.setDotsPerMeterX(2835), drawn.setDotsPerMeterY(2835);
    drawn.fill(255);
    {
        QPainter p(&drawn);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.translate(fit.penX, up);
        p.scale(fit.advance / natural, 1);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(QPointF(0, 0), text);
    }
    // (the scan round the line: its darkness, paper 0 to ink 1 - in columns)
    const int col = std::max(2, int(std::lround(0.1 * em)));
    const int cols = (box.width() + col - 1) / col;
    const int top = std::max(0, box.top() - int(0.5 * em)), bottom = std::min(grey.height() - 1, box.bottom() + int(0.5 * em));
    const int rows = bottom - top + 1;
    int paper = 255, ink = 0;
    levels(grey, box, &paper, &ink);
    const double range = std::max(30, paper - ink);
    std::vector<double> scan(size_t(cols) * size_t(rows), 0), mine(size_t(cols) * size_t(up + down), 0);
    for (int y = 0; y < rows; ++y) {
        const uchar *row = grey.constScanLine(top + y);
        for (int x = 0; x < box.width(); ++x)
            scan[size_t(y) * size_t(cols) + size_t(x / col)] += std::clamp((paper - row[box.left() + x]) / range, 0.0, 1.0);
    }
    for (int y = 0; y < up + down; ++y) {
        const uchar *row = drawn.constScanLine(y);
        for (int x = 0; x < box.width(); ++x)
            mine[size_t(y) * size_t(cols) + size_t(x / col)] += (255 - row[x]) / 255.0;
    }
    const double base0 = box.top() + fit.baseline;  // (the page's pixels)
    double best = -2, bestK = 1, bestB = base0;
    std::vector<double> m(size_t(cols) * size_t(rows));
    // (a size near the first guess - its ink's height, without the lines' round it: squashed flat, a line's ink all on
    // Arabic's dark baseline is "more alike" - and wrong)
    for (double k = 0.9; k <= 1.101; k += 0.02) {
        for (double b = base0 - 0.3 * em; b <= base0 + 0.3 * em; b += 1) {
            // (the drawing made k as tall, its baseline at b: each of the scan's rows, from the drawing's row it falls on)
            double ss = 0, s2 = 0, mm = 0, m2 = 0, sm = 0;
            const double n = double(cols) * rows;
            for (int y = 0; y < rows; ++y) {
                const double src = up + (top + y - b) / k;
                const int r0 = int(std::floor(src));
                const double fr = src - r0;
                const bool in0 = r0 >= 0 && r0 < up + down, in1 = r0 + 1 >= 0 && r0 + 1 < up + down;
                const double *a = in0 ? &mine[size_t(r0) * size_t(cols)] : nullptr, *c = in1 ? &mine[size_t(r0 + 1) * size_t(cols)] : nullptr;
                const double *s = &scan[size_t(y) * size_t(cols)];
                for (int x = 0; x < cols; ++x) {
                    const double v = (a ? a[x] * (1 - fr) : 0) + (c ? c[x] * fr : 0);
                    ss += s[x], s2 += s[x] * s[x], mm += v, m2 += v * v, sm += s[x] * v;
                }
            }
            const double cov = sm - ss * mm / n, vs = s2 - ss * ss / n, vm = m2 - mm * mm / n;
            const double r = vs > 0 && vm > 0 ? cov / std::sqrt(vs * vm) : -1;
            if (r > best)
                best = r, bestK = k, bestB = b;
        }
    }
    if (best < 0.2)
        return best;  // (like nothing: the first guess kept)
    fit.size *= bestK;
    fit.baseline = bestB - box.top();
    return best;
}

struct Read {
    Document::OcrLine line;
    int block = 0, para = 0;
    int group = 0;  // (the lines set together on the page - stacked close, one above the other, as big: one typeface)
    QPoint base0, base1;  // (its baseline as Tesseract found it, the scan's pixels)
    bool arabic = false;
    QRect box;                                    // (the scan's pixels)
    std::vector<std::pair<QString, QRect>> words;  // (each word kept, its box in the line's)
    std::vector<float> wordSure;                   // (... how sure of each)
    double sure = 0;                               // (how sure Tesseract is of it, 0..100)
    bool wordy = true;                             // (words of letters - not a few marks, a lone number)
    std::map<std::pair<QString, bool>, Fit> fits;  // (each font and boldness tried)
};

// one page read: its lines, in the fonts matched (empty: none - error set if it couldn't be read)
// (the scan's fonts being made: one for each font file - and boldness - its lines were matched to)
using Builders = std::map<QString, std::unique_ptr<scanfont::Builder>>;

QString builderKey(const fonts::Found &f, bool bold)
{
    return f.path + QLatin1Char('#') + QString::number(f.index) + (bold ? QStringLiteral("b") : QStringLiteral("r"));
}

// (a line's greys: its paper's - most of it - and its ink's darkest)
void levels(const QImage &grey, const QRect &r, int *paper, int *ink)
{
    int hist[256] = {};
    int n = 0;
    const QRect a = r.intersected(grey.rect());
    for (int y = a.top(); y <= a.bottom(); ++y) {
        const uchar *row = grey.constScanLine(y);
        for (int x = a.left(); x <= a.right(); ++x)
            ++hist[row[x]], ++n;
    }
    auto at = [&](double share) {
        int sum = 0;
        for (int k = 0; k < 256; ++k)
            if ((sum += hist[k]) >= share * n)
                return k;
        return 255;
    };
    *paper = at(0.7);
    *ink = at(0.04);
}

// (n jobs done on the computer's cores - job(i, thread): each thread with its own things to work with)
template <typename Job>
void inParallel(size_t n, int threads, Job job)
{
    if (threads <= 1) {  // (one: here - Qt's fonts are slow to set up in another thread)
        for (size_t i = 0; i < n; ++i)
            job(i, 0);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (size_t i; (i = next++) < n;)
                job(i, t);
        });
    for (std::thread &th : pool)
        th.join();
}

// the page's ink in pieces (each mark: its box, its pixels' count) - 8 neighbours touching
struct Blob {
    QRect box;
    int area = 0;
};

std::vector<Blob> blobsOf(const std::vector<char> &ink, int W, int H)
{
    std::vector<Blob> out;
    std::vector<char> seen(ink.size(), 0);
    std::vector<int> todo;
    for (int start = 0; start < W * H; ++start) {
        if (!ink[size_t(start)] || seen[size_t(start)])
            continue;
        Blob b;
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        todo.assign(1, start);
        seen[size_t(start)] = 1;
        while (!todo.empty()) {
            const int i = todo.back();
            todo.pop_back();
            ++b.area;
            const int x = i % W, y = i / W;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H)
                        continue;
                    const int j = ny * W + nx;
                    if (ink[size_t(j)] && !seen[size_t(j)])
                        seen[size_t(j)] = 1, todo.push_back(j);
                }
        }
        b.box = QRect(QPoint(x0, y0), QPoint(x1, y1));
        out.push_back(b);
    }
    return out;
}

// (a line's picture with its thin strokes taken out - a signature's pen across the print: opened, the print's
// heavier strokes kept)
QImage withoutThinStrokes(const QImage &grey, int stroke)
{
    QRect box;
    const std::vector<char> ink = inkOf(grey, &box);
    const int W = grey.width(), H = grey.height(), r = std::max(1, int(std::lround(0.35 * stroke)));
    auto erodeOrDilate = [&](const std::vector<char> &in, bool erode) {
        std::vector<char> rows(in.size()), out(in.size());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                bool v = erode;
                for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); ++k)
                    v = erode ? (v && in[size_t(y * W + k)]) : (v || in[size_t(y * W + k)]);
                rows[size_t(y * W + x)] = v;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                bool v = erode;
                for (int k = std::max(0, y - r); k <= std::min(H - 1, y + r); ++k)
                    v = erode ? (v && rows[size_t(k * W + x)]) : (v || rows[size_t(k * W + x)]);
                out[size_t(y * W + x)] = v;
            }
        return out;
    };
    const std::vector<char> opened = erodeOrDilate(erodeOrDilate(ink, true), false);
    QImage out(grey.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < H; ++y) {
        uchar *row = out.scanLine(y);
        const uchar *src = grey.constScanLine(y);
        for (int x = 0; x < W; ++x)
            row[x] = opened[size_t(y * W + x)] ? src[x] : 255;
    }
    return out;
}

// ---- digits: Tesseract reads Arabic-Indic digits poorly ("١٤٤٧" as "١447", a phone number as "VevAeny"). A word
// of digits, Latin letters or signs: its marks matched against the digits drawn in the usual fonts - and Latin
// letters: the reading it's most like (a word of letters stays one)

constexpr int kNorm = 32;  // (a mark's shape: its ink's box scaled to this many pixels, its shape kept)

struct Shape {
    QString ch;
    std::vector<float> px;  // (kNorm x kNorm)
    double aspect = 1;      // (width over height)
    double height = 1;      // (over its font's digits' height)
    double middle = 0.5;    // (its middle: how high, over the digits' height, from the baseline)
    int kind = 0;           // 0 a sign, 1 an Arabic-Indic digit, 2 a Western digit, 3 a Latin letter
    int holes = 0;          // (paper it closes round: ٥ ٩ one; ٢ ٣ ٤ ٨ none)
    int teeth = 0;          // (strokes a line across its top quarter crosses: ٣ three, ٢ two)
};

// (a mark's holes and teeth - in its ink as it is)
void topology(const std::vector<char> &ink, int W, const QRect &box, int *holes, int *teeth)
{
    const int w = box.width() + 2, h = box.height() + 2;
    std::vector<char> paper(size_t(w) * size_t(h), 1);
    for (int y = 0; y < box.height(); ++y)
        for (int x = 0; x < box.width(); ++x)
            paper[size_t(y + 1) * size_t(w) + size_t(x + 1)] = !ink[size_t(box.top() + y) * size_t(W) + size_t(box.left() + x)];
    std::vector<char> seen(paper.size(), 0);
    int pieces = 0;
    std::vector<int> todo;
    for (int start = 0; start < w * h; ++start) {
        if (!paper[size_t(start)] || seen[size_t(start)])
            continue;
        ++pieces;
        todo.assign(1, start);
        seen[size_t(start)] = 1;
        int n = 0;
        while (!todo.empty()) {
            const int i = todo.back();
            todo.pop_back();
            ++n;
            const int x = i % w, y = i / w;
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k)
                if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < w && ny[k] < h) {
                    const int j = ny[k] * w + nx[k];
                    if (paper[size_t(j)] && !seen[size_t(j)])
                        seen[size_t(j)] = 1, todo.push_back(j);
                }
        }
        if (n < 2 && pieces > 1)
            --pieces;  // (a pixel's gap: not a hole)
    }
    *holes = std::max(0, pieces - 1);  // (the paper round it is one piece)
    *teeth = 0;
    for (const double at : {0.15, 0.25, 0.35}) {
        const int y = box.top() + int(at * box.height());
        int runs = 0;
        bool in = false;
        for (int x = box.left(); x <= box.right(); ++x) {
            const bool v = ink[size_t(y) * size_t(W) + size_t(x)];
            runs += v && !in ? 1 : 0;
            in = v;
        }
        *teeth = std::max(*teeth, runs);
    }
}

std::vector<float> normalised(const std::vector<char> &ink, int W, const QRect &box)
{
    std::vector<float> out(size_t(kNorm * kNorm), 0);
    const double scale = double(kNorm - 2) / std::max(box.width(), box.height());
    const double ox = (kNorm - box.width() * scale) / 2, oy = (kNorm - box.height() * scale) / 2;
    for (int y = 0; y < kNorm; ++y)
        for (int x = 0; x < kNorm; ++x) {
            // (each pixel of the shape: the share of ink under it)
            const double sx0 = box.left() + (x - ox) / scale, sy0 = box.top() + (y - oy) / scale;
            const double sx1 = sx0 + 1 / scale, sy1 = sy0 + 1 / scale;
            int n = 0, inked = 0;
            for (int sy = int(std::floor(sy0)); sy < int(std::ceil(sy1)); ++sy)
                for (int sx = int(std::floor(sx0)); sx < int(std::ceil(sx1)); ++sx) {
                    if (sx < box.left() || sy < box.top() || sx > box.right() || sy > box.bottom())
                        continue;
                    ++n;
                    inked += ink[size_t(sy) * size_t(W) + size_t(sx)] ? 1 : 0;
                }
            out[size_t(y * kNorm + x)] = n ? float(inked) / n : 0.0f;
        }
    return out;
}

// (drawn at px pixels to the em - as big as the marks matched: as blurred, as stepped as theirs)
const std::vector<Shape> &shapes(int px)
{
    static std::map<int, std::vector<Shape>> made;
    std::vector<Shape> &all = made[px];
    if (!all.empty())
        return all;
    const int iw = int(px * 2.2) + 20, ih = int(px * 2.5) + 20;
    const double baseY = px * 1.75 + 10, penX = px * 0.4 + 10;
    const QString arabicDigits = QString::fromUtf8("٠١٢٣٤٥٦٧٨٩");
    const QString western = QStringLiteral("0123456789");
    const QString letters = QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    const QString signs = QStringLiteral("/-.:,()+");
    const QStringList have = QFontDatabase::families();
    struct Set {
        QStringList fonts;
        QString chars;
        int kind;
        QChar ref;
    };
    const std::vector<Set> sets = {
        {allFamilies(true),
         arabicDigits,
         1,
         QChar(0x661)},
        // (a code's letter among its digits - "١أ-١٢٤": alef with its hamza, not taken for a "١")
        {allFamilies(true), QString::fromUtf8("أإ"), 4, QChar(0x661)},
        {{"Arial", "Times New Roman", "Tahoma", "Calibri", "Verdana", "Segoe UI"}, western + signs, 2, QLatin1Char('1')},
        {{"Arial", "Times New Roman", "Tahoma", "Calibri", "Verdana", "Segoe UI"}, letters, 3, QLatin1Char('1')},
    };
    for (const Set &set : sets)
        for (const QString &family : set.fonts) {
            if (!have.contains(family))
                continue;
            for (const bool bold : {false, true}) {
                QFont f(family);
                f.setPixelSize(px);
                f.setBold(bold);
                auto drawn = [&](const QString &ch, QRect *box, std::vector<char> *ink, double *base) {
                    QImage im(iw, ih, QImage::Format_Grayscale8);
                    im.fill(255);
                    QPainter p(&im);
                    p.setFont(f);
                    p.setPen(Qt::black);
                    p.setRenderHint(QPainter::TextAntialiasing);
                    p.drawText(QPointF(penX, baseY), ch);
                    p.end();
                    *ink = std::vector<char>(size_t(iw * ih), 0);
                    int x0 = iw, y0 = ih, x1 = -1, y1 = -1;
                    for (int y = 0; y < ih; ++y)
                        for (int x = 0; x < iw; ++x)
                            if (im.constScanLine(y)[x] < 128) {
                                (*ink)[size_t(y * iw + x)] = 1;
                                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                            }
                    *box = x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
                    *base = baseY;
                };
                QRect refBox;
                std::vector<char> refInk;
                double base = 0;
                drawn(QString(set.ref), &refBox, &refInk, &base);
                if (refBox.isEmpty())
                    continue;
                const double refH = refBox.height();
                for (const QChar c : set.chars) {
                    QRect box;
                    std::vector<char> ink;
                    drawn(QString(c), &box, &ink, &base);
                    if (box.isEmpty())
                        continue;
                    Shape sh;
                    sh.ch = QString(c);
                    sh.px = normalised(ink, iw, box);
                    topology(ink, iw, box, &sh.holes, &sh.teeth);
                    sh.aspect = double(box.width()) / box.height();
                    sh.height = box.height() / refH;
                    sh.middle = (base - (box.top() + box.bottom()) / 2.0) / refH;
                    sh.kind = set.kind == 2 && signs.contains(c) ? 0 : set.kind;
                    all.push_back(std::move(sh));
                }
            }
        }
    return all;
}

// (how alike a mark is to a shape: their ink in common, less for a mark of another build, height, place)
double alike(const std::vector<float> &px, double aspect, double height, double middle, int holes, int teeth, const Shape &s)
{
    double both = 0, either = 0;
    for (size_t i = 0; i < px.size(); ++i)
        both += std::min(px[i], s.px[i]), either += std::max(px[i], s.px[i]);
    const double iou = either > 0 ? both / either : 0;
    return iou - 0.25 * std::abs(std::log(std::max(0.05, aspect) / std::max(0.05, s.aspect))) - 0.8 * std::abs(height - s.height) -
           0.6 * std::abs(middle - s.middle) - 0.04 * std::abs(holes - s.holes) - 0.06 * std::abs(teeth - s.teeth);
}

void fixDigits(std::vector<ocr::Line> &lines, const std::vector<char> &ink, int W, const std::vector<Blob> &blobs)
{
    if (shapes(64).empty())
        return;
    for (ocr::Line &line : lines) {
        bool arabicLine = false;
        for (const QChar c : line.text)
            arabicLine = arabicLine || (c.unicode() >= 0x620 && c.unicode() <= 0x64A) || (c.unicode() >= 0x660 && c.unicode() <= 0x669);
        for (ocr::Word &w : line.words) {
            bool letterArabic = false, digit = false, any = false;
            int arabicLetters = 0, digits = 0;
            for (const QChar c : w.text) {
                letterArabic = letterArabic || (c.unicode() >= 0x620 && c.unicode() <= 0x64A);
                arabicLetters += c.unicode() >= 0x620 && c.unicode() <= 0x64A ? 1 : 0;
                digit = digit || c.isDigit();
                digits += c.isDigit() ? 1 : 0;
                any = any || c.isLetterOrNumber();
            }
            // (a code - digits and a letter or two, "١أ-١٢٤" - as a number)
            if (letterArabic && digits >= 2 && arabicLetters <= 2 && w.confidence < 80)
                letterArabic = false;
            // (a short word of "letters", unsure, beside a number: maybe digits read as letters - a phone number's "٠١٧"
            // read "لاد": tried as digits, taken if its marks are more like them)
            bool besideNumber = false;
            if (letterArabic && arabicLetters <= 3 && digits == 0 && w.confidence < 70 && arabicLine) {
                const qsizetype i = &w - &line.words[0];
                for (const qsizetype j : {i - 1, i + 1}) {
                    if (j < 0 || j >= qsizetype(line.words.size()))
                        continue;
                    int nd = 0, other = 0;
                    for (const QChar c : line.words[size_t(j)].text)
                        (c.isDigit() ? nd : other) += c.isLetter() ? 1 : (c.isDigit() ? 1 : 0);
                    besideNumber = besideNumber || (nd >= 3 && other == 0);
                }
                if (besideNumber)
                    letterArabic = false;
            }
            // (a sign alone that isn't one Arabic or English has - "©", "®" - read unsurely in an Arabic line: an icon's marks, a
            // phone's: not text)
            if (arabicLine && w.confidence < 70 && w.text.trimmed().size() == 1 &&
                (w.text.trimmed()[0].category() == QChar::Symbol_Other || w.text.trimmed()[0].category() == QChar::Symbol_Math)) {
                w.text.clear();
                w.confidence = -1;
                continue;
            }
            if (letterArabic || !any || (!arabicLine && !digit && w.confidence >= 70))
                continue;  // (an Arabic word; a sign alone - a full stop; an English line's word of letters)
            bool surely = true;  // (a number of Western digits read surely: as it is - two of its digits run together
            for (const QChar c : w.text)  // by the scan's blur are one mark, a digit lost)
                surely = surely && ((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringLiteral("/.,:-").contains(c));
            if (surely && w.confidence >= 85)
                continue;
            // its marks: the page's ink pieces in it - pieces one above another (a colon's dots) as one
            std::vector<QRect> marks;
            for (const Blob &b : blobs)
                if (w.box.adjusted(-2, -2, 2, 2).contains(b.box.center()) && b.area >= 6)
                    marks.push_back(b.box);
            std::sort(marks.begin(), marks.end(), [](const QRect &a, const QRect &b) { return a.left() < b.left(); });
            for (size_t k = 1; k < marks.size();) {
                const QRect &a = marks[k - 1], &b = marks[k];
                const int over = std::min(a.right(), b.right()) - std::max(a.left(), b.left());
                if (over > 0.5 * std::min(a.width(), b.width())) {
                    marks[k - 1] = a | b;
                    marks.erase(marks.begin() + long(k));
                } else {
                    ++k;
                }
            }
            if (marks.empty() || marks.size() > 30)
                continue;
            // (the digits' height and baseline: its tall marks')
            std::vector<int> hs, bottoms;
            for (const QRect &m : marks)
                hs.push_back(m.height());
            std::sort(hs.begin(), hs.end());
            const double tall = hs[size_t(hs.size() * 0.8 > 0 ? std::min(hs.size() - 1, size_t(hs.size() * 0.8)) : 0)];
            if (tall < 6)
                continue;
            for (const QRect &m : marks)
                if (m.height() > 0.7 * tall)
                    bottoms.push_back(m.bottom());
            std::sort(bottoms.begin(), bottoms.end());
            const double base = bottoms.empty() ? marks.front().bottom() : bottoms[bottoms.size() / 2];
            // (the shapes drawn as big as these digits: a font's digits stand about 0.7 of its size)
            int px = 64;
            for (const int size : {12, 16, 20, 24, 28, 34, 40, 48, 64})
                if (std::abs(size * 0.7 - tall) < std::abs(px * 0.7 - tall))
                    px = size;
            const std::vector<Shape> &all = shapes(px);
            // each mark: the best digit (or sign) and the best letter (or sign)
            struct Mark {
                std::vector<float> px;
                double aspect, height, middle;
                int holes, teeth;
            };
            std::vector<Mark> ms;
            for (const QRect &m : marks) {
                Mark mk{normalised(ink, W, m), double(m.width()) / m.height(), m.height() / tall, (base - (m.top() + m.bottom()) / 2.0) / tall, 0, 0};
                topology(ink, W, m, &mk.holes, &mk.teeth);
                ms.push_back(std::move(mk));
            }
            auto best = [&](const Mark &m, auto allowed, double *score) -> const Shape * {
                const Shape *b = nullptr;
                *score = -9;
                for (const Shape &sh : all)
                    if (allowed(sh)) {
                        const double sc = alike(m.px, m.aspect, m.height, m.middle, m.holes, m.teeth, sh);
                        if (sc > *score)
                            *score = sc, b = &sh;
                    }
                return b;
            };
            double sumD = 0, sumL = 0;
            int arabic = 0, western = 0;
            for (const Mark &m : ms) {
                double sd = 0, sl = 0;
                const Shape *d = best(m, [](const Shape &sh) { return sh.kind != 3; }, &sd);
                best(m, [](const Shape &sh) { return sh.kind == 3 || sh.kind == 0; }, &sl);
                if (!d)
                    break;
                sumD += sd;
                sumL += sl;
                arabic += d->kind == 1 ? 1 : 0;
                western += d->kind == 2 ? 1 : 0;
            }
            const double meanD = sumD / ms.size(), meanL = sumL / ms.size();
            if (arabicLine && std::max(meanD, meanL) < 0.55 && ms.size() <= 3 && w.confidence < 85) {
                w.text.clear();  // (like no letter, no digit: an icon's marks read - a phone's, an envelope's)
                w.confidence = -1;  // (an icon: not read again as a word)
                continue;
            }
            if (arabic + western == 0)
                continue;
            // (digits, not letters: clearly so - in an Arabic line, as likely: its Latin "letters" are digits read wrong; beside a
            // number, a little less like them too)
            if (meanD < 0.45 || meanD < meanL + (besideNumber ? -0.05 : arabicLine && arabic >= western ? -0.02 : 0.03))
                continue;
            // (one kind of digits - the kind most of them are: each mark the best of those, or a sign)
            const int kind = arabic >= western ? 1 : 2;
            QString asDigits;
            bool letter = false;  // (an Arabic letter among them: a code - "١أ-١٢٤")
            for (const Mark &m : ms) {
                double sc = 0;
                const Shape *d = best(m, [kind, arabicLine](const Shape &sh) { return sh.kind == kind || sh.kind == 0 || (arabicLine && sh.kind == 4); }, &sc);
                asDigits += d ? d->ch : QString();
                letter = letter || (d && d->kind == 4);
            }
            if (asDigits.size() != int(ms.size()))
                continue;
            if (letter) {  // (read from the right: its pieces right to left - a run of digits left to right inside)
                std::vector<QString> tokens;
                for (const QChar c : asDigits)
                    if (c.isDigit() && !tokens.empty() && tokens.back().back().isDigit())
                        tokens.back() += c;
                    else
                        tokens.push_back(QString(c));
                std::reverse(tokens.begin(), tokens.end());
                asDigits.clear();
                for (const QString &t : tokens)
                    asDigits += t;
            }
            line.text.replace(w.text, asDigits);
            w.text = asDigits;
            w.confidence = std::max(w.confidence, float(60 + 40 * std::min(1.0, meanD)));
        }
    }
}

// an Arabic word's last sign read wrong - Tesseract reads Arabic's comma "،" as "»", "؛", ":", "." or ",": the marks at its
// end (left of its letters, apart from them) told by their ink - one mark, as tall as a third or more of the line's letters,
// on or over its baseline: a comma; one small and round on it: a full stop; two side by side: "»" - as read
void fixMarks(std::vector<ocr::Line> &lines, const std::vector<Blob> &blobs, const std::vector<char> &ink, int W)
{
    static const QString signs = QString::fromUtf8("»«،؛.:,");
    for (ocr::Line &l : lines) {
        // (Arabic: its words as they're read now - a word read again in Arabic, the line's own text Latin junk)
        if (l.words.empty() || std::none_of(l.words.begin(), l.words.end(), [](const ocr::Word &w) { return arabic(w.text); }))
            continue;
        auto baseAt = [&](double x) {
            return l.baseEnd.x() != l.baseStart.x()
                       ? l.baseStart.y() + (l.baseEnd.y() - l.baseStart.y()) * (x - l.baseStart.x()) / double(l.baseEnd.x() - l.baseStart.x())
                       : double(l.baseStart.y());
        };
        std::vector<double> tops;  // (its letters' height: baseline to the top of its words)
        for (const ocr::Word &w : l.words)
            if (arabic(w.text))
                tops.push_back(baseAt(w.box.center().x()) - w.box.top());
        if (tops.empty())
            continue;
        std::nth_element(tops.begin(), tops.begin() + long(tops.size() / 2), tops.end());
        double letterH = tops[tops.size() / 2];
        {  // (Tesseract's baseline off - a short piece's: its words' own height, as tall as its letters go)
            std::vector<int> hs;
            for (const ocr::Word &w : l.words)
                if (arabic(w.text))
                    hs.push_back(w.box.height());
            std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
            const double own = 0.8 * hs[hs.size() / 2];
            if (letterH < 0.5 * own || letterH > 1.6 * own)
                letterH = own;
        }
        if (letterH < 6)
            continue;
        for (ocr::Word &w : l.words) {
            // (a flat little "word" of letters, not read surely - a dash read "YT": a dash)
            if (w.confidence < 50 && w.box.height() < 0.3 * letterH && w.box.width() > 2 * w.box.height() &&
                std::any_of(w.text.begin(), w.text.end(), [](QChar c) { return c.isLetter(); })) {
                w.text = QStringLiteral("-");
                continue;
            }
            int k = 0;  // (its signs at the end)
            while (k < w.text.size() && signs.contains(w.text[w.text.size() - 1 - k]))
                ++k;
            // (a "١" alone, much shorter than the letters: a sign's dots read as one - a colon)
            const QString alone = w.text.trimmed();
            if (alone.size() == 1 && QStringLiteral("١1|lI!").contains(alone[0]) && w.box.height() < 0.5 * letterH)
                k = int(w.text.size());
            if (k == 0)
                continue;
            std::vector<QRect> in;
            // (its marks: the ink in it - across the line's letters' height, not just its box: Tesseract's box round a colon
            // read "١" took in one of its dots)
            const double wb = baseAt(w.box.center().x());
            for (const Blob &b : blobs)
                if (b.area >= 4 && b.box.center().x() >= w.box.left() - 2 && b.box.center().x() <= w.box.right() + 2 &&
                    b.box.center().y() >= std::min(double(w.box.top()), wb - 1.1 * letterH) - 2 &&
                    b.box.center().y() <= std::max(double(w.box.bottom()), wb + 0.3 * letterH) + 2)
                    in.push_back(b.box);
            if (in.empty())
                continue;
            std::sort(in.begin(), in.end(), [](const QRect &a, const QRect &b) { return a.left() < b.left(); });
            // (the marks at its end: the leftmost and those over / under it)
            QRect mark = in.front();
            int count = 1;
            for (size_t n = 1; n < in.size(); ++n) {
                const int over = std::min(mark.right(), in[n].right()) - std::max(mark.left(), in[n].left());
                if (over >= 0.5 * std::min(mark.width(), in[n].width()))
                    mark |= in[n], ++count;
            }
            int letters = 1 << 30;  // (the word's letters start right of them, apart)
            for (const QRect &b : in)
                if (!mark.contains(b))
                    letters = std::min(letters, b.left());
            if (k < w.text.size() && letters < mark.right() + 1)
                continue;
            const double base = baseAt(mark.center().x());
            const double tall = mark.height() / letterH, wide = double(mark.width()) / std::max(1, mark.height());
            const double up = (base - mark.bottom()) / letterH;  // (its bottom over the baseline)
            // (one mark, its halves alike - as much ink above as below, one over the other: a colon's two dots run together
            // by the blur; a comma's head is at one end, its thin tail at the other)
            bool halvesAlike = false;
            if (count == 1 && mark.height() >= 6) {
                double inkTop = 0, inkBottom = 0, xTop = 0, xBottom = 0;
                const int mid = mark.top() + mark.height() / 2;
                for (int y = mark.top(); y <= mark.bottom(); ++y)
                    for (int x = mark.left(); x <= mark.right(); ++x)
                        if (ink[size_t(y) * size_t(W) + size_t(x)]) {
                            (y < mid ? inkTop : inkBottom) += 1;
                            (y < mid ? xTop : xBottom) += x;
                        }
                if (inkTop > 0 && inkBottom > 0) {
                    const double ratio = inkTop / inkBottom;
                    halvesAlike = ratio > 0.7 && ratio < 1.43 && std::abs(xTop / inkTop - xBottom / inkBottom) < 0.15 * mark.width() + 0.5;
                }
            }
            QChar is;
            if (count == 1 && halvesAlike && tall >= 0.3 && tall <= 0.8 && wide < 0.75 && up > -0.15)
                is = QLatin1Char(':');
            else if (count == 1 && tall >= 0.28 && tall <= 0.75 && up > -0.12 && wide < 0.9)
                is = QChar(0x060C);
            else if (count == 1 && tall < 0.28 && wide > 0.55 && std::abs(up) < 0.15)
                is = QLatin1Char('.');
            else if (count == 2 && tall >= 0.2 && tall <= 1.2 && wide < 0.9 && up > -0.15)  // (two dots one over the other)
                is = QLatin1Char(':');
            if (is.isNull())
                continue;
            w.text = w.text.left(w.text.size() - k) + is;
            if (k == w.text.size())  // (a sign alone, told by its shape: not a speck's junk)
                w.confidence = std::max(w.confidence, 75.0f);
        }
    }
}
void wordsFromDictionary(std::vector<ocr::Line> &lines);  // (below)


// a word read in the other script than its line's, not surely - Arabic and English read together, an Arabic word read as
// English ("معمل" as "Jara", "جهاز" as "lea"): read again on its own in the line's language, the surer reading kept
void rereadWords(std::vector<ocr::Line> &lines, const QImage &grey, const QString &langs)
{
    if (!langs.contains(QLatin1Char('+')))
        return;
    auto script = [](const QString &t, int *arab, int *latin) {
        *arab = *latin = 0;
        for (const QChar c : t)
            if (pagetext::isArabic(c.unicode()) && c.isLetter())
                ++*arab;
            else if (c.isLetter() && c.unicode() < 0x250)
                ++*latin;
    };
    ocr::Engine engines[2];  // (Arabic alone, English alone - started when first needed)
    int tried[2] = {0, 0};
    auto engine = [&](int i) -> ocr::Engine * {
        if (!tried[i]) {
            QString why;
            tried[i] = engines[i].start(i == 0 ? QStringLiteral("ara") : QStringLiteral("eng"), &why) ? 1 : -1;
        }
        return tried[i] > 0 ? &engines[i] : nullptr;
    };
    for (ocr::Line &l : lines) {
        stepIn(0.45, 0.5, double(&l - &lines[0]), double(lines.size()));
        if (!l.inText)
            continue;
        int arabSure = 0, latinSure = 0;
        for (const ocr::Word &w : l.words) {
            int a = 0, b = 0;
            script(w.text, &a, &b);
            if (w.confidence >= 80)
                arabSure += a, latinSure += b;
        }
        if (arabSure + latinSure < 4)
            continue;
        const bool arabicLine = arabSure > latinSure;
        int h = 0;  // (its words' height - the middle one's: a frame's corner read as a word is taller)
        {
            std::vector<int> hs;
            for (const ocr::Word &w : l.words)
                hs.push_back(w.box.height());
            std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
            h = hs[hs.size() / 2];
        }
        for (ocr::Word &w : l.words) {
            int a = 0, b = 0;
            script(w.text, &a, &b);
            // (the script round it: its sure neighbours' - a letterhead's Arabic and English halves read as one line - else
            // its line's)
            int na = 0, nl = 0;
            for (const ocr::Word &o : l.words) {
                if (&o == &w || o.confidence < 80)
                    continue;
                const int gap = std::max(o.box.left() - w.box.right(), w.box.left() - o.box.right());
                if (gap > 3 * h)
                    continue;
                int oa = 0, ob = 0;
                script(o.text, &oa, &ob);
                na += oa, nl += ob;
            }
            const bool arabicHere = na + nl > 0 ? na > nl : arabicLine;
            const bool other = arabicHere ? b > a : a > b;
            if (!other || w.confidence >= 88)
                continue;
            ocr::Engine *e = engine(arabicHere ? 0 : 1);
            if (!e)
                return;
            const int pad = std::max(4, h / 3);
            const QRect crop = w.box.adjusted(-pad, -pad, pad, pad).intersected(grey.rect());
            QString why;
            static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
            QStringList text;
            double sure = 0;
            int n = 0, mine = 0;
            // (as a line - psm 7; nothing in its script: as the line it is, no layout of Tesseract's - 13: a heavy display
            // face's word alone came back empty as a line)
            for (const int psm : {7, 13}) {
                if (mine > 0)
                    break;
                text.clear(), sure = 0, n = 0;
                for (const ocr::Line &r : e->read(grey.copy(crop), psm, &why))
                    for (const ocr::Word &x : r.words) {
                        // (just what's over the word itself - not a neighbour's mark the room round it took in: a digit
                        // above it, the next word)
                        const QRect at = x.box.translated(crop.topLeft());
                        const int over = std::min(at.right(), w.box.right()) - std::max(at.left(), w.box.left());
                        const int overY = std::min(at.bottom(), w.box.bottom()) - std::max(at.top(), w.box.top());
                        if (over < 0.5 * std::min(at.width(), w.box.width()) || overY < 0.5 * std::min(at.height(), w.box.height()))
                            continue;
                        if (std::none_of(x.text.begin(), x.text.end(), [](QChar c) { return c.isLetterOrNumber(); }))
                            continue;  // (a mark alone - a rule beside it read ".": not the word's)
                        text << QString(x.text).remove(marks);
                        sure += x.confidence, ++n;
                        int xa = 0, xb = 0;
                        script(x.text, &xa, &xb);
                        mine += arabicHere ? xa : xb;
                    }
            }
            // (read in its neighbours' script: taken unless it's much less sure - the other script's reading was the odd one)
            // (an unsure reading in the other script - "SL" at 45% for a heavy face's "الأسنان" - isn't one to keep over it)
            if (n == 0 || mine == 0 || sure / n < 30 || (sure / n < w.confidence - 10 && w.confidence >= 50))
                continue;
            const bool hadDigits = std::any_of(w.text.begin(), w.text.end(), [](QChar c) { return c.isDigit(); });
            w.text = text.join(QLatin1Char(' '));
            if (!hadDigits && mine >= 3)  // (a word of letters: a digit above it - "٠٣٨"'s - read into it, not its)
                w.text.removeIf([](QChar c) { return c.isDigit(); });
            if (sure / n < 50) {  // (unsure: its vowel marks - a heavy face's dots taken for tanween - left out, as for any word)
                static const QRegularExpression harakat(QStringLiteral("[\\x{064B}-\\x{065F}\\x{0670}]"));
                w.text.remove(harakat);
            }
            w.confidence = std::max(w.confidence, float(sure / n));  // (taken as the better reading: no less sure than the one it replaced)
        }
    }
}

// a number Tesseract read as two words - a date's first digit apart: "١" and "٤٤٧/٤/٢١" (Arabic's "١" is a thin stroke with
// room round it) - one word again, its pieces left to right: a digit alone beside a date, or close beside a number
// an Arabic word read unsurely, its letters' dots the likeliest mistake (a heavy face's, a blurred scan's dots run
// together: "الأسثان"): the same word read surely elsewhere on the page - the same letters without their dots, as many -
// gives them (its own hamza kept: "الأسنان" from the body's "الاسنان")
void dotsFromPage(std::vector<ocr::Line> &lines)
{
    static const QString alefs = QStringLiteral("\u0627\u0623\u0625\u0622");
    // (each letter as it's drawn without its dots - its skeleton; a final ن, ي, ق drawn unlike ب's, ف's)
    auto skeleton = [](const QString &word, QString *letters) {
        static const std::vector<std::pair<QString, char>> groups = {
            {QStringLiteral("\u0627\u0623\u0625\u0622\u0671"), 'A'}, {QStringLiteral("\u0628\u062A\u062B"), 'B'},
            {QStringLiteral("\u062C\u062D\u062E"), 'H'}, {QStringLiteral("\u062F\u0630"), 'D'}, {QStringLiteral("\u0631\u0632"), 'R'},
            {QStringLiteral("\u0633\u0634"), 'S'}, {QStringLiteral("\u0635\u0636"), 'C'}, {QStringLiteral("\u0637\u0638"), 'T'},
            {QStringLiteral("\u0639\u063A"), 'E'}, {QStringLiteral("\u0647\u0629"), 'O'}, {QStringLiteral("\u0648\u0624"), 'W'}};
        letters->clear();
        for (const QChar c : word)
            if (pagetext::isArabic(c.unicode()) && c.isLetter() && c.unicode() != 0x0640)
                letters->append(c);
        QString out;
        for (int i = 0; i < letters->size(); ++i) {
            const QChar c = letters->at(i);
            const bool last = i == letters->size() - 1;
            QChar k = c;
            for (const auto &[set, key] : groups)
                if (set.contains(c))
                    k = QLatin1Char(key);
            if (c == QChar(0x0646) || QStringLiteral("\u064A\u0649\u0626").contains(c))  // (ن, ي: ب's tooth, but at the end)
                k = last ? (c == QChar(0x0646) ? QLatin1Char('N') : QLatin1Char('Y')) : QLatin1Char('B');
            if ((c == QChar(0x0641) || c == QChar(0x0642)) && !last)
                k = QLatin1Char('F');
            out += k;
        }
        return out;
    };
    auto sameDots = [](const QString &a, const QString &b) {  // (but for an alef's hamza)
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i] != b[i] && !(alefs.contains(a[i]) && alefs.contains(b[i])))
                return false;
        return true;
    };
    QHash<QString, QString> sure;  // (skeleton -> the letters read surely; empty: two readings that differ - left)
    for (const ocr::Line &l : lines)
        for (const ocr::Word &w : l.words) {
            QString letters;
            const QString k = skeleton(w.text, &letters);
            if (w.confidence < 90 || letters.size() < 4)
                continue;
            auto it = sure.find(k);
            if (it == sure.end())
                sure.insert(k, letters);
            else if (!sameDots(*it, letters))
                it->clear();
        }
    for (ocr::Line &l : lines)
        for (ocr::Word &w : l.words) {
            QString letters;
            const QString k = skeleton(w.text, &letters);
            if (w.confidence >= 85 || letters.size() < 4 || letters.size() != w.text.size())
                continue;  // (sure, short - or with marks, digits: left as read)
            const QString page = sure.value(k);
            if (page.isEmpty() || sameDots(page, letters))
                continue;
            QString fixed = letters;
            for (int i = 0; i < fixed.size(); ++i)
                if (!alefs.contains(fixed[i]))
                    fixed[i] = page[i];
            w.text = fixed;
        }
}

// an Arabic word read unsurely that's no word - small print, a display face: "الرقو", "المرققات": the dictionary's word a letter
// off it (Tesseract's own, its model's), if one is clearly nearest - a letter like it in the print the nearest: one only its
// dots apart (ق ف, ب ت ن), or drawn alike (a final م and و, ه and ة); another letter, one more or less, further. None, or
// two as near: left as read
void wordsFromDictionary(std::vector<ocr::Line> &lines)
{
    const QSet<QString> &words = ocr::dictionary(QStringLiteral("ara"));
    if (words.isEmpty())
        return;
    static const std::vector<QString> dots = {
        QStringLiteral("\u0628\u062A\u062B\u0646\u064A\u0626"), QStringLiteral("\u062C\u062D\u062E"), QStringLiteral("\u062F\u0630"),
        QStringLiteral("\u0631\u0632"), QStringLiteral("\u0633\u0634"), QStringLiteral("\u0635\u0636"), QStringLiteral("\u0637\u0638"),
        QStringLiteral("\u0639\u063A"), QStringLiteral("\u0641\u0642"), QStringLiteral("\u0647\u0629"), QStringLiteral("\u064A\u0649"),
        QStringLiteral("\u0627\u0623\u0625\u0622")};
    static const std::vector<QString> alike = {QStringLiteral("\u0645\u0648"),
                                               QStringLiteral("\u0644\u0627")};
    auto cost = [](QChar a, QChar b) {
        for (const QString &g : dots)
            if (g.contains(a) && g.contains(b))
                return 0.3;
        for (const QString &g : alike)
            if (g.contains(a) && g.contains(b))
                return 0.6;
        return 1.0;
    };
    auto isLetter = [](QChar c) { return c.unicode() >= 0x0621 && c.unicode() <= 0x064A && c.unicode() != 0x0640; };
    for (ocr::Line &l : lines)
        for (ocr::Word &w : l.words) {
            if (w.confidence >= 70)
                continue;
            // (its letters - signs round them kept as they are)
            int a = 0, z = int(w.text.size());
            while (a < z && !isLetter(w.text[a]))
                ++a;
            while (z > a && !isLetter(w.text[z - 1]))
                --z;
            const QString core = w.text.mid(a, z - a);
            if (core.size() < 4 || words.contains(core) || std::any_of(core.begin(), core.end(), [&](QChar c) { return !isLetter(c); }))
                continue;
            double best = 9, second = 9;
            QString found;
            auto consider = [&](const QString &c, double k) {
                if (!words.contains(c))
                    return;
                if (c == found) {
                    best = std::min(best, k);
                    return;
                }
                if (k < best) {
                    second = best;
                    best = k;
                    found = c;
                } else {
                    second = std::min(second, k);
                }
            };
            for (int i = 0; i < core.size(); ++i) {
                for (char16_t u = 0x0621; u <= 0x064A; ++u) {
                    const QChar ch(u);
                    if (ch == core[i] || u == 0x0640)
                        continue;
                    QString c = core;
                    c[i] = ch;
                    consider(c, cost(core[i], ch));
                }
                consider(QString(core).remove(i, 1), 1.0);  // (a letter too many: a mark read as one)
            }
            for (int i = 0; i <= core.size(); ++i)  // (... too few)
                for (char16_t u = 0x0621; u <= 0x064A; ++u)
                    if (u != 0x0640)
                        consider(QString(core).insert(i, QChar(u)), 1.0);
            if (found.isEmpty() || best > 0.6 || second - best < 0.3)
                continue;
            w.text = w.text.left(a) + found + w.text.mid(z);
        }
}

// a word not read surely once the digits are told (fixDigits) - small print among other things on its row: read again alone
void rereadUnsure(std::vector<ocr::Line> &lines, const QImage &grey, const QString &langs)
{
    if (!langs.contains(QLatin1Char('+')))
        return;
    auto script = [](const QString &t, int *arab, int *latin) {
        *arab = *latin = 0;
        for (const QChar c : t)
            if (pagetext::isArabic(c.unicode()) && c.isLetter())
                ++*arab;
            else if (c.isLetter() && c.unicode() < 0x250)
                ++*latin;
    };
    ocr::Engine engines[2];
    int tried[2] = {0, 0};
    auto engine = [&](int i) -> ocr::Engine * {
        if (!tried[i]) {
            QString why;
            tried[i] = engines[i].start(i == 0 ? QStringLiteral("ara") : QStringLiteral("eng"), &why) ? 1 : -1;
        }
        return tried[i] > 0 ? &engines[i] : nullptr;
    };
    // a word not read surely - small print among other things on its row (a letterhead's footer: "الرقم" read "Ady",
    // "المرفقات" "المرققات", "١٤ هـ" nothing): read again alone, as a word - its line's script, else the page's; the surer
    // reading kept
    const bool pageArabic = langs.startsWith(QLatin1String("ara"));
    static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
    for (ocr::Line &l : lines) {
        if (!l.inText)
            continue;
        int arab = 0, latin = 0;
        for (const ocr::Word &w : l.words) {
            int a = 0, b = 0;
            script(w.text, &a, &b);
            if (w.confidence >= 80)
                arab += a, latin += b;
        }
        const bool arabicHere = arab + latin >= 4 ? arab > latin : pageArabic;
        for (ocr::Word &w : l.words) {
            if (scanCancel)
                throw ScanCancelled();  // (each word read again takes a while: the X answered between them)
            if (w.confidence >= 65 || w.confidence < 0 || w.box.width() < 8 || w.box.height() < 8)
                continue;
            ocr::Engine *e = engine(arabicHere ? 0 : 1);
            if (!e)
                return;
            const int pad = std::max(4, w.box.height() / 3);
            const QRect crop = w.box.adjusted(-pad, -pad, pad, pad).intersected(grey.rect());
            QString why, best;
            double bestSure = w.confidence + 10;  // (clearly surer than it was)
            // (digits taken for a word that had digits, or nothing - not for letters: a logo's stylised name read as a number)
            const bool numbersToo = w.text.trimmed().isEmpty() || std::any_of(w.text.begin(), w.text.end(), [](QChar c) { return c.isDigit(); });
            for (const int psm : {8, 13}) {
                QStringList text;
                double sure = 0;
                int n = 0, mine = 0;
                for (const ocr::Line &r : e->read(grey.copy(crop), psm, &why))
                    for (const ocr::Word &x : r.words) {
                        const QRect at = x.box.translated(crop.topLeft());
                        const int over = std::min(at.right(), w.box.right()) - std::max(at.left(), w.box.left());
                        const int overY = std::min(at.bottom(), w.box.bottom()) - std::max(at.top(), w.box.top());
                        if (over < 0.5 * std::min(at.width(), w.box.width()) || overY < 0.5 * std::min(at.height(), w.box.height()))
                            continue;
                        if (std::none_of(x.text.begin(), x.text.end(), [](QChar c) { return c.isLetterOrNumber(); }))
                            continue;
                        text << QString(x.text).remove(marks);
                        sure += x.confidence, ++n;
                        int xa = 0, xb = 0;
                        script(x.text, &xa, &xb);
                        mine += arabicHere ? xa : xb;
                        for (const QChar c : x.text)
                            mine += c.isDigit() && numbersToo ? 1 : 0;
                    }
                // (a number, a date, a code kept one: its digits all still there - "١٤٤٧/٤/٢١" read alone was "آمالك";
                // a word of letters still letters)
                auto count = [](const QString &t, bool digits) {
                    return int(std::count_if(t.begin(), t.end(), [digits](QChar c) { return digits ? c.isDigit() : c.isLetter() && !c.isDigit(); }));
                };
                const QString joined = text.join(QLatin1Char(' '));
                const bool keepsDigits = count(joined, true) >= count(w.text, true);
                const bool keepsLetters = count(w.text, false) == 0 || count(joined, false) > 0;
                if (n > 0 && mine > 0 && sure / n > bestSure && keepsDigits && keepsLetters) {
                    bestSure = sure / n;
                    best = joined;
                }
            }
            if (best.isEmpty())
                continue;
            w.text = best;
            w.confidence = float(bestSure);
        }
    }
}

void dropDuplicates(std::vector<ocr::Line> &lines)
{
    // (one reading given to two words side by side - Tesseract's word put on an icon beside it too: "أنها-52", "أنها-"): the
    // less sure one's letters not its
    for (ocr::Line &l : lines)
        for (size_t i = 0; i + 1 < l.words.size(); ++i) {
            ocr::Word &x = l.words[i], &y = l.words[i + 1];
            auto letters = [](const QString &t) {
                QString o;
                for (const QChar c : t)
                    if (c.isLetter())
                        o += c;
                return o;
            };
            const QString lx = letters(x.text), ly = letters(y.text);
            if (lx.size() < 3 || lx != ly)
                continue;
            ocr::Word &drop = x.confidence < y.confidence ? x : y;
            QString rest;
            for (const QChar c : drop.text)
                if (!c.isLetter())
                    rest += c;
            drop.text = rest;
        }
}

// a "word" of Latin letters left after the digits are told (fixDigits), unsure, on an Arabic page: maybe an Arabic word
// Tesseract read in Latin letters - read again in Arabic: a word of its dictionary, or a letter off one, taken
void arabicForLatin(std::vector<ocr::Line> &lines, const QImage &grey, const QString &langs)
{
    auto script = [](const QString &t, int *arab, int *latin) {
        *arab = *latin = 0;
        for (const QChar c : t)
            if (pagetext::isArabic(c.unicode()) && c.isLetter())
                ++*arab;
            else if (c.isLetter() && c.unicode() < 0x250)
                ++*latin;
    };
    static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
    ocr::Engine engine;
    bool started = false, tried = false;
    auto engineFor = [&](int) -> ocr::Engine * {
        if (!tried) {
            QString why;
            started = engine.start(QStringLiteral("ara"), &why);
            tried = true;
        }
        return started ? &engine : nullptr;
    };
    // a "word" of Latin letters, unsure, on an Arabic page - not a word of its own: read in Arabic, a word of its dictionary
    // (or a letter off one: "الرقم" read "Ady" alone, "الرقو" in Arabic) - that word
    const QSet<QString> &arabicWords = ocr::dictionary(QStringLiteral("ara"));
    if (langs.startsWith(QLatin1String("ara")) && !arabicWords.isEmpty())
        for (ocr::Line &l : lines)
            for (ocr::Word &w : l.words) {
                if (scanCancel)
                    throw ScanCancelled();
                int a = 0, b = 0;
                script(w.text, &a, &b);
                // (not a number read as letters - the digits are told by their shapes first: "VEEV/E/Y)" a date)
                if (!l.inText || w.confidence >= 65 || a > 0 || b < 2 || w.box.width() < 8 || w.box.height() < 8 ||
                    std::any_of(w.text.begin(), w.text.end(), [](QChar c) { return c.isDigit() || c == QLatin1Char('/'); }))
                    continue;
                ocr::Engine *e = engineFor(0);
                if (!e)
                    return;
                const int pad = std::max(4, w.box.height() / 3);
                const QRect crop = w.box.adjusted(-pad, -pad, pad, pad).intersected(grey.rect());
                QString why;
                for (const int psm : {8, 7, 13}) {
                    std::vector<ocr::Line> again = e->read(grey.copy(crop), psm, &why);
                    QString found;
                    for (ocr::Line &r : again)
                        for (ocr::Word &x : r.words) {
                            x.text.remove(marks);
                            int xa = 0, xb = 0;
                            script(x.text, &xa, &xb);
                            if (xa >= 3 && xb == 0)
                                found = x.text;
                        }
                    if (found.isEmpty())
                        continue;
                    std::vector<ocr::Line> one(1);
                    one[0].words.push_back({found, w.box, 0});
                    wordsFromDictionary(one);
                    QString core = one[0].words[0].text;
                    core.remove(QRegularExpression(QStringLiteral("[^\\x{0621}-\\x{064A}]")));
                    if (arabicWords.contains(core)) {
                        w.text = one[0].words[0].text;
                        w.confidence = 66;
                        break;
                    }
                }
            }
}

void joinNumbers(std::vector<ocr::Line> &lines)
{
    static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
    static const QRegularExpression date(QStringLiteral("\\d[/\\-]\\d"));
    auto numeric = [](const QString &t) {
        bool digit = false;
        for (const QChar c : t) {
            if (c.isDigit())
                digit = true;
            else if (!QStringLiteral("/.,:-").contains(c))
                return false;
        }
        return digit;
    };
    for (ocr::Line &l : lines)
        for (bool joined = true; joined;) {
            joined = false;
            for (size_t a = 0; a < l.words.size() && !joined; ++a)
                for (size_t b = 0; b < l.words.size() && !joined; ++b) {
                    if (a == b)
                        continue;
                    ocr::Word &x = l.words[a];  // (y just right of x)
                    const ocr::Word &y = l.words[b];
                    QString tx = x.text, ty = y.text;
                    tx.remove(marks), ty.remove(marks);
                    if (!numeric(tx) || !numeric(ty))
                        continue;
                    const int gap = y.box.left() - x.box.right();
                    const int h = std::max(x.box.height(), y.box.height());
                    const int over = std::min(x.box.bottom(), y.box.bottom()) - std::max(x.box.top(), y.box.top());
                    if (gap < -2 || over < 0.5 * std::min(x.box.height(), y.box.height()))
                        continue;
                    const bool lone = tx.size() == 1 || ty.size() == 1;
                    const bool beside = lone && (tx.contains(date) || ty.contains(date)) ? gap <= 0.8 * h : lone && tx.size() + ty.size() >= 4 && gap <= 0.3 * h;
                    if (!beside)
                        continue;
                    x.text = tx + ty;
                    x.box |= y.box;
                    x.confidence = std::min(x.confidence, y.confidence);
                    l.words.erase(l.words.begin() + long(b));
                    joined = true;
                }
        }
}

// an Arabic word the scan draws stretched - a letterhead's "المملكـة", a justified line's kashida - its tatweels put in
// where they are: the scan's thin baseline stroke longer than the word drawn in its font (em pixels to the em) has it; the
// place - of those two of its letters join at - where a tatweel drawn in falls on the scan's thin stroke, as many as it's
// long (box: the word's ink on the page; the text it's read as - unchanged if it isn't stretched)
QString kashidas(const QString &word, const QString &family, bool bold, double em, const QImage &grey, const QRect &box)
{
    if (em < 8 || box.width() < 4 || word.size() < 2 || !arabic(word))
        return word;
    QRect scanBox;
    const QImage crop = grey.copy(box);
    const std::vector<char> ink = inkOf(crop, &scanBox);
    if (scanBox.isEmpty())
        return word;
    QFont f(family);
    f.setPointSizeF(em);  // (at 72 dots an inch: a point a pixel)
    f.setBold(bold);
    f.setStyleStrategy(QFont::PreferAntialias);
    struct Drawing {
        std::vector<char> ink;
        QRect box;
        int w = 0, h = 0;
    };
    auto drawn = [&](const QString &t) {
        Drawing d;
        const QFontMetricsF fm(f);
        d.w = int(std::ceil(fm.horizontalAdvance(t) + em)), d.h = int(std::ceil(2.5 * em));
        QImage im(d.w, d.h, QImage::Format_RGB32);
        im.setDotsPerMeterX(2835), im.setDotsPerMeterY(2835);
        im.fill(Qt::white);
        {
            QPainter p(&im);
            QTextLayout lay(t, f, &im);
            QTextOption o;
            o.setTextDirection(Qt::RightToLeft);
            lay.setTextOption(o);
            lay.beginLayout();
            QTextLine line = lay.createLine();
            line.setLineWidth(d.w);
            line.setPosition(QPointF(0, 0));
            lay.endLayout();
            p.setPen(Qt::black);
            lay.draw(&p, QPointF(0.5 * em - line.naturalTextRect().left(), 1.6 * em - line.ascent()));
        }
        d.ink.assign(size_t(d.w) * size_t(d.h), 0);
        int x0 = d.w, y0 = d.h, x1 = -1, y1 = -1;
        for (int y = 0; y < d.h; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            for (int x = 0; x < d.w; ++x) {
                const int r = qRed(row[x]), g = qGreen(row[x]);
                if (g < 128 || r < 128) {
                    d.ink[size_t(y) * size_t(d.w) + size_t(x)] = 1;
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                }
            }
        }
        d.box = x1 >= 0 ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
        return d;
    };
    Drawing now = drawn(word);
    if (now.box.isEmpty())
        return word;
    // (its kashidas: inside it - letters on both sides - a run of columns of just a thin stroke on its baseline (as thin as
    // one of its strokes: a sin's teeth are taller), longer by a third of an em or more than the font draws there: not a
    // final ب's flat bowl - that ends the word - nor a kaf's or a sad's base - its stroke over it - nor any join the font has)
    struct Profile {
        std::vector<char> thin, letters;
    };
    auto profile = [&](const std::vector<char> &in, int w, const QRect &b) {
        Profile p;
        p.thin.assign(size_t(w), 0), p.letters.assign(size_t(w), 0);
        std::vector<int> down;  // (its strokes' thickness: the middle run of ink down its columns)
        for (int x = b.left(); x <= b.right(); ++x)
            for (int y = b.top(), run = 0; y <= b.bottom() + 1; ++y) {
                const bool v = y <= b.bottom() && in[size_t(y) * size_t(w) + size_t(x)];
                if (v)
                    ++run;
                else if (run)
                    down.push_back(run), run = 0;
            }
        if (down.size() < 8)
            return p;
        std::nth_element(down.begin(), down.begin() + long(down.size() / 2), down.end());
        const int stroke = down[down.size() / 2];
        for (int x = b.left(); x <= b.right(); ++x) {
            int top = -1, bottom = -1;
            for (int y = b.top(); y <= b.bottom(); ++y)
                if (in[size_t(y) * size_t(w) + size_t(x)]) {
                    if (top < 0)
                        top = y;
                    bottom = y;
                }
            if (top >= 0)
                (bottom - top + 1 <= 1.5 * stroke && bottom >= b.top() + 0.5 * b.height() ? p.thin : p.letters)[size_t(x)] = 1;
        }
        return p;
    };
    const Profile scanP = profile(ink, crop.width(), scanBox), fontP = profile(now.ink, now.w, now.box);
    if (scanP.thin.empty() || fontP.thin.empty())
        return word;
    // (the font's drawing laid over the scan in proportion: its letters a little wider or narrower than the scan's)
    const double k = double(scanBox.width()) / std::max(1, now.box.width());
    auto toFont = [&](double x) { return now.box.right() - (scanBox.right() - x) / k; };
    auto toScan = [&](double x) { return scanBox.right() - (now.box.right() - x) * k; };
    struct Run {
        int from, to;
        double extra;
    };
    std::vector<Run> runs;
    for (int x = scanBox.left(); x <= scanBox.right();) {
        if (!scanP.thin[size_t(x)]) {
            ++x;
            continue;
        }
        int e = x;
        while (e <= scanBox.right() && scanP.thin[size_t(e)])
            ++e;
        const int reach = int(0.15 * em);
        bool before = false, after = false;
        for (int q = std::max(scanBox.left(), x - reach); q < x; ++q)
            before = before || scanP.letters[size_t(q)];
        for (int q = e; q <= std::min(scanBox.right(), e - 1 + reach); ++q)
            after = after || scanP.letters[size_t(q)];
        int fontThin = 0;
        for (int q = int(std::floor(toFont(x))); q <= int(std::ceil(toFont(e - 1))); ++q)
            fontThin += q >= 0 && q < now.w && fontP.thin[size_t(q)] ? 1 : 0;
        const double extra = (e - x) - fontThin * k;
        if (extra >= 0.3 * em && before && after)
            runs.push_back({x, e - 1, extra});
        x = e;
    }
    if (runs.empty())
        return word;  // (not stretched)
    // (where two letters join: the first joins on to the next one - not ا د ذ ر ز و ة ى ء, a letter before them - and the
    // next is a letter)
    static const QString endsJoin = QString::fromUtf8("اأإآدذرزوؤةىء");
    auto joins = [&](int i) {  // (between word[i - 1] and word[i])
        if (i <= 0 || i >= word.size())
            return false;
        const QChar a = word[i - 1], b = word[i];
        return pagetext::isArabic(a.unicode()) && a.isLetter() && !endsJoin.contains(a) && pagetext::isArabic(b.unicode()) && b.isLetter();
    };
    // (where each join is in the font's drawing - its caret there, laid out as it's drawn - on the scan)
    std::vector<std::pair<int, double>> places;
    {
        QImage dev(1, 1, QImage::Format_RGB32);
        dev.setDotsPerMeterX(2835), dev.setDotsPerMeterY(2835);
        QTextLayout lay(word, f, &dev);
        QTextOption o;
        o.setTextDirection(Qt::RightToLeft);
        lay.setTextOption(o);
        lay.beginLayout();
        QTextLine line = lay.createLine();
        line.setLineWidth(now.w);
        line.setPosition(QPointF(0, 0));
        lay.endLayout();
        const double off = 0.5 * em - line.naturalTextRect().left();
        for (int i = 1; i < word.size(); ++i)
            if (joins(i))
                places.emplace_back(i, toScan(line.cursorToX(i) + off));
    }
    // (each kashida: in at the join nearest it - as many tatweels as it's longer than the font's join)
    const double tatweel = std::max(1.0, QFontMetricsF(f).horizontalAdvance(QString(QChar(0x0640))) * k);
    std::map<int, int> put;  // (before which letter: how many)
    for (const Run &r : runs) {
        const double mid = (r.from + r.to) / 2.0;
        int best = -1;
        double near = 0.5 * (r.to - r.from) + 0.3 * em;
        for (const auto &[i, x] : places)
            if (std::abs(x - mid) < near)
                near = std::abs(x - mid), best = i;
        if (best >= 0)
            put[best] += std::clamp(int(std::lround(r.extra / tatweel)), 1, 12);
    }
    QString text = word;
    for (auto it = put.rbegin(); it != put.rend(); ++it)
        text = text.left(it->first) + QString(it->second, QChar(0x0640)) + text.mid(it->first);
    return text;
}

// a code in an Arabic line Tesseract read as junk words side by side - "١أ-١٢٤" as "VY", "ran": one word (its pieces left to
// right), its marks told as digits - and its letter - after
void joinCodes(std::vector<ocr::Line> &lines)
{
    static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
    auto foreign = [](const ocr::Word &w) {  // (not an Arabic word: no Arabic letters - or a letter or two among digits)
        int arab = 0, digits = 0;
        for (const QChar c : w.text)
            arab += pagetext::isArabic(c.unicode()) && c.isLetter() ? 1 : 0, digits += c.isDigit() ? 1 : 0;
        return w.confidence < 80 && (arab == 0 || (arab <= 2 && digits >= 2));
    };
    for (ocr::Line &l : lines) {
        if (!arabic(l.text))
            continue;
        for (bool joined = true; joined;) {
            joined = false;
            for (size_t a = 0; a < l.words.size() && !joined; ++a)
                for (size_t b = 0; b < l.words.size() && !joined; ++b) {
                    ocr::Word &x = l.words[a];  // (y just right of x)
                    const ocr::Word &y = l.words[b];
                    if (a == b || !foreign(x) || !foreign(y))
                        continue;
                    const int gap = y.box.left() - x.box.right();
                    const int h = std::max(x.box.height(), y.box.height());
                    const int over = std::min(x.box.bottom(), y.box.bottom()) - std::max(x.box.top(), y.box.top());
                    if (gap < -2 || gap > 0.6 * h || over < 0.5 * std::min(x.box.height(), y.box.height()))
                        continue;
                    x.text = QString(x.text).remove(marks) + QString(y.text).remove(marks);
                    x.box |= y.box;
                    x.confidence = std::min(x.confidence, y.confidence);
                    l.words.erase(l.words.begin() + long(b));
                    joined = true;
                }
        }
    }
}

double meanSure(const std::vector<ocr::Line> &ls)
{
    double sum = 0;
    int n = 0;
    for (const ocr::Line &l : ls)
        for (const ocr::Word &w : l.words)
            sum += w.confidence, ++n;
    return n ? sum / n : 0;
}

// lines Tesseract's page layout missed - a short line under a paragraph, a footer, a line a signature crosses:
// the page's ink not in a line it read - letter-sized marks side by side, a line high - each read as a line as
// it is (its raw line: no layout of its own); a line a signature's pen crosses read without its thin strokes too
std::vector<ocr::Line> missedLines(const QString &langs, const QImage &grey, const std::vector<char> &ink, const std::vector<Blob> &blobs,
                                   const std::vector<ocr::Line> &found)
{
    std::vector<ocr::Line> out;
    std::vector<int> heights;
    for (const ocr::Line &l : found)
        if (l.inText && l.confidence >= 60 && !l.text.trimmed().isEmpty())
            heights.push_back(l.box.height());
    if (heights.empty())
        return out;
    std::nth_element(heights.begin(), heights.begin() + long(heights.size() / 2), heights.end());
    const double lineH = heights[heights.size() / 2];
    const int W = grey.width(), H = grey.height();
    // (the print's strokes: how thick - the middle run of ink across a line read)
    int stroke = std::max(2, int(lineH * 0.08));
    {
        std::vector<int> runs;
        for (const ocr::Line &l : found) {
            if (!l.inText || l.confidence < 80 || runs.size() > 20000)
                continue;
            const int y = l.box.center().y();
            if (y < 0 || y >= H)
                continue;
            int run = 0;
            for (int x = std::max(0, l.box.left()); x <= std::min(W - 1, l.box.right()); ++x)
                if (ink[size_t(y * W + x)])
                    ++run;
                else if (run)
                    runs.push_back(run), run = 0;
        }
        if (runs.size() > 20) {
            std::nth_element(runs.begin(), runs.begin() + long(runs.size() / 2), runs.end());
            stroke = std::max(2, runs[runs.size() / 2]);
        }
    }
    auto covered = [&](const QRect &b) {  // (in a word read surely - a line's junk around it doesn't cover the rest)
        const QPoint c = b.center();
        for (const ocr::Line &l : found)
            if (l.inText)
                for (const ocr::Word &w : l.words)
                    if (w.confidence >= 60 && w.box.adjusted(-4, -4, 4, 4).contains(c))
                        return true;
        return false;
    };
    // (the marks a line's letters can be: a letter, a joined Arabic word's piece - not a rule, a frame, a speck)
    std::vector<size_t> seeds;
    for (size_t n = 0; n < blobs.size(); ++n) {
        const QRect &b = blobs[n].box;
        if (b.height() >= 0.3 * lineH && b.height() <= 1.6 * lineH && b.width() <= 10 * lineH && !covered(b))
            seeds.push_back(n);
    }
    // (marks side by side, a line high: one line)
    std::vector<size_t> up(seeds.size());
    for (size_t k = 0; k < seeds.size(); ++k)
        up[k] = k;
    std::function<size_t(size_t)> root = [&](size_t k) { return up[k] == k ? k : up[k] = root(up[k]); };
    for (size_t a = 0; a < seeds.size(); ++a)
        for (size_t b = a + 1; b < seeds.size(); ++b) {
            const QRect &x = blobs[seeds[a]].box, &y = blobs[seeds[b]].box;
            const int overlap = std::min(x.bottom(), y.bottom()) - std::max(x.top(), y.top());
            const int gap = std::max(x.left(), y.left()) - std::min(x.right(), y.right());
            // (their middles at one height: a line's - a signature under a line, a stroke across it, isn't of it)
            if (overlap >= 0.4 * std::min(x.height(), y.height()) && gap <= 1.2 * lineH && std::abs(x.center().y() - y.center().y()) <= 0.45 * lineH)
                up[root(a)] = root(b);
        }
    std::map<size_t, QRect> groups;
    std::map<size_t, int> counts;
    for (size_t k = 0; k < seeds.size(); ++k) {
        groups[root(k)] |= blobs[seeds[k]].box;
        ++counts[root(k)];
    }
    std::vector<QRect> jobs;
    for (const auto &[key, box] : groups)
        if (!(box.width() < 0.8 * lineH || (counts[key] < 2 && box.width() < 1.5 * lineH) || box.height() > 2.2 * lineH))
            jobs.push_back(box);
    // (a line read, its ink going on along its row past its words, not read - its words a signature's pen crosses, all of it
    // one mark with the pen's strokes, never a line's letter: the whole line read again, without thin strokes)
    std::vector<char> lineAgain(jobs.size(), 0);
    auto extended = [&](const std::vector<ocr::Line> &from, const std::vector<ocr::Line> &others, std::vector<QRect> *to, std::vector<char> *again) {
    for (const ocr::Line &l : from) {
        if (!l.inText || l.confidence < 50 || l.words.empty() || l.box.height() > 2 * lineH)
            continue;
        const int top = std::max(0, l.box.top()), bottom = std::min(H - 1, l.box.bottom());
        std::vector<char> sure(size_t(W), 0);  // (columns of the row in words read surely - this line's or another's)
        for (const std::vector<ocr::Line> *set : {&from, &others})
            for (const ocr::Line &o : *set)
                for (const ocr::Word &w : o.words)
                    if (w.confidence >= 60 && w.box.bottom() >= top && w.box.top() <= bottom)
                        for (int x = std::max(0, w.box.left()); x <= std::min(W - 1, w.box.right()); ++x)
                            sure[size_t(x)] = 1;
        auto inked = [&](int x) {
            int n = 0;
            for (int y = top + (bottom - top) / 4; y <= bottom - (bottom - top) / 4; ++y)
                n += ink[size_t(y) * size_t(W) + size_t(x)];
            return n > 0;
        };
        QRect grow = l.box;
        for (const int dir : {-1, 1}) {  // (to the left, to the right: ink not read, no wider gap than a word's)
            int x = dir < 0 ? l.box.left() - 1 : l.box.right() + 1, last = x - dir, gap = 0;
            for (; x >= 0 && x < W && std::abs(x - (dir < 0 ? l.box.left() : l.box.right())) < 8 * lineH; x += dir) {
                if (sure[size_t(x)])
                    break;
                if (inked(x))
                    last = x, gap = 0;
                else if (++gap > 0.8 * lineH)
                    break;
            }
            if (std::abs(last - (dir < 0 ? l.box.left() : l.box.right())) >= 1.5 * lineH)
                (dir < 0 ? grow.setLeft(last) : grow.setRight(last));
        }
        if (grow != l.box)
            to->push_back(grow), again->push_back(1);
    }
    };
    extended(found, {}, &jobs, &lineAgain);
    // (read on the computer's cores: each with its own readers - the languages together, and, started when first
    // needed, each alone)
    struct Readers {
        ocr::Engine engines[3];
        int tried[3] = {0, 0, 0};
    };
    const int threads = 1;  // (Tesseract started again in each thread costs what more threads save)
    std::vector<Readers> readers(static_cast<size_t>(threads));
    const QString each[3] = {langs, QStringLiteral("ara"), QStringLiteral("eng")};
    auto readJobs = [&](const std::vector<QRect> &jobs, const std::vector<char> &lineAgain, double from, double to) {
    std::vector<ocr::Line> out;
    std::vector<std::vector<ocr::Line>> results(jobs.size());
    std::vector<QRect> crops(jobs.size());
    inParallel(jobs.size(), threads, [&](size_t job, int thread) {
        auto engine = [&](int i) -> ocr::Engine * {
            Readers &r = readers[size_t(thread)];
            if (i > 0 && !langs.contains(QLatin1Char('+')))
                return nullptr;
            if (!r.tried[i]) {
                QString why;
                r.tried[i] = r.engines[i].start(each[i], &why) ? 1 : -1;
            }
            return r.tried[i] > 0 ? &r.engines[i] : nullptr;
        };
        const QRect box = jobs[job];
        const QRect crop = box.adjusted(-int(0.3 * lineH), -int(0.4 * lineH), int(0.3 * lineH), int(0.4 * lineH)).intersected(grey.rect());
        crops[job] = crop;
        const QImage part = grey.copy(crop);
        QString why;
        // (read as it is, and without its thin strokes - a signature's pen across it; in the languages together, and
        // - if that's not sure - in each alone: Arabic's digits read as English letters, an Arabic word as "55". The
        // reading with the most letters read surely kept)
        auto worth = [](const std::vector<ocr::Line> &r) {
            double w = 0;
            for (const ocr::Line &l : r)
                for (const ocr::Word &wd : l.words)
                    if (wd.confidence >= 30)
                        for (const QChar c : wd.text)
                            w += c.isLetter() ? wd.confidence : c.isDigit() ? 0.5 * wd.confidence : 0;  // (a letter - a digit counts less: junk reads as digits)
            return w;
        };
        // (its own print's strokes: how thick - a footer's small print is thinner than the page's body: not taken out)
        int ownStroke = stroke;
        {
            std::vector<int> runs;
            for (int y = box.top() + box.height() / 4; y <= box.bottom() - box.height() / 4; y += std::max(1, box.height() / 8))
                for (int x = box.left(), run = 0; x <= box.right() + 1; ++x) {
                    const bool v = x <= box.right() && ink[size_t(y) * size_t(W) + size_t(x)];
                    if (v)
                        ++run;
                    else if (run)
                        runs.push_back(run), run = 0;
                }
            if (runs.size() >= 6) {
                std::nth_element(runs.begin(), runs.begin() + long(runs.size() / 2), runs.end());
                ownStroke = std::max(2, std::min(stroke, runs[runs.size() / 2]));
            }
        }
        const QImage clean = withoutThinStrokes(part, ownStroke);
        std::vector<ocr::Line> ls;
        double best = -1;
        // (in turn - as soon as one reads it surely, that one: the languages together, its raw line or Tesseract's own
        // line - each reads some the other can't - without its thin strokes; then each language alone)
        struct Try {
            int engine;
            bool clean;
            int psm;
        };
        static const Try tries[] = {{0, false, 13}, {0, false, 7}, {0, true, 13}, {1, false, 13}, {1, false, 7}, {2, false, 13}, {2, false, 7}};
        // (a line read again whole - a pen across it: without thin strokes first, and that reading preferred)
        static const Try crossed[] = {{0, true, 7}, {0, true, 13}, {1, true, 7}, {0, false, 7}};
        const bool again = lineAgain[job] != 0;
        for (const Try &t : again ? std::vector<Try>(std::begin(crossed), std::end(crossed)) : std::vector<Try>(std::begin(tries), std::end(tries))) {
            if (!ls.empty() && meanSure(ls) >= 80)
                break;
            if (t.engine > 0 && best <= 0 && !again)
                break;  // (no letters at all read in it: a logo's, a scrawl's marks - not read in each language too)
            ocr::Engine *e = engine(t.engine);
            if (!e)
                continue;
            std::vector<ocr::Line> r = e->read(t.clean ? clean : part, t.psm, &why);
            const double w = worth(r) * (t.clean == again ? 1.0 : 0.9);
            if (w > best)
                best = w, ls = std::move(r);
        }
        results[job] = std::move(ls);
        stepIn(from, to, double(job + 1), double(jobs.size()));
    });
    for (size_t job = 0; job < jobs.size(); ++job)
        for (ocr::Line &l : results[job]) {
            const QPoint at = crops[job].topLeft();
            l.box.translate(at);
            l.baseStart += at, l.baseEnd += at;
            for (ocr::Word &w : l.words)
                w.box.translate(at);
            l.block = 100000 + int(job);
            l.para = 100000 + int(job);
            l.inText = true;
            out.push_back(std::move(l));
        }
    return out;
    };
    out = readJobs(jobs, lineAgain, 0.3, 0.42);
    // (... and a line just read so, its row going on: read again whole too)
    std::vector<QRect> more;
    std::vector<char> moreAgain;
    extended(out, found, &more, &moreAgain);
    std::vector<ocr::Line> second = readJobs(more, moreAgain, 0.42, 0.45);
    for (ocr::Line &l : second) {
        l.block += 50000, l.para += 50000;
        out.push_back(std::move(l));
    }
    return out;
}

// the page's pictures - a logo, a signature, a stamp, an icon: its ink that isn't its words' (nor its frame's rules,
// nor specks), its pieces near each other one picture; a "line" read in one that's like no font - a logo's
// lettering - the picture's too, if it's mostly in it (absorbed: those lines)
std::vector<QRect> pictureRegions(const std::vector<Blob> &blobs, const std::vector<QRect> &words, const std::vector<QRect> &text,
                                  const std::vector<double> &fits, const std::vector<double> &sures, const std::vector<double> &surest,
                                  const std::vector<int> &counts,
                                  const std::vector<char> &plain, double lineH, QSize page, std::vector<char> *absorbed,
                                  const std::vector<char> &rules)
{
    absorbed->assign(text.size(), 0);
    const int pad = int(0.1 * lineH);
    auto inWords = [&](const QRect &b) {
        const QPoint c = b.center();
        for (const QRect &w : words)
            if (w.adjusted(-pad, -pad, pad, pad).contains(c))
                return true;
        return false;
    };
    std::vector<QRect> marks;
    std::vector<double> markArea;
    for (const Blob &b : blobs) {
        const QRect &r = b.box;
        const double fill = double(b.area) / std::max(1, r.width() * r.height());
        if (b.area < 0.01 * lineH * lineH)
            continue;  // (a speck)
        if ((r.width() > 0.25 * page.width() || r.height() > 0.25 * page.height()) && fill < 0.08)
            continue;  // (the page's frame)
        if ((r.height() < 0.25 * lineH && r.width() > 3 * lineH) || (r.width() < 0.25 * lineH && r.height() > 3 * lineH))
            continue;  // (a rule)
        if (inWords(r))
            continue;
        marks.push_back(r);
        markArea.push_back(b.area);
    }
    // (pieces near each other: one picture)
    const int g = int(0.35 * lineH);
    std::vector<size_t> up(marks.size());
    for (size_t k = 0; k < marks.size(); ++k)
        up[k] = k;
    std::function<size_t(size_t)> root = [&](size_t k) { return up[k] == k ? k : up[k] = root(up[k]); };
    for (size_t a = 0; a < marks.size(); ++a)
        for (size_t b = a + 1; b < marks.size(); ++b)
            if (marks[a].adjusted(-g, -g, g, g).intersects(marks[b]))
                up[root(a)] = root(b);
    std::map<size_t, QRect> groups;
    std::map<size_t, double> inked;
    std::map<size_t, int> pieces;
    for (size_t k = 0; k < marks.size(); ++k) {
        groups[root(k)] |= marks[k];
        inked[root(k)] += markArea[k];
        ++pieces[root(k)];
    }
    std::vector<QRect> regions;
    for (const auto &[key, box] : groups) {
        // (big enough to be a picture - a footer's icon is - and inked enough: not a frame's corner's thin arc)
        if (box.width() < 0.4 * lineH || box.height() < 0.4 * lineH || inked[key] < 0.12 * lineH * lineH ||
            inked[key] < 0.05 * double(box.width()) * box.height())
            continue;
        if (double(box.width()) * box.height() > 0.5 * page.width() * page.height())
            continue;
        const double fill = inked[key] / (double(box.width()) * box.height());
        if (fill < 0.15) {  // (thin, touching the page's rules taken out: a frame's curved corner)
            bool touches = false;
            const QRect near = box.adjusted(-3, -3, 3, 3).intersected(QRect(QPoint(0, 0), page));
            for (int y = near.top(); y <= near.bottom() && !touches; ++y)
                for (int x = near.left(); x <= near.right() && !touches; ++x)
                    touches = rules[size_t(y) * size_t(page.width()) + size_t(x)] != 0;
            if (touches)
                continue;
        }
        // (small, in a row of text, of three pieces or more: letters not read - left in the scan; an icon is one
        // or two pieces)
        if (box.height() <= 1.2 * lineH && pieces[key] >= 3) {
            bool inRow = false;
            for (const QRect &t : text) {
                const int over = std::min(t.bottom(), box.bottom()) - std::max(t.top(), box.top());
                inRow = inRow || over >= 0.5 * box.height();
            }
            if (inRow)
                continue;
        }
        regions.push_back(box);
    }
    // a logo's lettering read as a line: a short "line" like no font, not read surely, not a word of letters
    // ("2030" read "2103100", a signature read "لوست") - a picture; one like no font at all touching a picture
    // ("VISION" over "2030"), or one like no font mostly in one - the picture's (a heading in a font none is
    // like - its words read surely, nothing round it - stays text)
    for (size_t t = 0; t < text.size(); ++t)
        if (counts[t] <= 3 && sures[t] < 60 && surest[t] < 85 && (fits[t] < 0.2 || (fits[t] < 0.35 && !plain[t]))) {
            regions.push_back(text[t]);
            (*absorbed)[t] = 1;
        }
    for (bool more = true; more;) {
        more = false;
        for (size_t t = 0; t < text.size(); ++t) {
            // (words read surely: text - or words of letters, two or more, read fairly surely and not quite unlike their font:
            // a heading in a display face, a line a signature crosses)
            if ((*absorbed)[t] || fits[t] >= 0.45 || (sures[t] >= 80 && plain[t] && fits[t] >= 0.3) ||
                (sures[t] >= 65 && plain[t] && counts[t] >= 2 && fits[t] >= 0.15) || (surest[t] >= 90 && plain[t] && counts[t] >= 2 && fits[t] >= 0.15))
                continue;
            for (QRect &r : regions) {
                const QRect both = r.adjusted(-g, -g, g, g) & text[t];
                const bool mostly = double(both.width()) * both.height() >= 0.5 * double(text[t].width()) * text[t].height();
                if (mostly || (fits[t] < 0.25 && !both.isEmpty())) {
                    r |= text[t];
                    (*absorbed)[t] = 1;
                    more = true;
                    break;
                }
            }
        }
    }
    // (pictures that grew into each other: one)
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t a = 0; a < regions.size() && !merged; ++a)
            for (size_t b = a + 1; b < regions.size() && !merged; ++b)
                if (regions[a].intersects(regions[b])) {
                    regions[a] |= regions[b];
                    regions.erase(regions.begin() + long(b));
                    merged = true;
                }
    }
    return regions;
}

// (an Arabic line's other font - its words cut into glyphs where they fell, a font of its own - and what to tell
// which is more like it by, once both are made: the scan's line, its words)
struct Choice {
    QString other;
    QImage crop;  // (the page read - shared, not copied - and the line's box on it, as it's fitted)
    QRect box;
    Fit fit;
    QString text;
    std::vector<std::pair<QString, QRect>> words;
    bool bold = false;
};


std::vector<Document::OcrLine> readPage(Document *doc, int page, QString &langs, QString *error, Builders *builders, std::vector<QString> *keys,
                                        std::vector<Document::ScanPicture> *pictures, Picture *background, QByteArray *cleaned,
                                        std::vector<Choice> *choices, const QRectF &area)
{
    const double zoom = kDpi / 72;
    const QSizeF size = doc->pageSize(page);
    QImage scan = doc->render(page, zoom, QRect(0, 0, int(size.width() * zoom), int(size.height() * zoom)));
    if (scan.isNull()) {
        *error = QObject::tr("The page couldn't be drawn.");
        return {};
    }
    if (!area.isEmpty()) {  // (just a part of the page scanned: the rest of it blank - nothing there read, nothing taken out)
        const QRect in = QRectF(area.topLeft() * zoom, area.size() * zoom).toAlignedRect().intersected(scan.rect());
        QImage blank(scan.size(), scan.format());
        blank.fill(Qt::white);
        QPainter p(&blank);
        p.drawImage(in.topLeft(), scan.copy(in));
        p.end();
        scan = blank;
    }
    const QStringList have = ocr::languages();
    const bool both = have.contains(QStringLiteral("ara")) && have.contains(QStringLiteral("eng"));
    const bool first = langs.isEmpty();
    if (first)
        langs = both ? QStringLiteral("ara+eng") : QString();
    // (Tesseract's reading: the page's first 30 parts in 100 - as it reads its words)
    const auto reading = [](int p) { readStep(0.3 * p / 100.0, QString()); };
    const auto off = [] { return scanCancel.load(); };
    std::vector<ocr::Line> lines = ocr::read(scan, error, false, langs, reading, off);
    if (first && both) {
        // (hardly any Arabic: read again in English alone - and the pages after)
        int letters = 0, arab = 0;
        for (const ocr::Line &l : lines)
            for (const QChar c : l.text)
                if (c.isLetter())
                    ++letters, arab += pagetext::isArabic(c.unicode()) ? 1 : 0;
        if (letters > 20 && arab < letters / 20) {
            langs = QStringLiteral("eng");
            lines = ocr::read(scan, error, false, langs, reading, off);
        }
    }
    if (!error->isEmpty())
        return {};
    const QImage grey = scan.convertToFormat(QImage::Format_Grayscale8);
    QRect pageBox;
    const std::vector<char> pageInk = inkOf(grey, &pageBox);
    const std::vector<Blob> blobs = blobsOf(pageInk, grey.width(), grey.height());
    {  // the lines its page layout missed, read one by one
        stepAt(0.3, QObject::tr("Looking for lines the first look missed"));
        const std::vector<ocr::Line> more = missedLines(langs, grey, pageInk, blobs, lines);
        lines.insert(lines.end(), more.begin(), more.end());
    }
    {  // (a word read twice - in the page's line, in a line read again: the surer one kept)
        std::vector<std::pair<size_t, size_t>> all;
        for (size_t a = 0; a < lines.size(); ++a)
            for (size_t b = 0; b < lines[a].words.size(); ++b)
                all.emplace_back(a, b);
        std::vector<char> gone(all.size(), 0);
        for (size_t x = 0; x < all.size(); ++x)
            for (size_t y = x + 1; y < all.size(); ++y) {
                if (all[x].first == all[y].first || gone[x] || gone[y])
                    continue;
                const ocr::Word &p = lines[all[x].first].words[all[x].second], &q = lines[all[y].first].words[all[y].second];
                const QRect both = p.box & q.box;
                if (both.isEmpty())
                    continue;
                const double share = double(both.width()) * both.height() /
                                     std::max(1.0, std::min(double(p.box.width()) * p.box.height(), double(q.box.width()) * q.box.height()));
                if (share <= 0.4)
                    continue;
                // (kept in the line with more words - a number read again on its own stays in its sentence - read as
                // the surer reading has it)
                const bool keepX = lines[all[x].first].words.size() >= lines[all[y].first].words.size();
                ocr::Word &kept = lines[all[keepX ? x : y].first].words[all[keepX ? x : y].second];
                const ocr::Word &other = keepX ? q : p;
                auto worth = [](const ocr::Word &w) {
                    double n = 0;
                    for (const QChar c : w.text)
                        n += c.isLetter() ? 1 : c.isDigit() ? 0.5 : 0;
                    return n * w.confidence;
                };
                if (worth(other) > worth(kept)) {
                    kept.text = other.text;
                    kept.confidence = other.confidence;
                }
                gone[keepX ? y : x] = 1;
            }
        for (size_t k = all.size(); k-- > 0;)
            if (gone[k])
                lines[all[k].first].words.erase(lines[all[k].first].words.begin() + long(all[k].second));
    }
    stepAt(0.45, QObject::tr("Reading the words again"));
    rereadWords(lines, grey, langs);
    dotsFromPage(lines);
    wordsFromDictionary(lines);
    joinNumbers(lines);
    joinCodes(lines);
    fixMarks(lines, blobs, pageInk, grey.width());
    fixDigits(lines, pageInk, grey.width(), blobs);
    rereadUnsure(lines, grey, langs);
    arabicForLatin(lines, grey, langs);
    dropDuplicates(lines);
    fixMarks(lines, blobs, pageInk, grey.width());  // (a word made Arabic: the sign beside it told again)
    std::vector<Read> reads;
    for (const ocr::Line &l : lines) {
        if (!l.inText)
            continue;  // (a picture's, a ruled line's, noise)
        // (its words read surely - and a word of letters Tesseract wasn't sure of in a line it read well: "جهاز")
        std::vector<float> sures;
        for (const ocr::Word &w : l.words)
            sures.push_back(w.confidence);
        std::sort(sures.begin(), sures.end());
        const double lineSure = sures.empty() ? 0 : sures[sures.size() / 2];
        std::vector<const ocr::Word *> words;
        for (const ocr::Word &w : l.words) {
            bool letters = !w.text.isEmpty();
            int count = 0;
            for (const QChar c : w.text) {
                letters = letters && (c.isLetter() || c.isMark());
                count += c.isLetter() ? 1 : 0;
            }
            // (... or a long word of letters read not quite surely: a pen's stroke across it)
            if ((w.confidence >= 30 || (letters && (lineSure >= 80 || (count >= 4 && w.confidence >= 25)))) && !w.text.trimmed().isEmpty())
                words.push_back(&w);
        }
        if (words.empty())
            continue;
        // (a line split where its words are far apart - a letterhead's Arabic and English either side of a logo:
        // each a line of its own, as it's seen; the words kept in the order they're read)
        std::vector<int> heights, middles;
        for (const ocr::Word *w : words)
            heights.push_back(w->box.height()), middles.push_back(w->box.center().y());
        std::nth_element(heights.begin(), heights.begin() + long(heights.size() / 2), heights.end());
        std::nth_element(middles.begin(), middles.begin() + long(middles.size() / 2), middles.end());
        const int wordH = heights[heights.size() / 2], middle = middles[middles.size() / 2];
        const int gapMax = std::max(8, int(wordH * 1.5));
        // (a word not read surely - a code read as junk - still where the line goes on: not a gap - if it's as big as
        // its words, in their row; a logo's marks read aren't)
        std::vector<const ocr::Word *> across = words;
        for (const ocr::Word &w : l.words)
            if (std::find(words.begin(), words.end(), &w) == words.end() && !w.text.trimmed().isEmpty() && w.box.height() <= 1.3 * wordH &&
                std::abs(w.box.center().y() - middle) <= 0.5 * wordH)
                across.push_back(&w);
        std::sort(across.begin(), across.end(), [](const ocr::Word *a, const ocr::Word *b) { return a->box.left() < b->box.left(); });
        QHash<const ocr::Word *, int> pieceOf;
        int pieces = 0, right = -1000000;
        for (const ocr::Word *w : across) {
            if (w->box.left() - right > gapMax)
                ++pieces;
            pieceOf.insert(w, pieces - 1);
            right = std::max(right, w->box.right());
        }
        for (int piece = 0; piece < pieces; ++piece) {
            QStringList kept;
            QRect box;
            std::vector<std::pair<QString, QRect>> boxes;
            std::vector<float> boxSure;
            double sure = 0, wordSure = 0;
            int letters = 0, shown = 0, wordCount = 0;
            bool anyLetter = false;
            // (its words - not a sign read at either end of it, unsure: a rule's end read ",=", a stain's "|")
            std::vector<const ocr::Word *> pw;
            for (const ocr::Word *w : words)
                if (pieceOf.value(w) == piece)
                    pw.push_back(w);
            auto junk = [wordH](const ocr::Word *w) {
                if (w->box.height() > 1.8 * wordH && w->confidence < 80)
                    return true;  // (much taller than its words, not sure: a frame's curved corner read "r—")
                for (const QChar c : w->text)
                    if (c.isLetterOrNumber())
                        return false;
                return w->confidence < 70;
            };
            {  // (in the order it's read, by where its words are: an Arabic piece right to left, a run of Latin words in it left
               // to right - an English one the other way - not as Tesseract's line had them: a line it read across a letterhead's
               // two halves, a word read again)
                int arab = 0, latin = 0;
                auto kind = [](const ocr::Word *w, int *a, int *b) {
                    *a = *b = 0;
                    for (const QChar c : w->text)
                        if (c.isLetter())
                            (pagetext::isArabic(c.unicode()) ? *a : (c.unicode() < 0x250 ? *b : *a)) += 1;
                };
                for (const ocr::Word *w : pw) {
                    int a = 0, b = 0;
                    kind(w, &a, &b);
                    arab += a, latin += b;
                }
                // (right to left as the page's line is told when it's read back - its Arabic, digits and signs too, against its
                // Latin letters: written, read back, edited the same way)
                int arabic = 0;
                for (const ocr::Word *w : pw)
                    for (const QChar c : w->text)
                        arabic += pagetext::isArabic(c.unicode()) ? 1 : 0;
                const bool rtl = arabic > 0 && arabic >= latin;
                // (as Unicode's bidi reads words lying across - as they're written back, and the editor shows them: a run of
                // Latin words left to right in an Arabic piece, numbers side by side right to left, Arabic's numbers too in an
                // English piece - "Email: … ٠١٧ ٢٤١٨١٩٧")
                std::stable_sort(pw.begin(), pw.end(), [](const ocr::Word *a, const ocr::Word *b) { return a->box.center().x() < b->box.center().x(); });
                std::vector<pagetext::Bidi> types;
                for (size_t i = 0; i < pw.size(); ++i) {
                    if (i > 0)
                        types.push_back(pagetext::Bidi::NI);  // (the space between)
                    int a = 0, b = 0, an = 0, en = 0;
                    kind(pw[i], &a, &b);
                    for (const QChar c : pw[i]->text) {
                        an += pagetext::bidiType(c.unicode()) == pagetext::Bidi::AN ? 1 : 0;
                        en += pagetext::bidiType(c.unicode()) == pagetext::Bidi::EN ? 1 : 0;
                    }
                    types.push_back(a > 0 && a >= b ? pagetext::Bidi::R : b > 0 ? pagetext::Bidi::L : an > 0 ? pagetext::Bidi::AN
                                    : en > 0 ? pagetext::Bidi::EN : pagetext::Bidi::NI);
                }
                std::vector<const ocr::Word *> read;
                for (size_t k : pagetext::bidiOrder(pagetext::bidiLevels(types, rtl)))
                    if (k % 2 == 0)
                        read.push_back(pw[k / 2]);
                pw = read;
            }
            while (!pw.empty() && junk(pw.front()))
                pw.erase(pw.begin());
            while (!pw.empty() && junk(pw.back()))
                pw.pop_back();
            for (const ocr::Word *w : pw) {
                // (Tesseract's direction marks left out: written, a font without them would be taken for another)
                QString text = w->text;
                text.remove(QRegularExpression(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]")));
                if (w->confidence < 50)  // (a fatha, a damma read in a word not read surely: a stroke across it)
                    text.remove(QRegularExpression(QStringLiteral("[\\x{064B}-\\x{0652}]")));
                if (text.trimmed().isEmpty())
                    continue;
                // (ة, ى end a word - one in its middle: two words the reader ran together)
                text.replace(QRegularExpression(QStringLiteral("([\\x{0629}\\x{0649}])(?=[\\x{0621}-\\x{064A}])")), QStringLiteral("\\1 "));
                kept << text;
                box |= w->box;
                boxes.emplace_back(text, w->box);
                boxSure.push_back(w->confidence);
                sure += w->confidence;
                bool word = false;  // (a sign alone - "+", "|" - not counted: how sure, how plain its words are)
                for (const QChar c : text)
                    word = word || c.isLetterOrNumber();
                if (!word)
                    continue;
                wordSure += w->confidence;
                ++wordCount;
                for (const QChar c : text) {
                    letters += c.isLetterOrNumber() ? 1 : 0;
                    anyLetter = anyLetter || c.isLetter();
                    shown += c.isSpace() ? 0 : 1;
                }
            }
            // (not sure of it - but a word of letters, a short Arabic word read alone: its font's fit tells)
            const bool plain = anyLetter && letters >= 4 && letters == shown;
            if (kept.isEmpty() || letters < 2 ||
                (sure / kept.size() < 50 && !(plain && wordCount > 0 && wordSure / wordCount >= 30))) {
                continue;  // (not sure: a stamp's, a signature's scrawl)
            }
            if (box.height() / zoom > 72)
                continue;  // (letters that big: a logo)
            Read r;
            r.line.text = kept.join(QLatin1Char(' '));
            r.block = l.block;
            r.para = l.para * 64 + piece;
            r.arabic = arabic(r.line.text);
            if (r.arabic)
                r.line.text = arabicDigits(r.line.text);
            r.box = box.adjusted(-2, -2, 2, 2).intersected(grey.rect());
            r.base0 = l.baseStart;
            r.base1 = l.baseEnd;
            r.sure = sure / kept.size();
            // (what looks like text: words of letters - not a few marks, a number on its own)
            r.wordy = anyLetter && letters >= 4 && letters >= 0.75 * shown;
            for (auto &[word, wb] : boxes)
                r.words.emplace_back(r.arabic ? arabicDigits(word) : word, wb.translated(-r.box.topLeft()));
            r.wordSure = boxSure;
            reads.push_back(r);
        }
    }
    // each block's fonts (a letterhead, a body of text: one typeface): every font that writes it looked over (its longest words, small), the likeliest
    // few - and the usual ones - tried closely on each of its lines, regular and bold
    {
        std::vector<size_t> up(reads.size());
        for (size_t n = 0; n < reads.size(); ++n)
            up[n] = n;
        std::function<size_t(size_t)> root = [&](size_t n) { return up[n] == n ? n : up[n] = root(up[n]); };
        for (size_t a = 0; a < reads.size(); ++a)
            for (size_t b = a + 1; b < reads.size(); ++b) {
                const QRect &x = reads[a].box, &y = reads[b].box;
                const int h = std::max(x.height(), y.height());
                const int gap = std::max(x.top(), y.top()) - std::min(x.bottom(), y.bottom());
                const int across = std::min(x.right(), y.right()) - std::max(x.left(), y.left());
                if (reads[a].arabic == reads[b].arabic && gap < h && across > 0.3 * std::min(x.width(), y.width()) &&
                    std::min(x.height(), y.height()) > 0.65 * h)
                    up[root(a)] = root(b);
            }
        for (size_t n = 0; n < reads.size(); ++n)
            reads[n].group = int(root(n));
    }
    std::map<std::tuple<int, int, bool>, std::vector<size_t>> paraReads;
    for (size_t n = 0; n < reads.size(); ++n)
        paraReads[{reads[n].group, 0, reads[n].arabic}].push_back(n);
    std::vector<std::vector<char>> inks(reads.size());
    std::vector<QRect> inkBoxes(reads.size());
    std::vector<std::vector<float>> darks(reads.size());  // (how dark: paper 0, ink 1 - its own ink and its blur round it)
    // (a line much taller than the page's lines - a signature's strokes through it: its ink without thin strokes - the
    // print's strokes' width: the middle run of ink across lines read surely)
    int lineH = 0, stroke = 2;
    {
        std::vector<int> hs, runs;
        for (const Read &r : reads)
            hs.push_back(r.box.height());
        if (!hs.empty()) {
            std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
            lineH = hs[hs.size() / 2];
        }
        const int W = grey.width();
        for (const Read &r : reads) {
            if (r.sure < 80 || runs.size() > 20000)
                continue;
            const int y = r.box.center().y();
            for (int x = r.box.left(), run = 0; x <= r.box.right() + 1; ++x) {
                const bool v = x <= r.box.right() && pageInk[size_t(y) * size_t(W) + size_t(x)];
                if (v)
                    ++run;
                else if (run)
                    runs.push_back(run), run = 0;
            }
        }
        if (runs.size() > 20) {
            std::nth_element(runs.begin(), runs.begin() + long(runs.size() / 2), runs.end());
            stroke = std::max(2, runs[runs.size() / 2]);
        }
    }
    for (size_t n = 0; n < reads.size(); ++n) {
        inks[n] = inkOf(grey.copy(reads[n].box), &inkBoxes[n]);
        if (lineH > 0 && reads[n].box.height() > 1.6 * lineH) {  // (just its densest rows, a line high: a pen's stroke is thin)
            const int W0 = reads[n].box.width(), H0 = reads[n].box.height(), band = int(std::lround(1.25 * lineH));
            std::vector<long> rows(size_t(H0), 0);
            for (int y = 0; y < H0; ++y)
                for (int x = 0; x < W0; ++x)
                    rows[size_t(y)] += inks[n][size_t(y) * size_t(W0) + size_t(x)];
            long sum = 0, best = -1;
            int top = 0;
            for (int y = 0; y < H0; ++y) {
                sum += rows[size_t(y)] - (y >= band ? rows[size_t(y - band)] : 0);
                if (y >= band - 1 && sum > best)
                    best = sum, top = y - band + 1;
            }
            const int pad = int(0.1 * lineH);
            for (int y = 0; y < H0; ++y)
                if (y < top - pad || y > top + band + pad)
                    std::fill(inks[n].begin() + long(y) * W0, inks[n].begin() + long(y + 1) * W0, 0);
        }
        ownInk(inks[n], reads[n].box.size(), &inkBoxes[n]);
        int paper = 255, ink = 0;
        levels(grey, reads[n].box, &paper, &ink);
        const QRect &b = reads[n].box;
        const int W = b.width(), H = b.height(), r = 2;
        std::vector<char> near(inks[n].size(), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (inks[n][size_t(y) * size_t(W) + size_t(x)])
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx)
                            if (x + dx >= 0 && y + dy >= 0 && x + dx < W && y + dy < H)
                                near[size_t(y + dy) * size_t(W) + size_t(x + dx)] = 1;
        darks[n].assign(inks[n].size(), 0.0f);
        const double range = std::max(30, paper - ink);
        for (int y = 0; y < H; ++y) {
            const uchar *row = grey.constScanLine(b.top() + y);
            for (int x = 0; x < W; ++x)
                if (near[size_t(y) * size_t(W) + size_t(x)])
                    darks[n][size_t(y) * size_t(W) + size_t(x)] = float(std::clamp((paper - row[b.left() + x]) / range, 0.0, 1.0));
        }
    }
    int group = 0;  // (the page's 50th to 80th parts in 100: its groups' fonts tried)
    for (const auto &[key, members] : paraReads) {
        const double from = 0.5 + 0.3 * group / double(paraReads.size()), to = 0.5 + 0.3 * (group + 1) / double(paraReads.size());
        ++group;
        stepAt(from, group == 1 ? QObject::tr("Matching the fonts") : QString());
        struct Look {
            QString word;
            QImage ink;
            int width;
        };
        std::vector<Look> looks;
        for (size_t n : members)
            for (const auto &[word, box] : reads[n].words) {
                int letters = 0;
                for (const QChar c : word)
                    letters += c.isLetter() ? 1 : 0;
                if (letters < 3)
                    continue;
                const QRect wb = inkIn(inks[n], reads[n].box.size(), box);
                const QImage small = smallInk(inks[n], reads[n].box.size(), wb);
                if (!small.isNull())
                    looks.push_back({word, small, wb.width()});
            }
        std::sort(looks.begin(), looks.end(), [](const Look &a, const Look &b) { return a.width > b.width; });
        if (looks.size() > 5)
            looks.resize(5);
        std::vector<std::pair<double, QString>> ranked;
        if (!looks.empty()) {  // (every font looked over on the computer's cores)
            const QStringList &fams = allFamilies(std::get<2>(key));
            std::vector<double> score(size_t(fams.size()), -9);
            inParallel(size_t(fams.size()), 1, [&](size_t f, int) {
                stepIn(from, from + 0.4 * (to - from), double(f), double(fams.size()));
                double sum = 0, weight = 0;
                for (const Look &l : looks) {
                    const double sc = quickLook(l.word, fams[int(f)], l.ink);
                    if (sc > -1)
                        sum += sc * l.width, weight += l.width;
                }
                if (weight > 0)
                    score[f] = sum / weight;
            });
            for (int f = 0; f < fams.size(); ++f)
                if (score[size_t(f)] > -9)
                    ranked.emplace_back(score[size_t(f)], fams[f]);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        QStringList tried = candidates(std::get<2>(key));
        for (size_t k = 0; k < ranked.size() && k < 5; ++k)
            if (!tried.contains(ranked[k].second))
                tried << ranked[k].second;
        // (each line in each, regular and bold - on the computer's cores)
        std::vector<std::tuple<size_t, QString, bool>> trials;
        for (size_t n : members)
            for (const QString &family : tried)
                for (const bool bold : {false, true})
                    trials.emplace_back(n, family, bold);
        std::vector<Fit> fitted(trials.size());
        inParallel(trials.size(), 1, [&](size_t i, int) {
            stepIn(from + 0.4 * (to - from), to, double(i), double(trials.size()));
            const auto &[n, family, bold] = trials[i];
            fitted[i] = fitFont(reads[n].line.text, reads[n].words, family, bold, inks[n], reads[n].box.size(), inkBoxes[n], &darks[n]);
        });
        for (size_t i = 0; i < trials.size(); ++i)
            reads[std::get<0>(trials[i])].fits[{std::get<1>(trials[i]), std::get<2>(trials[i])}] = fitted[i];
    }
    // one font a block (its script's): the one most alike over all its lines; each line regular or bold in it
    std::map<std::tuple<int, int, bool>, std::map<QString, double>> votes;
    for (const Read &r : reads)
        for (const auto &[fb, fit] : r.fits)
            if (!fb.second) {
                const double s = std::max(fit.score, r.fits.at({fb.first, true}).score);
                if (s > -1)
                    votes[{r.group, 0, r.arabic}][fb.first] += s;
            }
    std::vector<Document::OcrLine> out;
    std::vector<QRect> outBoxes;  // (each line's box on the scan, and how like its font it is - and its words')
    std::vector<double> outFits;
    std::vector<std::vector<QRect>> outWords;
    std::vector<double> outSures;
    std::vector<double> outSurest;  // (its surest word)
    std::vector<int> outCounts;
    std::vector<char> outPlain;  // (just words of letters - four or more)
    std::map<std::tuple<int, int, bool>, std::vector<size_t>> paragraphs;
    // bold or not: by the page's ink spread, below (boldness: a line's own guess - the scan's ink as much as the bold font
    // draws, or about - where its spread can't be told)
    auto familyOf = [&](const Read &r) {
        const auto &vote = votes[{r.group, 0, r.arabic}];
        return vote.empty() ? QString() : std::max_element(vote.begin(), vote.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
    };
    auto boldness = [&](const Read &r, const QString &family) {  // (> 0: bold - how far from halfway, as a log of the ink)
        const auto reg = r.fits.find({family, false}), bold = r.fits.find({family, true});
        if (reg == r.fits.end() || bold == r.fits.end() || bold->second.score <= -1 || reg->second.weight <= 0 || bold->second.weight <= 0)
            return -9.0;
        return std::log(1 / bold->second.weight) - std::log(0.95);
    };
    // (the page's ink spread: printed and scanned, every stroke's edge out by about as much - a distance, the same on the
    // whole page, a big letter's ink grown less than a small one's. Each line: how far out its edge would be, were it
    // regular - were it bold; the page's: the one most lines agree with, the least of those - ink spreads, it doesn't
    // shrink much; each line then the one of the two nearer it)
    struct Spread {
        double reg = 0, bold = 0, weight = 0;
    };
    std::vector<Spread> spreads(reads.size());
    std::vector<double> candidates;
    for (size_t n = 0; n < reads.size(); ++n) {
        const QString family = familyOf(reads[n]);
        const auto reg = reads[n].fits.find({family, false}), bold = reads[n].fits.find({family, true});
        if (family.isEmpty() || reg == reads[n].fits.end() || bold == reads[n].fits.end() || reg->second.edge <= 0 || bold->second.edge <= 0 ||
            reg->second.scanInk <= 0)
            continue;
        spreads[n] = {(reg->second.scanInk - reg->second.drawnInk) / reg->second.edge, (bold->second.scanInk - bold->second.drawnInk) / bold->second.edge,
                      reg->second.scanInk};
        for (const double d : {spreads[n].reg, spreads[n].bold})
            if (d >= -0.3)
                candidates.push_back(d);
    }
    double spread = 0;
    {
        auto cost = [&](double d) {
            double c = 0;
            for (const Spread &s : spreads)
                if (s.weight > 0)
                    c += s.weight * std::min({std::abs(s.reg - d), std::abs(s.bold - d), 1.5});
            return c;
        };
        double least = 1e300;
        for (const double d : candidates)
            least = std::min(least, cost(d));
        double best = 1e300;
        for (const double d : candidates)
            if (cost(d) <= least * 1.05 + 1e-9)
                best = std::min(best, d);
        if (best < 1e299)
            spread = best;
    }
    for (Read &r : reads) {
        stepIn(0.8, 0.88, double(&r - &reads[0]), double(reads.size()));  // (each line's size, place, font)
        const QString family = familyOf(r);
        if (family.isEmpty())
            continue;
        const Fit &reg = r.fits[{family, false}], &bold = r.fits[{family, true}];
        const double own = boldness(r, family);
        const Spread &sp = spreads[size_t(&r - &reads[0])];
        const bool isBold = sp.weight > 0 ? std::abs(sp.bold - spread) < std::abs(sp.reg - spread) : own > 0;
        Fit fit = isBold ? bold : reg;
        if (lineH > 0 && r.box.height() > 1.6 * lineH && !r.words.empty() && r.arabic) {
            // (a line a pen's stroke crosses - its ink's height isn't its letters': as big as its words are wide, its
            // baseline its densest row - Arabic's joins, a Latin line's feet)
            int x0 = 1 << 30, x1 = -1;
            for (const auto &[word, wb] : r.words)
                x0 = std::min(x0, wb.left()), x1 = std::max(x1, wb.right());
            QFont f(family);
            f.setPixelSize(100);
            f.setBold(isBold);
            const double natural = QFontMetricsF(f).horizontalAdvance(r.line.text);
            const size_t n = size_t(&r - &reads[0]);
            const int W0 = r.box.width();
            int row = -1;
            long most = 0;
            for (int y = 0; y < r.box.height(); ++y) {
                long c = 0;
                for (int x = 0; x < W0; ++x)
                    c += inks[n][size_t(y) * size_t(W0) + size_t(x)];
                if (c > most)
                    most = c, row = y;
            }
            if (natural > 0 && x1 > x0 && row >= 0) {
                fit.size = 100.0 * (x1 - x0) / natural / zoom;
                fit.advance = x1 - x0;
                fit.penX = x0;
                fit.baseline = row + 0.03 * fit.size * zoom;
            }
        }
        refine(fit, r.line.text, family, isBold, grey, r.box);
        if (r.arabic) {  // (its words the scan draws stretched: their tatweels in)
            bool stretched = false;
            for (auto &[word, wb] : r.words) {
                const QString with = kashidas(word, family, isBold, fit.size * zoom, grey, wb.translated(r.box.topLeft()));
                stretched = stretched || with != word;
                word = with;
            }
            if (stretched) {
                QStringList ws;
                for (const auto &[word, wb] : r.words)
                    ws << word;
                r.line.text = ws.join(QLatin1Char(' '));
            }
        }
        const double likeFont = std::max(reg.score, bold.score);  // (like its font, bold or not)
        if (likeFont < 0 || (likeFont < 0.3 && !r.wordy)) {
            continue;  // (like no font: not text after all - a logo's, a stamp's)
        }
        Document::OcrLine o;
        o.text = r.line.text;
        o.family = family;
        o.bold = isBold;
        o.size = std::max(2.0, fit.size);
        o.baseline = QPointF(r.box.left() + fit.penX, r.box.top() + fit.baseline) / zoom;
        o.width = fit.advance / zoom;
        // (the line's letters - read surely, fitted well - copied for the scan's own font)
        // (the line's letters copied for the font matched - if most of them are its glyphs as the scan has them; an Arabic
        // line in a typeface no font here is like - a letterhead's display Arabic - a font of its own: its words cut
        // into glyphs where they fall on the scan)
        const fonts::Found base = fonts::forText(fonts::findFamily(family, isBold, false), o.text);
        QString key = builderKey(base, isBold);
        auto &shared = (*builders)[key];
        if (!shared)
            shared = std::make_unique<scanfont::Builder>(base);
        int paper = 255, ink = 0;
        levels(grey, r.box, &paper, &ink);
        {  // (its ink's colour: the middle of its darkest pixels' - a scan's dark grey, a blue heading's blue)
            std::vector<int> cr, cg, cb;
            const QRect a = r.box.intersected(grey.rect());
            const int cut = ink + (paper - ink) / 4;
            for (int y = a.top(); y <= a.bottom(); ++y) {
                const uchar *g = grey.constScanLine(y);
                for (int x = a.left(); x <= a.right(); ++x)
                    if (g[x] <= cut) {
                        const QRgb c = scan.pixel(x, y);
                        cr.push_back(qRed(c)), cg.push_back(qGreen(c)), cb.push_back(qBlue(c));
                    }
            }
            if (cr.size() >= 20) {
                auto mid = [](std::vector<int> &v) {
                    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                    return v[v.size() / 2];
                };
                o.color = QColor(mid(cr), mid(cg), mid(cb));
            }
        }
        if (shared->ok() && r.sure >= 70 && (r.arabic || fit.score >= 0.42))
            shared->addLine(grey, o.text, QPointF(r.box.left() + fit.penX, r.box.top() + fit.baseline), fit.size * zoom, fit.advance, paper, ink);
        Choice choice;
        if (shared->ok() && r.arabic && r.sure >= 70) {
            choice.other = key + QStringLiteral("~%1.%2").arg(page).arg(r.group);
            choice.crop = grey;
            choice.box = r.box;
            choice.fit = fit;
            choice.text = o.text;
            choice.words = r.words;
            choice.bold = isBold;
            auto &builder = (*builders)[choice.other];
            if (!builder)
                builder = std::make_unique<scanfont::Builder>(base);
            // (its words as big as the line's mostly are - a word whose box took in a mark of the line above isn't made small)
            std::vector<double> scales;
            for (const auto &[word, wb] : r.words) {
                const double sc = builder->wordScale(grey, word, wb.translated(r.box.topLeft()), fit.size * zoom, paper, ink);
                if (sc > 0)
                    scales.push_back(sc);
            }
            double scale = 1;
            if (!scales.empty()) {
                std::nth_element(scales.begin(), scales.begin() + long(scales.size() / 2), scales.end());
                scale = scales[scales.size() / 2];
            }
            for (const auto &[word, wb] : r.words) {
                const QRect at = wb.translated(r.box.topLeft());
                const double x = at.center().x();
                const double y = r.base1.x() != r.base0.x()
                                     ? r.base0.y() + (r.base1.y() - r.base0.y()) * (x - r.base0.x()) / double(r.base1.x() - r.base0.x())
                                     : r.base0.y();
                builder->addWord(grey, word, at, y, fit.size * zoom * scale, paper, ink, nullptr, false);
            }
        }
        paragraphs[{r.block, r.para, r.arabic}].push_back(out.size());
        keys->push_back(key);
        choices->push_back(std::move(choice));
        out.push_back(o);
        outBoxes.push_back(r.box);
        outFits.push_back(std::max(reg.score, bold.score));  // (how like its font - bold or not: a bold heading taken for a regular one isn't a logo's lettering)
        outSures.push_back(r.sure);
        outSurest.push_back(r.wordSure.empty() ? r.sure : double(*std::max_element(r.wordSure.begin(), r.wordSure.end())));
        {
            int letters = 0, other = 0;  // (its words' letters - a sign alone, "+", not counted)
            for (const QString &word : r.line.text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                bool any = false;
                for (const QChar c : word)
                    any = any || c.isLetterOrNumber();
                if (any)
                    for (const QChar c : word)
                        (c.isLetter() ? letters : other) += 1;
            }
            outPlain.push_back(char(other == 0 && letters >= 4));
        }
        outCounts.push_back(int(r.words.size()));
        outWords.emplace_back();
        for (const auto &[word, wb] : r.words) {
            QRect at = wb.translated(r.box.topLeft());
            // (a word as tall as the line read again whole - a pen's strokes through it: just its letters' rows, round
            // its baseline - the strokes above and below it a picture's, taken out)
            if (lineH > 0 && at.height() > 1.6 * lineH) {
                const double base = r.box.top() + fit.baseline, em = fit.size * zoom;
                at.setTop(std::max(at.top(), int(base - 0.95 * em)));
                at.setBottom(std::min(at.bottom(), int(base + 0.4 * em)));
            }
            outWords.back().push_back(at);
        }
    }
    // (a paragraph's sizes alike: those near its middle size made it)
    for (const auto &[key, members] : paragraphs) {
        std::vector<double> sizes;
        for (size_t n : members)
            sizes.push_back(out[n].size);
        std::nth_element(sizes.begin(), sizes.begin() + long(sizes.size() / 2), sizes.end());
        const double middle = sizes[sizes.size() / 2];
        for (size_t n : members)
            if (std::abs(out[n].size - middle) < 0.12 * middle)
                out[n].size = std::round(middle * 2) / 2;
    }
    // the page's pictures, each its own: cut out of the scan - and a logo's lettering not taken for text
    if (!outBoxes.empty()) {
        std::vector<int> hs;
        for (const QRect &b : outBoxes)
            hs.push_back(b.height());
        std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
        const double lineH = hs[hs.size() / 2];
        std::vector<QRect> words;
        for (const auto &ws : outWords)
            words.insert(words.end(), ws.begin(), ws.end());
        std::vector<char> absorbed;
        // (the page's rules and frame - long straight runs of ink - taken out first: a logo touching a rule is a logo)
        std::vector<char> markInk = pageInk;
        {
            const int W0 = grey.width(), H0 = grey.height(), longest = int(2.5 * lineH);
            for (int y = 0; y < H0; ++y)
                for (int x = 0; x < W0;) {
                    if (!pageInk[size_t(y) * size_t(W0) + size_t(x)]) {
                        ++x;
                        continue;
                    }
                    int e = x;
                    while (e < W0 && pageInk[size_t(y) * size_t(W0) + size_t(e)])
                        ++e;
                    if (e - x >= longest)
                        for (int k = x; k < e; ++k)
                            markInk[size_t(y) * size_t(W0) + size_t(k)] = 0;
                    x = e;
                }
            for (int x = 0; x < W0; ++x)
                for (int y = 0; y < H0;) {
                    if (!pageInk[size_t(y) * size_t(W0) + size_t(x)]) {
                        ++y;
                        continue;
                    }
                    int e = y;
                    while (e < H0 && pageInk[size_t(e) * size_t(W0) + size_t(x)])
                        ++e;
                    if (e - y >= longest)
                        for (int k = y; k < e; ++k)
                            markInk[size_t(k) * size_t(W0) + size_t(x)] = 0;
                    y = e;
                }
        }
        const std::vector<Blob> marks = blobsOf(markInk, grey.width(), grey.height());
        std::vector<char> ruleInk(pageInk.size(), 0);  // (the rules and frame taken out)
        for (size_t i = 0; i < pageInk.size(); ++i)
            ruleInk[i] = pageInk[i] && !markInk[i];
        stepAt(0.88, QObject::tr("Finding the pictures"));
        std::vector<QRect> regions = pictureRegions(marks, words, outBoxes, outFits, outSures, outSurest, outCounts, outPlain, lineH, grey.size(), &absorbed, ruleInk);
        std::vector<QRect> kept;  // (the words left text - not a picture's)
        for (size_t n = 0; n < outWords.size(); ++n)
            if (!absorbed[n])
                kept.insert(kept.end(), outWords[n].begin(), outWords[n].end());
        // (each picture as big as its marks are: a mark touching it - not a word's that's text, not a frame - its too: a logo's
        // "2030" read as a line of junk, then taken for the picture's - grown till nothing more touches it)
        {
            const int W0 = grey.width();
            const int g = int(0.35 * lineH), pad = int(0.1 * lineH);
            auto inKept = [&](const QPoint &c) {
                for (const QRect &wb : kept)
                    if (wb.adjusted(-pad, -pad, pad, pad).contains(c))
                        return true;
                return false;
            };
            auto inTextRow = [&](const QRect &b) {  // (level with a word of text, not far along: an unread part of its line)
                for (const QRect &wb : kept)
                    if (b.center().y() >= wb.top() && b.center().y() <= wb.bottom() &&
                        std::max(wb.left() - b.right(), b.left() - wb.right()) <= 8 * lineH)
                        return true;
                return false;
            };
            std::vector<char> usable(marks.size(), 0);
            for (size_t m = 0; m < marks.size(); ++m) {
                const QRect &b = marks[m].box;
                // (not a piece of a rule - a tilted rule's straight runs are short: its steps left as thin pieces)
                const QRect near = b.adjusted(-3, -3, 3, 3).intersected(grey.rect());
                bool touches = false;  // (the page's rules right by it)
                for (int y = near.top(); y <= near.bottom() && !touches; ++y)
                    for (int x = near.left(); x <= near.right() && !touches; ++x)
                        touches = ruleInk[size_t(y) * size_t(W0) + size_t(x)] != 0;
                const bool elongated = (b.height() <= 0.3 * lineH && b.width() >= 3 * b.height()) || (b.width() <= 0.3 * lineH && b.height() >= 3 * b.width());
                // (... nor a frame's curved corner: a thin curve, little of its box inked, touching the page's rules)
                const bool thinPiece = (elongated && (std::max(b.width(), b.height()) >= 1.2 * lineH || touches)) ||
                                       (touches && double(marks[m].area) < 0.18 * b.width() * b.height() && std::max(b.width(), b.height()) >= lineH);
                usable[m] = marks[m].area >= 0.005 * lineH * lineH && b.width() < 0.25 * W0 && b.height() < 0.25 * grey.height() && !inKept(b.center()) &&
                            !thinPiece;
            }
            for (QRect &r : regions)
                for (bool more = true; more;) {
                    more = false;
                    for (size_t m = 0; m < marks.size(); ++m)
                        // (... or a small mark a line away: a signature's pen lifted, its strokes apart)
                        if (usable[m] && !r.contains(marks[m].box) &&
                            (r.adjusted(-g, -g, g, g).intersects(marks[m].box) ||
                             (marks[m].box.width() <= 1.5 * lineH && marks[m].box.height() <= 1.5 * lineH && !inTextRow(marks[m].box) &&
                              r.adjusted(-int(lineH), -int(lineH), int(lineH), int(lineH)).intersects(marks[m].box)))) {
                            r |= marks[m].box;
                            more = true;
                        }
                }
            for (bool merged = true; merged;) {
                merged = false;
                for (size_t a = 0; a < regions.size() && !merged; ++a)
                    for (size_t b = a + 1; b < regions.size() && !merged; ++b)
                        if (regions[a].intersects(regions[b])) {
                            regions[a] |= regions[b];
                            regions.erase(regions.begin() + long(b));
                            merged = true;
                        }
            }
        }
        const QImage rgb = scan.convertToFormat(QImage::Format_RGB888);
        const int W = grey.width();
        // (the scan itself - the page's picture behind all - if it's upright and whole: its pictures' marks taken out of it)
        QImage behind;
        QRectF behindAt;
        for (const Picture &p : doc->pictures(page))
            if (!p.vector && p.crop.isNull() && p.rect.width() * p.rect.height() > 0.5 * size.width() * size.height() &&
                (p.corners.size() != 4 || (std::abs(p.corners[0].y() - p.corners[1].y()) < 0.5 && std::abs(p.corners[0].x() - p.corners[3].x()) < 0.5))) {
                behind = doc->pictureImage(p).convertToFormat(QImage::Format_RGB888);
                behindAt = p.rect;
                *background = p;
                break;
            }
        const QImage original = behind;  // (as it was: the pictures cut from it)
        int pagePaper = 255;  // (the page's paper: its lightest half's middle grey)
        {
            int hist[256] = {};
            for (int y = 0; y < grey.height(); y += 4) {
                const uchar *row = grey.constScanLine(y);
                for (int x = 0; x < grey.width(); x += 4)
                    ++hist[row[x]];
            }
            long total = 0, sum = 0;
            for (int k = 0; k < 256; ++k)
                total += hist[k];
            for (int k = 255; k >= 0; --k)
                if ((sum += hist[k]) >= total / 4) {
                    pagePaper = k;
                    break;
                }
        }
        // (a pen's strokes - a signature in blue ink over black print: told from the text by their colour, even where they
        // cross a line of it - its arc over the words, its stroke through them: the signature's, all of it, its own picture)
        std::vector<char> pen;  // (empty: the print isn't black, or no coloured strokes)
        {
            const int H = grey.height();
            auto chroma = [&](int x, int y) {
                const uchar *c = rgb.constScanLine(y) + 3 * x;
                return int(std::max({c[0], c[1], c[2]})) - int(std::min({c[0], c[1], c[2]}));
            };
            std::vector<int> printed;  // (the words' ink: how coloured the print is)
            for (const QRect &wb : kept)
                for (int y = std::max(0, wb.top()); y <= std::min(H - 1, wb.bottom()); y += 2)
                    for (int x = std::max(0, wb.left()); x <= std::min(W - 1, wb.right()); x += 2)
                        if (pageInk[size_t(y) * size_t(W) + size_t(x)])
                            printed.push_back(chroma(x, y));
            int printChroma = 255;
            if (printed.size() > 200) {
                std::nth_element(printed.begin(), printed.begin() + long(printed.size() / 2), printed.end());
                printChroma = printed[printed.size() / 2];
            }
            if (printChroma <= 4) {
                std::vector<char> ink(size_t(W) * size_t(H), 0);
                bool any = false;
                for (int y = 0; y < H; ++y) {
                    const uchar *g = grey.constScanLine(y);
                    for (int x = 0; x < W; ++x)
                        if (g[x] < pagePaper - 40 && chroma(x, y) >= 8 && !ruleInk[size_t(y) * size_t(W) + size_t(x)])
                            ink[size_t(y) * size_t(W) + size_t(x)] = 1, any = true;
                }
                if (any) {
                    std::vector<char> near(ink.size(), 0);  // (its strokes a little grown: a stroke broken where it crosses a letter, one)
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x)
                            if (ink[size_t(y) * size_t(W) + size_t(x)])
                                for (int dy = -2; dy <= 2; ++dy)
                                    for (int dx = -2; dx <= 2; ++dx) {
                                        const int nx = x + dx, ny = y + dy;
                                        if (nx >= 0 && ny >= 0 && nx < W && ny < H)
                                            near[size_t(ny) * size_t(W) + size_t(nx)] = 1;
                                    }
                    pen.assign(ink.size(), 0);
                    const int pad = int(0.1 * lineH);
                    std::vector<QRect> strong, weak;  // (strokes surely a pen's; a pen's over words - taken only with those)
                    for (const Blob &st : blobsOf(near, W, H)) {
                        const QRect &b = st.box;
                        if (std::max(b.width(), b.height()) < lineH || st.area < 0.3 * lineH * lineH || b.width() > 0.4 * W ||
                            b.height() > 0.25 * H || std::min(b.width(), b.height()) <= 0.25 * lineH ||
                            std::max(b.width(), b.height()) >= 8 * std::min(b.width(), b.height()))
                            continue;  // (... nor the page's frame, a tinge of colour all round - a straight line: a rule)
                        // (most of it off the words: a pen's - not coloured print, a blue heading; the dark off them mostly
                        // coloured, one colour on the whole - not a black line's coloured fringe, a scan's every hue)
                        long all = 0, off = 0, dark = 0;
                        double br = 0, bg = 0, gr = 0;
                        for (int y = b.top(); y <= b.bottom(); ++y)
                            for (int x = b.left(); x <= b.right(); ++x) {
                                const size_t i = size_t(y) * size_t(W) + size_t(x);
                                if (!near[i] || (!ink[i] && grey.constScanLine(y)[x] >= pagePaper - 40))
                                    continue;
                                bool word = false;
                                for (const QRect &wb : kept)
                                    if (wb.adjusted(-pad, -pad, pad, pad).contains(x, y)) {
                                        word = true;
                                        break;
                                    }
                                if (ink[i]) {
                                    ++all, off += word ? 0 : 1;
                                    const uchar *c = rgb.constScanLine(y) + 3 * x;
                                    br += c[2] - c[0], bg += c[2] - c[1], gr += c[1] - c[0];
                                }
                                dark += word ? 0 : 1;
                            }
                        const double hue = all ? std::max({std::abs(br), std::abs(bg), std::abs(gr)}) / double(all) : 0;
                        if (all == 0 || hue < 12)  // (a pen's ink: clearly coloured - a letterhead's navy print, much less)
                            continue;
                        if (off >= 0.4 * all && off >= 0.6 * dark)
                            strong.push_back(b);
                        else
                            weak.push_back(b);
                    }
                    // (each joined to the picture it touches - a signature's scribble - grown over it: its arc, the stroke
                    // through the words between, one picture; a big one touching none: its own)
                    std::vector<char> used(strong.size() + weak.size(), 0);
                    const int L = int(1.6 * lineH);  // (a signature's pen lifted: its parts a line or so apart)
                    for (bool more = true; more;) {
                        more = false;
                        for (size_t k = 0; k < used.size(); ++k) {
                            if (used[k])
                                continue;
                            const QRect &b = k < strong.size() ? strong[k] : weak[k - strong.size()];
                            for (QRect &r : regions)
                                if (r.adjusted(-L, -L, L, L).intersects(b))
                                    r |= b, used[k] = 1;
                            more = more || used[k];
                        }
                        for (size_t k = 0; k < strong.size() && !more; ++k)
                            if (!used[k] && std::max(strong[k].width(), strong[k].height()) >= 2 * lineH)
                                regions.push_back(strong[k]), used[k] = 1, more = true;
                    }
                    // (over a word, only what's clearly the pen's: a black letter's own pixels have a little colour too - a
                    // JPEG's - and stay the letter's)
                    std::vector<char> overWords(ink.size(), 0);
                    for (const QRect &wb : kept) {
                        const QRect a = wb.adjusted(-pad, -pad, pad, pad).intersected(grey.rect());
                        for (int y = a.top(); y <= a.bottom(); ++y)
                            std::fill_n(overWords.begin() + long(size_t(y) * size_t(W) + size_t(a.left())), a.width(), char(1));
                    }
                    for (size_t k = 0; k < used.size(); ++k) {
                        if (!used[k])
                            continue;
                        const QRect &b = k < strong.size() ? strong[k] : weak[k - strong.size()];
                        for (int y = b.top(); y <= b.bottom(); ++y)
                            for (int x = b.left(); x <= b.right(); ++x) {
                                const size_t i = size_t(y) * size_t(W) + size_t(x);
                                if (!ink[i])
                                    continue;
                                const uchar *c = rgb.constScanLine(y) + 3 * x;
                                if (overWords[i] && std::abs(int(c[2]) - int(c[0])) < 8)
                                    continue;
                                pen[i] = 1;
                            }
                    }
                    for (bool merged = true; merged;) {
                        merged = false;
                        for (size_t a = 0; a < regions.size() && !merged; ++a)
                            for (size_t c = a + 1; c < regions.size() && !merged; ++c)
                                if (regions[a].intersects(regions[c])) {
                                    regions[a] |= regions[c];
                                    regions.erase(regions.begin() + long(c));
                                    merged = true;
                                }
                    }
                }
            }
        }
        const int grow = std::max(5, int(0.14 * lineH));  // (its marks grown - their soft edges, a JPEG's fringe too: no ghost left behind)
        int regionNo = 0;
        for (QRect r : regions) {
            stepIn(0.88, 0.97, double(regionNo++), double(regions.size()));
            r = r.adjusted(-int(0.12 * lineH), -int(0.12 * lineH), int(0.12 * lineH), int(0.12 * lineH)).intersected(rgb.rect());
            // (the long straight runs in it: its own - a logo's bars, most of the run in it - or the page's rules going on past
            // it: those neither cut out nor taken with it, nor a little round them)
            const int H0 = grey.height(), longest = int(2.5 * lineH);
            std::vector<char> own(size_t(r.width()) * size_t(r.height()), 0), pageRule(own.size(), 0);
            // (a rule's whole length: followed through the page's ink across the pixel steps a tilted scan's line makes - it
            // drifts a little, never far: not into a letter it touches)
            auto runOf = [&](int x, int y, bool across, int *from, int *to) {
                auto ink = [&](int p, int s) {
                    const int u = across ? p : s, v = across ? s : p;
                    return u >= 0 && v >= 0 && u < W && v < H0 && pageInk[size_t(v) * size_t(W) + size_t(u)];
                };
                const int p0 = across ? x : y, s0 = across ? y : x, drift = std::max(2, int(0.3 * lineH));
                for (int dir : {1, -1}) {
                    int p = p0, s = s0;
                    for (int steps = 0; steps < 200; ++steps) {
                        while (ink(p + dir, s))
                            p += dir;
                        if (std::abs(s - 1 - s0) <= drift && ink(p + dir, s - 1))
                            p += dir, --s;
                        else if (std::abs(s + 1 - s0) <= drift && ink(p + dir, s + 1))
                            p += dir, ++s;
                        else
                            break;
                    }
                    (dir > 0 ? *to : *from) = p;
                }
            };
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x) {
                    const int px = r.left() + x, py = r.top() + y;
                    if (!ruleInk[size_t(py) * size_t(W) + size_t(px)])
                        continue;
                    int a0, a1, d0, d1;
                    runOf(px, py, true, &a0, &a1);
                    runOf(px, py, false, &d0, &d1);
                    const bool across = a1 - a0 >= d1 - d0;
                    const int from = across ? a0 : d0, to = across ? a1 : d1, lo = across ? r.left() : r.top(), hi = across ? r.right() : r.bottom();
                    const bool inside = to - from + 1 < longest || std::min(to, hi) - std::max(from, lo) + 1 >= 0.85 * (to - from + 1);
                    (inside ? own : pageRule)[size_t(y) * size_t(r.width()) + size_t(x)] = inside ? 1 : (across ? 1 : 2);  // (a page rule: 1 across, 2 down)
                }
            {  // (a tilted rule's steps - thin pieces, long one way, touching the page's rule or the picture's edge: the rule's too)
                const int rw = r.width(), rh = r.height();
                std::vector<char> seen(own.size(), 0);
                std::vector<size_t> piece, todo;
                for (size_t start = 0; start < own.size(); ++start) {
                    const int sx = int(start % size_t(rw)), sy = int(start / size_t(rw));
                    if (seen[start] || !markInk[size_t(r.top() + sy) * size_t(W) + size_t(r.left() + sx)])
                        continue;
                    piece.clear();
                    todo.assign(1, start);
                    seen[start] = 1;
                    int x0 = sx, x1 = sx, y0 = sy, y1 = sy;
                    bool touches = false, across = false, down = false;  // (... the page's rules both ways: a frame's corner)
                    while (!todo.empty()) {
                        const size_t i = todo.back();
                        todo.pop_back();
                        piece.push_back(i);
                        const int x = int(i % size_t(rw)), y = int(i / size_t(rw));
                        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                        touches = touches || x == 0 || y == 0 || x == rw - 1 || y == rh - 1;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= rw || ny >= rh)
                                    continue;
                                const size_t j = size_t(ny) * size_t(rw) + size_t(nx);
                                touches = touches || pageRule[j];
                                across = across || pageRule[j] == 1, down = down || pageRule[j] == 2;
                                if (!seen[j] && markInk[size_t(r.top() + ny) * size_t(W) + size_t(r.left() + nx)])
                                    seen[j] = 1, todo.push_back(j);
                            }
                    }
                    const int pw = x1 - x0 + 1, ph = y1 - y0 + 1;
                    const bool thin = (ph <= 0.3 * lineH && pw >= 3 * ph) || (pw <= 0.3 * lineH && ph >= 3 * pw) ||
                                      (double(piece.size()) < 0.18 * pw * ph && std::max(pw, ph) >= lineH) ||  // (or a frame's curved corner)
                                      (double(piece.size()) < 0.35 * pw * ph && across && down);
                    if (thin && touches)
                        for (size_t i : piece)
                            pageRule[i] = 1;
                }
            }
            {  // (a page's rule a little grown: its soft edge too)
                std::vector<char> grown = pageRule;
                for (int y = 0; y < r.height(); ++y)
                    for (int x = 0; x < r.width(); ++x)
                        if (pageRule[size_t(y) * size_t(r.width()) + size_t(x)])
                            for (int dy = -2; dy <= 2; ++dy)
                                for (int dx = -2; dx <= 2; ++dx) {
                                    const int nx = x + dx, ny = y + dy;
                                    if (nx >= 0 && ny >= 0 && nx < r.width() && ny < r.height())
                                        grown[size_t(ny) * size_t(r.width()) + size_t(nx)] = 1;
                                }
                pageRule = std::move(grown);
            }
            // (a picture a pen's strokes are in - a signature over a line of text: the letters it crosses, all of each - past
            // its word's box, a letter's top the box cut off - the text's: found from the words, through their black ink -
            // not the pen's coloured strokes - in the line's band; empty: none)
            std::vector<char> textInk;
            bool penIn = false;
            for (int y = r.top(); y <= r.bottom() && !pen.empty() && !penIn; ++y)
                for (int x = r.left(); x <= r.right() && !penIn; ++x)
                    penIn = pen[size_t(y) * size_t(W) + size_t(x)] != 0;
            if (penIn) {
                const int rw = r.width(), rh = r.height();
                auto penish = [&](int gx, int gy) {  // (the pen's, or by it - its stroke's black core - or bluish as it is)
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int nx = gx + dx, ny = gy + dy;
                            if (nx >= 0 && ny >= 0 && nx < W && ny < grey.height() && pen[size_t(ny) * size_t(W) + size_t(nx)])
                                return true;
                        }
                    const uchar *c = rgb.constScanLine(gy) + 3 * gx;
                    return int(std::max({c[0], c[1], c[2]})) - int(std::min({c[0], c[1], c[2]})) >= 8 && std::abs(int(c[2]) - int(c[0])) >= 6;
                };
                std::vector<char> band(own.size(), 0), letters(own.size(), 0);
                std::vector<size_t> todo;
                for (const QRect &wb : kept) {
                    const QRect a = wb.adjusted(-int(0.5 * lineH), -int(0.5 * lineH), int(0.5 * lineH), int(0.5 * lineH)).intersected(r);
                    for (int y = a.top(); y <= a.bottom(); ++y)
                        for (int x = a.left(); x <= a.right(); ++x)
                            band[size_t(y - r.top()) * size_t(rw) + size_t(x - r.left())] = 1;
                    const QRect in = wb.intersected(r);
                    for (int y = in.top(); y <= in.bottom(); ++y)
                        for (int x = in.left(); x <= in.right(); ++x) {
                            const size_t li = size_t(y - r.top()) * size_t(rw) + size_t(x - r.left());
                            if (pageInk[size_t(y) * size_t(W) + size_t(x)] && !penish(x, y) && !letters[li])
                                letters[li] = 1, todo.push_back(li);
                        }
                }
                while (!todo.empty()) {
                    const size_t i = todo.back();
                    todo.pop_back();
                    const int x = int(i % size_t(rw)), y = int(i / size_t(rw));
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= rw || ny >= rh)
                                continue;
                            const size_t j = size_t(ny) * size_t(rw) + size_t(nx);
                            if (!letters[j] && band[j] && pageInk[size_t(r.top() + ny) * size_t(W) + size_t(r.left() + nx)] &&
                                !penish(r.left() + nx, r.top() + ny))
                                letters[j] = 1, todo.push_back(j);
                        }
                }
                textInk.assign(own.size(), 0);
                for (int y = 0; y < rh; ++y)  // (and their soft edges)
                    for (int x = 0; x < rw; ++x)
                        if (letters[size_t(y) * size_t(rw) + size_t(x)])
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const int nx = x + dx, ny = y + dy;
                                    if (nx >= 0 && ny >= 0 && nx < rw && ny < rh && !pen[size_t(r.top() + ny) * size_t(W) + size_t(r.left() + nx)])
                                        textInk[size_t(ny) * size_t(rw) + size_t(nx)] = 1;
                                }
            }
            // (its marks: the ink in it that isn't a word's, a little grown - just those, see-through round them)
            std::vector<char> mask(size_t(r.width()) * size_t(r.height()), 0);
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x) {
                    const QPoint at(r.left() + x, r.top() + y);
                    const size_t gi = size_t(at.y()) * size_t(W) + size_t(at.x());
                    if (!textInk.empty()) {  // (a signature's picture: all but the letters it crosses - their ink found above)
                        if (textInk[size_t(y) * size_t(r.width()) + size_t(x)] ||
                            (!pageInk[gi] && !pen[gi] && grey.constScanLine(at.y())[at.x()] >= pagePaper - 50))
                            continue;
                        for (int dy = -grow; dy <= grow; ++dy)
                            for (int dx = -grow; dx <= grow; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && ny >= 0 && nx < r.width() && ny < r.height())
                                    mask[size_t(ny) * size_t(r.width()) + size_t(nx)] = 1;
                            }
                        continue;
                    }
                    // (... and what's lighter but clearly not paper - a blue pen's thin strokes)
                    if (!markInk[size_t(at.y()) * size_t(W) + size_t(at.x())] && !own[size_t(y) * size_t(r.width()) + size_t(x)] &&
                        grey.constScanLine(at.y())[at.x()] >= pagePaper - 50)
                        continue;
                    // (a word's - its letters' faint edges too, round its box, when it's light)
                    const int margin = markInk[size_t(at.y()) * size_t(W) + size_t(at.x())] || own[size_t(y) * size_t(r.width()) + size_t(x)] ? 0 : int(0.12 * lineH);
                    bool word = false;
                    for (const QRect &wb : kept)
                        if (wb.adjusted(-margin, -margin, margin, margin).contains(at)) {
                            word = true;
                            break;
                        }
                    if (word)
                        continue;
                    for (int dy = -grow; dy <= grow; ++dy)
                        for (int dx = -grow; dx <= grow; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < r.width() && ny < r.height())
                                mask[size_t(ny) * size_t(r.width()) + size_t(nx)] = 1;
                        }
                }
            // (the picture shows more than is taken out from under it - the scan behind saved again blurs its paper's
            // edge a few pixels round: under the picture still)
            std::vector<char> shown(mask.size(), 0);
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x)
                    if (mask[size_t(y) * size_t(r.width()) + size_t(x)])
                        for (int dy = -8; dy <= 8; ++dy)
                            for (int dx = -8; dx <= 8; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && ny >= 0 && nx < r.width() && ny < r.height())
                                    shown[size_t(ny) * size_t(r.width()) + size_t(nx)] = 1;
                            }
            {  // (a picture on a tinted ground of its own - a logo's grey box: all of its box, not just its marks)
                long paperPx = 0, tinted = 0;
                for (int y = 0; y < r.height(); ++y) {
                    const uchar *row = grey.constScanLine(r.top() + y);
                    for (int x = 0; x < r.width(); ++x)
                        if (!pageInk[size_t(r.top() + y) * size_t(W) + size_t(r.left() + x)])
                            ++paperPx, tinted += row[r.left() + x] < pagePaper - 12 ? 1 : 0;
                }
                if (paperPx > 0 && tinted >= 0.25 * paperPx)
                    for (int y = 0; y < r.height(); ++y)
                        for (int x = 0; x < r.width(); ++x) {
                            const QPoint at(r.left() + x, r.top() + y);
                            bool word = false;
                            for (const QRect &wb : kept)
                                word = word || wb.adjusted(-int(0.12 * lineH), -int(0.12 * lineH), int(0.12 * lineH), int(0.12 * lineH)).contains(at);
                            if (!word)
                                mask[size_t(y) * size_t(r.width()) + size_t(x)] = shown[size_t(y) * size_t(r.width()) + size_t(x)] = 1;
                        }
            }
            for (size_t i = 0; i < textInk.size(); ++i)  // (the letters a pen crossed: left behind, not shown with it)
                if (textInk[i])
                    mask[i] = 0, shown[i] = 0;
            if (!textInk.empty())  // (a "rule" by a pen's coloured edge: the pen's stroke - a signature's long flat tail, its core black)
                for (int y = 0; y < r.height(); ++y)
                    for (int x = 0; x < r.width(); ++x) {
                        char &rule = pageRule[size_t(y) * size_t(r.width()) + size_t(x)];
                        for (int dy = -3; dy <= 3 && rule; ++dy)
                            for (int dx = -3; dx <= 3 && rule; ++dx) {
                                const int gx = r.left() + x + dx, gy = r.top() + y + dy;
                                if (gx >= 0 && gy >= 0 && gx < W && gy < grey.height() && pen[size_t(gy) * size_t(W) + size_t(gx)])
                                    rule = 0;
                            }
                    }
            for (size_t i = 0; i < mask.size(); ++i)  // (the page's rule: left where it is - not taken out, not taken along)
                if (pageRule[i])
                    mask[i] = 0, shown[i] = 0;
            const unsigned paper = pagetext::paperIn(rgb, r);
            Document::ScanPicture sp;
            sp.paper = paper;
            if (!behind.isNull()) {
                // cut from the scan behind itself - its own pixels, where they are: exactly as it showed - and taken
                // out of it, paper where they were
                const double kx = behind.width() / behindAt.width(), ky = behind.height() / behindAt.height();
                const QRect in = QRectF((r.left() / zoom - behindAt.left()) * kx, (r.top() / zoom - behindAt.top()) * ky, r.width() / zoom * kx,
                                        r.height() / zoom * ky)
                                     .toAlignedRect()
                                     .intersected(behind.rect());
                if (in.isEmpty())
                    continue;
                auto at = [&](int x, int y, const std::vector<char> &m) {  // (this pixel of the scan behind: in m - on the page read?)
                    const int sx = int(((x + 0.5) / kx + behindAt.left()) * zoom) - r.left(), sy = int(((y + 0.5) / ky + behindAt.top()) * zoom) - r.top();
                    return sx >= 0 && sy >= 0 && sx < r.width() && sy < r.height() && m[size_t(sy) * size_t(r.width()) + size_t(sx)];
                };
                QImage pic(in.size(), QImage::Format_ARGB32);
                for (int y = in.top(); y <= in.bottom(); ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(pic.scanLine(y - in.top()));
                    const uchar *src = original.constScanLine(y);
                    for (int x = in.left(); x <= in.right(); ++x) {
                        const uchar *c = src + 3 * x;
                        row[x - in.left()] = qRgba(c[0], c[1], c[2], at(x, y, shown) ? 255 : 0);  // (see-through round it in its own colours: no white fringe where it is drawn smooth)
                    }
                }
                sp.box = QRectF(behindAt.left() + in.left() / kx, behindAt.top() + in.top() / ky, in.width() / kx, in.height() / ky);
                sp.image = pic;
                const uchar pr = uchar(paper >> 16), pg = uchar(paper >> 8), pb = uchar(paper);
                for (int y = in.top(); y <= in.bottom(); ++y) {
                    uchar *row = behind.scanLine(y);
                    for (int x = in.left(); x <= in.right(); ++x)
                        if (at(x, y, mask))
                            row[3 * x] = pr, row[3 * x + 1] = pg, row[3 * x + 2] = pb;
                }
            } else {
                QImage pic(r.size(), QImage::Format_ARGB32);
                for (int y = 0; y < r.height(); ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(pic.scanLine(y));
                    const uchar *src = rgb.constScanLine(r.top() + y);
                    for (int x = 0; x < r.width(); ++x) {
                        const uchar *c = src + 3 * (r.left() + x);
                        row[x] = qRgba(c[0], c[1], c[2], shown[size_t(y) * size_t(r.width()) + size_t(x)] ? 255 : 0);
                    }
                }
                sp.box = QRectF(QPointF(r.topLeft()) / zoom, QSizeF(r.size()) / zoom);
                sp.image = pic;
            }
            pictures->push_back(std::move(sp));
        }
        if (!behind.isNull() && !regions.empty()) {
            QBuffer buf(cleaned);
            buf.open(QIODevice::WriteOnly);
            behind.save(&buf, "JPG", 95);
        }
        for (size_t n = out.size(); n-- > 0;)
            if (absorbed[n]) {
                out.erase(out.begin() + long(n));
                keys->erase(keys->begin() + long(n));
                choices->erase(choices->begin() + long(n));
            }
    }
    return out;
}

}  // namespace

namespace {

// the scan's box: in the window's bottom right while it reads - what it's doing (and which page of how many, at
// its right), how far. Its X stops the scan (nothing of it kept): it goes once the reading has stopped
class ScanBox : public ClassicWindow {
public:
    ScanBox(QWidget *owner, QWidget *corner) : ClassicWindow(false, owner), corner_(corner)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setTitle(QObject::tr("Scanning"));
        setHelpButton(false);
        setShownActive(true);
        auto *box = new QVBoxLayout(body());
        box->setContentsMargins(12, 12, 12, 14);
        box->setSpacing(8);
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        label_ = new QLabel(body());
        label_->setFont(theme::font());
        count_ = new QLabel(body());
        count_->setFont(theme::font());
        count_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(label_, 1);
        row->addWidget(count_);
        box->addLayout(row);
        bar_ = new ClassicProgress(body());
        box->addWidget(bar_);
        body()->setFixedWidth(340);
    }
    void step(double f, const QString &what, const QString &count)
    {
        if (!what.isEmpty())
            label_->setText(what);
        if (!count.isEmpty())
            count_->setText(count);
        bar_->setValue(f);
    }
    // (shown over the pages' bottom right - only while its PDF is the tab shown)
    void place(bool shown)
    {
        if (!shown) {
            hide();
            return;
        }
        if (isVisible())
            return;
        adjustSize();
        const QPoint at = corner_->mapToGlobal(QPoint(corner_->width(), corner_->height()));
        move(at.x() - width() - 12, at.y() - height() - 12);
        show();
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        e->ignore();  // (closed by the scan's end)
        if (scanCancel)
            return;
        scanCancel = true;  // (the reading stops at its next step - the scan's end takes the box away)
        label_->setText(QObject::tr("Stopping..."));
    }

private:
    QWidget *corner_;
    QLabel *label_, *count_;
    ClassicProgress *bar_;
};

// what a font made of the scan's letters came to (made on the scan's thread, put in on the window's)
struct Built {
    QString key, ps;
    QByteArray font;
    bool bold = false;
    int count = 0;
};

}  // namespace

// The scan reading (its own thread): the pages asked for, what was read of them
struct ScanJob {
    std::shared_ptr<Document> doc;
    std::vector<int> pages;
    QRectF area;  // (just this part of the page, in its points - empty: the whole page)
    int count = 0;  // (the PDF's pages)
    std::thread thread;
    std::unique_ptr<ScanBox> box;
    std::unique_ptr<class ScanLock> lock;
    bool cancelled = false;
    // what was read
    QString langs, error;
    int skipped = 0;
    Builders builders;
    std::vector<std::pair<int, std::vector<Document::OcrLine>>> read;
    std::vector<std::vector<QString>> readKeys;
    std::vector<std::vector<Document::ScanPicture>> readPictures;
    std::vector<std::pair<Picture, QByteArray>> readBehind;  // (each page's scan behind - cleaned of its pictures' marks)
    std::vector<std::vector<Choice>> readChoices;
    std::vector<Built> built;
    QHash<QString, double> likeness;  // (a font cut from words: how like the font matched its glyphs are)
    ~ScanJob();
    void run();
};

// While a scan reads: its PDF's tab locked - its pages, tools, menus, keys take nothing (the hourglass over them) -
// the rest of the app as ever: the tabs (another PDF chosen, used), the title bar, other windows
class ScanLock : public QObject {
public:
    explicit ScanLock(MainWindow *w) : w_(w) { qApp->installEventFilter(this); }
    ~ScanLock() override
    {
        qApp->removeEventFilter(this);
        if (hourglass_)
            QApplication::restoreOverrideCursor();
    }
    bool locked() const
    {
        return w_->scan_ && w_->current_ >= 0 && w_->current_ < int(w_->docs_.size()) &&
               w_->docs_[size_t(w_->current_)].doc == w_->scan_->doc;
    }
    // (a widget of the window's, not the tabs' or the title bar's)
    bool lockedWidget(const QWidget *w) const
    {
        if (!w || w->window() != w_)
            return false;
        for (const QWidget *p = w; p; p = p->parentWidget())
            if (p == w_->tabs_ || p == w_->titleBar())
                return false;
        return true;
    }
    void hourglass()
    {
        const bool on = locked() && lockedWidget(QApplication::widgetAt(QCursor::pos()));
        if (on == hourglass_)
            return;
        hourglass_ = on;
        if (on)
            QApplication::setOverrideCursor(classic::waitCursor());
        else
            QApplication::restoreOverrideCursor();
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::MouseMove:
        case QEvent::Enter:
        case QEvent::Leave:
            hourglass();
            if (e->type() == QEvent::MouseMove && locked() && lockedWidget(qobject_cast<QWidget *>(o)))
                return true;  // (no hover: nothing lit under the mouse)
            return false;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::ContextMenu:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
            if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease)
                QTimer::singleShot(0, this, [this] { hourglass(); });  // (a tab chosen: the lock's gone, or come)
            return locked() && lockedWidget(qobject_cast<QWidget *>(o));
        case QEvent::Shortcut: {  // (the window's keys: only Ctrl+Tab's - another tab)
            if (!locked())
                return false;
            const QKeySequence k = static_cast<QShortcutEvent *>(e)->key();
            const bool tab = k == QKeySequence(Qt::CTRL | Qt::Key_Tab) || k == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab);
            if (tab)
                QTimer::singleShot(0, this, [this] { hourglass(); });
            return !tab;
        }
        default:
            return false;
        }
    }

private:
    MainWindow *w_;
    bool hourglass_ = false;
};

ScanJob::~ScanJob()
{
    if (thread.joinable()) {
        scanCancel = true;
        thread.join();
    }
}

void ScanJob::run()
{
    try {
        for (int page : pages) {
            // (a page with text of its own, or scanned already: left)
            const auto chars = doc->text(page);
            int drawn = 0;
            bool scanned = false;
            for (const TextChar &c : *chars) {
                if (!area.isEmpty() && !area.intersects(c.box))
                    continue;  // (a part of the page scanned: just what's in it)
                scanned = scanned || (c.flags & 32);
                drawn += (c.flags & 32) ? 0 : 1;
            }
            if (scanned || drawn > 30) {
                ++skipped;
                continue;
            }
            {  // (the box: this page's share of the whole - the fonts made after them all, the last tenth)
                const size_t at = size_t(std::find(pages.begin(), pages.end(), page) - pages.begin());
                const QString of = QObject::tr("Page %1 of %2").arg(page + 1).arg(count);
                ScanBox *b = box.get();
                auto last = std::make_shared<double>(-1);
                readStep = [b, at, of, last, n = pages.size()](double f, const QString &what) {
                    const double all = 0.9 * (double(at) + f) / double(n);
                    if (what.isEmpty() && all < *last + 0.002)
                        return;  // (not worth telling yet)
                    *last = all;
                    const QString doing = what.isEmpty() ? QString() : what + QStringLiteral("...");
                    QMetaObject::invokeMethod(b, [b, all, doing, of] { b->step(all, doing, of); }, Qt::QueuedConnection);
                };
                readStep(0, QObject::tr("Reading the text"));
            }
            std::vector<QString> keys;
            std::vector<Document::ScanPicture> pictures;
            Picture behind;
            QByteArray cleaned;
            std::vector<Choice> choices;
            std::vector<Document::OcrLine> found =
                readPage(doc.get(), page, langs, &error, &builders, &keys, &pictures, &behind, &cleaned, &choices, area);
            if (scanCancel)
                throw ScanCancelled();  // (Tesseract stopped part way: not an error)
            if (!error.isEmpty())
                break;
            read.emplace_back(page, std::move(found));
            readKeys.push_back(std::move(keys));
            readPictures.push_back(std::move(pictures));
            readBehind.emplace_back(behind, cleaned);
            readChoices.push_back(std::move(choices));
        }
        // the scan's own fonts made (Acrobat's custom fonts): each font matched, with the scan's letters' shapes
        if (error.isEmpty() && !read.empty()) {
            ScanBox *b = box.get();
            size_t made = 0;
            for (auto &[key, builder] : builders) {
                const double all = 0.9 + 0.07 * double(made++) / double(std::max<size_t>(1, builders.size()));
                const QString doing = made == 1 ? QObject::tr("Making fonts of the scan's letters...") : QString();
                if (scanCancel)
                    throw ScanCancelled();
                QMetaObject::invokeMethod(b, [b, all, doing] { b->step(all, doing, QString()); }, Qt::QueuedConnection);
                if (!builder->ok())
                    continue;
                const bool bold = key.section(QLatin1Char('~'), 0, 0).endsWith(QLatin1Char('b'));
                // (a font cut from a group's words: made only if the scan's typeface isn't the one matched - its glyphs
                // unlike the matched font's)
                if (key.contains(QLatin1Char('~'))) {
                    const double like = builder->measureLikeness();
                    if (like < 0 || like >= 0.65)
                        continue;
                }
                // (the font matched: its own shapes as heavy, as soft as the scan's - one cut from words: the scan's shapes)
                builder->setCopyShapes(key.contains(QLatin1Char('~')));
                Built f;
                f.key = key;
                f.bold = bold;
                f.font = builder->build(builder->base().family + (bold ? QStringLiteral("Bold") : QString()), bold, &f.count, &f.ps);
                likeness.insert(key, builder->likeness());
                if (!f.font.isEmpty())
                    built.push_back(std::move(f));
            }
        }
    } catch (const ScanCancelled &) {
        cancelled = true;
    }
    readStep = [](double, const QString &) {};
}

void MainWindow::stopScan(const std::shared_ptr<Document> &doc)
{
    if (!scan_ || (doc && scan_->doc != doc))
        return;
    scanCancel = true;
    if (!doc && scan_->thread.joinable())  // (the window closing: waited for here)
        scan_->thread.join();
}

// Scan (the toolbar's): what to scan, from a menu under it - the whole PDF, the page shown, pages typed in, or a part of
// the page boxed with the mouse
void MainWindow::scanDialog()
{
    auto doc = view_->document();
    if (!doc || current_ < 0)
        return;
    if (scan_) {
        classicMessage(this, tr("Scan"), tr("A scan is reading already - another can be started once it's done."));
        return;
    }
    if (!doc->editable()) {
        classicMessage(this, tr("Scan"), tr("Only a PDF's pages can be scanned for text."));
        return;
    }
    if (ocr::languages().isEmpty()) {
        classicMessage(this, tr("Scan"), tr("Text recognition's languages weren't found (a \"tessdata\" folder next to the program)."));
        return;
    }
    const int count = doc->pageCount();
    QMenu menu(this);
    menu.addAction(count == 1 ? tr("&Whole document (1 page)") : tr("&Whole document (%1 pages)").arg(count), this, [this, count] {
        std::vector<int> pages;
        for (int n = 0; n < count; ++n)
            pages.push_back(n);
        startScan(pages);
    });
    menu.addAction(tr("&This page (page %1)").arg(std::max(0, view_->currentPage()) + 1), this,
                   [this] { startScan({std::max(0, view_->currentPage())}); });
    menu.addAction(tr("&Pages..."), this, [this] { scanPages(); })->setEnabled(count > 1);
    menu.addSeparator();
    menu.addAction(tr("An &area of the page..."), this, [this] {
        setTool(QStringLiteral("scanarea"), false);
        say(tr("Scan an area: drag a box round the part of the page to read (Esc: never mind)."));
    });
    QWidget *b = buttons_.value(QStringLiteral("scan"));
    menu.exec(b ? b->mapToGlobal(QPoint(0, b->height())) : QCursor::pos());
}

// Scan > Pages...: which pages, typed ("1-3, 6", "8-": to the end) - the page shown, to start with
void MainWindow::scanPages()
{
    auto doc = view_->document();
    if (!doc)
        return;
    const int n = doc->pageCount();
    ClassicDialog dlg(this, tr("Scan pages"));
    auto *box = new QVBoxLayout(dlg.body());
    box->setContentsMargins(12, 12, 12, 12);
    box->setSpacing(8);
    auto *note = new QLabel(tr("Pages to scan (1 to %1 - for example 1-3, 5, 8-):").arg(n), dlg.body());
    note->setFont(theme::font());
    box->addWidget(note);
    auto *pages = new QLineEdit(dlg.body());
    pages->setFont(theme::font());
    pages->setText(QString::number(std::max(0, view_->currentPage()) + 1));
    pages->selectAll();
    box->addWidget(pages);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *scanB = new QPushButton(tr("Scan"), dlg.body());
    auto *cancelB = new QPushButton(tr("Cancel"), dlg.body());
    for (QPushButton *b : {scanB, cancelB}) {
        b->setFont(theme::font());
        b->setMinimumWidth(75);
        row->addWidget(b);
    }
    scanB->setDefault(true);
    box->addLayout(row);
    std::vector<int> chosen;
    auto go = [&] {
        QString error;
        const auto ranges = jobs::parseRanges(pages->text(), &error);
        if (ranges.empty()) {
            classicMessage(&dlg, tr("Scan pages"), error.isEmpty() ? tr("Type the pages to scan, like 1-3, 5.") : error);
            return;
        }
        std::vector<int> out;
        for (const auto &[a, b] : ranges) {
            if (a > n) {
                classicMessage(&dlg, tr("Scan pages"), tr("The PDF has only %1 pages, so it has no page %2.").arg(n).arg(a));
                return;
            }
            for (int p = a; p <= std::min(n, b.value_or(n)); ++p)
                if (std::find(out.begin(), out.end(), p - 1) == out.end())
                    out.push_back(p - 1);
        }
        std::sort(out.begin(), out.end());
        chosen = out;
        dlg.close();
    };
    connect(scanB, &QPushButton::clicked, &dlg, go);
    connect(pages, &QLineEdit::returnPressed, &dlg, go);
    connect(cancelB, &QPushButton::clicked, &dlg, [&] { dlg.close(); });
    pages->setFocus();
    dlg.exec();
    if (!chosen.empty())
        startScan(chosen);
}

void MainWindow::startScan(const std::vector<int> &pages, const QRectF &area)
{
    auto doc = view_->document();
    if (!doc || current_ < 0 || pages.empty() || scan_)
        return;
    const int count = doc->pageCount();
    if (view_->lineEditorOpen())
        view_->commitEditor();
    auto job = std::make_shared<ScanJob>();
    job->doc = doc;
    job->count = count;
    job->pages = pages;
    job->area = area;
    scan_ = job;
    scanCancel = false;
    job->box = std::make_unique<ScanBox>(this, view_->viewport());
    job->box->step(0, tr("Reading the text..."), tr("Page %1 of %2").arg(job->pages.front() + 1).arg(count));
    job->box->place(true);
    job->lock = std::make_unique<ScanLock>(this);
    job->lock->hourglass();
    say(job->pages.size() > 1 ? tr("Scanning %1 pages...").arg(job->pages.size()) : tr("Scanning page %1...").arg(job->pages.front() + 1));
    // (the box shown only while the scanned PDF's tab is: looked at now and then)
    auto *watch = new QTimer(job->box.get());
    connect(watch, &QTimer::timeout, this, [this] {
        if (scan_ && scan_->box)
            scan_->box->place(scan_->lock->locked());
    });
    watch->start(150);
    job->thread = std::thread([job = job.get(), this] {
        job->run();
        QMetaObject::invokeMethod(this, [this] { scanFinished(); }, Qt::QueuedConnection);
    });
}

// Home's Scan: a PDF - or a picture, made a PDF - from the computer, opened in a tab and scanned straight away, every page
void MainWindow::scanFile()
{
    if (scan_) {
        classicMessage(this, tr("Scan"), tr("A scan is reading already - another can be started once it's done."));
        return;
    }
    if (ocr::languages().isEmpty()) {
        classicMessage(this, tr("Scan"), tr("Text recognition's languages weren't found (a \"tessdata\" folder next to the program)."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Scan"), theme::settings().value("last_folder").toString(),
        tr("Scanned PDFs and pictures (*.pdf *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp);;All files (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
        if (!openFile(path))
            return;
    } else {  // (a picture: a PDF of it, named as it is)
        QString error;
        QStringList problems;
        auto doc = Document::create(&error);
        if (!doc || doc->insertFiles({path}, 0, &problems) == 0) {
            sounds::play(sounds::Error);
            classicMessage(this, tr("Scan"), problems.isEmpty() ? error : problems.join(QLatin1Char('\n')));
            return;
        }
        theme::saveSetting("last_folder", QFileInfo(path).absolutePath());
        showDocument(doc, QFileInfo(path).completeBaseName() + QStringLiteral(".pdf"));
    }
    auto doc = view_->document();
    if (!doc || !doc->editable()) {
        classicMessage(this, tr("Scan"), tr("Only a PDF's pages can be scanned for text."));
        return;
    }
    std::vector<int> pages;
    for (int n = 0; n < doc->pageCount(); ++n)
        pages.push_back(n);
    startScan(pages);
}

void MainWindow::scanFinished()
{
    if (!scan_)
        return;
    std::shared_ptr<ScanJob> job = std::move(scan_);
    scan_.reset();
    if (job->thread.joinable())
        job->thread.join();
    job->lock.reset();
    job->box.reset();
    // (its tab closed meanwhile, or called off: what was read, dropped)
    const auto open = std::find_if(docs_.begin(), docs_.end(), [&](const DocTab &t) { return t.doc == job->doc; });
    if (job->cancelled || scanCancel || open == docs_.end()) {  // (stopped with the X as it was finishing: still stopped)
        scanCancel = false;
        say(tr("The scan was called off."));
        return;
    }
    std::shared_ptr<Document> doc = job->doc;
    QString error = job->error;
    // the fonts made put in: each to write with, and to draw with here
    QHash<QString, QString> made;    // (builder -> its font's name)
    QHash<QString, QString> family;  // (... and its family, as Qt draws it: to tell which is more like a line)
    int traced = 0;
    for (const Built &f : job->built) {
        fonts::addEmbedded(f.ps, f.font, f.bold ? 1 : 0);
        if (!fonts::isEmbedded(fonts::findFont(f.ps, f.bold ? 1 : 0)))
            continue;
        made.insert(f.key, f.ps);
        traced += f.count;
        const int id = QFontDatabase::addApplicationFontFromData(f.font);
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty())
            family.insert(f.key, QFontDatabase::applicationFontFamilies(id).front());
    }
    auto &read = job->read;
    auto &readKeys = job->readKeys;
    // each Arabic line: its font - the one matched with the scan's glyphs copied in, or its own cut from its words
    // when the scan's typeface isn't the one matched (a letterhead's display Arabic)
    for (size_t n = 0; n < read.size(); ++n)
        for (size_t k = 0; k < job->readChoices[n].size() && k < readKeys[n].size(); ++k) {
            const Choice &c = job->readChoices[n][k];
            if (c.other.isEmpty() || !family.contains(c.other))
                continue;
            if (!family.contains(readKeys[n][k])) {
                readKeys[n][k] = c.other;
                continue;
            }
            // (both made: the one that draws the line most like the scan - its words cut where they fell wins only over a
            // font that isn't the scan's typeface; a calligraphic one's joined letters cut wrong lose to it)
            Fit own = c.fit, matched = c.fit;
            const double likeOwn = refine(own, c.text, family.value(c.other), c.bold, c.crop, c.box);
            const double likeMatched = refine(matched, c.text, family.value(readKeys[n][k]), c.bold, c.crop, c.box);
            if (likeOwn > likeMatched + 0.02)
                readKeys[n][k] = c.other;
        }
    int done = 0, lines = 0;
    if (error.isEmpty()) {
        if (view_->document() == doc && view_->lineEditorOpen())
            view_->commitEditor();
        doc->beginChanges("scan");
        for (size_t n = 0; n < read.size() && error.isEmpty(); ++n) {
            auto &[page, found] = read[n];
            for (size_t k = 0; k < found.size(); ++k)
                found[k].fontName = made.value(readKeys[n][k]);
            const auto &behind = job->readBehind[n];
            if (!doc->scanPictures(page, job->readPictures[n], behind.second.isEmpty() ? nullptr : &behind.first, behind.second, &error))
                break;
            if (!found.empty() && !doc->scanText(page, found, &error))
                break;
            lines += int(found.size());
            ++done;
        }
        doc->endChanges();
    }
    const bool shown = view_->document() == doc;
    if (!error.isEmpty())
        return failed(tr("scan the pages"), error);
    if (shown)
        afterChange(-1, true);
    if (done == 0)
        say(job->skipped ? tr("Nothing to scan: the page(s) have text already.") : tr("No text was found."));
    else
        say(tr("Scanned %1 page(s): %2 line(s) of text found, %3 letter shape(s) taken from the scan%4.")
                .arg(done)
                .arg(lines)
                .arg(traced)
                .arg(job->skipped ? tr(" (%1 page(s) with text already left)").arg(job->skipped) : QString()));
}
