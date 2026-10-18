#include "ui/Icons.h"
#include <cmath>
#include <vector>
#include <QPainterPath>

#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QFile>
#include <QHash>
#include <QPaintDevice>

namespace icons {

namespace {

QImage load(const QString &file)
{
    QImage im(QStringLiteral(":/icons/") + file);
    return im.isNull() ? im : im.convertToFormat(QImage::Format_ARGB32);
}

}  // namespace

QImage get(const QString &name)
{
    static QHash<QString, QImage> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.insert(name, load(name + QStringLiteral(".png")));
    return *it;
}

bool has(const QString &name)
{
    return !get(name).isNull();
}

static QImage big(const QString &name)
{
    static QHash<QString, QImage> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.insert(name, load(name + QStringLiteral("@4x.png")));
    return *it;
}

void draw(QPainter &p, const QRectF &r, const QImage &image, bool enabled, int whiten)
{
    QImage im = image;
    if (whiten > 0 && theme::dark())
        im = classic::whitened(im, whiten);  // (black white, dark blues grey)
    else if (whiten > 0)
        im = classic::greyBlues(im, false);  // (dark blues grey: the standard in every theme)
    if (!enabled)
        im = classic::engravedImage(im);
    if (im.width() >= 32) {  // a smooth drawing (64 px): made the size it shows at, smoothly
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const double dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const QSize px((r.size() * dpr).toSize());
        QImage scaled = im.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        p.drawImage(r.topLeft(), scaled);
        p.restore();
        return;
    }
    // pixel art at its own size, in the middle of its place (the marker is 18 x 18, the brush
    // 11 x 16 - as the Python app: not squeezed or stretched into 16 x 16)
    const QRectF own(std::round(r.center().x() - im.width() / 2.0), std::round(r.center().y() - im.height() / 2.0),
                     im.width(), im.height());
    classic::pixelArt(p, own, im);
}

void draw(QPainter &p, const QRectF &r, const QString &name, bool enabled, int whiten)
{
    const QImage b = big(name);
    draw(p, r, b.isNull() ? get(name) : b, enabled, whiten);
}

}  // namespace icons

namespace icons {

QImage swatch(const QColor &color)
{
    QImage im(16, 16, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    for (int y = 1; y <= 14; ++y)
        for (int x = 1; x <= 14; ++x)
            im.setPixelColor(x, y, (x == 1 || y == 1 || x == 14 || y == 14) ? QColor(Qt::black) : color);
    return im;
}

QImage tinted(const QImage &src, const QColor &from, const QColor &to)
{
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    auto close = [](const QColor &a, int r, int g, int b) {
        return std::abs(a.red() - r) + std::abs(a.green() - g) + std::abs(a.blue() - b) < 60;
    };
    const QColor dark(int(from.red() * 0.75), int(from.green() * 0.75), int(from.blue() * 0.75));
    const QColor toDark(int(to.red() * 0.75), int(to.green() * 0.75), int(to.blue() * 0.75));
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QRgb p = im.pixel(x, y);
            if (!qAlpha(p))
                continue;
            if (close(from, qRed(p), qGreen(p), qBlue(p)))
                im.setPixel(x, y, qRgba(to.red(), to.green(), to.blue(), qAlpha(p)));
            else if (close(dark, qRed(p), qGreen(p), qBlue(p)))
                im.setPixel(x, y, qRgba(toDark.red(), toDark.green(), toDark.blue(), qAlpha(p)));
        }
    return im;
}

QImage best(const QString &name)
{
    const QImage b = big(name);
    return b.isNull() ? get(name) : b;
}

QImage layout(const QString &mode, bool small)
{
    const int W = small ? 16 : 32, H = small ? 16 : 26, k = 4;  // (the dome drawn 4 x bigger, made smaller: smooth)
    const QColor blue(46, 116, 181), pale(150, 168, 186);
    const QColor grey = theme::current() == QLatin1String("dark") ? QColor(200, 200, 200) : QColor(80, 80, 80);
    const int lt = small ? 1 : 2;
    const std::vector<int> rows = small ? std::vector<int>{2, 6, 10, 14} : std::vector<int>{3, 9, 15, 21};
    double R = small ? 3.5 : 6.5, th = small ? 2 : 3.5, legs = small ? 2 : 3, ax = small ? 4.5 : 9.5, ay = small ? 5.5 : 8.5;
    QImage lines(W, H, QImage::Format_ARGB32_Premultiplied);
    lines.fill(Qt::transparent);
    auto line = [&](double x0, double x1, int y) {
        for (int yy = y; yy < std::min(H, y + lt); ++yy)
            for (int x = std::max(0, int(std::lround(x0))); x < std::min(W, int(std::lround(x1))); ++x)
                lines.setPixelColor(x, yy, blue);
    };
    auto dome = [&](QColor colour) {
        QImage big(W * k, H * k, QImage::Format_ARGB32_Premultiplied);
        big.fill(Qt::transparent);
        QPainter p(&big);
        p.setRenderHint(QPainter::Antialiasing);
        const double x0 = ax * k, y0 = ay * k, r = R * k, t = th * k;
        QPainterPath outer;
        outer.moveTo(x0, y0 + r);
        outer.arcTo(QRectF(x0, y0, 2 * r, 2 * r), 180, -180);
        outer.lineTo(x0 + 2 * r, y0 + r + legs * k);
        outer.lineTo(x0 + 2 * r - t, y0 + r + legs * k);
        outer.lineTo(x0 + 2 * r - t, y0 + r);
        outer.arcTo(QRectF(x0 + t, y0 + t, 2 * (r - t), 2 * (r - t)), 0, 180);
        outer.lineTo(x0 + t, y0 + r + legs * k);
        outer.lineTo(x0, y0 + r + legs * k);
        outer.closeSubpath();
        p.fillPath(outer, colour);
        p.end();
        return big.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    const double left = ax, right = ax + 2 * R;
    const int top = rows.front(), bottom = rows.back();
    QImage under, over;
    if (mode == QLatin1String("inline")) {
        line(1, W - 1, top);
        ax = small ? 1 : 2;
        ay = small ? 6 : 10;
        line(ax + 2 * R + (small ? 1.5 : 2), W - 1, int(std::lround(ay + R + legs)) - lt);
        over = dome(grey);
    } else if (mode == QLatin1String("square") || mode == QLatin1String("tight") || mode == QLatin1String("through")) {
        line(1, W - 1, top);
        line(1, W - 1, bottom);
        const double gap = mode == QLatin1String("square") ? (small ? 2 : 3) : (small ? 1 : 1.5);
        for (size_t n = 1; n + 1 < rows.size(); ++n) {
            const double hug = mode != QLatin1String("square") && n == 1 ? (small ? 1 : 2) : 0;  // (round its top)
            line(1, left - gap + hug, rows[n]);
            line(right + gap - hug, W - 1, rows[n]);
        }
        if (mode == QLatin1String("through"))  // (text inside it too)
            line(left + th + 0.5, right - th - 0.5, rows[2]);
        over = dome(grey);
    } else if (mode == QLatin1String("top_bottom")) {
        line(1, W - 1, top);
        line(1, W - 1, bottom);
        over = dome(grey);
    } else if (mode == QLatin1String("behind")) {
        for (int y : rows)
            line(1, W - 1, y);
        under = dome(pale);
    } else {  // (in front)
        for (int y : rows)
            line(1, W - 1, y);
        over = dome(grey);
    }
    QImage im(W, H, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    for (const QImage &layer : {under, lines, over})
        if (!layer.isNull())
            p.drawImage(0, 0, layer);
    return im;
}

}  // namespace icons
