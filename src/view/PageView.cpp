#include "view/PageView.h"

#include <QTimer>

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QTextEdit>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kGap = 14;          // between pages, and round them (the Python app's)
constexpr int kTile = 512;        // a tile's side, in screen pixels
constexpr int kPreviewWidth = 300;  // the small picture of a whole page, in pixels
constexpr int kMaxTiles = 220;    // kept in memory (about 170 MB at most)
constexpr int kMaxPreviews = 80;
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 8.0;
}  // namespace

PageView::PageView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    viewport()->setMouseTracking(true);  // (an I-beam over text)
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    pool_.setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 1));
}

PageView::~PageView()
{
    {
        std::lock_guard guard(wanted_->mutex);
        wanted_->keys.clear();  // (whatever is waiting is skipped)
    }
    pool_.clear();
    pool_.waitForDone();
}

quint64 PageView::tileKey(int page, int gen, int pageGen, int tx, int ty)
{
    return (quint64(quint32(page) & 0xFFFFF) << 44) | (quint64(quint32(gen) & 0xFFF) << 32) |
           (quint64(quint32(pageGen) & 0xFF) << 24) | (quint64(quint32(tx) & 0xFFF) << 12) | quint64(quint32(ty) & 0xFFF);
}

double PageView::pixelsPerPoint() const
{
    return zoom_ * 96.0 / 72.0;  // (100%: a point the size it is on paper)
}

QPoint PageView::scrollOffset() const
{
    return {horizontalScrollBar()->value(), verticalScrollBar()->value()};
}

QRect PageView::pageRect(int page) const
{
    return pages_[size_t(page)];
}

void PageView::setDocument(std::shared_ptr<Document> doc)
{
    clearGroup();
    {
        std::lock_guard guard(wanted_->mutex);
        wanted_->keys.clear();
    }
    pool_.clear();
    doc_ = std::move(doc);
    ++serial_;
    ++gen_;
    tiles_.clear();
    pending_.clear();
    previewPending_.clear();
    previews_.assign(doc_ ? size_t(doc_->pageCount()) : 0, QImage());
    previewStale_.assign(previews_.size(), false);
    pickPage_ = -1;
    pickId_ = 0;
    linePage_ = lineIndex_ = hoverPage_ = hoverIndex_ = -1;
    linesPage_ = -1;
    picPage_ = picIndex_ = picsPage_ = -1;
    press_ = Press::None;
    lastPage_ = -1;
    selPage_ = -1;
    selBoxes_.clear();
    selText_.clear();
    pressPage_ = -1;
    hits_.clear();
    hitNow_ = -1;
    emit selectionChanged(false);
    layoutPages();
    horizontalScrollBar()->setValue(0);
    verticalScrollBar()->setValue(0);
    if (doc_)
        fitWidth();
    viewport()->update();
}

void PageView::documentChanged(bool keep)
{
    if (!doc_)
        return;
    clearGroup();  // (what's picked together: found again by whoever changed it - the page's things may be others now)
    {
        std::lock_guard guard(wanted_->mutex);
        wanted_->keys.clear();
    }
    pool_.clear();
    ++serial_;  // (what's still being drawn from before the change: not shown)
    pending_.clear();
    previewPending_.clear();
    // (one page changed - an edit, its Undo / Redo: just it drawn again, the others as they are)
    const int only = keep && previews_.size() == size_t(doc_->pageCount()) ? doc_->lastChangedPage() : -1;
    if (pageGen_.size() != size_t(doc_->pageCount()))
        pageGen_.assign(size_t(doc_->pageCount()), 0);
    if (only >= 0 && only < int(previews_.size())) {
        ++pageGen_[size_t(only)];
        previewStale_.assign(previews_.size(), false);
        previewStale_[size_t(only)] = true;
    } else if (++gen_, keep && previews_.size() == size_t(doc_->pageCount())) {
        // the same pages: the old pictures shown (stretched, under the new) until redrawn
        previewStale_.assign(previews_.size(), true);
    } else {
        tiles_.clear();
        previews_.assign(size_t(doc_->pageCount()), QImage());
        previewStale_.assign(previews_.size(), false);
    }
    pickPage_ = -1;
    pickId_ = 0;
    selPage_ = -1;
    selBoxes_.clear();
    selText_.clear();
    pressPage_ = -1;
    hits_.clear();
    hitNow_ = -1;
    emit selectionChanged(false);
    const int y = verticalScrollBar()->value();
    layoutPages();
    if (fit_ == Fit::Width)
        fitWidth();
    else if (fit_ == Fit::Page)
        fitPage();
    verticalScrollBar()->setValue(y);
    lastPage_ = -1;
    viewport()->update();
}

