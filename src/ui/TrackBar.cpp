#include "ui/TrackBar.h"

#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kX0 = 7;  // where the groove's stops start and end, from each side
constexpr int kTY = 3;  // the thumb's top
}  // namespace

TrackBar::TrackBar(QWidget *parent, int length) : QWidget(parent), length_(length)
{
    setFixedSize(length, 22);
    setFocusPolicy(Qt::ClickFocus);
}

void TrackBar::setRange(int lo, int hi, int step)
{
    lo_ = lo;
    hi_ = std::max(lo + 1, hi);
    step_ = std::max(1, step);
    value_ = std::clamp(value_, lo_, hi_);
    update();
}

void TrackBar::setValue(int v)
{
    v = std::clamp(lo_ + (v - lo_ + step_ / 2) / step_ * step_, lo_, hi_);
    if (v == value_)
        return;
    value_ = v;
    update();
    emit valueChanged(v);
}

double TrackBar::xOf(int v) const
{
    return kX0 + double(v - lo_) / (hi_ - lo_) * (length_ - 2 * kX0);
}

void TrackBar::setFromX(double x)
{
    const double f = std::clamp((x - kX0) / (length_ - 2 * kX0), 0.0, 1.0);
    setValue(lo_ + int(std::lround(f * (hi_ - lo_))));
}

void TrackBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
    using namespace theme;
    const QPointF at(0, 0);
    auto px = [&](int x0, int y0, int x1, int y1, const QColor &c) { classic::block(p, at, x0, y0, x1, y1, c); };
    // the groove: shadow and dark at the top / left, light at the bottom / right
    const int x0 = kX0 - 2, x1 = length_ - kX0 + 2, gy = kTY + 5;
    px(x0, gy, x1, gy + 1, shadow());
    px(x0, gy, x0 + 1, gy + 4, shadow());
    px(x0 + 1, gy + 1, x1 - 1, gy + 2, darkEdge());
    px(x0 + 1, gy + 2, x1, gy + 3, face());
    px(x0, gy + 3, x1 + 1, gy + 4, light());
    px(x1, gy, x1 + 1, gy + 4, light());
    // the ticks (every stop when there are few, else the two ends)
    const QColor ink = isEnabled() ? text() : c("#999999", Text);
    const int stops = (hi_ - lo_) / step_;
    for (int k = 0; k <= stops; ++k) {
        if (stops > 20 && k != 0 && k != stops)
            continue;
        const int tx = int(std::lround(xOf(lo_ + k * step_)));
        px(tx, kTY + 16, tx + 1, kTY + 18, ink);
    }
    // the thumb: 9 px wide, a point at the bottom
    const int cx = int(std::lround(xOf(value_)));
    const int l = cx - 4, r = cx + 4, b = kTY + 10;
    px(l, kTY, r + 1, b + 1, face());
    for (int k = 0; k < 4; ++k)  // (the point)
        px(l + k + 1, b + 1 + k, r - k, b + 2 + k, face());
    px(l, kTY, l + 1, b + 1, light());  // left and top
    px(l, kTY, r, kTY + 1, light());
    for (int k = 0; k <= 4; ++k) {  // the point's sides: light down the left, dark down the right
        px(l + k, b + k, l + k + 1, b + k + 1, light());
        px(r - k, b + k, r - k + 1, b + k + 1, darkEdge());
        if (k < 4)
            px(r - 1 - k, b + k, r - k, b + k + 1, shadow());
    }
    px(r, kTY, r + 1, b + 1, darkEdge());
    px(r - 1, kTY + 1, r, b, shadow());
}

void TrackBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled())
        setFromX(e->position().x());
}

void TrackBar::mouseMoveEvent(QMouseEvent *e)
{
    if ((e->buttons() & Qt::LeftButton) && isEnabled())
        setFromX(e->position().x());
}

void TrackBar::wheelEvent(QWheelEvent *e)
{
    if (e->angleDelta().y())
        setValue(value_ + (e->angleDelta().y() > 0 ? step_ : -step_));
    e->accept();
}

void TrackBar::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Down)
        setValue(value_ - step_);
    else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Up)
        setValue(value_ + step_);
    else
        QWidget::keyPressEvent(e);
}
