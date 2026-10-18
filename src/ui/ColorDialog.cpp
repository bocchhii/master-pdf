// The colour picker, as the Python app's color_dialog: laid out exactly like Windows 95 / 98's "Edit Colors"
// box (positions measured from it) - basic and custom colours on the left, the rainbow field, brightness bar,
// preview and Hue / Sat / Lum / Red / Green / Blue boxes on the right, the eyedropper, OK / Cancel / Add to
// Custom Colors under them. The colours are drawn as they are (a dark theme leaves them alone); custom
// colours are remembered.

#include "ui/Classic.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QToolTip>
#include <QWhatsThis>

#include <QVBoxLayout>

#include <array>
#include <map>
#include <cmath>
#include <functional>

namespace {

const char *kBasic[] = {  // the 48 "Basic colors" of Windows' classic colour picker
    "#FF8080", "#FFFF80", "#80FF80", "#00FF80", "#80FFFF", "#0080FF", "#FF80C0", "#FF80FF",
    "#FF0000", "#FFFF00", "#80FF00", "#00FF40", "#00FFFF", "#0080C0", "#8080C0", "#FF00FF",
    "#804040", "#FF8040", "#00FF00", "#008080", "#004080", "#8080FF", "#800040", "#FF0080",
    "#800000", "#FF8000", "#008000", "#008040", "#0000FF", "#0000A0", "#800080", "#8000FF",
    "#400000", "#804000", "#004000", "#004040", "#000080", "#000040", "#400040", "#400080",
    "#000000", "#808000", "#808040", "#808080", "#408080", "#C0C0C0", "#400040", "#FFFFFF",
};

// (Python's colorsys - hue, lightness, saturation, each 0..1 - so the numbers are the Python app's)
struct Hls {
    double h = 0, l = 0, s = 0;
};

Hls toHls(const QColor &c)
{
    const double r = c.redF(), g = c.greenF(), b = c.blueF();
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    Hls o;
    o.l = (lo + hi) / 2;
    if (hi == lo)
        return o;
    o.s = o.l <= 0.5 ? (hi - lo) / (hi + lo) : (hi - lo) / (2 - hi - lo);
    const double rc = (hi - r) / (hi - lo), gc = (hi - g) / (hi - lo), bc = (hi - b) / (hi - lo);
    double h = r == hi ? bc - gc : g == hi ? 2 + rc - bc : 4 + gc - rc;
    h = std::fmod(h / 6, 1.0);
    o.h = h < 0 ? h + 1 : h;
    return o;
}

double part(double m1, double m2, double hue)
{
    hue = std::fmod(hue, 1.0);
    if (hue < 0)
        hue += 1;
    if (hue < 1.0 / 6)
        return m1 + (m2 - m1) * hue * 6;
    if (hue < 0.5)
        return m2;
    if (hue < 2.0 / 3)
        return m1 + (m2 - m1) * (2.0 / 3 - hue) * 6;
    return m1;
}

QColor fromHls(double h, double l, double s)
{
    auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    if (s == 0)
        return QColor(byte(l), byte(l), byte(l));
    const double m2 = l <= 0.5 ? l * (1 + s) : l + s - l * s, m1 = 2 * l - m2;
    return QColor(byte(part(m1, m2, h + 1.0 / 3)), byte(part(m1, m2, h)), byte(part(m1, m2, h - 1.0 / 3)));
}

// a Windows 98-style eyedropper (16 x 16): a dark rubber bulb at the top right, a glass tube down to the
// tip at the bottom left, lit from the top left
QImage dropperIcon()
{
    static const char *rows[] = {"...........KKK..", "..........KDDDK.", "......K..KDDDDK.", ".......KKDDDDK..",
                                 ".......KKKDDK...", "......KSWKKK....", ".....KSWSK.K....", "....KSWSK.......",
                                 "...KSWSK........", "..KSWSK.........", ".KSWSK..........", ".KWSK...........",
                                 "KKKK............", "KK.............."};
    QImage im(16, 16, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    for (int y = 0; y < 14; ++y)
        for (int x = 0; x < 16; ++x) {
            const char ch = rows[y][x];
            const QColor c = ch == 'K' ? QColor(0, 0, 0) : ch == 'D' ? QColor(96, 96, 96) : ch == 'S' ? QColor(176, 196, 222)
                                                                       : ch == 'W' ? QColor(255, 255, 255) : QColor();
            if (c.isValid() && x + 1 < 16)
                im.setPixelColor(x + 1, y + 1, c);
        }
    return im;
}

// the picker's own layout (the original's): rainbow field, brightness bar, preview
constexpr int FX = 212, FY = 7, FW = 175, FH = 187;
constexpr int BX = 404, BW = 10;
constexpr int PX = 212, PY = 204, PW = 58, PH = 44;

// the eyedropper's layer: over every screen, all but see-through - the screen as it was a moment ago read
// under the mouse; a click takes the colour, Esc or a right-click puts back the one there was
class Dropper : public QWidget {
public:
    std::function<void(const QColor &)> moved;
    std::function<void(bool)> done;

    // (owner: the colour box - modal: a window of its own would get no input while it's up, yet cover every
    // screen holding the mouse - the app stuck. Its own window, and modal itself: the one that gets input)
    explicit Dropper(QWidget *owner) : QWidget(owner, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    {
        setWindowModality(Qt::ApplicationModal);
        setAttribute(Qt::WA_DeleteOnClose, false);
        QRect all;
        for (QScreen *s : QGuiApplication::screens()) {
            all |= s->geometry();
            shots_.push_back({s->geometry(), s->devicePixelRatio(), s->grabWindow(0).toImage()});
        }
        setGeometry(all);
        setWindowOpacity(0.01);
        setCursor(Qt::CrossCursor);
        setMouseTracking(true);
        tag_ = new QWidget(this, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::ToolTip);
        tag_->setAttribute(Qt::WA_TransparentForMouseEvents);
        tag_->setFixedSize(84, 18);
    }

    QColor under() const
    {
        const QPoint at = QCursor::pos();
        for (const Shot &s : shots_)
            if (s.area.contains(at)) {
                const QPoint px = ((at - s.area.topLeft()) * s.dpr);
                if (s.image.rect().contains(px))
                    return s.image.pixelColor(px);
            }
        return {};
    }
    void start()
    {
        show();
        raise();
        activateWindow();
        grabMouse();
        grabKeyboard();
        follow();
    }

protected:
    void mouseMoveEvent(QMouseEvent *) override { follow(); }
    void mouseReleaseEvent(QMouseEvent *e) override { finish(e->button() == Qt::LeftButton); }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            finish(false);
    }

private:
    struct Shot {
        QRect area;
        double dpr;
        QImage image;
    };
    std::vector<Shot> shots_;
    QWidget *tag_;
    QColor tagColor_;

    void follow()
    {
        const QColor c = under();
        if (c.isValid() && moved)
            moved(c);
        // (beside the pointer: the colour and its number)
        tagColor_ = c;
        tag_->move(QCursor::pos() + QPoint(16, 18));
        paintTag();
        tag_->show();
        tag_->update();
    }
    void paintTag()
    {
        // (drawn into a picture shown as the tag's background - a frameless window painted here)
        QPixmap pm(tag_->size() * tag_->devicePixelRatioF());
        pm.setDevicePixelRatio(tag_->devicePixelRatioF());
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.fillRect(QRect(0, 0, 18, 18), tagColor_.isValid() ? tagColor_ : Qt::white);
        p.setPen(Qt::black);
        p.drawRect(QRectF(0.5, 0.5, 17, 17));
        p.fillRect(QRect(18, 0, 66, 18), QColor("#FFFFE1"));
        p.drawRect(QRectF(18.5, 0.5, 65, 17));
        p.setFont(theme::font());
        p.drawText(QRect(18, 0, 66, 18), Qt::AlignCenter, tagColor_.isValid() ? tagColor_.name().toUpper() : QString());
        p.end();
        QPalette pal = tag_->palette();
        pal.setBrush(QPalette::Window, pm);
        tag_->setPalette(pal);
        tag_->setAutoFillBackground(true);
    }
    void finish(bool take)
    {
        releaseMouse();
        releaseKeyboard();
        tag_->hide();
        hide();
        if (done)
            done(take);
    }
};

// the box itself: one surface under everything - swatches, rainbow, bar and preview drawn on it
class EditColors : public QWidget {
public:
    std::function<void()> accepted;  // (OK - or Enter)

    explicit EditColors(const QColor &initial, QWidget *parent) : QWidget(parent)
    {
        setFixedSize(429, 309);
        setFont(theme::font());
        const Hls c = toHls(initial.isValid() ? initial : QColor("#000085"));
        h_ = c.h, l_ = c.l, s_ = c.s;
        const QJsonArray saved = theme::settings().value(QStringLiteral("custom_colors")).toArray();
        for (int i = 0; i < 16; ++i)
            customs_[size_t(i)] = i < saved.size() && saved[i].isString() ? QColor(saved[i].toString()) : QColor();
        slot_ = 0;
        for (int i = 0; i < 16; ++i)
            if (!customs_[size_t(i)].isValid()) {
                slot_ = i;
                break;
            }
        // the rainbow: hue across, saturation down, at the middle lightness
        rainbow_ = QImage(FW, FH, QImage::Format_RGB32);
        for (int y = 0; y < FH; ++y)
            for (int x = 0; x < FW; ++x)
                rainbow_.setPixelColor(x, y, fromHls(double(x) / (FW - 1), 0.5, 1 - double(y) / (FH - 1)));
        for (int i = 0; i < 48; ++i)
            if (initial.isValid() && QColor(kBasic[i]).rgb() == initial.rgb()) {
                pick_ = {0, i};
                break;
            }

        label(tr("<u>B</u>asic colors:"), 4, 4);
        label(tr("<u>C</u>ustom colors:"), 4, 186);
        // the colour's number (#RRGGBB) under the preview: type or paste one in, Enter
        hex_ = new QLineEdit(this);
        hex_->setFont(theme::font());
        hex_->setAlignment(Qt::AlignCenter);
        hex_->setGeometry(PX - 1, PY + PH + 3, PW + 2, 20);
        hex_->installEventFilter(this);
        // each column's names start on one line, as far left as the longest one needs to end just
        // before its boxes
        const QFontMetrics fm(theme::font());
        struct Name {
            const char *key, *plain, *marked;
        };
        const std::array<std::pair<std::array<Name, 3>, int>, 2> columns = {{
            {{{{"hue", "Hue:", "H<u>u</u>e:"}, {"sat", "Sat:", "<u>S</u>at:"}, {"lum", "Lum:", "<u>L</u>um:"}}}, 305},
            {{{{"red", "Red:", "<u>R</u>ed:"}, {"green", "Green:", "<u>G</u>reen:"}, {"blue", "Blue:", "Bl<u>u</u>e:"}}}, 386},
        }};
        for (const auto &[names, boxX] : columns) {
            int widest = 0;
            for (const Name &n : names)
                widest = std::max(widest, fm.horizontalAdvance(QString::fromLatin1(n.plain)));
            const int left = boxX - 5 - widest;
            for (int r = 0; r < 3; ++r) {
                label(QString::fromLatin1(names[size_t(r)].marked), left, 205 + r * 24);
                auto *e = new QLineEdit(this);
                e->setFont(theme::font());
                e->setGeometry(boxX, 203 + r * 24, 28, 20);
                e->installEventFilter(this);
                boxes_[QString::fromLatin1(names[size_t(r)].key)] = e;
            }
        }
        // the eyedropper - a small square button with its picture - OK, Cancel, Add to Custom Colors
        picker_ = new ClassicButton(QString(), this);
        picker_->setIcon(QIcon(QPixmap::fromImage(dropperIcon())));
        picker_->setIconSize(QSize(16, 16));
        picker_->setGeometry(211, 280, 23, 23);
        picker_->setToolTip(tr("Pick a colour from the screen"));
        connect(picker_, &QPushButton::clicked, this, [this] { pickFromScreen(); });
        ok_ = button(tr("OK"), 5, 281, 66, [this] {
            if (accepted)
                accepted();
        });
        cancel_ = button(tr("Cancel"), 77, 281, 66, [this] { window()->close(); });
        add_ = button(tr("&Add to Custom Colors"), 238, 280, 188, [this] { addCustom(); });
        notes();
        refresh();
    }

    QColor current() const { return fromHls(h_, l_, s_); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::face());
        // the basic and custom colours: 20 x 20 sunken boxes (16 x 16 inside), 24 apart each way
        for (int k = 0; k < 64; ++k) {
            const QPoint at = cell(k);
            const QColor c = k < 48 ? QColor(kBasic[k]) : customs_[size_t(k - 48)].isValid() ? customs_[size_t(k - 48)] : QColor(Qt::white);
            classic::sunkenEdge(p, QRectF(at.x() - 2, at.y() - 2, 20, 20), c);
        }
        // the chosen one: a black frame and a dotted one round it, like Windows'
        if (pick_.first >= 0) {
            const QPoint at = cell(pick_.first == 0 ? pick_.second : 48 + pick_.second);
            classic::Px x(p);
            QPen solid(Qt::black, x.k);
            solid.setJoinStyle(Qt::MiterJoin);
            p.setPen(solid);
            const QRect a = x.dev(QRectF(at.x() - 3, at.y() - 3, 22, 22));
            p.drawRect(QRectF(a).adjusted(x.k / 2.0, x.k / 2.0, -x.k / 2.0, -x.k / 2.0));
            QPen dots(Qt::black, x.k, Qt::CustomDashLine);
            dots.setDashPattern({1, 1});
            p.setPen(dots);
            const QRect b = x.dev(QRectF(at.x() - 5, at.y() - 5, 26, 26));
            p.drawRect(QRectF(b).adjusted(x.k / 2.0, x.k / 2.0, -x.k / 2.0, -x.k / 2.0));
        }
        // the rainbow field, the brightness bar (this colour, light to dark), each in a 1 px sunken edge
        classic::thinSunken(p, QRectF(FX - 1, FY - 1, FW + 2, FH + 2));
        classic::thinSunken(p, QRectF(BX - 1, FY - 1, BW + 2, FH + 2));
        {
            classic::Px x(p);
            p.drawImage(x.dev(QRectF(FX, FY, FW, FH)), rainbow_);
            QImage strip(1, FH, QImage::Format_RGB32);
            for (int y = 0; y < FH; ++y)
                strip.setPixelColor(0, y, fromHls(h_, 1 - double(y) / (FH - 1), s_));
            p.drawImage(x.dev(QRectF(BX, FY, BW, FH)), strip);
            p.fillRect(x.dev(QRectF(PX, PY, PW, PH)), current());
        }
        classic::thinSunken(p, QRectF(PX - 1, PY - 1, PW + 2, PH + 2));
        p.setRenderHint(QPainter::Antialiasing);
        // the cross over the colour chosen in the field: four short black strokes round it
        const double cx = FX + h_ * (FW - 1), cy = FY + (1 - s_) * (FH - 1);
        p.setPen(QPen(Qt::black, 2, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(cx - 8, cy), QPointF(cx - 3, cy));
        p.drawLine(QPointF(cx + 3, cy), QPointF(cx + 8, cy));
        p.drawLine(QPointF(cx, cy - 8), QPointF(cx, cy - 3));
        p.drawLine(QPointF(cx, cy + 3), QPointF(cx, cy + 8));
        // the arrow beside the bar, at the lightness
        const double ay = FY + (1 - l_) * (FH - 1);
        QPainterPath arrow;
        arrow.moveTo(BX + BW + 3, ay);
        arrow.lineTo(BX + BW + 9, ay - 6);
        arrow.lineTo(BX + BW + 9, ay + 6);
        arrow.closeSubpath();
        p.fillPath(arrow, theme::ink());
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const QPointF at = e->position();
        const bool inField = at.x() >= FX && at.x() < FX + FW && at.y() >= FY && at.y() < FY + FH;
        const bool inBar = at.x() >= BX && at.x() < BX + BW + 12 && at.y() >= FY - 3 && at.y() < FY + FH + 3;
        drag_ = inField ? 1 : inBar ? 2 : 0;
        for (int k = 0; k < 64; ++k) {
            const QPoint c = cell(k);
            if (at.x() >= c.x() - 2 && at.x() <= c.x() + 17 && at.y() >= c.y() - 2 && at.y() <= c.y() + 17) {
                if (k >= 48) {
                    slot_ = k - 48;
                    if (!customs_[size_t(slot_)].isValid()) {  // (an empty one: just chosen for Add)
                        pick_ = {1, slot_};
                        refresh();
                        return;
                    }
                    setColor(customs_[size_t(slot_)], {1, slot_});
                } else {
                    setColor(QColor(kBasic[k]), {0, k});
                }
                return;
            }
        }
        if (drag_ == 1)
            dragField(at);
        else if (drag_ == 2)
            dragBar(at);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (drag_ == 1)
            dragField(e->position());
        else if (drag_ == 2)
            dragBar(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *) override { drag_ = 0; }
    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && accepted) {  // (Enter: OK)
            accepted();
            return;
        }
        QWidget::keyPressEvent(e);
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::WhatsThis) {  // (the drawn parts: which one was clicked)
            const auto *h = static_cast<QHelpEvent *>(e);
            const QString note = noteAt(h->pos());
            if (!note.isEmpty())
                QWhatsThis::showText(h->globalPos(), note, this);
            return true;
        }
        return QWidget::event(e);
    }
    bool eventFilter(QObject *o, QEvent *e) override
    {
        // Enter / leaving a box: what was typed taken, if it makes sense (Enter in a number box also OKs,
        // as the original's; in the #RRGGBB one it doesn't)
        if (e->type() == QEvent::KeyPress) {
            const int k = static_cast<QKeyEvent *>(e)->key();
            if (k == Qt::Key_Return || k == Qt::Key_Enter) {
                if (o == hex_) {
                    typedHex();
                    return true;
                }
                typed(static_cast<QLineEdit *>(o));
                if (accepted)
                    accepted();
                return true;
            }
        } else if (e->type() == QEvent::FocusOut && !refreshing_) {
            if (o == hex_)
                typedHex();
            else
                typed(static_cast<QLineEdit *>(o));
        }
        return QWidget::eventFilter(o, e);
    }

private:
    double h_ = 0, l_ = 0, s_ = 0;
    std::pair<int, int> pick_{-1, -1};  // (0 basic / 1 custom, which)
    std::array<QColor, 16> customs_;
    int slot_ = 0;  // (the custom box Add puts the colour in)
    int drag_ = 0;
    bool refreshing_ = false;
    QImage rainbow_;
    QLineEdit *hex_ = nullptr;
    std::map<QString, QLineEdit *> boxes_;
    QPushButton *picker_ = nullptr, *ok_ = nullptr, *cancel_ = nullptr, *add_ = nullptr;