void PageView::layoutPages()
{
    pages_.clear();
    const int vw = viewport()->width();
    const int vh = viewport()->height();
    if (!doc_) {
        contentSize_ = QSize();
        horizontalScrollBar()->setRange(0, 0);
        verticalScrollBar()->setRange(0, 0);
        return;
    }
    const double ppp = pixelsPerPoint();
    int widest = 0;
    std::vector<QSize> sizes;
    sizes.reserve(size_t(doc_->pageCount()));
    for (int i = 0; i < doc_->pageCount(); ++i) {
        const QSizeF s = doc_->pageSize(i);
        sizes.emplace_back(std::max(1, int(std::lround(s.width() * ppp))),
                           std::max(1, int(std::lround(s.height() * ppp))));
        widest = std::max(widest, sizes.back().width());
    }
    const int width = std::max(vw, widest + 2 * kGap);
    int y = kGap;
    for (const QSize &s : sizes) {
        pages_.emplace_back((width - s.width()) / 2, y, s.width(), s.height());
        y += s.height() + kGap;
    }
    contentSize_ = QSize(width, y);
    horizontalScrollBar()->setPageStep(vw);
    horizontalScrollBar()->setRange(0, std::max(0, width - vw));
    verticalScrollBar()->setPageStep(vh);
    verticalScrollBar()->setRange(0, std::max(0, y - vh));
    placeBar();
}

void PageView::fitPage()
{
    if (!doc_ || doc_->pageCount() == 0)
        return;
    const QSizeF s = doc_->pageSize(std::max(0, currentPage()));
    const double vw = std::max(50, viewport()->width() - 2 * kGap);
    const double vh = std::max(50, viewport()->height() - 2 * kGap);
    const int page = std::max(0, currentPage());
    setZoom(std::min(vw / s.width(), vh / s.height()) / (96.0 / 72.0), QPoint(0, 0));
    goToPage(page);
    fit_ = Fit::Page;
    emit zoomChanged(zoom_);
}

void PageView::setZoom(double zoom, QPoint anchor)
{
    fit_ = Fit::None;
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (!doc_ || std::abs(zoom - zoom_) < 1e-4) {
        zoom_ = zoom;
        return;
    }
    if (anchor.x() < 0)
        anchor = viewport()->rect().center();
    // The point of the page under the anchor stays under it
    const QPoint c = anchor + scrollOffset();
    int page = 0;
    for (int i = 0; i < int(pages_.size()); ++i) {
        if (pages_[size_t(i)].bottom() + kGap >= c.y()) {
            page = i;
            break;
        }
        page = i;
    }
    double fx = 0.5, fy = 0;
    if (!pages_.empty()) {
        const QRect r = pages_[size_t(page)];
        fx = double(c.x() - r.left()) / r.width();
        fy = double(c.y() - r.top()) / r.height();
    }
    zoom_ = zoom;
    ++gen_;
    layoutPages();
    if (!pages_.empty()) {
        const QRect r = pages_[size_t(page)];
        horizontalScrollBar()->setValue(int(std::lround(r.left() + fx * r.width())) - anchor.x());
        verticalScrollBar()->setValue(int(std::lround(r.top() + fy * r.height())) - anchor.y());
    }
    rescaleEditor();  // (typing: the box over its text at the new zoom)
    viewport()->update();
    emit zoomChanged(zoom_);
}

void PageView::zoomBy(double factor, QPoint anchor)
{
    setZoom(zoom_ * factor, anchor);
}

void PageView::fitWidth()
{
    if (!doc_ || doc_->pageCount() == 0)
        return;
    double widest = 0;
    for (int i = 0; i < doc_->pageCount(); ++i)
        widest = std::max(widest, doc_->pageSize(i).width());
    const int room = std::max(50, viewport()->width() - 2 * kGap);
    const Fit keep = Fit::Width;
    setZoom(room / (widest * 96.0 / 72.0), QPoint(0, 0));
    fit_ = keep;
    emit zoomChanged(zoom_);
}

int PageView::currentPage() const
{
    if (pages_.empty())
        return -1;
    const int y = verticalScrollBar()->value() + viewport()->height() / 3;
    for (int i = 0; i < int(pages_.size()); ++i)
        if (pages_[size_t(i)].bottom() + kGap >= y)
            return i;
    return int(pages_.size()) - 1;
}

