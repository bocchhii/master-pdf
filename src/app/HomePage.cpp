#include "app/HomePage.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Theme.h"
#include "ui/Icons.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"

#include <QActionGroup>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace {

constexpr int kRecentMax = 20;
constexpr int kThumb = 96;  // (the Thumbnails view's pictures: in a box this big)
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kSortRole = Qt::UserRole + 2;

// "Today, 6:24 PM", "Yesterday, 9:02 AM", "10/5/2026 3:10 PM" - as Windows 98 says when
QString whenText(qint64 ms)
{
    if (ms <= 0)
        return QString();
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(ms);
    const QLocale en(QLocale::English);
    const QString time = en.toString(t.time(), QStringLiteral("h:mm AP"));
    if (t.date() == QDate::currentDate())
        return QObject::tr("Today, %1").arg(time);
    if (t.date() == QDate::currentDate().addDays(-1))
        return QObject::tr("Yesterday, %1").arg(time);
    return en.toString(t.date(), QStringLiteral("M/d/yyyy")) + QLatin1Char(' ') + time;
}

QString sizeText(qint64 bytes)  // (Explorer's: "1,433KB")
{
    return QLocale(QLocale::English).toString(std::max<qint64>(1, (bytes + 1023) / 1024)) + QStringLiteral("KB");
}

QString thumbKey(const QString &path)
{
    return path + QLatin1Char('|') + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
}

// ---- Windows 98's little pictures, pixel by pixel ----

QImage art(std::initializer_list<const char *> rows, const std::map<char, QColor> &colors)
{
    QImage im(16, 16, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    int y = 0;
    for (const char *row : rows) {
        for (int x = 0; x < 16 && row[x]; ++x)
            if (auto it = colors.find(row[x]); it != colors.end())
                im.setPixel(x, y, it->second.rgba());
        ++y;
    }
    return im;
}

const std::map<char, QColor> kColors = {{'k', QColor(0, 0, 0)},       {'W', QColor(255, 255, 255)}, {'Y', QColor(255, 255, 0)},
                                        {'D', QColor(128, 128, 0)},   {'g', QColor(128, 128, 128)}, {'s', QColor(192, 192, 192)},
                                        {'R', QColor(192, 0, 0)},     {'B', QColor(0, 0, 128)},     {'c', QColor(0, 128, 128)}};

QImage folderIcon()
{
    return art({"................", "................", "..DDDDD.........", ".DWYYYYD........", "DWYYYYYYDDDDDDD.", "DWYYYYYYYYYYYYDk",
                "DWYYYYYYYYYYYYDk", "DWYYYYYYYYYYYYDk", "DWYYYYYYYYYYYYDk", "DWYYYYYYYYYYYYDk", "DWYYYYYYYYYYYYDk", "DWYYYYYYYYYYYYDk",
                "DDDDDDDDDDDDDDDk", ".kkkkkkkkkkkkkkk"},
               kColors);
}

QImage documentIcon()  // (a PDF: a page, its corner folded, Acrobat's red band)
{
    return art({"..kkkkkkkk......", "..kWWWWWWkk.....", "..kWWWWWWkWk....", "..kWWWWWWkkkk...", "..kWWWWWWWWWk...", "..kWRRRRRRWWk...",
                "..kWRRRRRRWWk...", "..kWWWWWWWWWk...", "..kWggggggWWk...", "..kWWWWWWWWWk...", "..kWggggggWWk...", "..kWWWWWWWWWk...",
                "..kWggggWWWWk...", "..kWWWWWWWWWk...", "..kkkkkkkkkkk..."},
               kColors);
}

double screenScale();

// (the toolbar's pictures: this big; Star and Delete drawn at the screen's own size - sharp, not a smaller
// picture stretched)
constexpr int kIcon = 20;
int iconPixels()
{
    return int(std::lround(kIcon * screenScale()));
}

QImage starIcon()  // (a faceted star: bright yellow on its left, darker gold on its right, outlined in black)
{
    const int n = iconPixels();
    QImage im(n, n, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing, false);
    const double cx = (n - 1) / 2.0, cy = n * 0.54, R = n * 0.49, r = R * 0.42;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const double a = -M_PI / 2 + i * M_PI / 5, rad = i % 2 ? r : R;
        star << QPointF(cx + rad * std::cos(a), cy + rad * std::sin(a));
    }
    QPainterPath path;
    path.addPolygon(star);
    path.closeSubpath();
    p.setClipPath(path);
    p.fillRect(im.rect(), QColor(255, 255, 0));
    p.fillRect(QRectF(cx + 0.5, 0, n, n), QColor(230, 180, 0));
    p.setClipping(false);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(star);
    return im;
}

