#include "ui/ToolButton.h"

#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>

ToolButton::ToolButton(const QString &icon, const QString &tip, const QString &note, bool split,
                       const QString &label, QWidget *parent)
    : QWidget(parent), icon_(icon), label_(label), split_(split)
{
    setToolTip(tip);
    setWhatsThis(note.isEmpty() ? tip : note);
    setFixedSize(sizeHint());
    setAttribute(Qt::WA_Hover);
}

QSize ToolButton::sizeHint() const
{
    const int labelW = label_.isEmpty() ? 0 : QFontMetrics(theme::font()).horizontalAdvance(label_) + 6;
    return {W + (split_ ? ARROW_W : 0) + labelW, H};
}

void ToolButton::setIcon(const QString &name)
{
    icon_ = name;
    image_ = QImage();
    update();
}

void ToolButton::setImage(const QImage &image)
{
    image_ = image;
    update();
}

void ToolButton::setLatched(bool on)
{
    if (on != latched_) {
        latched_ = on;
        update();
    }
}

void ToolButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int w = width(), h = height();
    const bool enabled = isEnabled();
    const bool pressed = down_ && hot_;
    const bool sunk = latched_ || pressed;
    p.fillRect(rect(), theme::face());
    if (latched_ && !pressed)  // a checkered face, like Windows 98's toggled buttons
        classic::checkered(p, rect(), theme::light(), theme::face());
    if (sunk)
        classic::thinSunken(p, rect());
    else if (hot_ && enabled)
        classic::thinRaised(p, rect());
    const int o = sunk ? 1 : 0;  // (the picture moves when pressed, like a real button)
    const int split = split_ ? ARROW_W : 0;
    const int cx = label_.isEmpty() ? (w - split) / 2 : W / 2;
    const QRectF pic(cx - 8 + o, h / 2 - 8 + o, 16, 16);
    if (greyBlues_)  // (its dark blues grey, in every theme)
        icons::draw(p, pic, classic::greyBlues(image_.isNull() ? icons::best(icon_) : image_, theme::dark()), enabled, whiten_);
    else if (!image_.isNull())
        icons::draw(p, pic, image_, enabled, whiten_);
    else
        icons::draw(p, pic, icon_, enabled, whiten_);
    if (!label_.isEmpty()) {
        p.setFont(theme::font());
        const QRectF r(W - 2 + o, o, w - W, h);
        if (enabled) {
            p.setPen(theme::text());
            classic::crispText(p, r, Qt::AlignLeft | Qt::AlignVCenter, label_);
        } else {
            classic::engravedText(p, r, Qt::AlignLeft | Qt::AlignVCenter, label_);
        }
    }
    if (split_) {  // the arrow part: a small triangle, a line between when raised
        const int ax = w - ARROW_W;
        if (hot_ && enabled && !sunk)
            classic::etchedV(p, ax, 2, h - 2);
        const QColor ink = enabled ? theme::ink() : theme::engraved().second;
        classic::downArrow(p, QRectF(ax + ARROW_W / 2 - 2 + o, h / 2 - 1 + o, 5, 3), ink);
    }
}

void ToolButton::enterEvent(QEnterEvent *)
{
    hot_ = true;
    update();
}

void ToolButton::leaveEvent(QEvent *)
{
    hot_ = false;
    update();
}

void ToolButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled()) {
        down_ = true;
        update();
    }
}

void ToolButton::mouseMoveEvent(QMouseEvent *e)
{
    const bool in = rect().contains(e->position().toPoint());
    if (in != hot_) {
        hot_ = in;
        update();
    }
}

void ToolButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const bool go = down_ && hot_ && isEnabled();
    down_ = false;
    update();
    if (!go)
        return;
    if (split_ && e->position().x() >= width() - ARROW_W)
        emit arrowClicked();
    else
        emit clicked();
}

void ToolButton::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange)
        update();
    QWidget::changeEvent(e);
}

ToolSeparator::ToolSeparator(QWidget *parent) : QWidget(parent)
{
    setFixedSize(10, ToolButton::H);
}

void ToolSeparator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
    classic::etchedV(p, 4, 2, height() - 2);
}

ToolGrip::ToolGrip(QWidget *parent) : QWidget(parent)
{
    setFixedWidth(10);
    setMinimumHeight(ToolButton::H);
}

void ToolGrip::paint(QPainter &p, int height) const
{
    // (in the screen's own pixels - both bars alike at any scale: each 3 wide - light, face, shadow - lit
    // along its top, shadowed along its bottom; a pixel between them)
    classic::Px x(p);
    const QRect d = x.dev(QRectF(3, 2, 7, height - 4));
    const int k = x.k;
    for (int b = 0; b < 2; ++b) {
        const QRect bar(d.left() + b * 4 * k, d.top(), 3 * k, d.height());
        x.fill(bar, theme::face());
        x.fill(QRect(bar.left(), bar.top(), k, bar.height()), theme::light());
        x.fill(QRect(bar.left(), bar.top(), 3 * k, k), theme::light());
        x.fill(QRect(bar.left() + 2 * k, bar.top(), k, bar.height()), theme::shadow());
        x.fill(QRect(bar.left() + k, bar.bottom() - k + 1, 2 * k, k), theme::shadow());
    }
}

void ToolGrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
    paint(p, height());
}

EtchedLine::EtchedLine(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(2);
}

void EtchedLine::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (theme::mono()) {  // (Black and White: a plain black line)
        p.fillRect(QRect(0, 0, width(), 1), theme::darkEdge());
        p.fillRect(QRect(0, 1, width(), 1), theme::face());
        return;
    }
    classic::etchedH(p, 0, width(), 0);
}
