#include "view/ThumbnailBar.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kGap = 36;  // between the pages (room for the +)
constexpr int kNumberH = 18;
constexpr int kPlusW = 17, kPlusH = 15;
}  // namespace

ThumbnailBar::ThumbnailBar(QWidget *parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    verticalScrollBar()->setSingleStep(20);
    pool_.setMaxThreadCount(1);  // (the pages themselves come first)
    slideTimer_ = new QTimer(this);
    slideTimer_->setInterval(16);
    connect(slideTimer_, &QTimer::timeout, this, &ThumbnailBar::slide);
    setFocusPolicy(Qt::ClickFocus);
    connect(theme::Notifier::instance(), &theme::Notifier::changed, viewport(),
            [this] { viewport()->update(); });
}

ThumbnailBar::~ThumbnailBar()
{
    pool_.clear();
    pool_.waitForDone();
}

void ThumbnailBar::setDocument(std::shared_ptr<Document> doc)
{
    pool_.clear();
    doc_ = std::move(doc);
    ++serial_;
    const size_t n = doc_ ? size_t(doc_->pageCount()) : 0;
    images_.assign(n, QImage());
    drawnSize_.assign(n, QSize());
    pending_.clear();
    current_ = doc_ && n ? 0 : -1;
    plusDown_ = -1;
    selected_.clear();
    lastClick_ = -1;
    drag_.reset();
    layoutCells();
    verticalScrollBar()->setValue(0);
    viewport()->update();
}

int ThumbnailBar::thumbWidth() const
{
    // (the Python app's: the panel's width less 58 - its scrollbar, edges and margins)
    return std::max(40, viewport()->width() - 37);
}

void ThumbnailBar::layoutCells()
{
    cells_.clear();
    const int W = std::max(60, viewport()->width());
    int y = kGap;
    if (doc_) {
        const double t = thumbWidth();
        for (int i = 0; i < doc_->pageCount(); ++i) {
            const QSizeF s = doc_->pageSize(i);
            const double k = std::min(t / std::max(1.0, s.width()), t * 1.3 / std::max(1.0, s.height()));
            const int tw = int(std::lround(s.width() * k)), th = int(std::lround(s.height() * k));
            cells_.emplace_back((W - tw) / 2, y, std::max(1, tw), std::max(1, th));
            y += th + kNumberH + kGap;
        }
    }
    contentH_ = y;
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, std::max(0, y - viewport()->height()));
}

QRect ThumbnailBar::cellRect(int page) const
{
    if (page < 0 || page >= int(cells_.size()))
        return {};
    return cells_[size_t(page)].translated(0, -verticalScrollBar()->value());
}

int ThumbnailBar::plusY(int n) const
{
    // just as far from the page number above as from the page frame below
    const QFontMetrics f(theme::font());
    const double underNumber = 10 - f.height() / 2.0 + f.ascent();
    const double half = (kNumberH + kGap - 3 - underNumber) / 2;
    if (n < int(cells_.size()))
        return int(std::lround(cells_[size_t(n)].top() - 3 - half));
    const QRect &last = cells_.back();
    return int(std::lround(last.top() + last.height() + underNumber + half));
}

QRect ThumbnailBar::plusRect(int n) const
{
    if (cells_.empty())
        return {};
    const int W = std::max(60, viewport()->width());
    return QRect((W - kPlusW) / 2, plusY(n) - kPlusH / 2 - verticalScrollBar()->value(), kPlusW, kPlusH);
}

void ThumbnailBar::setCurrent(int page)
{
    if (page == current_ || page < 0 || page >= int(cells_.size()))
        return;
    current_ = page;
    const QRect r = cells_[size_t(page)];
    const int top = verticalScrollBar()->value();
    const int vh = viewport()->height();
    if (r.top() - 4 < top || r.bottom() + kNumberH > top + vh)
        verticalScrollBar()->setValue(std::max(0, r.top() - kGap));
    viewport()->update();
}