void PageView::goToPage(int page)
{
    if (page < 0 || page >= int(pages_.size()))
        return;
    verticalScrollBar()->setValue(pages_[size_t(page)].top() - kGap);
}

void PageView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), theme::workspace());
    if (!doc_)
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPoint off = scrollOffset();
    const QRect visible(off, viewport()->size());
    const double dpr = devicePixelRatioF();
    const double ppp = pixelsPerPoint();
    const double scale = ppp * dpr;  // device pixels per point, for the tiles drawn now
    QSet<quint64> wanted;
    for (int i = 0; i < int(pages_.size()); ++i) {
        const QRect r = pages_[size_t(i)];
        if (r.top() > visible.bottom())
            break;
        if (!r.intersects(visible))
            continue;
        const QRect sr = r.translated(-off);
        // a shadow down and right, the paper, and a black line round it
        p.fillRect(sr.translated(3, 3), theme::pageShadow());
        p.fillRect(sr, Qt::white);
        {
            classic::Px x(p);
            const QRect d = x.dev(sr);
            x.bevel(d.adjusted(-x.k, -x.k, x.k, x.k), Qt::black, Qt::black, x.k);
        }
        if (!previews_[size_t(i)].isNull())
            p.drawImage(QRectF(sr), previews_[size_t(i)]);
        if (previews_[size_t(i)].isNull() || (i < int(previewStale_.size()) && previewStale_[size_t(i)]))
            requestPreview(i);
        // tiles drawn at other zooms (stretched) first, then this zoom's over them - the old ones the
        // oldest first: the page as it was before a change (a drawing just added) under it as it is
        // now (in the hash's order, one drawn over the other by chance: the drawing came and went)
        std::vector<Tile *> old, now;
        for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
            Tile &t = it.value();
            if (t.page == i)
                (t.gen == gen_ && t.pageGen == pageGen(i) ? now : old).push_back(&t);
        }
        std::sort(old.begin(), old.end(), [](const Tile *a, const Tile *b) { return a->pageGen != b->pageGen ? a->pageGen < b->pageGen : a->used < b->used; });
        for (const std::vector<Tile *> *set : {&old, &now})
            for (Tile *t : *set) {
                const double k = ppp / t->scale;
                p.drawImage(QRectF(sr.left() + t->area.left() * k, sr.top() + t->area.top() * k, t->area.width() * k, t->area.height() * k),
                            t->image);
                if (set == &now)
                    t->used = ++useCounter_;
            }
        paintOverlays(p, i, sr);
        if (rubKept_ && i == rubPage_)  // (the eraser let go: what it took kept out of sight until drawn anew)
            paintRubbed(p, rubPath_);
        // the tiles still needed for what can be seen of the page
        const QSizeF size = doc_->pageSize(i);
        const QRect whole(0, 0, int(std::ceil(size.width() * scale)),
                          int(std::ceil(size.height() * scale)));
        const QRect seen = (visible & r).translated(-r.topLeft());
        const int x0 = int(std::floor(seen.left() * dpr)) / kTile;
        const int y0 = int(std::floor(seen.top() * dpr)) / kTile;
        const int x1 = int(std::ceil((seen.right() + 1) * dpr)) / kTile;
        const int y1 = int(std::ceil((seen.bottom() + 1) * dpr)) / kTile;
        for (int ty = y0; ty <= y1; ++ty) {
            for (int tx = x0; tx <= x1; ++tx) {
                const QRect area = QRect(tx * kTile, ty * kTile, kTile, kTile) & whole;
                if (area.isEmpty())
                    continue;
                const quint64 key = tileKey(i, gen_, pageGen(i), tx, ty);
                wanted.insert(key);
                if (!tiles_.contains(key) && !pending_.contains(key))
                    requestTile(i, tx, ty, scale, area, key);
            }
        }
    }
    paintTool(p);
    {
        std::lock_guard guard(wanted_->mutex);
        wanted_->keys = wanted;  // (tiles no longer in sight: skipped by the threads)
    }
    const int page = currentPage();
    if (page != lastPage_) {
        lastPage_ = page;
        emit currentPageChanged(page);
    }
}