QImage removeIcon()  // (Delete: a red x, edged in dark red)
{
    const int n = iconPixels();
    QImage im(n, n, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int w = std::max(2, int(std::lround(2.2 * screenScale()))), m = int(std::lround(5 * screenScale()));  // (its margin: bigger - a smaller x)
    for (const auto &[c, width] : {std::pair{QColor(128, 0, 0), w + 2}, std::pair{QColor(230, 0, 0), w}}) {
        p.setPen(QPen(c, width, Qt::SolidLine, Qt::SquareCap));
        p.drawLine(QPointF(m, m), QPointF(n - 1 - m, n - 1 - m));
        p.drawLine(QPointF(n - 1 - m, m), QPointF(m, n - 1 - m));
    }
    return im;
}

QImage createIcon()  // (Create: Windows 98's New - a sheet of text, its corner folded, a yellow sparkle at its top)
{
    return art({"...Y............", ".Y.YkYkkkkkk....", "..YYYWWWWWWkk...", "YYYWYYYWWWWksk..", "..YYYWWWWWWkssk.", ".Y.YkYWWWWWkkkkk",
                "...YkWWWWWWWWWWk", "....kWggggggggWk", "....kWWWWWWWWWWk", "....kWggggggggWk", "....kWWWWWWWWWWk", "....kWggggggggWk",
                "....kWWWWWWWWWWk", "....kWggggggWWWk", "....kWWWWWWWWWWk", "....kkkkkkkkkkkk"},
               kColors);
}

// (Scan and Clear Recent: Create's sheet - a scanner's green beam across it; a red x at its top)
QImage scanIcon()
{
    std::map<char, QColor> colors = kColors;
    colors['G'] = QColor(0, 255, 0);
    colors['H'] = QColor(0, 128, 0);
    // (the sheet in the middle, the beam across its middle - as far past it on the left as on the right)
    return art({"..kkkkkkkk......", "..kWWWWWWkk.....", "..kWWWWWWksk....", "..kWWWWWWkssk...", "..kWWWWWWkkkkk..", "..kWWWWWWWWWWk..",
                "..kWggggggggWk..", "GGGGGGGGGGGGGGGG", "HHHHHHHHHHHHHHHH", "..kWggggggggWk..", "..kWWWWWWWWWWk..", "..kWggggggggWk..",
                "..kWWWWWWWWWWk..", "..kWggggggWWWk..", "..kWWWWWWWWWWk..", "..kkkkkkkkkkkk.."},
               colors);
}

QImage clearIcon()
{
    std::map<char, QColor> colors = kColors;
    colors['R'] = QColor(230, 0, 0);  // (Delete's red: bright on the dark bar too)
    return art({"RR..RR..........", ".RRRRkkkkkkk....", "..RRkWWWWWWkk...", ".RRRRWWWWWWksk..", "RR..kRWWWWWkssk.", "....kWWWWWWkkkkk",
                "....kWWWWWWWWWWk", "....kWggggggggWk", "....kWWWWWWWWWWk", "....kWggggggggWk", "....kWWWWWWWWWWk", "....kWggggggggWk",
                "....kWWWWWWWWWWk", "....kWggggggWWWk", "....kWWWWWWWWWWk", "....kkkkkkkkkkkk"},
               colors);
}

QImage viewIcon(bool thumbs)
{
    // (Thumbnails: as Create's, Scan's - a sheet, its corner folded, its PDFs listed; Details: a list - each row a blue icon lit
    // at its corner, its name and size engraved: a black line over a white one, seen on the dark bar and the light one)
    if (thumbs)
        return art({"..kkkkkkkk......", "..kWWWWWWkk.....", "..kWsssssksk....", "..kWWWWWWkssk...", "..kWWWWWWkkkkk..", "..kWBWkkkkWgWk..",
                    "..kWWWWWWWWWWk..", "..kWBWkkkWWgWk..", "..kWWWWWWWWWWk..", "..kWBWkkkkWgWk..", "..kWWWWWWWWWWk..", "..kWBWkkWWWgWk..",
                    "..kWWWWWWWWWWk..", "..kWBWkkkkWgWk..", "..kWWWWWWWWWWk..", "..kkkkkkkkkkkk.."},
                   kColors);
    std::map<char, QColor> colors = kColors;
    colors['L'] = QColor(0, 128, 255);
    return art({"................", ".LB..kkkkkkk.kkk", ".BB..WWWWWWW.WWW", "................", "................", ".LB..kkkkkkkk.kk",
                ".BB..WWWWWWWW.WW", "................", "................", ".LB..kkkkkk..kkk", ".BB..WWWWWW..WWW", "................",
                "................", ".LB..kkkkkkk..kk", ".BB..WWWWWWW..WW", "................"},
               colors);
}

// (the little ones drawn over another's corner: the star on a starred PDF, a page in a folder)
QImage miniStar()
{
    return art({"...k...", "..kYk..", "kkYYYkk", ".kYYYk.", ".kYkYk.", "kk...kk"}, kColors);
}

QImage miniPage()
{
    return art({"kkkkk.", "kWWWkk", "kWWWWk", "kWggWk", "kWWWWk", "kWggWk", "kWWWWk", "kkkkkk"}, kColors);
}

QImage overlay(const QImage &under, const QImage &over, QPoint at)
{
    QImage out = under;
    QPainter p(&out);
    p.drawImage(at, over);
    return out;
}

// the web view panel's big picture, 32 x 32 as Windows 98's large icons: a folder - a page sticking out of
// it (Recent), or a star on it (Starred)
QImage bigFolder(bool star)
{
    // Windows 98's large closed folder: its tab at the top left, its face yellow dithered with grey (as 256
    // colours showed it), light along its top and left, a black edge and shadow at its right and bottom
    QImage im(32, 32, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    QPainter p(&im);
    const QColor yellow(255, 255, 0), silver(192, 192, 192), pale(255, 255, 204), olive(128, 128, 0), black(0, 0, 0);
    auto box = [&](int x, int y, int w, int h, const QColor &c) { p.fillRect(x, y, w, h, c); };
    auto face = [&](int x0, int y0, int x1, int y1) {  // (both ends in)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                im.setPixel(x, y, ((x + y) % 2 ? silver : yellow).rgba());
    };
    // its tab
    face(3, 5, 11, 7);
    box(4, 4, 7, 1, olive);
    box(3, 5, 1, 1, olive), box(11, 5, 1, 1, olive), box(12, 6, 1, 1, olive);
    box(2, 6, 1, 2, olive);
    box(4, 5, 7, 1, pale);
    // its body
    face(2, 8, 27, 25);
    box(1, 8, 1, 18, olive);     // its left edge
    box(2, 8, 1, 18, pale);      // ... lit inside it
    box(13, 7, 15, 1, olive);    // its top edge, after the tab
    box(3, 8, 25, 1, pale);
    box(28, 8, 1, 19, black);    // its right and bottom: black
    box(1, 26, 28, 1, black);
    box(29, 9, 1, 19, olive);    // ... and its shadow
    box(2, 27, 28, 1, olive);
    if (star) {  // a star on it - its own, nothing else changed
        const QPolygon s({{22, 12}, {24, 18}, {30, 18}, {25, 22}, {27, 28}, {22, 24}, {17, 28}, {19, 22}, {14, 18}, {20, 18}});
        p.setPen(black);
        p.setBrush(yellow);
        p.drawPolygon(s);
    }
    return im;
}

// the web view panel's sky: blue with white clouds streaked across it, in its top left corner, fading to
// white along a slant - dithered to the 216 web colours (256-colour Windows 98's noise)
QImage skyImage(QSize size, double dpr, double drift)
{
    const int w = std::max(1, int(std::lround(size.width() * dpr))), h = std::max(1, int(std::lround(size.height() * dpr)));
    auto hash = [](int x, int y) {
        quint32 n = quint32(x) * 374761393u + quint32(y) * 668265263u;
        n = (n ^ (n >> 13)) * 1274126177u;
        return double((n ^ (n >> 16)) & 0xffff) / 65535.0;
    };
    auto noise = [&](double x, double y) {  // (smooth value noise)
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        const double a = hash(x0, y0), b = hash(x0 + 1, y0), c = hash(x0, y0 + 1), d = hash(x0 + 1, y0 + 1);
        return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
    };
    const QColor bg = theme::box();
    const bool dark = theme::dark();
    // (its colours the theme's own: the title bar's light end, its clouds lighter - on a dark theme dimmer)
    auto mix = [](const QColor &a, const QColor &b, double t) {
        return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                      int(a.blue() + (b.blue() - a.blue()) * t));
    };
    const QColor tint = theme::caption(true).second;
    const QColor blue = dark ? mix(bg, tint, 0.55) : mix(tint, QColor(255, 255, 255), 0.5);
    const QColor cloud = dark ? mix(blue, theme::text(), 0.35) : mix(tint, QColor(255, 255, 255), 0.93);
    // WHERE THE SKY FADES - along a slant: a point's distance is kAcross x + kDown y, in the panel's pixels from its top
    // left corner (its right edge is 250 across). Full sky before kFadeStart, none after kFadeEnd.
    const double kFadeStart = 190;  // (the fade starts here - smaller: it starts nearer the corner)
    const double kFadeEnd = 300;    // (... and is gone here - bigger: the sky reaches further)
    const double kWobble = 6;      // (how uneven the edge is, in pixels: 0 - a straight, smooth edge)
    const double kAcross = 1.10;    // (the top: bigger - the sky reaches less far across; smaller - further)
    const double kDown = 1.05;     // (the left side: bigger - the sky reaches less far down; smaller - further)
    const double kSway = 15;       // (how far the edge sways back and forth as it moves, in pixels: 0 - it stays put)
    const double k = 1.0 / dpr;
    std::vector<float> rgb(size_t(w) * size_t(h) * 3);
    std::vector<float> alpha(size_t(w) * size_t(h));  // (how much sky each pixel is)
    const double c = std::cos(-0.6), sn = std::sin(-0.6);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double px = x * k, py = y * k;
            // (clouds: streaked along a slant - stretched one way, fine the other)
            const double u = (px * c - py * sn) / 90.0 + drift, v = (px * sn + py * c) / 34.0;  // (drift: the clouds moving along)
            double n = 0.55 * noise(u, v) + 0.28 * noise(u * 2.1, v * 2.1) + 0.17 * noise(u * 4.3, v * 4.3);
            const double t = std::clamp((n - 0.38) / 0.4, 0.0, 1.0);
            // (its edge: the slant, wandered a little by noise)
            // (the edge moving with the clouds: its unevenness drifting along it, the whole edge swaying)
            const double d = kAcross * px + kDown * py +
                             (noise(px / 38.0 + 17 + drift * 3, py / 38.0 + 5 - drift * 3) - 0.5) * 2 * kWobble +
                             std::sin(drift * 4) * kSway;
            const double sky = 1.0 - std::clamp((d - kFadeStart) / (kFadeEnd - kFadeStart), 0.0, 1.0);
            const double a = sky * sky * (3 - 2 * sky);
            alpha[size_t(y) * size_t(w) + size_t(x)] = float(a);
            float *o = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
            const double r = blue.red() + (cloud.red() - blue.red()) * t, g = blue.green() + (cloud.green() - blue.green()) * t,
                         b = blue.blue() + (cloud.blue() - blue.blue()) * t;
            o[0] = float(bg.red() + (r - bg.red()) * a);
            o[1] = float(bg.green() + (g - bg.green()) * a);
            o[2] = float(bg.blue() + (b - bg.blue()) * a);
        }
    // (Floyd-Steinberg to the web's 216 colours: 0, 51, ... 255 in each)
    QImage im(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < w; ++x) {
            float *o = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
            const float a = alpha[size_t(y) * size_t(w) + size_t(x)];
            if (a < 0.02f) {  // (no sky: the page plain - no dots, nothing carried on)
                line[x] = bg.rgb();
                continue;
            }
            int q[3];
            for (int ch = 0; ch < 3; ++ch) {
                const float v = std::clamp(o[ch], 0.0f, 255.0f);
                q[ch] = int(std::lround(v / 51.0f)) * 51;
                const float err = v - float(q[ch]);
                auto spread = [&](int dx, int dy, float f) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < w && ny < h)
                        rgb[(size_t(ny) * size_t(w) + size_t(nx)) * 3 + size_t(ch)] += err * f;
                };
                spread(1, 0, 7 / 16.0f), spread(-1, 1, 3 / 16.0f), spread(0, 1, 5 / 16.0f), spread(1, 1, 1 / 16.0f);
            }
            // (the dots thinning out as the sky fades: fewer of them, the page between)
            line[x] = hash(x + 7919, y + 104729) < std::min(1.0f, a * 2.5f) ? qRgb(q[0], q[1], q[2]) : bg.rgb();
        }
    }
    im.setDevicePixelRatio(dpr);
    return im;
}