void ThumbnailBar::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), theme::box());
    if (!doc_)
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();
    const int W = std::max(60, viewport()->width());
    const double dpr = devicePixelRatioF();
    p.setFont(theme::font());
    if (drag_) {
        paintDrag(p);
        return;
    }
    const auto [selBg, selText] = theme::selection();
    for (int i = 0; i < int(cells_.size()); ++i) {
        const QRect c = cells_[size_t(i)];
        if (c.bottom() + kNumberH + kGap < top)
            continue;
        if (c.top() - kGap > bottom)
            break;
        const QRect r = c.translated(0, -top);
        // its frame, 2 px out: a selected page's in the selection colour, the current page's
        // blue - both 2 px thick
        const bool sel = selected_.count(i) > 0;
        const bool cur = sel || i == current_;
        {
            classic::Px x(p);
            const int n = cur ? 2 * x.k : x.k;
            const QColor color = sel ? selBg : i == current_ ? theme::c("#7DA2CE", theme::Edge)
                                                              : theme::c("#B0B0B0", theme::Edge);
            const QRect d = x.dev(QRectF(r).adjusted(-3, -3, 3, 3));
            x.bevel(cur ? d : d.adjusted(x.k, x.k, -x.k, -x.k), color, color, n);
        }
        p.fillRect(r, QColor("#FFFFFE"));
        const QImage &im = images_[size_t(i)];
        if (!im.isNull())
            p.drawImage(QRectF(r), im);
        const QSize want((r.size() * dpr));
        if (im.isNull() || drawnSize_[size_t(i)] != want)
            request(i, want);
        const QString number = QString::number(i + 1);
        const QRect numberBox(r.left() - 40, r.bottom() + 1 + 10 - kNumberH / 2, r.width() + 80, kNumberH);
        if (sel) {  // (a selected page's number on the selection colour)
            const int tw = p.fontMetrics().horizontalAdvance(number);
            const int th = p.fontMetrics().height();
            p.fillRect(QRect(numberBox.center().x() - tw / 2 - 3, numberBox.center().y() - th / 2, tw + 6, th), selBg);
        }
        p.setPen(sel ? selText : theme::text());
        classic::crispText(p, numberBox, Qt::AlignHCenter | Qt::AlignVCenter, number);
    }
    // the + buttons, each on an etched line across the gap
    for (int n = 0; n <= int(cells_.size()); ++n) {
        const QRect b = plusRect(n);
        if (b.bottom() < -4 || b.top() > viewport()->height() + 4)
            continue;
        const int my = b.top() + kPlusH / 2;
        classic::etchedH(p, 8, b.left() - 3, my);
        classic::etchedH(p, b.right() + 1 + 3, W - 8, my);
        const bool pressed = plusDown_ == n;
        classic::smallButton(p, b, pressed);
        const int o = pressed ? 1 : 0;
        const QPointF centre(b.left() + kPlusW / 2 + o, b.top() + kPlusH / 2 + o);
        classic::block(p, centre, -3, 0, 4, 1, theme::ink());
        classic::block(p, centre, 0, -3, 1, 4, theme::ink());
    }
}

void ThumbnailBar::request(int page, QSize size)
{
    if (pending_.contains(page))
        return;
    pending_.insert(page);
    auto doc = doc_;
    QPointer<ThumbnailBar> self(this);
    const int serial = serial_;
    const double scale = size.width() / std::max(1.0, doc->pageSize(page).width());
    const QRect area(QPoint(0, 0), size);
    pool_.start([=] {
        QImage image = doc->render(page, scale, area);
        QMetaObject::invokeMethod(
            qApp, [self, page, serial, size, image]() {
                if (!self || serial != self->serial_)
                    return;
                self->pending_.remove(page);
                if (!image.isNull()) {
                    self->images_[size_t(page)] = image;
                    self->drawnSize_[size_t(page)] = size;
                }
                self->viewport()->update();
            },
            Qt::QueuedConnection);
    });
}

void ThumbnailBar::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    layoutCells();
}

void ThumbnailBar::scrollContentsBy(int, int)
{
    // (the thumbnails scrolled out of sight before their turn: not drawn)
    pool_.clear();
    pending_.clear();
    viewport()->update();
}

void ThumbnailBar::plusClosed()
{
    plusDown_ = -1;
    viewport()->update();
}

int ThumbnailBar::cellAt(QPoint at) const
{
    const int cy = at.y() + verticalScrollBar()->value();
    for (int i = 0; i < int(cells_.size()); ++i) {
        const QRect &c = cells_[size_t(i)];
        if (c.top() - kGap / 2 <= cy && cy <= c.bottom() + kNumberH + kGap / 2)
            return i;
    }
    return -1;
}

std::vector<int> ThumbnailBar::selection() const
{
    if (!selected_.empty())
        return {selected_.begin(), selected_.end()};
    if (current_ >= 0)
        return {current_};
    return {};
}

void ThumbnailBar::select(const std::vector<int> &pages)
{
    selected_.clear();
    for (int p : pages)
        if (p >= 0 && p < int(cells_.size()))
            selected_.insert(p);
    viewport()->update();
}