void PageView::requestTile(int page, int tx, int ty, double scale, const QRect &area, quint64 key)
{
    Q_UNUSED(tx);
    Q_UNUSED(ty);
    pending_.insert(key);
    {
        std::lock_guard guard(wanted_->mutex);
        wanted_->keys.insert(key);
    }
    auto doc = doc_;
    auto wanted = wanted_;
    QPointer<PageView> self(this);
    const int gen = gen_;
    const int pgen = pageGen(page);
    const int serial = serial_;
    pool_.start([=] {
        bool want;
        {
            std::lock_guard guard(wanted->mutex);
            want = wanted->keys.contains(key);
        }
        Tile tile;
        tile.page = page;
        tile.gen = gen;
        tile.pageGen = pgen;
        tile.scale = scale;
        tile.area = area;
        if (want)
            tile.image = doc->render(page, scale, area);
        QMetaObject::invokeMethod(
            qApp, [self, key, serial, tile]() mutable {
                if (self)
                    self->tileDone(key, serial, std::move(tile));
            },
            Qt::QueuedConnection);
    });
}

void PageView::tileDone(quint64 key, int serial, Tile tile)
{
    if (serial != serial_)
        return;
    pending_.remove(key);
    if (tile.image.isNull())
        return;  // (skipped: out of sight by the time its turn came)
    tile.used = ++useCounter_;
    const int tilePage = tile.page, tileGen = tile.pageGen;
    tiles_.insert(key, std::move(tile));
    if (rubKept_ && tilePage == rubPage_ && tileGen > rubKeepGen_) {  // (the page drawn anew without what the eraser took)
        bool more = false;
        for (quint64 k : std::as_const(pending_))
            more = more || int(k >> 44) == rubPage_;
        if (!more) {
            rubKept_ = false;
            rubClean_ = QImage();
            rubPage_ = -1;
        }
    }
    evictTiles();
    viewport()->update();
}

void PageView::requestPreview(int page)
{
    if (previewPending_.contains(page))
        return;
    previewPending_.insert(page);
    auto doc = doc_;
    QPointer<PageView> self(this);
    const int serial = serial_;
    const QSizeF size = doc->pageSize(page);
    const double scale = kPreviewWidth / std::max(1.0, size.width());
    const QRect area(0, 0, kPreviewWidth, std::max(1, int(std::ceil(size.height() * scale))));
    pool_.start([=] {
        QImage image = doc->render(page, scale, area);
        QMetaObject::invokeMethod(
            qApp, [self, page, serial, image]() {
                if (self)
                    self->previewDone(page, serial, image);
            },
            Qt::QueuedConnection);
    });
}

void PageView::previewDone(int page, int serial, QImage image)
{
    if (serial != serial_)
        return;
    previewPending_.remove(page);
    previews_[size_t(page)] = std::move(image);
    if (page < int(previewStale_.size()))
        previewStale_[size_t(page)] = false;
    // only the previews of the pages near the one shown are kept
    int kept = 0;
    for (const QImage &im : previews_)
        kept += im.isNull() ? 0 : 1;
    if (kept > kMaxPreviews) {
        const int here = std::max(0, currentPage());
        for (int i = 0; i < int(previews_.size()); ++i)
            if (std::abs(i - here) > kMaxPreviews / 2)
                previews_[size_t(i)] = QImage();
    }
    viewport()->update();
}

void PageView::evictTiles()
{
    if (tiles_.size() <= kMaxTiles)
        return;
    std::vector<std::pair<quint64, quint64>> byUse;  // (last used, key)
    byUse.reserve(size_t(tiles_.size()));
    for (auto it = tiles_.cbegin(); it != tiles_.cend(); ++it)
        byUse.emplace_back(it.value().used, it.key());
    std::sort(byUse.begin(), byUse.end());
    const int drop = tiles_.size() - kMaxTiles * 4 / 5;
    for (int n = 0; n < drop && n < int(byUse.size()); ++n)
        tiles_.remove(byUse[size_t(n)].second);
}

void PageView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    placeBar();
    layoutPages();
    if (fit_ == Fit::Width)
        fitWidth();
    else if (fit_ == Fit::Page)
        fitPage();
}

void PageView::scrollContentsBy(int dx, int dy)
{
    if (lineEditor_) {  // (the box being typed in: with its text, where it is on the page - its place kept so too: laid out
                        // again as it's typed in, it stays there, not where it was on the screen)
        lineEditor_->move(lineEditor_->pos() + QPoint(dx, dy));
        editorTop_ += dy;
    }
    placeBar();
    viewport()->update();
}

void PageView::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {  // (a touchpad's pinch comes this way too)
        zoomBy(std::pow(1.0015, e->angleDelta().y()), e->position().toPoint());
        e->accept();
        return;
    }
    if (!e->pixelDelta().isNull()) {  // a touchpad: exactly as far as the fingers went
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - e->pixelDelta().x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - e->pixelDelta().y());
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