double screenScale()
{
    const QScreen *s = QGuiApplication::primaryScreen();
    return s ? s->devicePixelRatio() : 1.0;
}

// a picture for a list's item: drawn to the screen's own pixels (crisp at 125%, not blurred by the list)
QPixmap crisp(const QImage &im, int size)
{
    const double dpr = screenScale();
    QPixmap pm(QSize(int(std::lround(size * dpr)), int(std::lround(size * dpr))));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    classic::pixelArt(p, QRectF(0, 0, size, size), im);
    return pm;
}

// Details' rows sorted by what each column holds (sizes and dates as numbers), not by their words
class Row : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &o) const override
    {
        const int c = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant a = data(c, kSortRole), b = o.data(c, kSortRole);
        if (a.isValid() && b.isValid())
            return a.toLongLong() < b.toLongLong();
        return text(c).compare(o.text(c), Qt::CaseInsensitive) < 0;
    }
};

}  // namespace

// Explorer's toolbar button: flat, its picture over its name; raised under the mouse, pressed in while
// held - greyed out (engraved) when it can't be used; a little arrow beside it for its list (Views)
class BigButton : public QWidget {
public:
    BigButton(const QImage &icon, const QString &text, const QString &note, bool arrow, QWidget *parent)
        : QWidget(parent), icon_(icon), text_(text), arrow_(arrow)
    {
        setFont(theme::font());
        setToolTip(text);
        setWhatsThis(note);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        const QFontMetrics fm(theme::font());
        setFixedSize(std::max(fm.horizontalAdvance(text) + 14, 48) + (arrow ? kArrow : 0), kIcon + fm.height() + 14);
    }
    std::function<void()> clicked, arrowClicked;
    int iconDown = 0;  // (its picture moved down this many pixels: one drawn high in its square)
    void setIcon(const QImage &icon)
    {
        icon_ = icon;
        update();
    }

protected:
    static constexpr int kArrow = 13;
    QRect main() const { return rect().adjusted(0, 0, arrow_ ? -kArrow : 0, 0); }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::face());
        const bool on = isEnabled();
        const QRect m = main();
        if (on && down_ && hot_)
            classic::thinSunken(p, QRectF(m));
        else if (on && (hot_ || down_))
            classic::thinRaised(p, QRectF(m));
        if (arrow_ && on && hot_)
            classic::thinRaised(p, QRectF(m.right() + 1, 0, kArrow, height()));
        const int shift = on && down_ && hot_ ? 1 : 0;
        // (every picture fitted to the same box - the 16-pixel ones made bigger, Star and Delete drawn that big)
        classic::pixelArt(p, QRectF(m.center().x() - kIcon / 2 + 1 + shift, 4 + iconDown + shift, kIcon, kIcon),
                          on ? icon_ : classic::engravedImage(icon_));
        const QRect t(m.left() + shift, kIcon + 7 + shift, m.width(), height() - kIcon - 10);
        if (on) {
            p.setPen(theme::text());
            classic::crispText(p, t, Qt::AlignHCenter | Qt::AlignTop, text_);
        } else {
            classic::engravedText(p, t, Qt::AlignHCenter | Qt::AlignTop, text_);
        }
        if (arrow_)
            classic::downArrow(p, QRectF(m.right() + 2, 12, kArrow - 4, 3), on ? theme::ink() : theme::shadow());
    }
    void enterEvent(QEnterEvent *) override
    {
        hot_ = true;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        hot_ = false;
        update();
    }
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::EnabledChange)
            update();
        QWidget::changeEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const bool in = rect().contains(e->position().toPoint());
        if (in != hot_) {
            hot_ = in;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !isEnabled())
            return;
        if (arrow_ && e->position().x() > main().right() && arrowClicked) {
            arrowClicked();
            return;
        }
        down_ = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !down_)
            return;
        down_ = false;
        update();
        if (hot_ && clicked)
            clicked();
    }