void ThumbnailBar::documentChanged(bool keep)
{
    pool_.clear();
    ++serial_;
    const size_t n = doc_ ? size_t(doc_->pageCount()) : 0;
    if (!keep || images_.size() != n)  // (the same pages: shown until drawn again)
        images_.assign(n, QImage());
    const int only = keep && doc_ && images_.size() == n && drawnSize_.size() == n ? doc_->lastChangedPage() : -1;
    if (only >= 0 && only < int(n))  // (one page changed: just its picture drawn again)
        drawnSize_[size_t(only)] = QSize();
    else
        drawnSize_.assign(n, QSize());
    pending_.clear();
    drag_.reset();
    for (auto it = selected_.begin(); it != selected_.end();)
        it = *it >= int(n) ? selected_.erase(it) : std::next(it);
    current_ = std::min(current_, int(n) - 1);
    layoutCells();
    viewport()->update();
}

void ThumbnailBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint at = e->position().toPoint();
    for (int n = 0; n <= int(cells_.size()) && !cells_.empty(); ++n) {
        if (plusRect(n).adjusted(-2, -2, 2, 2).contains(at)) {
            plusDown_ = n;
            viewport()->update();
            const QRect b = plusRect(n);
            emit plusClicked(n, viewport()->mapToGlobal(QPoint(b.left() + 2, b.bottom() + 1 + 4)));
            return;
        }
    }
    const int i = cellAt(at);
    if (i < 0)
        return;
    if (e->modifiers() & Qt::ControlModifier) {  // Ctrl: add / take away
        if (!selected_.erase(i))
            selected_.insert(i);
    } else if ((e->modifiers() & Qt::ShiftModifier) && lastClick_ >= 0) {  // Shift: a range
        selected_.clear();
        for (int q = std::min(lastClick_, i); q <= std::max(lastClick_, i); ++q)
            selected_.insert(q);
    } else {
        if (!selected_.count(i))
            selected_ = {i};
        pressPage_ = i;  // (a drag from here moves the selected pages)
        pressPos_ = at + QPoint(0, verticalScrollBar()->value());
    }
    lastClick_ = i;
    setCurrent(i);
    viewport()->update();
    emit pageClicked(i);
}

void ThumbnailBar::mouseMoveEvent(QMouseEvent *e)
{
    if (pressPage_ < 0 || !(e->buttons() & Qt::LeftButton))
        return;
    const QPoint at = e->position().toPoint();
    if (at.y() < 0)  // near the ends: the list scrolls along
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 20);
    else if (at.y() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 20);
    const QPoint c = at + QPoint(0, verticalScrollBar()->value());
    if (!drag_) {
        if (std::abs(c.y() - pressPos_.y()) < 6 && std::abs(c.x() - pressPos_.x()) < 6)
            return;  // (still a click, not a drag)
        startDrag();
    }
    drag_->y = c.y();
    // where they'd go: before the first of the other pages whose middle is below the mouse,
    // as they're shown now (so the gap opened under the mouse holds steady)
    int k = 0;
    for (int q : drag_->rest)
        if (drag_->shown[size_t(q)] + cells_[size_t(q)].height() / 2.0 < c.y())
            ++k;
    if (k != drag_->k) {
        drag_->k = k;
        drag_->from = drag_->shown;
        std::vector<int> order = dragOrder();
        drag_->to.assign(cells_.size(), 0);
        double y = kGap;
        for (int q : order) {
            drag_->to[size_t(q)] = y;
            y += cells_[size_t(q)].height() + kNumberH + kGap;
        }
        drag_->frame = 0;
        slideTimer_->start();
    }
    viewport()->update();
}

std::vector<int> ThumbnailBar::dragOrder() const
{
    std::vector<int> order(drag_->rest.begin(), drag_->rest.begin() + drag_->k);
    order.insert(order.end(), drag_->moving.begin(), drag_->moving.end());
    order.insert(order.end(), drag_->rest.begin() + drag_->k, drag_->rest.end());
    return order;
}

void ThumbnailBar::startDrag()
{
    auto d = std::make_unique<Drag>();
    d->moving.assign(selected_.begin(), selected_.end());
    for (int q = 0; q < int(cells_.size()); ++q)
        if (!selected_.count(q))
            d->rest.push_back(q);
    d->k = int(std::count_if(d->rest.begin(), d->rest.end(), [&](int q) { return q < d->moving.front(); }));
    const QRect cell = cells_[size_t(pressPage_)];
    d->grab = pressPos_ - cell.topLeft();
    for (const QRect &c : cells_)
        d->shown.push_back(c.top());
    d->to = d->shown;
    // the copy under the mouse: the page's picture, see-through, with a frame round it (and
    // with several, how many in a little badge)
    const double dpr = devicePixelRatioF();
    QImage ghost(cell.size() * dpr, QImage::Format_ARGB32_Premultiplied);
    ghost.fill(Qt::white);
    QPainter g(&ghost);
    if (!images_[size_t(pressPage_)].isNull())
        g.drawImage(ghost.rect(), images_[size_t(pressPage_)]);
    g.setPen(QPen(Qt::black, 1));
    g.drawRect(ghost.rect().adjusted(0, 0, -1, -1));
    if (d->moving.size() > 1) {
        const auto [bg, fg] = theme::selection();
        g.scale(dpr, dpr);
        g.fillRect(QRect(cell.width() - 22, 2, 20, 15), bg);
        g.setPen(fg);
        g.setFont(theme::font());
        g.drawText(QRect(cell.width() - 22, 2, 20, 15), Qt::AlignCenter, QString::number(d->moving.size()));
    }
    g.end();
    ghost.setDevicePixelRatio(dpr);
    d->ghost = ghost;
    drag_ = std::move(d);
}

