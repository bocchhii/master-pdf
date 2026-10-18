#include "ui/Classic.h"

#include "ui/Theme.h"

#include <QGuiApplication>
#include <QHash>
#include <QPaintDevice>
#include <QPixmap>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace classic {

Px::Px(QPainter &painter) : p(painter)
{
    p.save();
    dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    // (where the painter's coordinates really land on the screen - a widget at 125% starts part way into
    // a screen pixel: its edges rounded from there, as Qt rounds the widget's own area, so its last row
    // and column are inside it - not cut off, not over the next one)
    full_ = p.deviceTransform();
    const QTransform toDevice = p.worldTransform().inverted() * full_;
    const QPointF origin = toDevice.map(QPointF(0, 0));
    base_ = QPoint(int(std::lround(origin.x())), int(std::lround(origin.y())));
    p.setWorldTransform(QTransform(1 / dpr, 0, 0, 1 / dpr, (base_.x() - origin.x()) / dpr, (base_.y() - origin.y()) / dpr));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    k = std::max(1, int(dpr + 0.25));
}

Px::~Px()
{
    p.restore();
}

QRect Px::dev(const QRectF &r) const
{
    const QRectF m = full_.mapRect(r);
    const int x0 = int(std::lround(m.left())) - base_.x(), y0 = int(std::lround(m.top())) - base_.y();
    const int x1 = int(std::lround(m.right())) - base_.x(), y1 = int(std::lround(m.bottom())) - base_.y();
    return QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

void Px::bevel(const QRect &d, const QColor &tl, const QColor &br, int n) const
{
    if (d.width() <= 0 || d.height() <= 0)
        return;
    p.fillRect(QRect(d.left(), d.top(), d.width(), n), tl);
    p.fillRect(QRect(d.left(), d.top(), n, d.height()), tl);
    p.fillRect(QRect(d.left(), d.top() + d.height() - n, d.width(), n), br);
    p.fillRect(QRect(d.left() + d.width() - n, d.top(), n, d.height()), br);
}

static QRect shrink(const QRect &d, int n)
{
    return d.adjusted(n, n, -n, -n);
}

void raisedEdge(QPainter &painter, const QRectF &r, bool pressed, bool fill)
{
    using namespace theme;
    Px x(painter);
    QRect d = x.dev(r);
    if (fill)
        x.fill(d, face());
    const int k = x.k;
    if (pressed && !mono()) {  // shadow and dark top / left, light bottom / right
        x.bevel(d, shadow(), light(), 2 * k);
        x.bevel(shrink(d, 2 * k), darkEdge(), face(), k);
    } else {  // 2 light top / left, 2 dark bottom / right; then 1 face / 1 shadow
        x.bevel(d, light(), darkEdge(), 2 * k);
        x.bevel(shrink(d, 2 * k), face(), shadow(), k);
    }
}

void sunkenEdge(QPainter &painter, const QRectF &r, const QColor &inside)
{
    using namespace theme;
    Px x(painter);
    QRect d = x.dev(r);
    if (inside.isValid())
        x.fill(d, inside);
    if (mono()) {  // (Black and White: a plain black line, a pixel of face round it)
        x.bevel(d, face(), face(), x.k);
        x.bevel(shrink(d, x.k), darkEdge(), darkEdge(), x.k);
        return;
    }
    x.bevel(d, shadow(), light(), x.k);
    x.bevel(shrink(d, x.k), darkEdge(), face(), x.k);
}

void thinRaised(QPainter &painter, const QRectF &r)
{
    Px x(painter);
    x.bevel(x.dev(r), theme::light(), theme::thinShadow(), x.k);
}

void thinSunken(QPainter &painter, const QRectF &r)
{
    Px x(painter);
    x.bevel(x.dev(r), theme::thinShadow(), theme::light(), x.k);
}

void smallButton(QPainter &painter, const QRectF &r, bool pressed)
{
    using namespace theme;
    Px x(painter);
    QRect d = x.dev(r);
    x.fill(d, face());
    if (pressed) {
        x.bevel(d, darkEdge(), light(), x.k);
        x.bevel(shrink(d, x.k), shadow(), face(), x.k);
    } else {
        x.bevel(d, light(), darkEdge(), x.k);
        x.bevel(shrink(d, x.k), face(), shadow(), x.k);
    }
}

void scrollBox(QPainter &painter, const QRectF &r)
{
    using namespace theme;
    Px x(painter);
    QRect d = x.dev(r);
    x.fill(d, face());
    x.bevel(d, face(), darkEdge(), x.k);
    x.bevel(shrink(d, x.k), light(), shadow(), x.k);
}

void etchedH(QPainter &painter, int x0, int x1, int y)
{
    Px x(painter);
    QRect d = x.dev(QRectF(x0, y, x1 - x0, 2));
    x.fill(QRect(d.left(), d.top(), d.width(), x.k), theme::shadow());
    x.fill(QRect(d.left(), d.top() + x.k, d.width(), x.k), theme::light());
}

void etchedV(QPainter &painter, int xx, int y0, int y1)
{
    Px x(painter);
    QRect d = x.dev(QRectF(xx, y0, 2, y1 - y0));
    x.fill(QRect(d.left(), d.top(), x.k, d.height()), theme::shadow());
    x.fill(QRect(d.left() + x.k, d.top(), x.k, d.height()), theme::light());
}

void checkered(QPainter &painter, const QRectF &r, const QColor &a, const QColor &b, QPoint phase)
{
    Px x(painter);
    const QRect d = x.dev(r);
    if (d.isEmpty())
        return;
    QImage tile(2, 2, QImage::Format_RGB32);
    tile.setPixelColor(0, 0, a);
    tile.setPixelColor(1, 1, a);
    tile.setPixelColor(1, 0, b);
    tile.setPixelColor(0, 1, b);
    QBrush brush(tile);
    QTransform t;
    t.translate(phase.x() * x.dpr, phase.y() * x.dpr);
    brush.setTransform(t);
    x.p.fillRect(d, brush);
}

void pixelArt(QPainter &painter, const QRectF &r, const QImage &im)
{
    if (im.isNull())
        return;
    Px x(painter);
    // (its place on whole screen pixels - its size from its own width, so every icon is as big)
    const QRect e = x.dev(r);
    const QSize size(int(std::lround(r.width() * x.dpr)), int(std::lround(r.height() * x.dpr)));
    const QRect d(e.topLeft(), size);
    if (size.width() % im.width() == 0 && size.height() % im.height() == 0) {  // (100%, 200%...: each pixel a block)
        x.p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        x.p.drawImage(d, im);
        return;
    }
    // (125%, 150%...: blocks of uneven sizes would make its edges ragged - made bigger in even
    // blocks, then evenly smaller, to the screen's pixels: its corners stay square)
    static QHash<qint64, QImage> made;
    const qint64 key = im.cacheKey() ^ (qint64(size.width()) << 48) ^ (qint64(size.height()) << 40);
    auto it = made.find(key);
    if (it == made.end()) {
        QImage big = im;
        while (big.width() < size.width() * 2 && big.width() < 256)
            big = big.scaled(big.size() * 2, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (made.size() > 500)
            made.clear();
        it = made.insert(key, big.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
    x.p.drawImage(d.topLeft(), *it);
}

void block(QPainter &painter, QPointF origin, int x0, int y0, int x1, int y1, const QColor &c)
{
    Px x(painter);
    x.fill(x.dev(QRectF(origin.x() + x0, origin.y() + y0, x1 - x0, y1 - y0)), c);
}

void downArrow(QPainter &painter, const QRectF &box, const QColor &c)
{
    Px x(painter);
    const QRect d = x.dev(box);
    const int rows = std::max(int(box.height()), int(std::lround(box.height() * x.dpr)));
    const int even = int(std::lround(box.width())) % 2 == 0 ? 1 : 0;  // (its last row: 2 pixels, or 1)
    const int wide = 2 * rows - 1 + even;
    const int left = d.left() + (d.width() - wide) / 2, top = d.top() + (d.height() - rows) / 2;
    for (int r = 0; r < rows; ++r)
        x.fill(QRect(left + r, top + r, wide - 2 * r, 1), c);
}

// (the screen's scale - and where the painter's coordinates start, on it; false: drawn as usual - a whole
// scale, or a painter turned or stretched)
static bool screenPixels(QPainter &p, double *dpr, QPointF *origin)
{
    *dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const QTransform world = p.worldTransform();
    if (std::abs(*dpr - std::round(*dpr)) < 0.01 || world.type() > QTransform::TxTranslate)
        return false;
    *origin = world.map(QPointF(0, 0));
    return true;
}

// (the font at the screen's own size: 8 points at 125% - 13 screen pixels, a whole number; its letter
// spacing whole screen pixels too)
static QFont screenFont(const QFont &font, QPaintDevice *device, double dpr)
{
    QFont f = font;
    const double px = f.pixelSize() > 0 ? f.pixelSize() : f.pointSizeF() * (device ? device->logicalDpiY() : 96) / 72.0;
    f.setPixelSize(std::max(1, int(std::lround(px * dpr))));
    if (f.letterSpacingType() == QFont::AbsoluteSpacing && f.letterSpacing() != 0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, std::round(f.letterSpacing() * dpr));
    return f;
}

static QRect onScreen(const QRectF &r, QPointF origin, double dpr)
{
    const QRectF d((r.left() + origin.x()) * dpr, (r.top() + origin.y()) * dpr, r.width() * dpr, r.height() * dpr);
    return QRect(QPoint(int(std::lround(d.left())), int(std::lround(d.top()))),
                 QPoint(int(std::lround(d.right())) - 1, int(std::lround(d.bottom())) - 1));
}

void crispText(QPainter &p, const QRectF &r, int flags, const QString &text, QRectF *bounds, Qt::TextElideMode elide)
{
    double dpr;
    QPointF origin;
    if (!screenPixels(p, &dpr, &origin)) {
        const QString shown = elide == Qt::ElideNone ? text : p.fontMetrics().elidedText(text, elide, int(r.width()));
        QRectF out;
        p.drawText(r, flags, shown, &out);
        if (bounds)
            *bounds = out;
        return;
    }
    p.save();
    p.setWorldTransform(QTransform::fromScale(1 / dpr, 1 / dpr));
    p.setFont(screenFont(p.font(), p.device(), dpr));
    const QRect d = onScreen(r, origin, dpr);
    const QString shown = elide == Qt::ElideNone ? text : p.fontMetrics().elidedText(text, elide, d.width());
    QRect out;
    p.drawText(d, flags, shown, &out);
    p.restore();
    if (bounds)
        *bounds = QRectF(out.left() / dpr - origin.x(), out.top() / dpr - origin.y(), out.width() / dpr, out.height() / dpr);
}

QRectF crispBounds(QPainter &p, const QRectF &r, int flags, const QString &text)
{
    double dpr;
    QPointF origin;
    if (!screenPixels(p, &dpr, &origin))
        return p.boundingRect(r, flags, text);
    p.save();
    p.setWorldTransform(QTransform::fromScale(1 / dpr, 1 / dpr));
    p.setFont(screenFont(p.font(), p.device(), dpr));
    const QRect out = p.boundingRect(onScreen(r, origin, dpr), flags, text);
    p.restore();
    return QRectF(out.left() / dpr - origin.x(), out.top() / dpr - origin.y(), out.width() / dpr, out.height() / dpr);
}

QCursor waitCursor()
{
    // (#: black, .: the glass - white, s: sand - black on every other pixel, as Windows 98 dithered it; space: none)
    static const char *const art[] = {
        "###############",
        "#.............#",
        "###############",
        " #...........# ",
        " #...........# ",
        " #sssssssssss# ",
        " #sssssssssss# ",
        "  #sssssssss#  ",
        "   #sssssss#   ",
        "    #sssss#    ",
        "     #sss#     ",
        "      #s#      ",
        "      #s#      ",
        "     #.s.#     ",
        "    #..s..#    ",
        "   #...s...#   ",
        "  #...sss...#  ",
        " #...sssss...# ",
        " #..sssssss..# ",
        " #sssssssssss# ",
        " #sssssssssss# ",
        "###############",
        "#.............#",
        "###############",
    };
    const int w = 15, h = int(sizeof art / sizeof *art);
    const QScreen *screen = QGuiApplication::primaryScreen();
    const double dpr = screen ? screen->devicePixelRatio() : 1.0;
    const int k = std::max(1, int(std::lround(dpr)));  // (each of its pixels a block of k screen pixels)
    const int side = 32 * k;  // (a cursor's 32 x 32, the hourglass in its middle)
    QImage im(side, side, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    const int x0 = (side - w * k) / 2, y0 = (side - h * k) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const char c = art[y][x];
            if (c == ' ')
                continue;
            const bool black = c == '#' || (c == 's' && (x + y) % 2 == 0);
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx)
                    im.setPixel(x0 + x * k + dx, y0 + y * k + dy, black ? 0xff000000u : 0xffffffffu);
        }
    QPixmap pm = QPixmap::fromImage(im);
    pm.setDevicePixelRatio(dpr);  // (its pixels the screen's own: not scaled again)
    const int hot = int(std::lround(side / dpr / 2));
    return QCursor(pm, hot, hot);
}

void engravedText(QPainter &p, const QRectF &r, int flags, const QString &text)
{
    const auto [lightC, darkC] = theme::engraved();
    p.save();
    p.setPen(lightC);
    crispText(p, r.translated(1, 1), flags, text);
    p.setPen(darkC);
    crispText(p, r, flags, text);
    p.restore();
}

QImage engravedImage(const QImage &src)
{
    const QImage im = src.convertToFormat(QImage::Format_ARGB32);
    const auto [lightC, darkC] = theme::engraved();
    QImage out(im.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    // (in 16 x 16 pixels: the light copy 1 px down and right - in bigger pictures, as far)
    const int o = std::max(1, im.width() / 16);
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            if (qAlpha(im.pixel(x, y)) > 100 && x + o < im.width() && y + o < im.height())
                out.setPixel(x + o, y + o, lightC.rgb());
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            if (qAlpha(im.pixel(x, y)) > 100)
                out.setPixel(x, y, darkC.rgb());
    return out;
}

QImage whitened(const QImage &src, int rows)
{
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    const int upto = std::min(im.height(), rows * std::max(1, im.height() / 16));
    for (int y = 0; y < upto; ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QRgb px = im.pixel(x, y);
            const int r = qRed(px), g = qGreen(px), b = qBlue(px), a = qAlpha(px);
            const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
            if (a && hi < 110 && hi - lo < 40) {  // black / dark grey: white / light grey
                const int v = 255 - lo / 3;
                im.setPixel(x, y, qRgba(v, v, v, a));
            } else if (a && b - std::max(r, g) > 50 && std::max(r, g) < 130) {
                // a dark blue (the icons' navy marks, the signature's ink, the link's ring, the
                // arrows' blues): a light grey - lighter where it was lighter (not the sky blue)
                const int v = std::min(215, 130 + (r + g + b) / 6);
                im.setPixel(x, y, qRgba(v, v, v, a));
            }
        }
    return im;
}

QImage greyBlues(const QImage &src, bool dark)
{
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QRgb px = im.pixel(x, y);
            const int r = qRed(px), g = qGreen(px), b = qBlue(px), a = qAlpha(px);
            if (a && b - std::max(r, g) > 50 && std::max(r, g) < 130) {
                const int v = dark ? std::min(215, 130 + (r + g + b) / 6) : std::min(150, 70 + (r + g + b) / 6);
                im.setPixel(x, y, qRgba(v, v, v, a));
            }
        }
    return im;
}

}  // namespace classic