private:
    QImage icon_;
    QString text_;
    bool arrow_ = false, hot_ = false, down_ = false;
};

// the band's grip at its left, and the etched lines between its groups
class BandPart : public QWidget {
public:
    BandPart(bool grip, QWidget *parent) : QWidget(parent), grip_(grip) { setFixedWidth(10); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::face());
        if (grip_)
            ToolGrip().paint(p, height());  // (two raised bars, as every toolbar's)
        else
            classic::etchedV(p, 4, 3, height() - 3);
    }

private:
    bool grip_;
};

// A pane's title strip, as Explorer's All Folders pane has: its name on the face
class PaneHeader : public QWidget {
public:
    PaneHeader(const QString &text, QWidget *parent) : QWidget(parent), text_(text)
    {
        setFixedHeight(QFontMetrics(theme::font()).height() + 10);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::face());
        p.setFont(theme::font());
        p.setPen(theme::text());
        classic::crispText(p, rect().adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, text_);
    }

private:
    QString text_;
};

// Explorer's web view panel: the list's picture and name over its coloured line; then the PDF picked -
// its first page, what it is, its size and dates - or what to do
class InfoPanel : public QWidget {
public:
    explicit InfoPanel(QWidget *parent) : QWidget(parent)
    {
        setFixedWidth(250);
        setMouseTracking(true);
        // (the sky's clouds drifting slowly along - only while it's shown)
        auto *tick = new QTimer(this);
        tick->setInterval(kFrameMs);
        connect(tick, &QTimer::timeout, this, [this] {
            if (!isVisible() || (window() && window()->isMinimized()))
                return;
            drift_ += kDriftPerFrame;
            update();
        });
        tick->start();
    }
    QString title, note, name;
    QString link;  // (a blue underlined link under the note, as the web view's: "Open a PDF...")
    std::function<void()> linked;
    QImage icon, page;
    QStringList facts;

protected:
    void mouseMoveEvent(QMouseEvent *e) override
    {
        setCursor(linkRect_.contains(e->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && linkRect_.contains(e->pos()) && linked)
            linked();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::box());
        {  // (the web view's sky: made again only when the panel's size or the theme changes)
            const double dpr = devicePixelRatioF();
            const QString key = QStringLiteral("%1x%2@%3/%4/%5/%6/%7").arg(width()).arg(height()).arg(dpr).arg(theme::box().name(), theme::caption(true).second.name(), theme::text().name()).arg(theme::dark());  // (made again when a colour it uses changes)
            if (key != skyKey_ || drift_ != skyDrift_) {
                sky_ = skyImage(size(), dpr, drift_);
                skyKey_ = key;
                skyDrift_ = drift_;
            }
            p.drawImage(QPointF(0, 0), sky_);
        }
        const int x = 16, w = width() - 28;
        int y = 22;
        classic::pixelArt(p, QRectF(x, y, 32, 32), icon);
        y += 42;
        p.setPen(theme::text());
        p.setFont(verdana(18, true));
        QRectF r;
        classic::crispText(p, QRectF(x, y, w, 400), Qt::TextWordWrap, title, &r);
        y = int(r.bottom()) + 8;
        {  // (the rainbow line under it, from the panel's edge)
            QLinearGradient g(0, 0, x + w, 0);
            g.setColorAt(0, QColor(220, 30, 30));
            g.setColorAt(0.25, QColor(255, 150, 0));
            g.setColorAt(0.45, QColor(235, 215, 0));
            g.setColorAt(0.65, QColor(40, 170, 60));
            g.setColorAt(0.85, QColor(20, 120, 220));
            g.setColorAt(1, QColor(20, 120, 220));
            p.fillRect(QRect(0, y, x + w, 2), g);
        }
        y += 22;
        if (!page.isNull()) {  // (the PDF picked: its first page, on its sheet's edge - made the size of the screen's own
                               // pixels, not the panel's stretched by the screen's scale: sharp)
            const double dpr = devicePixelRatioF();
            const QString key = QStringLiteral("%1/%2/%3").arg(page.cacheKey()).arg(w).arg(dpr);
            if (key != shownKey_) {
                shown_ = page.scaled(QSize(w, 220) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                shown_.setDevicePixelRatio(dpr);
                shownKey_ = key;
            }
            const QRectF at(x, y, shown_.width() / dpr, shown_.height() / dpr);
            p.fillRect(at.adjusted(-1, -1, 1, 1), theme::shadow());
            p.drawImage(at.topLeft(), shown_);
            y = int(std::ceil(at.bottom())) + 12;
        }
        p.setFont(verdana(8, true));
        if (!name.isEmpty()) {
            classic::crispText(p, QRectF(x, y, w, 400), Qt::TextWordWrap | Qt::TextWrapAnywhere, name, &r);
            y = int(r.bottom()) + 4;
        }
        p.setFont(verdana(8, false));
        for (const QString &line : facts) {
            classic::crispText(p, QRectF(x, y, w, 400), Qt::TextWordWrap, line, &r);
            y = int(r.bottom()) + 2;
        }
        if (!note.isEmpty()) {
            y += facts.isEmpty() ? 0 : 10;
            classic::crispText(p, QRectF(x, y, w, 400), Qt::TextWordWrap, note, &r);
            y = int(r.bottom()) + 12;
        }
        linkRect_ = QRect();
        if (!link.isEmpty()) {  // (blue, underlined: a web page's link - lighter on a dark theme)
            QFont f = verdana(8, false);
            f.setUnderline(true);
            p.setFont(f);
            p.setPen(theme::dark() ? QColor(128, 170, 255) : QColor(0, 0, 255));
            classic::crispText(p, QRectF(x, y, w, 100), Qt::AlignLeft | Qt::TextSingleLine, link, &r);
            linkRect_ = r.toAlignedRect();
        }
    }

private:
    // (the web view's own font: Verdana, as Windows 98's folder pages had - its letters sharp, not smoothed)
    static QFont verdana(int points, bool bold)
    {
        QFont f(QStringLiteral("Verdana"));
        f.setPointSizeF(points);
        f.setBold(bold);
        f.setStyleStrategy(QFont::NoAntialias);
        f.setHintingPreference(QFont::PreferFullHinting);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0);  // (its own spacing - not the app font's)
        return f;
    }
    QRect linkRect_;
    QImage sky_;
    QString skyKey_;
    QImage shown_;  // (the picked PDF's page, as it's drawn - made again when the page, the width or the scale changes)
    QString shownKey_;
    // THE CLOUDS' DRIFT: how often the sky moves (ms) and how far each time (bigger - faster)
    static constexpr int kFrameMs = 50;
    static constexpr double kDriftPerFrame = 0.022;
    double drift_ = 0, skyDrift_ = -1;
};