    static QString tr(const char *s) { return QObject::tr(s); }
    static QPoint cell(int k)  // (a swatch's colour area, 16 x 16)
    {
        return k < 48 ? QPoint(10 + (k % 8) * 24, 25 + (k / 8) * 24) : QPoint(10 + ((k - 48) % 8) * 24, 205 + ((k - 48) / 8) * 24);
    }
    QLabel *label(const QString &text, int x, int y)
    {
        auto *l = new QLabel(text, this);
        l->setFont(theme::font());
        l->setContentsMargins(0, 0, 0, 0);
        l->setMargin(0);
        l->adjustSize();
        l->move(x, y);
        return l;
    }
    QPushButton *button(const QString &text, int x, int y, int w, std::function<void()> fn)
    {
        auto *b = new ClassicButton(text, this);
        b->setGeometry(x, y, w, 23);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    }
    void refresh()
    {
        refreshing_ = true;
        const QColor c = current();
        const std::map<QString, int> values = {{"hue", int(std::lround(h_ * 240)) % 240}, {"sat", int(std::lround(s_ * 240))},
                                               {"lum", int(std::lround(l_ * 240))},       {"red", c.red()},
                                               {"green", c.green()},                      {"blue", c.blue()}};
        for (const auto &[name, v] : values)
            boxes_[name]->setText(QString::number(v));
        hex_->setText(c.name().toUpper());
        refreshing_ = false;
        update();
    }
    void setColor(const QColor &c, std::pair<int, int> pick = {-1, -1})
    {
        const Hls x = toHls(c);
        h_ = x.h, l_ = x.l, s_ = x.s;
        pick_ = pick;
        refresh();
    }
    void dragField(QPointF at)
    {
        h_ = std::clamp((at.x() - FX) / (FW - 1), 0.0, 1.0);
        s_ = std::clamp(1 - (at.y() - FY) / (FH - 1), 0.0, 1.0);
        if (l_ == 0.0 || l_ == 1.0)  // (black / white has no colour to show: the middle lightness)
            l_ = 0.5;
        pick_ = {-1, -1};
        refresh();
    }
    void dragBar(QPointF at)
    {
        l_ = std::clamp(1 - (at.y() - FY) / (FH - 1), 0.0, 1.0);
        pick_ = {-1, -1};
        refresh();
    }
    void typed(QLineEdit *from)
    {
        bool okH, okS, okL, okR, okG, okB;
        const int hue = boxes_["hue"]->text().toInt(&okH), sat = boxes_["sat"]->text().toInt(&okS), lum = boxes_["lum"]->text().toInt(&okL);
        const int r = boxes_["red"]->text().toInt(&okR), g = boxes_["green"]->text().toInt(&okG), b = boxes_["blue"]->text().toInt(&okB);
        if (from == boxes_["hue"] || from == boxes_["sat"] || from == boxes_["lum"]) {
            if (okH && okS && okL) {
                h_ = std::clamp(hue, 0, 239) / 240.0;
                s_ = std::clamp(sat, 0, 240) / 240.0;
                l_ = std::clamp(lum, 0, 240) / 240.0;
                pick_ = {-1, -1};
            }
            refresh();  // (nonsense typed: the colour as it was)
            return;
        }
        if (okR && okG && okB)
            setColor(QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)));
        else
            refresh();
    }
    void typedHex()  // ("#FF8000", "ff8000", "F80"... - else the colour as it was)
    {
        QString t = hex_->text().trimmed();
        while (t.startsWith(QLatin1Char('#')))
            t.remove(0, 1);
        if (t.size() == 3)
            t = QString(t[0]) + t[0] + t[1] + t[1] + t[2] + t[2];
        static const QRegularExpression six(QStringLiteral("^[0-9A-Fa-f]{6}$"));
        if (six.match(t).hasMatch())
            setColor(QColor(QLatin1Char('#') + t));
        else
            refresh();
    }
    void addCustom()
    {
        customs_[size_t(slot_)] = current();
        QJsonArray keep;
        for (const QColor &c : customs_)
            keep.append(c.isValid() ? QJsonValue(c.name().toUpper()) : QJsonValue());
        theme::saveSetting(QStringLiteral("custom_colors"), keep);
        pick_ = {1, slot_};
        slot_ = (slot_ + 1) % 16;
        refresh();
    }
    void pickFromScreen()
    {
        const QColor was = current();
        auto *layer = new Dropper(window());
        layer->moved = [this](const QColor &c) { setColor(c); };
        layer->done = [this, layer, was](bool take) {
            const QColor c = take ? layer->under() : was;
            setColor(c.isValid() ? c : was);
            layer->deleteLater();
            window()->activateWindow();
            window()->raise();
        };
        layer->start();
    }
    // "What's This?" (the ? button): a note for each part of the box
    QString noteAt(QPoint at) const
    {
        for (int k = 0; k < 64; ++k) {
            const QPoint c = cell(k);
            if (at.x() >= c.x() - 3 && at.x() <= c.x() + 18 && at.y() >= c.y() - 3 && at.y() <= c.y() + 18)
                return k < 48 ? tr("Click a colour to choose it.")
                              : tr("Colours you've saved. Click one to choose it. To save the chosen colour, click an empty box, then "
                                   "click Add to Custom Colors.");
        }
        if (at.x() >= FX - 1 && at.x() <= FX + FW && at.y() >= FY - 1 && at.y() <= FY + FH)
            return tr("Click or drag in the colours to choose one: the shade changes from left to right, and the colour gets "
                      "greyer towards the bottom.");
        if (at.x() >= BX - 1 && at.x() <= BX + BW + 12 && at.y() >= FY - 4 && at.y() <= FY + FH + 4)
            return tr("Click or drag to make the colour lighter (up) or darker (down).");
        if (at.x() >= PX - 1 && at.x() <= PX + PW && at.y() >= PY - 1 && at.y() <= PY + PH)
            return tr("Shows the colour you've chosen.");
        if (at.y() < 24)
            return tr("Click a colour to choose it.");
        if (at.x() < 200 && at.y() >= 180 && at.y() < 204)
            return tr("Colours you've saved. Click one to choose it. To save the chosen colour, click an empty box, then click Add "
                      "to Custom Colors.");
        return {};
    }
    void notes()
    {
        hex_->setWhatsThis(tr("The colour's number, as used on web pages (#RRGGBB). Type or paste one in and press Enter."));
        boxes_["hue"]->setWhatsThis(tr("The colour's shade, from 0 (red) round through the rainbow to 239. Type a number and press Enter."));
        boxes_["sat"]->setWhatsThis(tr("How strong the colour is, from 0 (grey) to 240 (full colour). Type a number and press Enter."));
        boxes_["lum"]->setWhatsThis(tr("How light the colour is, from 0 (black) to 240 (white). Type a number and press Enter."));
        boxes_["red"]->setWhatsThis(tr("How much red is in the colour, from 0 to 255. Type a number and press Enter."));
        boxes_["green"]->setWhatsThis(tr("How much green is in the colour, from 0 to 255. Type a number and press Enter."));
        boxes_["blue"]->setWhatsThis(tr("How much blue is in the colour, from 0 to 255. Type a number and press Enter."));
        picker_->setWhatsThis(tr("The eyedropper: takes a colour from anywhere on the screen - the PDF, a picture, another window: "
                                 "point at it (the box shows the colour under the mouse) and click. Esc or a right-click cancels."));
        ok_->setWhatsThis(tr("Uses the chosen colour and closes this box."));
        cancel_->setWhatsThis(tr("Closes this box without changing anything."));
        add_->setWhatsThis(tr("Saves the chosen colour in the selected Custom colors box, so you can pick it again later."));
        for (QLabel *l : findChildren<QLabel *>())  // (the names: their boxes' notes)
            for (const auto &[name, box] : boxes_)
                if (QString(l->text()).remove(QRegularExpression(QStringLiteral("</?u>"))).startsWith(name.left(1).toUpper() + name.mid(1)))
                    l->setWhatsThis(box->whatsThis());
    }
};

}  // namespace

QColor colorDialog(QWidget *owner, const QString &title, const QColor &initial, bool beside)
{
    ClassicDialog dlg(owner, title);
    auto *layout = new QVBoxLayout(dlg.body());
    layout->setContentsMargins(0, 0, 0, 0);
    auto *box = new EditColors(initial, dlg.body());
    layout->addWidget(box);
    bool ok = false;
    box->accepted = [&] {
        ok = true;
        dlg.close();
    };
    if (beside) {  // (its left edge two thirds across its window, near the setting it's for - never off the screen)
        dlg.adjustSize();
        if (QWidget *top = owner ? owner->window() : nullptr) {
            const QRect a = top->frameGeometry(), screen = top->screen()->availableGeometry();
            dlg.move(std::min(a.left() + a.width() * 64 / 100, screen.right() - dlg.width() - 4),
                     std::min(a.top() + a.height() * 29 / 100, screen.bottom() - dlg.height() - 40));
        }
    }
    dlg.exec(beside);
    return ok ? box->current() : QColor();
}