bool PageView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomBy(1.0 + g->value(), g->position().toPoint());
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(e);
}

// ---- the text: selected, found ----

void PageView::paintOverlays(QPainter &p, int page, const QRect &onScreen)
{
    const double ppp = pixelsPerPoint();
    if (lineEditor_ && page == holePage_ && !holeLetters_.empty()) {  // (text edited in place: the page under it, without its letters)
        if (std::abs(holeScale_ - ppp * devicePixelRatioF()) > 1e-6)
            holeFor(page, holeLetters_);
        if (!hole_.isNull())
            p.drawImage(QRectF(onScreen.left() + holeRect_.left() * ppp, onScreen.top() + holeRect_.top() * ppp,
                               holeRect_.width() * ppp, holeRect_.height() * ppp),
                        hole_);
        paintTurnedEditor(p, page);
    }
    if (showBoxes_ && look_.tool == Tool::Edit && doc_) {  // every block's and picture's outline, at once
        // (dotted grey; the one under the mouse: its dots in the theme's selection colour, a pixel down
        // and right - not another outline)
        // (round dots a little bigger than a pixel's: 2 px squares, 3 px apart - flat ends, or the
        // pen's ends stretch each dot into a dash)
        QPen grey(QColor(128, 128, 128), 2, Qt::CustomDashLine, Qt::FlatCap);
        grey.setCosmetic(true);
        grey.setDashPattern({1, 1.5});
        QPen hot = grey;
        hot.setColor(theme::selection().first);
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QPointF o = onScreen.topLeft();
        const bool here = hoverPage_ == page;
        const int hoverLine = here && hoverIndex_ >= 0 && hoverIndex_ < 100000 ? hoverIndex_ : -1;
        const int hoverPic = here && hoverIndex_ >= 100000 ? hoverIndex_ - 100000 : -1;
        const int hoverFrame = hoverLine >= 0 ? pagetext::frameOf(framesOf(page), hoverLine) : -1;
        // (what's picked up - or typed in: its solid outline instead)
        const int pickedLine = linePage_ == page && lineIndex_ >= 0 && lineIndex_ < int(linesOf(page).size()) ? lineIndex_ : -1;
        const int pickedFrame = pickedLine >= 0 ? pagetext::frameOf(framesOf(page), pickedLine) : -1;
        const int pickedPic = picPage_ == page ? picIndex_ : -1;
        auto outline = [&](const QPolygonF &poly, bool on) {
            p.setPen(on ? hot : grey);
            p.drawPolygon(on ? poly.translated(1, 1) : poly);
        };
        const std::vector<pagetext::TextFrame> &fs = framesOf(page);  // (Acrobat's boxes - what's edited as one)
        const std::vector<TextLine> &lines = linesOf(page);
        // (the one under the mouse lit, the one picked up not drawn: its own outline is)
        std::vector<bool> inTurnedBlock(lines.size(), false);
        for (int f = 0; f < int(fs.size()); ++f) {
            bool turned = false;
            for (int k : fs[size_t(f)].lines)
                turned = turned || std::abs(lines[size_t(k)].angle) > 0.01;
            if (turned && fs[size_t(f)].lines.size() > 1) {  // (a turned block: one turned box round it)
                for (int k : fs[size_t(f)].lines)
                    inTurnedBlock[size_t(k)] = true;
                if (f != pickedFrame) {
                    QPolygonF poly;
                    for (const QPointF &c : frameOutline(page, fs[size_t(f)]))
                        poly << o + c * ppp;
                    outline(poly, f == hoverFrame);
                }
                continue;
            }
            if (turned || f == pickedFrame || (fs[size_t(f)].hidden && f != hoverFrame))
                continue;  // (turned lines: each its own turned box, below; a marker: in its neighbour's outline)
            const QRectF sized = sizedBox(page, fs[size_t(f)].rect);
            const QRectF r = sized == fs[size_t(f)].rect && !fs[size_t(f)].shown.isNull() ? fs[size_t(f)].shown : sized;
            outline(QPolygonF(QRectF(o + r.topLeft() * ppp, r.size() * ppp).adjusted(-2, -2, 2, 2)), f == hoverFrame);
        }
        for (int n = 0; n < int(lines.size()); ++n)  // (turned lines: their own turned box)
            if (std::abs(lines[size_t(n)].angle) > 0.01 && !inTurnedBlock[size_t(n)]) {
                QPolygonF poly;
                for (const QPointF &c : lines[size_t(n)].shown())
                    poly << o + c * ppp;
                if (n != pickedLine)
                    outline(poly, n == hoverLine);
            }
        const std::vector<Picture> &pics = picturesOf(page);
        for (int n = 0; n < int(pics.size()); ++n) {
            QPolygonF poly;
            for (const QPointF &c : pics[size_t(n)].corners)
                poly << o + c * ppp;
            if (n != pickedPic)
                outline(poly, n == hoverPic);
        }
        p.restore();
    }
    auto toScreen = [&](const QRectF &r) {
        return QRectF(onScreen.left() + r.left() * ppp, onScreen.top() + r.top() * ppp,
                      r.width() * ppp, r.height() * ppp);
    };
    if (look_.tool == Tool::Link) {  // where the links are: dotted boxes
        QPen dots(QColor(0, 0, 0xEE), 1, Qt::CustomDashLine);
        dots.setCosmetic(true);
        dots.setDashPattern({2, 2});
        p.setPen(dots);
        p.setBrush(Qt::NoBrush);
        for (const PdfLink &l : linksOf(page))
            p.drawRect(toScreen(l.rect));
    }
    // every place the words were found: yellow, as Word's; the one you're at in the
    // selection colour - see-through
    for (int k = 0; k < int(hits_.size()); ++k) {
        const FindHit &h = hits_[size_t(k)];
        if (h.page != page)
            continue;
        QColor c = k == hitNow_ ? theme::selection().first : QColor(255, 225, 0);
        c.setAlpha(k == hitNow_ ? 110 : 120);
        p.fillRect(toScreen(h.rect), c);
    }
    if (selPage_ == page) {
        QColor c = theme::selection().first;
        c.setAlpha(90);
        for (const QRectF &r : selBoxes_)
            p.fillRect(toScreen(r), c);
    }
}