// the white box the panel and the list are in: sunken, as a list view's
class SunkenBox : public QWidget {
public:
    explicit SunkenBox(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        classic::sunkenEdge(p, QRectF(rect()), theme::box());
    }
};

HomePage::HomePage(QWidget *parent) : QWidget(parent)
{
    setAutoFillBackground(true);
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);

    // the toolbar: as Explorer's - its grip, its big buttons in groups
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(2, 2, 4, 2);
    bar->setSpacing(1);
    bar->addWidget(new BandPart(true, this));
    auto button = [this, bar](const QImage &icon, const QString &text, const QString &note, bool arrow = false) {
        auto *b = new BigButton(icon, text, note, arrow, this);
        bar->addWidget(b);
        return b;
    };
    openButton_ = button(icons::get(QStringLiteral("open")), tr("Open"), tr("Opens a PDF from the computer, in a tab of its own."));
    openButton_->clicked = [this] { emit openDialogRequested(); };
    auto *create = button(createIcon(), tr("Create"),
                          tr("Makes a PDF from files or pictures: their pages shown first to put in order, add to or take out - "
                             "then named and created."));
    create->clicked = [this] { emit createRequested(); };
    auto *scan = button(scanIcon(), tr("Scan"),
                        tr("Opens a scanned PDF or a picture from the computer and reads its text (OCR) straight away."));
    scan->clicked = [this] { emit scanRequested(); };
    bar->addWidget(new BandPart(false, this));
    starButton_ = button(starIcon(), tr("Star"), tr("Stars the PDFs selected (or takes their star off): starred PDFs are listed under Starred."));
    starButton_->clicked = [this] { starPicked(); };
    removeButton_ = button(removeIcon(), tr("Delete"), tr("Takes the PDFs selected off this list (Del). The files themselves stay where they are."));
    removeButton_->clicked = [this] { removePicked(); };
    bar->addWidget(new BandPart(false, this));
    viewsButton_ = button(viewIcon(false), tr("Views"), tr("Click to show the PDFs the other way; its arrow lists the ways: Thumbnails or Details."), true);
    viewsButton_->clicked = [this] { setGrid(!gridOn_); };
    viewsButton_->iconDown = 2;
    viewsButton_->arrowClicked = [this] {
        QMenu menu(this);
        auto *group = new QActionGroup(&menu);
        QAction *thumbs = menu.addAction(tr("T&humbnails"), this, [this] { setGrid(true); });
        QAction *details = menu.addAction(tr("&Details"), this, [this] { setGrid(false); });
        for (QAction *a : {thumbs, details}) {
            a->setCheckable(true);
            group->addAction(a);
        }
        (gridOn_ ? thumbs : details)->setChecked(true);
        menu.exec(viewsButton_->mapToGlobal(QPoint(0, viewsButton_->height())));
    };
    clearSep_ = new BandPart(false, this);
    bar->addWidget(clearSep_);
    clearButton_ = button(clearIcon(), tr("Clear Recent"),
                          tr("Takes every PDF off the Recent list (the files themselves stay)."));
    clearButton_->clicked = [this] {
        if (classicMessage(this, tr("Master PDF"), tr("Clear the list of recent files?"), {tr("Yes"), tr("No")}) != tr("Yes"))
            return;
        theme::saveSetting("recent", QJsonArray());
        theme::saveSetting("recent_times", QJsonObject());
        refresh();
    };
    bar->addStretch(1);
    box->addLayout(bar);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(2, 0, 2, 0);
    line->addWidget(new EtchedLine(this));
    box->addLayout(line);

    // the panes: Recent and Starred | the web view panel and the PDFs
    split_ = new QSplitter(Qt::Horizontal, this);
    split_->setOpaqueResize(false);
    split_->setChildrenCollapsible(false);
    auto *left = new QWidget(split_);
    auto *lb = new QVBoxLayout(left);
    lb->setContentsMargins(0, 0, 0, 0);
    lb->setSpacing(0);
    lb->addWidget(new PaneHeader(tr("Home"), left));
    nav_ = new QTreeWidget(left);
    nav_->setFont(theme::font());
    nav_->setHeaderHidden(true);
    nav_->setRootIsDecorated(false);
    nav_->setIconSize(QSize(16, 16));
    nav_->setWhatsThis(tr("Recent: the PDFs you opened lately. Starred: the ones you starred."));
    const QImage folder = folderIcon();
    for (const auto &[name, mark] : {std::pair{tr("Recent"), miniPage()}, std::pair{tr("Starred"), miniStar()}}) {
        auto *it = new QTreeWidgetItem(nav_, {name});
        it->setIcon(0, crisp(overlay(folder, mark, QPoint(8, 6)), 16));
    }
    lb->addWidget(nav_, 1);
    left->setMinimumWidth(110);
    split_->addWidget(left);

    auto *right = new SunkenBox(split_);
    auto *rb = new QHBoxLayout(right);
    rb->setContentsMargins(2, 2, 2, 2);
    rb->setSpacing(0);
    info_ = new InfoPanel(right);
    info_->linked = [this] {
        if (section_ == 0)
            emit openDialogRequested();
        else
            showSection(0);
    };
    rb->addWidget(info_);
    views_ = new QStackedWidget(right);
    list_ = new QTreeWidget(views_);
    list_->setFont(theme::font());
    list_->setFrameShape(QFrame::NoFrame);
    list_->setColumnCount(5);
    list_->setHeaderLabels({tr("Name"), tr("Size"), tr("Type"), tr("Opened"), tr("Modified")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setIconSize(QSize(16, 16));
    list_->setAllColumnsShowFocus(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setSortingEnabled(true);
    list_->header()->setSortIndicatorShown(false);
    list_->header()->setSectionsMovable(false);
    list_->setColumnWidth(0, 300);
    list_->setColumnWidth(1, 80);
    list_->setColumnWidth(2, 110);
    list_->setColumnWidth(3, 150);
    list_->setColumnWidth(4, 150);
    list_->setWhatsThis(tr("The PDFs: double click one (or press Enter) to open it in a tab of its own. Right click "
                           "for more."));
    list_->installEventFilter(this);
    views_->addWidget(list_);
    grid_ = new QListWidget(views_);
    grid_->setFont(theme::font());
    grid_->setFrameShape(QFrame::NoFrame);
    grid_->setViewMode(QListView::IconMode);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setWordWrap(false);
    grid_->setIconSize(QSize(kThumb, kThumb));
    grid_->setGridSize(QSize(kThumb + 34, kThumb + 40));
    grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    grid_->setWhatsThis(list_->whatsThis());
    grid_->installEventFilter(this);
    views_->addWidget(grid_);
    rb->addWidget(views_, 1);
    split_->addWidget(right);
    split_->setStretchFactor(1, 1);
    split_->setSizes({std::clamp(theme::settings().value("home_side").toInt(170), 110, 400), 1000});
    connect(split_, &QSplitter::splitterMoved, this, [left] { theme::saveSetting("home_side", left->width()); });
    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(2, 2, 2, 2);
    middle->addWidget(split_);
    box->addLayout(middle, 1);

    connect(nav_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it && nav_->indexOfTopLevelItem(it) != section_)
            showSection(nav_->indexOfTopLevelItem(it));
    });
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) { emit openRequested(it->data(0, kPathRole).toString()); });
    connect(grid_, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) { emit openRequested(it->data(kPathRole).toString()); });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &HomePage::pickedChanged);
    connect(grid_, &QListWidget::itemSelectionChanged, this, &HomePage::pickedChanged);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
        if (list_->itemAt(at))
            menuFor(list_->viewport()->mapToGlobal(at));
    });
    connect(grid_, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
        if (grid_->itemAt(at))
            menuFor(grid_->viewport()->mapToGlobal(at));
    });
    gridOn_ = theme::settings().value("home_grid").toBool(false);
    setGrid(gridOn_);
    showSection(0);
}

