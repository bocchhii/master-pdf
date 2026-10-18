#include "ui/ClassicWindow.h"

#include "ui/Classic.h"
#include "ui/Theme.h"
#include "ui/WhatsThis.h"

#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWhatsThis>
#include <QWindow>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>
#endif

namespace {
constexpr int kBorder = 3;   // the 3D border
constexpr int kGrip = 5;     // how near the edge the mouse resizes
constexpr int kCorner = 16;  // ... a corner, along each side
}  // namespace

ClassicWindow::ClassicWindow(bool resizable, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), resizable_(resizable)
{
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(kBorder, kBorder, kBorder, kBorder);
    box->setSpacing(0);
    bar_ = new TitleBar(this);
    box->addWidget(bar_);
    body_ = new QWidget(this);
    box->addWidget(body_, 1);
    connect(theme::Notifier::instance(), &theme::Notifier::changed, this, [this] { update(); });
    // (every window: the ? - What's This? - by its X, unless turned off in Settings)
    setHelpButton(whatsThis::buttonsOn());
    connect(this, &ClassicWindow::helpClicked, this, [this] { whatsThis::start(this); });
}

void ClassicWindow::setTitle(const QString &title)
{
    title_ = title;
    setWindowTitle(title);  // (the taskbar's and Alt+Tab's)
    bar_->update();
}

void ClassicWindow::setUpdateButton(std::function<void()> onClick)
{
    bar_->setUpdate(std::move(onClick));
}

void ClassicWindow::setHelpButton(bool on)
{
    bar_->setHelp(on);
}

void ClassicWindow::setTitleIcon(const QImage &icon)
{
    bar_->setIcon(icon);
}

void ClassicWindow::paintEvent(QPaintEvent *)
{
    // light-grey top / left and black bottom / right outside; white top / left and grey
    // bottom / right inside; then a pixel of face (in Black and White: the same all round)
    QPainter p(this);
    classic::Px x(p);
    QRect d = x.dev(rect());
    x.fill(d, theme::face());
    const int k = x.k;
    if (theme::mono()) {
        x.bevel(d, theme::face(), theme::face(), k);
        x.bevel(d.adjusted(k, k, -k, -k), theme::light(), theme::darkEdge(), k);
    } else {
        x.bevel(d, theme::face(), theme::darkEdge(), k);
        x.bevel(d.adjusted(k, k, -k, -k), theme::light(), theme::shadow(), k);
    }
}

void ClassicWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange)
        bar_->update();  // (grey while another window is in front)
    if (e->type() == QEvent::WindowStateChange) {  // (maximized by Windows too: Win+Up, Snap...)
        maximized_ = isMaximized();
        bar_->update();
    }
    QWidget::changeEvent(e);
}

void ClassicWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
#ifdef Q_OS_WIN
    if (!styled_) {
        styled_ = true;
        HWND h = HWND(winId());
        // a real window to Windows (its own title bar and border drawn by us, see WM_NCCALCSIZE):
        // a sizing border, a caption, minimize / maximize boxes and the system menu - so Windows
        // resizes it from its edges, moves and snaps it (dragged to a screen's edges, Win+arrows,
        // Windows 11's Snap Layouts over the maximize button) and maximizes it
        LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
        style &= ~LONG_PTR(WS_POPUP);
        style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (resizable_)
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        SetWindowLongPtrW(h, GWL_STYLE, style);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        const DWORD square = 1;  // DWMWCP_DONOTROUND: Windows 98's square corners
        DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &square, sizeof square);
    }
#endif
}

void ClassicWindow::minimizeWindow()
{
    showMinimized();
}

void ClassicWindow::toggleMaximize()
{
    if (!resizable_)
        return;
#ifdef Q_OS_WIN
    // (Windows' own maximize, as its maximize box does: its Snap and Win+arrows know it)
    HWND h = HWND(winId());
    SendMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
#else
    if (isMaximized())
        showNormal();
    else
        showMaximized();
#endif
    maximized_ = isMaximized();
    bar_->update();
}

void ClassicWindow::startMove(QPoint)
{
    // (the title bar's moved by Windows itself: it's the caption - see WM_NCHITTEST)
    if (windowHandle())
        windowHandle()->startSystemMove();
}