void ThumbnailBar::slide()
{
    // the pages slide to their new places over a few frames (about 0.1 s)
    if (!drag_) {
        slideTimer_->stop();
        return;
    }
    constexpr int steps = 6;
    ++drag_->frame;
    for (size_t q = 0; q < drag_->shown.size(); ++q)
        drag_->shown[q] = drag_->from[q] + (drag_->to[q] - drag_->from[q]) * drag_->frame / steps;
    if (drag_->frame >= steps)
        slideTimer_->stop();
    viewport()->update();
}

void ThumbnailBar::paintDrag(QPainter &p)
{
    // every page where it's going; the ones being moved as dotted gaps; the copy under the mouse
    const int top = verticalScrollBar()->value();
    const auto [selBg, selText] = theme::selection();
    const std::vector<int> order = dragOrder();
    std::vector<int> number(cells_.size());
    for (size_t n = 0; n < order.size(); ++n)
        number[size_t(order[n])] = int(n) + 1;
    for (int q = 0; q < int(cells_.size()); ++q) {
        const QRect c = cells_[size_t(q)];
        const QRect r(c.left(), int(std::lround(drag_->shown[size_t(q)])) - top, c.width(), c.height());
        if (r.bottom() + kNumberH < 0 || r.top() > viewport()->height())
            continue;
        const bool moving = std::count(drag_->moving.begin(), drag_->moving.end(), q) > 0;
        if (moving) {
            QPen pen(selBg, 1, Qt::CustomDashLine);
            pen.setDashPattern({2, 2});
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(-2, -2, 2, 2));
        } else {
            classic::Px x(p);
            const QColor line = theme::c("#B0B0B0", theme::Edge);
            x.bevel(x.dev(QRectF(r).adjusted(-2, -2, 2, 2)), line, line, x.k);
        }
        if (!moving) {
            p.fillRect(r, QColor("#FFFFFE"));
            if (!images_[size_t(q)].isNull())
                p.drawImage(QRectF(r), images_[size_t(q)]);
        }
        p.setPen(theme::text());
        classic::crispText(p, QRect(r.left() - 40, r.bottom() + 1 + 10 - kNumberH / 2, r.width() + 80, kNumberH),
                           Qt::AlignHCenter | Qt::AlignVCenter, QString::number(number[size_t(q)]));
    }
    p.setOpacity(150 / 255.0);
    p.drawImage(QPointF(cells_[size_t(pressPage_)].left(), drag_->y - drag_->grab.y() - top), drag_->ghost);
    p.setOpacity(1);
}

void ThumbnailBar::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int pressed = pressPage_;
    pressPage_ = -1;
    if (drag_) {  // dropped: the pages go where the gap was
        std::unique_ptr<Drag> d = std::move(drag_);
        slideTimer_->stop();
        std::vector<int> order(d->rest.begin(), d->rest.begin() + d->k);
        order.insert(order.end(), d->moving.begin(), d->moving.end());
        order.insert(order.end(), d->rest.begin() + d->k, d->rest.end());
        bool same = true;
        for (size_t n = 0; n < order.size(); ++n)
            same = same && order[n] == int(n);
        viewport()->update();
        if (!same)
            emit movePages(d->moving, d->k < int(d->rest.size()) ? d->rest[size_t(d->k)] : int(cells_.size()));
        return;
    }
    if (pressed >= 0 && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        selected_ = {pressed};  // (a plain click: just this page)
        viewport()->update();
    }
}

void ThumbnailBar::contextMenuEvent(QContextMenuEvent *e)
{
    const int i = cellAt(e->pos());
    if (i < 0)
        return;
    if (!selected_.count(i)) {
        selected_ = {i};
        lastClick_ = i;
        setCurrent(i);
        emit pageClicked(i);
    }
    viewport()->update();
    emit menuRequested(e->globalPos());
}

void ThumbnailBar::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Delete) && !selected_.empty()) {
        emit deleteRequested();
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}