QStringList HomePage::recentPaths()
{
    QStringList out;
    for (const QJsonValue &v : theme::settings().value("recent").toArray())
        if (QFileInfo::exists(v.toString()))
            out << v.toString();
    return out;
}

void HomePage::noteOpened(const QString &path)
{
    QJsonArray recent{path};
    for (const QJsonValue &v : theme::settings().value("recent").toArray())
        if (v.toString().compare(path, Qt::CaseInsensitive) != 0 && recent.size() < kRecentMax)
            recent.append(v);
    theme::saveSetting("recent", recent);
    QJsonObject times = theme::settings().value("recent_times").toObject();
    for (auto it = times.begin(); it != times.end();)  // (only those still on the list)
        if (!recent.contains(it.key()))
            it = times.erase(it);
        else
            ++it;
    times.insert(path, QDateTime::currentMSecsSinceEpoch());
    theme::saveSetting("recent_times", times);
}

QStringList HomePage::starredPaths()
{
    QStringList out;
    for (const QJsonValue &v : theme::settings().value("starred").toArray())
        if (QFileInfo::exists(v.toString()))
            out << v.toString();
    return out;
}

bool HomePage::starred(const QString &path)
{
    for (const QJsonValue &v : theme::settings().value("starred").toArray())
        if (v.toString().compare(path, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

void HomePage::setStarred(const QString &path, bool on)
{
    QJsonArray out;
    if (on)
        out.append(path);
    for (const QJsonValue &v : theme::settings().value("starred").toArray())
        if (v.toString().compare(path, Qt::CaseInsensitive) != 0)
            out.append(v);
    theme::saveSetting("starred", out);
}

void HomePage::showSection(int which)
{
    section_ = which;
    if (nav_->indexOfTopLevelItem(nav_->currentItem()) != which)
        nav_->setCurrentItem(nav_->topLevelItem(which));
    refresh();
}

void HomePage::setGrid(bool on)
{
    gridOn_ = on;
    viewsButton_->setIcon(viewIcon(on));
    views_->setCurrentWidget(on ? static_cast<QWidget *>(grid_) : list_);
    theme::saveSetting("home_grid", on);
    pickedChanged();
}

void HomePage::refresh()
{
    clearButton_->setVisible(section_ == 0);
    removeButton_->setToolTip(section_ == 0 ? tr("Remove from Recent (Del)") : tr("Remove from Starred (Del)"));
    clearSep_->setVisible(section_ == 0);
    fill();
}

void HomePage::fill()
{
    const QStringList paths = section_ == 0 ? recentPaths() : starredPaths();
    const QStringList keep = picked();
    const QJsonObject times = theme::settings().value("recent_times").toObject();
    const QImage doc = documentIcon(), star = miniStar();
    list_->setSortingEnabled(false);
    list_->clear();
    grid_->clear();
    for (const QString &path : paths) {
        const QFileInfo fi(path);
        const qint64 when = times.value(path).toVariant().toLongLong();
        const bool on = starred(path);
        auto *it = new Row(list_);
        it->setData(0, kPathRole, path);
        it->setText(0, fi.fileName());
        it->setIcon(0, crisp(on ? overlay(doc, star, QPoint(0, 9)) : doc, 16));
        it->setToolTip(0, QDir::toNativeSeparators(path));
        it->setText(1, sizeText(fi.size()));
        it->setData(1, kSortRole, fi.size());
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it->setText(2, tr("PDF Document"));
        it->setText(3, whenText(when));
        it->setData(3, kSortRole, when);
        it->setText(4, whenText(fi.lastModified().toMSecsSinceEpoch()));
        it->setData(4, kSortRole, fi.lastModified().toMSecsSinceEpoch());
        it->setSelected(keep.contains(path));
        auto *g = new QListWidgetItem(grid_);
        g->setData(kPathRole, path);
        g->setText(fi.fileName());
        g->setToolTip(QDir::toNativeSeparators(path));
        g->setSelected(keep.contains(path));
        const QImage pic = thumbnail(path, QSize(kThumb, kThumb));
        g->setIcon(QPixmap::fromImage(pic.isNull() ? QImage() : pic));
        if (pic.isNull())
            thumbnailFor(path);
    }
    list_->setSortingEnabled(true);
    list_->header()->setSortIndicatorShown(false);  // (no arrow on the column: as Windows 98's Details)
    pickedChanged();
}

QStringList HomePage::picked() const
{
    QStringList out;
    if (gridOn_) {
        for (const QListWidgetItem *it : grid_->selectedItems())
            out << it->data(kPathRole).toString();
    } else {
        for (const QTreeWidgetItem *it : list_->selectedItems())
            out << it->data(0, kPathRole).toString();
    }
    return out;
}

// what's picked, in the panel and the status bar; the toolbar's buttons for it
void HomePage::pickedChanged()
{
    const QStringList sel = picked();
    const int all = gridOn_ ? grid_->count() : list_->topLevelItemCount();
    starButton_->setEnabled(!sel.isEmpty());
    removeButton_->setEnabled(!sel.isEmpty());
    clearButton_->setEnabled(all > 0);
    info_->title = section_ == 0 ? tr("Recent") : tr("Starred");
    info_->icon = bigFolder(section_ == 1);
    info_->page = QImage();
    info_->name.clear();
    info_->facts.clear();
    info_->link.clear();
    if (all == 0) {
        info_->note = section_ == 0 ? tr("There are no PDFs here yet. The PDFs you open are listed here.")
                                    : tr("There are no starred PDFs. Select one under Recent and click Star on the toolbar (or right "
                                         "click it) to star it.");
        info_->link = section_ == 0 ? tr("Open a PDF...") : tr("Show Recent");
    } else if (sel.size() == 1) {
        const QFileInfo fi(sel.front());
        const qint64 when = theme::settings().value("recent_times").toObject().value(sel.front()).toVariant().toLongLong();
        info_->page = thumbs_.value(thumbKey(sel.front()));
        info_->name = fi.fileName();
        info_->facts << tr("PDF Document") << QStringLiteral(" ") << tr("Size: %1").arg(sizeText(fi.size()));
        if (when > 0)
            info_->facts << tr("Opened: %1").arg(whenText(when));
        info_->facts << tr("Modified: %1").arg(whenText(fi.lastModified().toMSecsSinceEpoch()));
        if (starred(sel.front()))
            info_->facts << QStringLiteral(" ") << tr("Starred");
        info_->note.clear();
    } else if (sel.size() > 1) {
        info_->note = tr("%1 items selected.").arg(sel.size());
    } else {
        info_->note = tr("Select an item to view its description.");
    }
    qint64 bytes = 0;  // (the status bar's second panel: how big - the selected, else all listed)
    for (const QString &path : sel.isEmpty() ? (section_ == 0 ? recentPaths() : starredPaths()) : sel)
        bytes += QFileInfo(path).size();
    summary_ = all == 0 ? QString()
               : bytes < 1024 * 1024 ? sizeText(bytes)
                                     : QStringLiteral("%1MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 2);
    info_->update();
    emit message(sel.isEmpty() ? tr("%1 object(s)").arg(all) : tr("%1 object(s) selected").arg(sel.size()));
}

void HomePage::starPicked()
{
    const QStringList sel = picked();
    bool any = false;  // (all starred: their stars taken off; else all starred)
    for (const QString &path : sel)
        any = any || !starred(path);
    for (const QString &path : sel)
        setStarred(path, any);
    refresh();
}

void HomePage::removePicked()
{
    const QStringList sel = picked();
    if (sel.isEmpty())
        return;
    if (section_ == 1) {
        for (const QString &path : sel)
            setStarred(path, false);
    } else {
        QJsonArray recent;
        for (const QJsonValue &v : theme::settings().value("recent").toArray())
            if (!sel.contains(v.toString(), Qt::CaseInsensitive))
                recent.append(v);
        theme::saveSetting("recent", recent);
    }
    refresh();
}

bool HomePage::eventFilter(QObject *watched, QEvent *e)
{
    if ((watched == list_ || watched == grid_) && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Delete) {
        removePicked();
        return true;
    }
    return QWidget::eventFilter(watched, e);
}

// its first page, for the Thumbnails view (null: not drawn yet - asked for)
QImage HomePage::thumbnail(const QString &path, QSize size) const
{
    const QImage im = thumbs_.value(thumbKey(path));
    if (im.isNull())
        return {};
    const double dpr = screenScale();
    QImage out(size * dpr, QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);  // (just the page: nothing behind it)
    QPainter p(&out);
    const QImage page = im.scaled((size - QSize(10, 10)) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPointF at((size.width() - page.width() / dpr) / 2, (size.height() - page.height() / dpr) / 2);
    const QRectF sheet(at, page.size() / dpr);
    {  // (a sheet of paper: a thin dark line round it, its shadow along its right and bottom)
        classic::Px x(p);
        const QRect d = x.dev(sheet);
        x.fill(QRect(d.left() + 2 * x.k, d.bottom() + 1 + x.k, d.width(), 2 * x.k), theme::shadow());
        x.fill(QRect(d.right() + 1 + x.k, d.top() + 2 * x.k, 2 * x.k, d.height()), theme::shadow());
        x.bevel(d.adjusted(-x.k, -x.k, x.k, x.k), theme::c("#404040", theme::Edge), theme::c("#404040", theme::Edge), x.k);
    }
    p.drawImage(sheet, page);
    return out;
}

void HomePage::thumbnailFor(const QString &path)
{
    const QString key = thumbKey(path);
    if (asked_.contains(key))
        return;
    asked_.insert(key, true);
    QPointer<HomePage> self(this);
    QThreadPool::globalInstance()->start([self, path, key] {
        QString error;
        QImage im;
        if (auto doc = Document::open(path, &error); doc && doc->pageCount() > 0) {
            const QSizeF s = doc->pageSize(0);
            // (big enough for the side panel's picture on a scaled screen - the grid's made smaller from it)
            const double scale = 720.0 / std::max(1.0, std::max(s.width(), s.height()));
            im = doc->render(0, scale, QRect(0, 0, int(s.width() * scale), int(s.height() * scale)));
        }
        QMetaObject::invokeMethod(
            qApp,
            [self, key, im] {
                if (!self || im.isNull())
                    return;
                self->thumbs_.insert(key, im);
                self->fill();
            },
            Qt::QueuedConnection);
    });
}

void HomePage::menuFor(QPoint at)
{
    const QStringList sel = picked();
    if (sel.isEmpty())
        return;
    QMenu menu(this);
    QAction *open = menu.addAction(tr("&Open"), this, [this, sel] {
        for (const QString &path : sel)
            emit openRequested(path);
    });
    menu.setDefaultAction(open);  // (bold, as the double click's)
    bool any = false;
    for (const QString &path : sel)
        any = any || !starred(path);
    menu.addAction(any ? tr("&Star") : tr("Remove &Star"), this, &HomePage::starPicked);
    menu.addSeparator();
    if (sel.size() == 1)
        menu.addAction(tr("Open File &Location"), this, [path = sel.front()] {
            showInFolder(path);
        });
    menu.addAction(section_ == 0 ? tr("&Remove from Recent") : tr("&Remove from Starred"), this, &HomePage::removePicked);
    menu.exec(at);
}