void ClassicWindow::snapAt(QPoint)
{
    // (Windows snaps it: dragged by its caption)
}

bool ClassicWindow::nativeEvent(const QByteArray &type, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    MSG *msg = static_cast<MSG *>(message);
    switch (msg->message) {
    case WM_NCCALCSIZE:
        // all of the window is ours to draw (no frame of Windows'); maximized, Windows puts the
        // sizing border off the screen's edges - kept in, so nothing's cut off
        if (msg->wParam) {
            auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            if (IsZoomed(msg->hwnd)) {
                const UINT dpi = GetDpiForWindow(msg->hwnd);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy;
                p->rgrc[0].bottom -= fy;
            }
            *result = 0;
            return true;
        }
        break;
    case WM_NCHITTEST: {
        // which part is where: the border and corners resize, the title bar's empty part is the
        // caption (moved, double-clicked, snapped by Windows), the [] button is the maximize box
        // (Windows 11 shows its Snap Layouts over it); the rest is ours
        const int x = GET_X_LPARAM(msg->lParam), y = GET_Y_LPARAM(msg->lParam);
        RECT wr;
        GetWindowRect(msg->hwnd, &wr);
        if (resizable_ && !IsZoomed(msg->hwnd)) {
            const double dpr = devicePixelRatioF();
            const int g = int(kGrip * dpr), c = int(kCorner * dpr);
            const bool l = x < wr.left + g, r = x >= wr.right - g;
            const bool t = y < wr.top + g, b = y >= wr.bottom - g;
            const bool nl = x < wr.left + c, nr = x >= wr.right - c;
            const bool nt = y < wr.top + c, nb = y >= wr.bottom - c;
            LRESULT hit = 0;
            if ((t && nl) || (l && nt))
                hit = HTTOPLEFT;
            else if ((t && nr) || (r && nt))
                hit = HTTOPRIGHT;
            else if ((b && nl) || (l && nb))
                hit = HTBOTTOMLEFT;
            else if ((b && nr) || (r && nb))
                hit = HTBOTTOMRIGHT;
            else if (l)
                hit = HTLEFT;
            else if (r)
                hit = HTRIGHT;
            else if (t)
                hit = HTTOP;
            else if (b)
                hit = HTBOTTOM;
            if (hit) {
                *result = hit;
                return true;
            }
        }
        POINT pt{x, y};
        ScreenToClient(msg->hwnd, &pt);
        const QPoint at = bar_->mapFrom(this, QPointF(pt.x, pt.y).toPoint() / devicePixelRatioF());
        if (bar_->rect().contains(at)) {
            const QString button = bar_->buttonAt(at);
            *result = button.isEmpty() ? HTCAPTION
                      : (button == QLatin1String("max") || button == QLatin1String("restore")) ? HTMAXBUTTON : HTCLIENT;
            return true;
        }
        *result = HTCLIENT;
        return true;
    }
    // the maximize box is drawn and pressed here (Windows would draw its own over ours)
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (msg->wParam == HTMAXBUTTON) {
            bar_->pressMax(true);
            *result = 0;
            return true;
        }
        break;
    case WM_NCLBUTTONUP:
        if (msg->wParam == HTMAXBUTTON) {
            const bool was = bar_->maxPressed();
            bar_->pressMax(false);
            if (was)  // (after Windows' message: asked for inside it, the maximize was lost)
                QTimer::singleShot(0, this, [this] { toggleMaximize(); });
            *result = 0;
            return true;
        }
        break;
    case WM_NCMOUSEMOVE:
        if (msg->wParam != HTMAXBUTTON && bar_->maxPressed())
            bar_->pressMax(false);
        break;
    case WM_NCMOUSELEAVE:
        if (bar_->maxPressed())
            bar_->pressMax(false);
        break;
    default:
        break;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}

// ---- the title bar ----

TitleBar::TitleBar(ClassicWindow *window) : QWidget(window), win_(window)
{
    setFixedHeight(H);
}

std::vector<std::pair<QString, QRect>> TitleBar::buttons() const
{
    // from the right: X, [] and _ side by side, X 3 px from the edge; ? 2 px left of them
    const int y = (H - BTN_H) / 2;  // (a pixel taller than they were, at the top: their bottoms where they were)
    int x = width() - 3 - BTN_W;
    std::vector<std::pair<QString, QRect>> out;
    out.emplace_back(QStringLiteral("close"), QRect(x, y, BTN_W, BTN_H));
    if (win_->resizable_) {
        x -= BTN_W;
        out.emplace_back(win_->maximized_ ? QStringLiteral("restore") : QStringLiteral("max"), QRect(x, y, BTN_W, BTN_H));
        x -= BTN_W;
        out.emplace_back(QStringLiteral("min"), QRect(x, y, BTN_W, BTN_H));
    }
    if (update_) {  // (its own spot, 8 px apart, just left of _ - as the Python app's)
        x -= BTN_W + 8;
        out.emplace_back(QStringLiteral("update"), QRect(x, y, BTN_W, BTN_H));
    }
    if (help_) {
        x -= BTN_W + 2;
        out.emplace_back(QStringLiteral("help"), QRect(x, y, BTN_W, BTN_H));
    }
    return out;
}

QString TitleBar::buttonAt(QPoint p) const
{
    for (const auto &[kind, r] : buttons())
        if (r.contains(p))
            return kind;
    return {};
}

void TitleBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool active = win_->isActiveWindow() || win_->shownActive_;
    const auto [a, b] = theme::caption(active);
    QLinearGradient g(0, 0, width(), 0);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    p.fillRect(rect(), g);
    int x = 6;
    if (!icon_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(x, (H - 16) / 2.0, 16, 16), icon_);
        x += 16 + 5;
    }
    p.setFont(theme::font(10, true));  // (bold, as Windows 98's title bars - active or not)
    p.setPen(theme::captionText(active));
    const auto all = buttons();
    const int textEnd = all.empty() ? width() : all.back().second.left() - 4;
    classic::crispText(p, QRect(x, 0, std::max(0, textEnd - x), H), Qt::AlignLeft | Qt::AlignVCenter, win_->title(), nullptr,
                       Qt::ElideRight);
    for (const auto &[kind, r] : all)
        drawButton(p, kind, r, pressed_ == kind && down_);
}

