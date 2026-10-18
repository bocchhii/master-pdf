#include "ui/DocTabs.h"

#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kPad = 10;      // (text in from a tab's sides)
constexpr int kClose = 13;    // (its x's box, and the room it takes)
constexpr int kMaxTab = 190;  // (a long name: cut short with "...")
constexpr int kMinTab = 56;
constexpr int kLeft = 4;      // (the first tab in from the left)
}  // namespace

DocTabs::DocTabs(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFont(theme::font());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setWhatsThis(tr("The PDFs open in this window, one tab each - Home first. Click a tab to show it; its x "
                    "(or a middle click) closes it."));
}

QSize DocTabs::sizeHint() const
{
    return QSize(200, fontMetrics().height() + 11);
}

QSize DocTabs::minimumSizeHint() const
{
    return QSize(kMinTab * 2, sizeHint().height());
}

int DocTabs::addTab(const QString &text, const QString &tip, bool closable)
{
    tabs_.push_back({text, tip, closable, 0});
    layoutTabs();
    update();
    return int(tabs_.size()) - 1;
}

void DocTabs::removeTab(int index)
{
    if (index < 0 || index >= count())
        return;
    tabs_.erase(tabs_.begin() + index);
    pressedClose_ = hoverClose_ = -1;
    if (current_ >= count())
        current_ = count() - 1;
    else if (index < current_)
        --current_;
    layoutTabs();
    update();
}

void DocTabs::setTabText(int index, const QString &text, const QString &tip)
{
    if (index < 0 || index >= count() || (tabs_[size_t(index)].text == text && tabs_[size_t(index)].tip == tip))
        return;
    tabs_[size_t(index)].text = text;
    tabs_[size_t(index)].tip = tip;
    layoutTabs();
    update();
}

void DocTabs::setCurrent(int index)
{
    if (index < 0 || index >= count() || index == current_)
        return;
    current_ = index;
    update();
    emit currentChanged(index);
}

// each tab as wide as its name (and x) - all made narrower, their names cut short, when they don't fit
void DocTabs::layoutTabs()
{
    const QFontMetrics fm = fontMetrics();
    int total = 0, shrinkable = 0;
    for (Tab &t : tabs_) {
        t.width = std::clamp(fm.horizontalAdvance(t.text) + 2 * kPad + (t.closable ? kClose + 2 : 0), kMinTab, kMaxTab);
        total += t.width;
        shrinkable += t.closable ? t.width - kMinTab : 0;
    }
    const int room = width() - 2 * kLeft - 4;
    if (total > room && shrinkable > 0) {
        const double keep = std::max(0.0, 1.0 - double(total - room) / shrinkable);
        for (Tab &t : tabs_)
            if (t.closable)
                t.width = kMinTab + int((t.width - kMinTab) * keep);
    }
}

QRect DocTabs::tabRect(int index) const
{
    if (index < 0 || index >= count())
        return {};
    int x = kLeft;
    for (int i = 0; i < index; ++i)
        x += tabs_[size_t(i)].width;
    const int h = height();
    QRect r(x, 2, tabs_[size_t(index)].width, h - 2);
    if (index == current_)
        r.adjust(-2, -2, 2, 0);
    return r;
}

QRect DocTabs::closeRect(int index) const
{
    if (index < 0 || index >= count() || !tabs_[size_t(index)].closable)
        return {};
    const QRect r = tabRect(index);
    return QRect(r.right() - kClose - 3, r.top() + (r.height() - kClose) / 2 + (index == current_ ? 0 : -1), kClose, kClose - 1);
}

int DocTabs::tabAt(QPoint pos) const
{
    if (current_ >= 0 && tabRect(current_).contains(pos))  // (the one chosen: on top of its neighbours' edges)
        return current_;
    for (int i = 0; i < count(); ++i)
        if (tabRect(i).contains(pos))
            return i;
    return -1;
}