int PageView::pageAt(QPoint pos, QPointF *at) const
{
    const QPoint c = pos + scrollOffset();
    for (int i = 0; i < int(pages_.size()); ++i) {
        const QRect r = pages_[size_t(i)];
        if (r.top() > c.y())
            break;
        if (r.contains(c)) {
            if (at) {
                const double ppp = pixelsPerPoint();
                *at = QPointF((c.x() - r.left()) / ppp, (c.y() - r.top()) / ppp);
            }
            return i;
        }
    }
    return -1;
}

QPoint PageView::toView(int page, QPointF at) const
{
    if (page < 0 || page >= int(pages_.size()))
        return {};
    const QRect r = pages_[size_t(page)];
    const double ppp = pixelsPerPoint();
    return QPoint(int(std::lround(r.left() + at.x() * ppp)), int(std::lround(r.top() + at.y() * ppp))) -
           scrollOffset();
}

void PageView::showRect(int page, const QRectF &rect)
{
    if (page < 0 || page >= int(pages_.size()))
        return;
    const QRect r = pages_[size_t(page)];
    const double ppp = pixelsPerPoint();
    const double x0 = r.left() + rect.left() * ppp, x1 = r.left() + rect.right() * ppp;
    const double y0 = r.top() + rect.top() * ppp, y1 = r.top() + rect.bottom() * ppp;
    const int top = verticalScrollBar()->value(), vh = viewport()->height();
    if (!(top + 20 <= y0 && y1 <= top + vh - 20))
        verticalScrollBar()->setValue(int(y0 - vh / 3.0));
    const int left = horizontalScrollBar()->value(), vw = viewport()->width();
    if (!(left <= x0 && x1 <= left + vw))
        horizontalScrollBar()->setValue(int(std::max(0.0, x0 - 40)));
    viewport()->update();
}

void PageView::setFindHits(std::vector<FindHit> hits, int current)
{
    hits_ = std::move(hits);
    hitNow_ = current;
    viewport()->update();
}

void PageView::setSelection(int page, TextPick pick)
{
    const bool had = selPage_ >= 0;
    if (pick.boxes.empty()) {
        selPage_ = -1;
        selBoxes_.clear();
        selText_.clear();
    } else {
        selPage_ = page;
        selBoxes_ = std::move(pick.boxes);
        selText_ = std::move(pick.text);
    }
    viewport()->update();
    if (had || selPage_ >= 0)
        emit selectionChanged(selPage_ >= 0);
}

void PageView::clearSelection()
{
    setSelection(-1, {});
}