void TitleBar::drawButton(QPainter &p, const QString &kind, const QRect &r, bool down)
{
    classic::smallButton(p, r, down);
    // the symbols drawn pixel by pixel for a 21 x 19 button, like the Python app's (the button a pixel
    // taller at the top: they're where they were)
    const int o = down ? 1 : 0;
    const QPointF at(r.left() + o, r.top() + 1 + o);
    const QColor ink = theme::ink();
    auto px = [&](int x0, int y0, int x1, int y1) { classic::block(p, at, x0, y0, x1, y1, ink); };
    if (kind == "min") {
        px(6, 12, 14, 14);
    } else if (kind == "max") {
        px(5, 4, 15, 6);
        px(5, 6, 6, 14);
        px(14, 6, 15, 14);
        px(5, 13, 15, 14);
    } else if (kind == "restore") {
        px(8, 3, 16, 5);
        px(15, 5, 16, 11);
        px(13, 10, 15, 11);
        px(8, 5, 9, 7);
        px(4, 7, 13, 9);
        px(4, 9, 5, 14);
        px(12, 9, 13, 14);
        px(4, 13, 13, 14);
    } else if (kind == "help") {
        px(7, 3, 13, 5);
        {  // (its left end: as wide as the right side, in screen pixels - scaled on its own, 125% made it a pixel wider)
            classic::Px d(p);
            QRect end = d.dev(QRectF(at.x() + 6, at.y() + 4, 2, 3));
            end.setLeft(end.right() + 1 - d.dev(QRectF(at.x() + 12, at.y() + 4, 2, 4)).width());
            d.fill(end, ink);
        }
        px(12, 4, 14, 8);
        px(10, 7, 13, 9);
        px(9, 8, 11, 11);
        px(9, 12, 11, 14);
    } else if (kind == "update") {  // the Python app's: an arrow down onto a line, its strokes the X's (2 px) - drawn
                                    // in screen pixels as the X (in blocks scaled one by one, 125% made its steps ragged)
        classic::Px d(p);
        d.fill(d.dev(QRectF(at.x() + 6, at.y() + 12, 8, 2)).adjusted(0, 0, 1, 0), ink);  // (the line: _'s, a pixel wider)
        const QPoint o = d.dev(QRectF(at, QSizeF(20, 18))).topLeft();
        auto s = [&](double v) { return int(std::lround(v * d.dpr)); };
        const int t = std::max(2, s(2)), steps = std::max(5, s(5));  // (the strokes' thickness; the head's rows)
        const int mid = o.x() + s(10) - std::max(1, s(1)), head = o.y() + s(6);  // (between the shaft's two middle pixels -
                                                                                  // a pixel left of the button's; the head's top)
        d.fill(QRect(mid - t / 2, o.y() + s(2), t, head + steps - 1 - (o.y() + s(2))), ink);  // (the shaft, ending in the head)
        for (int i = 0; i < steps; ++i) {
            d.fill(QRect(mid - t / 2 - (steps - 1 - i), head + i, t, 1), ink);
            d.fill(QRect(mid - t / 2 + (steps - 1 - i), head + i, t, 1), ink);
        }    } else {  // close: Windows 98's X - square, its strokes two pixels thick, in screen pixels (drawn in
              // blocks scaled one by one, 125% made it ragged)
        classic::Px x(p);
        const QRect box = x.dev(QRectF(at.x() + 5, at.y() + 4, 10, 10));
        const int side = std::max(9, int(std::lround(9 * x.dpr))) | 1;  // (odd: a middle pixel where they cross)
        const int thick = std::max(2, int(std::lround(2 * x.dpr)));  // (as heavy as the other symbols)
        // (a pixel right of the middle)
        const int left = box.left() + (box.width() - side) / 2 + std::max(1, int(std::lround(x.dpr))), top = box.top() + (box.height() - side) / 2;
        for (int i = 0; i < side; ++i) {
            x.fill(QRect(left + i - (thick - 1) / 2, top + i, thick, 1), ink);
            x.fill(QRect(left + side - 1 - i - thick / 2, top + i, thick, 1), ink);
        }
    }
}

void TitleBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const QString kind = buttonAt(e->position().toPoint());
    if (!kind.isEmpty()) {
        pressed_ = kind;
        down_ = true;
        update();
        return;
    }
    barPress_ = true;
    pressAt_ = e->globalPosition().toPoint();
}

void TitleBar::mouseMoveEvent(QMouseEvent *e)
{
    if (!pressed_.isEmpty()) {
        const bool in = buttonAt(e->position().toPoint()) == pressed_;
        if (in != down_) {
            down_ = in;
            update();
        }
        return;
    }
    if (barPress_ && (e->globalPosition().toPoint() - pressAt_).manhattanLength() > 3) {
        barPress_ = false;
        win_->startMove(pressAt_);
    }
}

void TitleBar::mouseReleaseEvent(QMouseEvent *e)
{
    barPress_ = false;
    if (pressed_.isEmpty() || e->button() != Qt::LeftButton)
        return;
    const QString kind = pressed_;
    const bool go = down_;
    pressed_.clear();
    down_ = false;
    update();
    if (!go)
        return;
    if (kind == "close")
        win_->close();
    else if (kind == "max" || kind == "restore")
        win_->toggleMaximize();
    else if (kind == "min")
        win_->minimizeWindow();
    else if (kind == "help")
        emit win_->helpClicked();
    else if (kind == "update" && update_) {
        const auto clicked = update_;  // (a copy: the box it opens may take the button away)
        clicked();
    }
}

bool TitleBar::event(QEvent *e)
{
    // asking What's This?: a click on the title bar - ? again, X... - out of help mode, and the click does
    // what it does (as the Python app's)
    if (e->type() == QEvent::WhatsThis) {
        const QString kind = buttonAt(static_cast<QHelpEvent *>(e)->pos());
        QWhatsThis::leaveWhatsThisMode();
        if (kind == "close")
            win_->close();
        else if (kind == "max" || kind == "restore")
            win_->toggleMaximize();
        else if (kind == "min")
            win_->minimizeWindow();
        return true;
    }
    return QWidget::event(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && buttonAt(e->position().toPoint()).isEmpty())
        win_->toggleMaximize();
}