void DocTabs::paintEvent(QPaintEvent *)
{
    using namespace theme;
    QPainter p(this);
    p.fillRect(rect(), face());
    const int base = height() - 1;  // (the line under the tabs: the top edge of what's under them)
    auto paintTab = [&](int i) {
        const QRect r = tabRect(i);
        const bool on = i == current_;
        const int l = r.left(), t = r.top(), rt = r.right(), b = on ? base + 1 : base - 1;  // (the others stop above the line under them: it shows, as Windows 98's)
        {
            classic::Px x(p);
            auto line = [&](int x0, int y0, int x1, int y1, const QColor &c) {  // (both ends in)
                const QRect d = x.dev(QRectF(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
                x.fill(d, c);
            };
            line(l + 1, t + 1, rt - 1, b, face());
            line(l, t + 2, l, b, light());         // its left side
            line(l + 1, t + 1, l + 1, t + 1, light());
            line(l + 2, t, rt - 2, t, light());    // its top
            line(rt - 1, t + 1, rt - 1, t + 1, darkEdge());
            line(rt, t + 2, rt, b, darkEdge());    // its right side: black, the shadow inside it
            line(rt - 1, t + 2, rt - 1, b, shadow());
            if (on)  // (joined to what's under it: no line under it)
                line(l + 1, base, rt - 2, base, face());
        }
        QRect textBox = r.adjusted(kPad - (on ? 0 : 2), on ? 1 : 0, -kPad + (on ? 0 : 2), 0);
        if (tabs_[size_t(i)].closable)
            textBox.setRight(closeRect(i).left() - 3);
        p.setFont(font());
        p.setPen(text());
        classic::crispText(p, textBox, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, tabs_[size_t(i)].text, nullptr,
                           Qt::ElideRight);
        if (tabs_[size_t(i)].closable) {  // its x: a little button under the mouse, pressed in while held
            const QRect c = closeRect(i);
            const bool held = pressedClose_ == i && hoverClose_ == i;
            if (hoverClose_ == i || pressedClose_ == i)
                classic::smallButton(p, c, held);
            // (two diagonals, 2 screen pixels thick, as Windows 98's close x - drawn in the screen's own pixels,
            // so every step of them is alike at any scale)
            classic::Px x(p);
            const QRect d = x.dev(QRectF(c));
            const int n = std::max(6, int(std::lround(7 * x.dpr)));  // (its rows: 7 at 100%)
            const int shift = held ? x.k : 0;
            const int x0 = d.left() + (d.width() - (n + 1)) / 2 + shift, y0 = d.top() + (d.height() - n) / 2 + shift;
            for (int i = 0; i < n; ++i) {
                x.fill(QRect(x0 + i, y0 + i, 2, 1), ink());
                x.fill(QRect(x0 + n - 1 - i, y0 + i, 2, 1), ink());
            }
        }
    };
    {
        classic::Px x(p);
        x.fill(x.dev(QRectF(0, base, width(), 1)), light());
    }
    for (int i = 0; i < count(); ++i)
        if (i != current_)
            paintTab(i);
    if (current_ >= 0)
        paintTab(current_);
}

void DocTabs::mousePressEvent(QMouseEvent *e)
{
    const int i = tabAt(e->pos());
    if (i < 0)
        return;
    if (e->button() == Qt::MiddleButton && tabs_[size_t(i)].closable) {
        emit closeRequested(i);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (closeRect(i).contains(e->pos())) {
        pressedClose_ = hoverClose_ = i;
        update();
        return;
    }
    setCurrent(i);
}

void DocTabs::mouseReleaseEvent(QMouseEvent *e)
{
    if (pressedClose_ < 0)
        return;
    const int i = pressedClose_;
    pressedClose_ = -1;
    update();
    if (closeRect(i).contains(e->pos()))
        emit closeRequested(i);
}

void DocTabs::mouseMoveEvent(QMouseEvent *e)
{
    const int i = tabAt(e->pos());
    const int over = i >= 0 && closeRect(i).contains(e->pos()) ? i : -1;
    if (over != hoverClose_) {
        hoverClose_ = over;
        update();
    }
}

void DocTabs::leaveEvent(QEvent *)
{
    if (hoverClose_ >= 0) {
        hoverClose_ = -1;
        update();
    }
}

void DocTabs::resizeEvent(QResizeEvent *)
{
    layoutTabs();
}

bool DocTabs::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *h = static_cast<QHelpEvent *>(e);
        const int i = tabAt(h->pos());
        if (i >= 0 && closeRect(i).contains(h->pos()))
            QToolTip::showText(h->globalPos(), tr("Close (Ctrl+W)"), this);
        else if (i >= 0 && !tabs_[size_t(i)].tip.isEmpty())
            QToolTip::showText(h->globalPos(), tabs_[size_t(i)].tip, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}