void PageView::selectBetween(int page, QPointF a, QPointF b)
{
    if (!doc_ || page < 0 || page >= doc_->pageCount())
        return;
    setSelection(page, pagetext::pick(*doc_->text(page), a, b));
}

void PageView::copySelection()
{
    if (selText_.isEmpty())
        return;
    QApplication::clipboard()->setText(selText_);
    emit message(tr("Copied the selected text."));
}

void PageView::selectPress(QMouseEvent *e)
{
    if (!doc_ || e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    setFocus();
    const QPoint pos = e->position().toPoint();
    QPointF at;
    const int page = pageAt(pos, &at);
    // a third click soon after a double click: the whole line
    if (page >= 0 && QDateTime::currentMSecsSinceEpoch() - doubleAt_ < QApplication::doubleClickInterval() &&
        (pos - doublePos_).manhattanLength() < 6) {
        doubleAt_ = 0;
        pressPage_ = -1;
        TextPick line = pagetext::around(*doc_->text(page), at, true);
        if (!line.boxes.empty()) {
            setSelection(page, std::move(line));
            emit selectionMade();
        }
        return;
    }
    pressPage_ = page;
    pressAt_ = at;
    pressPos_ = pos;
    dragging_ = false;
}

void PageView::selectMove(QMouseEvent *e)
{
    if (!doc_)
        return;
    const QPoint pos = e->position().toPoint();
    if (pressPage_ >= 0 && (e->buttons() & Qt::LeftButton)) {
        if (!dragging_ && (pos - pressPos_).manhattanLength() < 3)
            return;
        dragging_ = true;
        // (the point on the page the drag started on, even outside it)
        const QRect r = pages_[size_t(pressPage_)];
        const double ppp = pixelsPerPoint();
        const QPoint c = pos + scrollOffset();
        const QPointF to((c.x() - r.left()) / ppp, (c.y() - r.top()) / ppp);
        setSelection(pressPage_, pagetext::pick(*doc_->text(pressPage_), pressAt_, to));
        // past the top or bottom: scrolled on
        if (pos.y() < 0)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + pos.y() / 2 - 4);
        else if (pos.y() > viewport()->height())
            verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                          (pos.y() - viewport()->height()) / 2 + 4);
        return;
    }
    if (e->buttons() == Qt::NoButton) {  // an I-beam over text
        QPointF at;
        const int page = pageAt(pos, &at);
        const bool onText = page >= 0 && pagetext::charAt(*doc_->text(page), at) >= 0;
        viewport()->setCursor(onText ? Qt::IBeamCursor : Qt::ArrowCursor);
    }
}

void PageView::selectRelease(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || pressPage_ < 0) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    if (!dragging_)  // a click, not a drag: the selection let go
        clearSelection();
    else if (selPage_ >= 0)
        emit selectionMade();
    pressPage_ = -1;
    dragging_ = false;
}

void PageView::selectDouble(QMouseEvent *e)
{
    if (!doc_ || e->button() != Qt::LeftButton)
        return;
    const QPoint pos = e->position().toPoint();
    QPointF at;
    const int page = pageAt(pos, &at);
    pressPage_ = -1;
    if (page < 0)
        return;
    doubleAt_ = QDateTime::currentMSecsSinceEpoch();
    doublePos_ = pos;
    TextPick word = pagetext::around(*doc_->text(page), at, false);
    if (!word.boxes.empty()) {
        setSelection(page, std::move(word));
        emit selectionMade();
    }
}

void PageView::contextMenuEvent(QContextMenuEvent *e)
{
    QPointF at;
    const int page = pageAt(e->pos(), &at);
    if (look_.tool == Tool::Link) {  // a link: changed, followed, deleted
        const int link = page >= 0 ? linkAt(page, at) : -1;
        if (link >= 0)
            emit linkMenu(page, link, e->globalPos());
        return;
    }
    if (look_.tool == Tool::Edit && page >= 0) {
        const int line = lineAt(page, at);
        if (line >= 0) {
            pickPicture(-1, -1);
            pickLine(page, line);
        } else {
            pickLine(-1, -1);
            pickPicture(page, pictureAt(page, at));
        }
        emit editMenu(page, at, e->globalPos());
        return;
    }
    if (page < 0 || page != selPage_)
        return;
    const bool onSelection = std::any_of(selBoxes_.begin(), selBoxes_.end(),
                                         [&](const QRectF &r) { return r.contains(at); });
    if (!onSelection)
        return;
    emit selectionMenu(e->globalPos());
}

bool PageView::nudge(QPointF by)
{
    if (lineEditor_ || cropping_ || readOnly_ || !doc_ || !doc_->editable())
        return false;
    const bool any = (!group_.empty() && groupPage_ >= 0) || (lineIndex_ >= 0 && linePage_ >= 0) || (picIndex_ >= 0 && picPage_ >= 0) ||
                     (pickId_ && pickPage_ >= 0);
    if (!any)
        return false;
    if (!nudgeTimer_) {
        nudgeTimer_ = new QTimer(this);
        nudgeTimer_->setSingleShot(true);
        nudgeTimer_->setInterval(450);
        connect(nudgeTimer_, &QTimer::timeout, this, &PageView::flushNudge);
    }
    nudge_ += by;
    nudgeTimer_->start();
    viewport()->update();
    return true;
}

void PageView::flushNudge()
{
    if (nudgeTimer_)
        nudgeTimer_->stop();
    const QPointF by = nudge_;
    nudge_ = QPointF();
    if (by.isNull())
        return;
    viewport()->update();
    if (!group_.empty() && groupPage_ >= 0) {
        std::vector<int> lines, pictures;
        for (const GroupItem &g : group_)
            (g.picture ? pictures : lines).push_back(g.index);
        emit groupMoved(groupPage_, lines, pictures, by);
    } else if (lineIndex_ >= 0 && linePage_ >= 0) {
        emit lineMoved(linePage_, lineIndex_, by);
    } else if (picIndex_ >= 0 && picPage_ >= 0) {
        emit pictureChanged(picPage_, picIndex_, QTransform::fromTranslate(by.x(), by.y()), QStringLiteral("move picture"));
    } else if (pickId_ && pickPage_ >= 0) {
        emit annotMoved(pickPage_, pickId_, by);
    }
}

void PageView::keyPressEvent(QKeyEvent *e)
{
    // the arrow keys: what's picked moved - a point a press, ten with Shift (nothing picked: the page scrolled, as ever)
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    if (doc_ && (mods == Qt::NoModifier || mods == Qt::ShiftModifier) &&
        (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right || e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)) {
        const double step = mods == Qt::ShiftModifier ? 10 : 1;
        const QPointF by = e->key() == Qt::Key_Left    ? QPointF(-step, 0)
                           : e->key() == Qt::Key_Right ? QPointF(step, 0)
                           : e->key() == Qt::Key_Up    ? QPointF(0, -step)
                                                       : QPointF(0, step);
        if (nudge(by))
            return;
    } else {
        flushNudge();  // (another key: the move put in first)
    }
    if (doc_ && e->modifiers() == Qt::NoModifier) {
        if (e->key() == Qt::Key_Home) {
            goToPage(0);
            return;
        }
        if (e->key() == Qt::Key_End) {
            goToPage(doc_->pageCount() - 1);
            return;
        }
        if (cropping_ && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Escape)) {
            finishCrop(e->key() != Qt::Key_Escape);  // (Crop: put in - or left as it was)
            return;
        }
        if (e->key() == Qt::Key_Escape && lineEditor_) {  // (typing: left as it was - as Esc in the box)
            closeLineEditor(false);
            return;
        }
        if (e->key() == Qt::Key_Escape) {  // (what's picked up put down, the selection let go,
            if (!group_.empty())             // or else the tool put down)
                clearGroup();
            else if (pickId_)
                pick(-1, 0);
            else if (lineIndex_ >= 0)
                pickLine(-1, -1);
            else if (picIndex_ >= 0)
                pickPicture(-1, -1);
            else if (selPage_ >= 0)
                clearSelection();
            else
                emit escaped();
            return;
        }
        if (e->key() == Qt::Key_Delete && !group_.empty() && groupPage_ >= 0) {  // (picked together: all of them)
            std::vector<int> lines, pictures;
            for (const GroupItem &g : group_)
                (g.picture ? pictures : lines).push_back(g.index);
            const int page = groupPage_;
            clearGroup();
            emit groupDelete(page, lines, pictures);
            return;
        }
        if (e->key() == Qt::Key_Delete && pickId_) {
            emit deletePicked();
            return;
        }
        if (e->key() == Qt::Key_Delete && lineIndex_ >= 0) {
            emit lineDelete(linePage_, lineIndex_);
            return;
        }
        if (e->key() == Qt::Key_Delete && picIndex_ >= 0) {
            emit pictureDelete(picPage_, picIndex_);
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(e);
}
