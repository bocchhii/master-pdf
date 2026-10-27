// PageView's tools: what the mouse does on the pages with each tool (the Python app's): select
// text and pick up marks; Move (drag the pages about, pick up and move annotations); the pen
// and the marker; shapes; white-out and the eraser - and what's shown while they're used.

#include "view/PageView.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/Keyboard.h"
#include "ui/Theme.h"

#include <memory>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QFontDatabase>
#include <QFontInfo>
#include <QTimer>
#include <QTextBlockFormat>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QTransform>
#include <QFrame>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QDateTime>
#include <QTextFrame>
#include <QTextCursor>
#include <QTextBlock>
#include <set>
#include <QTextEdit>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// (a letter's width as the page has it, % of its font's own - set with its look; the editor's drawing makes it so: Qt's
// font stretch is the font's own width times it - a condensed font asked for at 100 was made a third wider by Qt)
constexpr int kShownScaleProp = QTextFormat::UserProperty + 21;

// A stroke drawn with the mouse made smooth, like drawing programs do (the Python app's
// smooth_stroke): points closer than a pixel and a half dropped, the wobble straightened out
// (Douglas-Peucker), then the corners rounded off (Chaikin's cutting; the ends stay put).
// px: a screen pixel, in the stroke's units.
std::vector<QPointF> smoothStroke(const std::vector<QPointF> &points, double px, int rounds = 3)
{
    std::vector<QPointF> pts;
    for (const QPointF &p : points)
        if (pts.empty() || std::abs(p.x() - pts.back().x()) + std::abs(p.y() - pts.back().y()) >= 1.5 * px)
            pts.push_back(p);
    if (!points.empty() && (pts.empty() || pts.back() != points.back()))
        pts.push_back(points.back());
    if (pts.size() < 3)
        return pts;
    const double wobble = px;
    std::function<std::vector<QPointF>(size_t, size_t)> simplify = [&](size_t a, size_t b) -> std::vector<QPointF> {
        const QPointF p0 = pts[a], p1 = pts[b];
        const double dx = p1.x() - p0.x(), dy = p1.y() - p0.y();
        const double len = std::max(1e-9, std::hypot(dx, dy));
        double far = 0;
        size_t k = a;
        for (size_t n = a + 1; n < b; ++n) {
            const double d = std::abs(dy * (pts[n].x() - p0.x()) - dx * (pts[n].y() - p0.y())) / len;
            if (d > far) {
                far = d;
                k = n;
            }
        }
        if (far <= wobble)
            return {p0, p1};
        std::vector<QPointF> left = simplify(a, k), right = simplify(k, b);
        left.pop_back();
        left.insert(left.end(), right.begin(), right.end());
        return left;
    };
    pts = simplify(0, pts.size() - 1);
    for (int r = 0; r < rounds && pts.size() >= 3; ++r) {
        std::vector<QPointF> out{pts.front()};
        for (size_t n = 0; n + 1 < pts.size(); ++n) {
            out.push_back(pts[n] * 0.75 + pts[n + 1] * 0.25);
            out.push_back(pts[n] * 0.25 + pts[n + 1] * 0.75);
        }
        out.push_back(pts.back());
        pts = std::move(out);
    }
    return pts;
}

// The pencil's or the marker's own picture as the pointer (twice as big, its tip the hot spot)
QCursor drawingCursor(const QImage &picture)
{
    QImage big = picture.scaled(picture.size() * 2, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QPoint tip(0, big.height() - 1);
    for (int y = big.height() - 1; y >= 0; --y) {  // (its lowest, leftmost pixel)
        bool found = false;
        for (int x = 0; x < big.width() && !found; ++x)
            if (qAlpha(big.pixel(x, y)) > 128) {
                tip = QPoint(x, y);
                found = true;
            }
        if (found)
            break;
    }
    return QCursor(QPixmap::fromImage(big), tip.x(), tip.y());
}

}  // namespace

void PageView::setTool(const ToolLook &look)
{
    flushNudge();
    const bool changed = look.tool != look_.tool || look.variant != look_.variant;
    look_ = look;
    if (changed) {
        clearGroup();
        closeTextBox(true);
        clearSelection();
        if (look.tool != Tool::Select && look.tool != Tool::Move)
            pick(-1, 0);
    }
    updateCursor(hover_);
    viewport()->update();
}

void PageView::setReadOnly(bool on)
{
    readOnly_ = on;
    if (on) {  // (what's picked up put down: just reading)
        pick(-1, 0);
        pickLine(-1, -1);
        pickPicture(-1, -1);
    }
    viewport()->update();
}

void PageView::pick(int page, int id)
{
    if (page == pickPage_ && id == pickId_)
        return;
    pickPage_ = id ? page : -1;
    pickId_ = id;
    viewport()->update();
    emit picked(pickPage_, pickId_);
}

QPointF PageView::toPage(int page, QPoint pos) const
{
    const QRect r = pages_[size_t(page)];
    const double ppp = pixelsPerPoint();
    const QPoint c = pos + scrollOffset();
    return {(c.x() - r.left()) / ppp, (c.y() - r.top()) / ppp};
}

const Annot *PageView::annotAt(int page, QPointF at, bool markups, std::vector<Annot> &store) const
{
    // the topmost annotation there (a text mark: by its pieces, not its whole box)
    store = doc_->annots(page);
    const double slack = 3 / pixelsPerPoint();
    const Annot *found = nullptr;
    for (const Annot &a : store) {
        if (a.markup()) {
            if (!markups)
                continue;
            for (const QRectF &r : a.pieces)
                if (r.contains(at))
                    found = &a;
            continue;
        }
        if (a.rect.adjusted(-slack, -slack, slack, slack).contains(at))
            found = &a;
    }
    return found;
}

bool PageView::onWord(int page, QPointF at) const
{
    return pagetext::charAt(*doc_->text(page), at) >= 0;
}

void PageView::updateCursor(QPoint pos)
{
    QCursor cursor = Qt::ArrowCursor;
    switch (look_.tool) {
    case Tool::Select: {
        QPointF at;
        const int page = doc_ ? pageAt(pos, &at) : -1;
        if (page >= 0 && onWord(page, at))
            cursor = Qt::IBeamCursor;
        break;
    }
    case Tool::Move:
        cursor = press_ == Press::Pan ? Qt::ClosedHandCursor : Qt::PointingHandCursor;
        break;
    case Tool::Pen:
        cursor = drawingCursor(icons::get(QStringLiteral("pen_tool")));
        break;
    case Tool::Marker:
        cursor = drawingCursor(icons::tinted(icons::get(QStringLiteral("highlight_tool")), QColor(255, 255, 0), look_.color));
        break;
    case Tool::Eraser:
        cursor = look_.variant == QLatin1String("rub") ? Qt::BlankCursor : Qt::CrossCursor;
        break;
    case Tool::Shape:
        cursor = Qt::CrossCursor;
        break;
    case Tool::Text:
        cursor = Qt::IBeamCursor;
        break;
    case Tool::Edit: {
        int h = pictureHandleAt(pos);
        if (h < 0)
            h = handleAt(pos);
        static const Qt::CursorShape shapes[9] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                  Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                  Qt::PointingHandCursor};
        cursor = h >= 0 && h <= 8 ? shapes[h] : Qt::ArrowCursor;
        if (h < 0) {  // (a box's edge: moved by it; its text: typed in)
            int edgePage = -1;
            QPointF at;
            const int page = doc_ ? pageAt(pos, &at) : -1;
            if (doc_ && edgeAt(pos, &edgePage) >= 0)
                cursor = Qt::SizeAllCursor;
            else if (page >= 0 && lineAt(page, at) >= 0 && overPicture(page, at) < 0)
                cursor = Qt::IBeamCursor;
        }
        break;
    }
    case Tool::Image:
    case Tool::Sign:
    case Tool::Link:
        cursor = Qt::CrossCursor;
        break;
    default:
        break;
    }
    viewport()->setCursor(cursor);
}

// ---- the mouse ----

void PageView::mousePressEvent(QMouseEvent *e)
{
    flushNudge();  // (the arrow keys' move put in first - to what it was for)
    if (!doc_ || e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    if (turnedEditorMouse(e) || cropMouse(e))
        return;
    if (lineEditor_ && look_.tool == Tool::Edit)  // (typing: what's typed put in first - a press here
        commitEditor();                           // then finds the page's lines as they are now)
    setFocus();
    if (editor_)
        closeTextBox(true);
    const QPoint pos = e->position().toPoint();
    QPointF at;
    const int page = pageAt(pos, &at);
    std::vector<Annot> store;
    auto pressAnnot = [&](const Annot &a) {
        // an annotation clicked: picked up - and dragged about, if it can be moved
        pick(page, a.id);
        if (a.movable()) {
            press_ = Press::Move;
            toolPage_ = page;
            moveFrom_ = moveTo_ = pos;
            moveRect_ = a.rect;
        }
    };
    switch (look_.tool) {
    case Tool::Select: {
        if (page >= 0 && !readOnly_) {
            const Annot *a = annotAt(page, at, false, store);
            if (a && (a->movable() || !onWord(page, at))) {
                pressAnnot(*a);
                return;
            }
        }
        if (pickId_)
            pick(-1, 0);
        markAt_ = 0;
        if (page >= 0 && !readOnly_)
            for (const Annot &a : doc_->annots(page))
                if (a.markup())
                    for (const QRectF &r : a.pieces)
                        if (r.contains(at))
                            markAt_ = a.id;
        press_ = Press::Select;
        toolPage_ = page;
        linkPress_ = page >= 0 ? linkAt(page, at) : -1;
        selectPress(e);
        return;
    }
    case Tool::Move: {
        const Annot *a = page >= 0 ? annotAt(page, at, true, store) : nullptr;
        if (a) {
            pressAnnot(*a);
            return;
        }
        pick(-1, 0);
        press_ = Press::Pan;  // (the pages dragged about; a click on a link: followed)
        toolPage_ = page;
        linkPress_ = page >= 0 ? linkAt(page, at) : -1;
        panFrom_ = pos;
        panScroll_ = scrollOffset();
        updateCursor(pos);
        return;
    }
    case Tool::Pen:
    case Tool::Marker:
        if (page < 0)
            return;
        press_ = Press::Ink;
        toolPage_ = page;
        stroke_ = {at};
        return;
    case Tool::Shape:
        if (page < 0)
            return;
        press_ = Press::Box;
        toolPage_ = page;
        boxFrom_ = boxTo_ = at;
        return;
    case Tool::Eraser:
        if (page < 0)
            return;
        toolPage_ = page;
        if (look_.variant == QLatin1String("rub")) {
            press_ = Press::Rub;
            stroke_ = {at};
            {  // (what can be seen of the page, without the drawings: shown where it rubs)
                const QRect r = QRect(toView(page, QPointF(0, 0)), toView(page, QPointF(doc_->pageSize(page).width(), doc_->pageSize(page).height())))
                                    .intersected(viewport()->rect());
                const double scale = pixelsPerPoint() * devicePixelRatioF();
                const QPointF a = toPage(page, r.topLeft()), b = toPage(page, r.bottomRight());
                const QRect px = QRectF(a * scale, b * scale).toAlignedRect();
                rubRect_ = QRectF(QPointF(px.topLeft()) / scale, QSizeF(px.size()) / scale);
                rubClean_ = doc_->renderWithoutDrawings(page, scale, px);
                rubClean_.setDevicePixelRatio(devicePixelRatioF());
                rubPage_ = page;
                rubKept_ = false;
            }
        } else {
            press_ = Press::Box;
            boxFrom_ = boxTo_ = at;
        }
        return;
    case Tool::Text:
        if (page >= 0)
            startTextBox(page, at);
        return;
    case Tool::Edit:
        editPress(e);
        return;
    case Tool::Sign:
        if (page >= 0)
            emit signClicked(page, at);
        return;
    case Tool::Link:
    case Tool::Image:  // a click: the picture's own size there; a box: in the box
        if (page < 0)
            return;
        press_ = Press::Box;
        toolPage_ = page;
        boxFrom_ = boxTo_ = at;
        return;
    default:
        return;
    }
}

void PageView::mouseMoveEvent(QMouseEvent *e)
{
    if (!doc_ || turnedEditorMouse(e) || cropMouse(e))
        return;
    const QPoint pos = e->position().toPoint();
    hover_ = pos;
    switch (press_) {
    case Press::Select:
        selectMove(e);
        return;
    case Press::Pan:
        horizontalScrollBar()->setValue(panScroll_.x() - (pos.x() - panFrom_.x()));
        verticalScrollBar()->setValue(panScroll_.y() - (pos.y() - panFrom_.y()));
        return;
    case Press::Move:
        moveTo_ = pos;
        viewport()->update();
        return;
    case Press::Ink: {
        const QPointF at = toPage(toolPage_, pos);
        if (look_.tool == Tool::Marker && (e->modifiers() & Qt::ShiftModifier))
            stroke_.resize(1);  // (Shift: straight from where it started)
        const QPointF last = stroke_.back();
        if (std::abs(at.x() - last.x()) + std::abs(at.y() - last.y()) >= 1 / pixelsPerPoint() ||
            (e->modifiers() & Qt::ShiftModifier))
            stroke_.push_back(at);
        viewport()->update();
        return;
    }
    case Press::Box:
        boxTo_ = toPage(toolPage_, pos);
        viewport()->update();
        return;
    case Press::Rub:
        stroke_.push_back(toPage(toolPage_, pos));
        viewport()->update();
        return;
    case Press::MoveLine:
    case Press::ScaleLine:
    case Press::SizeText:
    case Press::TurnLine:
    case Press::MovePic:
    case Press::SizePic:
    case Press::TurnPic:
    case Press::MoveGroup:
        moveTo_ = pos;
        viewport()->update();
        return;
    default:
        break;
    }
    if (look_.tool == Tool::Edit) {  // a dotted box round the line under the mouse
        QPointF at;
        const int page = pageAt(pos, &at);
        int index = page >= 0 ? lineAt(page, at) : -1;
        if (index >= 0 && overPicture(page, at) >= 0)
            index = -1;
        if (index < 0 && page >= 0) {  // (a picture: its index after the lines')
            const int pic = pictureAt(page, at);
            if (pic >= 0)
                index = 100000 + pic;
        }
        if (page != hoverPage_ || index != hoverIndex_) {
            hoverPage_ = index >= 0 ? page : -1;
            hoverIndex_ = index;
            viewport()->update();
        }
        updateCursor(pos);
        return;
    }
    if (look_.tool == Tool::Select || look_.tool == Tool::Move) {  // over a link: the pointing hand, where it goes
        QPointF at;
        const int page = pageAt(pos, &at);
        const int link = page >= 0 ? linkAt(page, at) : -1;
        if (page != hoverLinkPage_ || link != hoverLink_) {
            hoverLinkPage_ = link >= 0 ? page : -1;
            hoverLink_ = link;
            if (link >= 0) {
                const PdfLink &l = linksOf(page)[size_t(link)];
                emit message(l.uri.isEmpty() ? tr("Link to page %1 (click to go there)").arg(l.page + 1)
                                             : tr("Link to %1 (click to go there)").arg(l.uri));
            }
        }
        if (link >= 0) {
            viewport()->setCursor(Qt::PointingHandCursor);
            return;
        }
        if (look_.tool == Tool::Move)
            updateCursor(pos);
    }
    if (look_.tool == Tool::Select)
        selectMove(e);  // (the I-beam over text)
    else if (look_.tool == Tool::Eraser && look_.variant == QLatin1String("rub"))
        viewport()->update();  // (its ring follows the mouse)
}

void PageView::mouseReleaseEvent(QMouseEvent *e)
{
    if (turnedEditorMouse(e) || cropMouse(e))
        return;
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    const Press p = press_;
    press_ = Press::None;
    const QPoint pos = e->position().toPoint();
    switch (p) {
    case Press::Select: {
        const bool drag = dragging_;
        const int page = pressPage_;
        selectRelease(e);
        if (!drag && markAt_ && page >= 0)  // (a click on a highlight: it's picked up)
            emit markClicked(page, markAt_);
        else if (!drag && linkPress_ >= 0 && page >= 0)  // (on a link: followed)
            emit linkClicked(page, linkPress_);
        markAt_ = 0;
        linkPress_ = -1;
        break;
    }
    case Press::Pan:
        if (linkPress_ >= 0 && (pos - panFrom_).manhattanLength() < 3)
            emit linkClicked(toolPage_, linkPress_);
        linkPress_ = -1;
        updateCursor(pos);
        break;
    case Press::Move:
        if ((moveTo_ - moveFrom_).manhattanLength() >= 3)
            emit annotMoved(toolPage_, pickId_, QPointF(moveTo_ - moveFrom_) / pixelsPerPoint());
        break;
    case Press::Ink: {
        std::vector<QPointF> line = stroke_.size() > 2 ? smoothStroke(stroke_, 1 / pixelsPerPoint()) : stroke_;
        stroke_.clear();
        emit inkDrawn(toolPage_, line);
        break;
    }
    case Press::Box: {
        const QPointF d = (boxTo_ - boxFrom_) * pixelsPerPoint();
        if (std::abs(d.x()) + std::abs(d.y()) >= 4)
            emit boxDrawn(toolPage_, boxFrom_, boxTo_);
        else if (look_.tool == Tool::Image)
            emit imageClicked(toolPage_, boxFrom_);
        break;
    }
    case Press::MovePic:
    case Press::SizePic:
    case Press::TurnPic:
        if ((moveTo_ - moveFrom_).manhattanLength() >= 3) {
            press_ = p;  // (what the drag was, for the move)
            const QTransform t = pictureMove();
            press_ = Press::None;
            const QString what = p == Press::MovePic ? QStringLiteral("move picture")
                                 : p == Press::SizePic ? QStringLiteral("resize picture") : QStringLiteral("turn picture");
            emit pictureChanged(picPage_, picIndex_, t, what);
        }
        break;
    case Press::MoveGroup:
        if ((moveTo_ - moveFrom_).manhattanLength() >= 3 && groupPage_ >= 0) {
            std::vector<int> lines, pictures;
            for (const GroupItem &g : group_)
                (g.picture ? pictures : lines).push_back(g.index);
            emit groupMoved(groupPage_, lines, pictures, toPage(groupPage_, moveTo_) - toPage(groupPage_, moveFrom_));
        }
        viewport()->update();
        break;
    case Press::Rub: {
        std::vector<QPointF> path = std::move(stroke_);
        stroke_.clear();
        // (the rubbed-out look kept until the page is drawn again without what it took)
        rubPath_ = path;
        rubKeepGen_ = pageGen(toolPage_);
        rubKept_ = true;
        emit rubbed(toolPage_, path);
        if (pageGen(toolPage_) == rubKeepGen_) {  // (nothing taken: it goes now)
            rubKept_ = false;
            rubClean_ = QImage();
            rubPage_ = -1;
        }
        viewport()->update();
        break;
    }
    case Press::MoveLine:
        if ((moveTo_ - moveFrom_).manhattanLength() >= 3)
            emit lineMoved(linePage_, lineIndex_, QPointF(moveTo_ - moveFrom_) / pixelsPerPoint());
        else if (!edgeMove_)  // (a click: picked up and typed in at once, the caret where it's clicked - as Acrobat)
            editLine(linePage_, lineIndex_, pos);
        edgeMove_ = false;
        break;
    case Press::TurnLine: {
        moveTo_ = pos;
        const double d = lineTurnDegrees();
        if (std::abs(d) >= 0.5)
            emit lineTurned(linePage_, lineIndex_, d);
        break;
    }
    case Press::SizeText: {
        if ((moveTo_ - moveFrom_).manhattanLength() < 3 || lineIndex_ < 0 || lineIndex_ >= int(linesOf(linePage_).size()))
            break;
        // (as Acrobat's: its box made that big - its sides: its text wrapped across it again, the
        // box kept that wide; its top / bottom: just the box; a corner: both)
        const QRectF text = pickedTextBox();
        const QRectF was = sizedBox(linePage_, text), now = draggedBox();
        const int h = textHandle_;
        textHandle_ = -1;
        auto keep = [this](const QRectF &key, const QRectF &box) {
            std::vector<std::pair<QRectF, QRectF>> &sized = boxSizes_[linePage_];
            sized.erase(std::remove_if(sized.begin(), sized.end(), [&](const auto &e) { return e.first == key; }), sized.end());
            if (box != key)
                sized.emplace_back(key, box);
        };
        const bool sides = std::abs(now.left() - was.left()) > 1 || std::abs(now.right() - was.right()) > 1;
        if (!sides) {
            keep(text, now);
            viewport()->update();
            break;
        }
        const bool leftMoved = h == 0 || h == 6 || h == 7, rightMoved = h == 2 || h == 3 || h == 4;
        emit textResized(linePage_, lineIndex_, leftMoved ? now.left() : -1e9, rightMoved ? now.right() : -1e9, 1);
        // (wrapped again - its line picked up where it is now: its new text's box, the box kept round it)
        if (lineIndex_ >= 0 && lineIndex_ < int(linesOf(linePage_).size())) {
            const QRectF after = pickedTextBox();
            QRectF box(QPointF(leftMoved ? now.left() : std::min(was.left(), after.left()), std::min(now.top(), after.top())),
                       QPointF(rightMoved ? now.right() : std::max(was.right(), after.right()), std::max(now.bottom(), after.bottom())));
            keep(after, box | after);
        }
        viewport()->update();
        break;
    }
    case Press::ScaleLine: {
        const QRectF r = linesOf(linePage_)[size_t(lineIndex_)].rect;
        const double w0 = std::max(1.0, r.width() * pixelsPerPoint());
        const double dx = (moveTo_ - moveFrom_).x();
        const double k = std::max(0.2, (w0 + (scaleRight_ ? dx : -dx)) / w0);
        if (std::abs(k - 1) > 0.02)
            emit lineScaled(linePage_, lineIndex_, k, !scaleRight_);
        break;
    }
    default:
        break;
    }
    viewport()->update();
}

void PageView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (!doc_ || e->button() != Qt::LeftButton || turnedEditorMouse(e))
        return;
    if (cropping_) {  // (Crop: a double click puts it in)
        finishCrop(true);
        return;
    }
    const QPoint pos = e->position().toPoint();
    QPointF at;
    const int page = pageAt(pos, &at);
    if (page >= 0 && !readOnly_ && (look_.tool == Tool::Select || look_.tool == Tool::Move)) {
        std::vector<Annot> store;
        if (const Annot *a = annotAt(page, at, look_.tool == Tool::Move, store)) {
            press_ = Press::None;
            emit annotEdit(page, a->id);
            return;
        }
    }
    if (look_.tool == Tool::Edit && page >= 0) {
        const int index = lineAt(page, at);
        if (index >= 0) {
            press_ = Press::None;
            pickLine(page, index);
            editLine(page, index);
        }
        return;
    }
    if (look_.tool == Tool::Select)
        selectDouble(e);
}

// ---- what's drawn over the pages while a tool is used ----

namespace {

// the 8 handles round an outline (its corners tl, tr, br, bl): tl, t, tr, r, br, b, bl, l
std::array<QPointF, 8> handleSpots(const QPolygonF &c)
{
    return {c[0], (c[0] + c[1]) / 2, c[1], (c[1] + c[2]) / 2, c[2], (c[2] + c[3]) / 2, c[3], (c[3] + c[0]) / 2};
}

// what's picked up, as Acrobat shows it: its outline, round white handles at its corners and the
// middles of its sides, the rotate handle - a round button with a turning arrow - on a stalk over
// its top (in the theme's selection colour)
// what's being moved, where it goes - as Acrobat shows it: a light see-through box there, its outline, and thin
// guide lines along its edges right across the view (to line it up with what's on the page)
void drawMoveGuide(QPainter &p, const QPolygonF &ghost, const QRectF &view, const QColor &sel)
{
    if (ghost.size() < 3)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QColor fill = sel;
    fill.setAlpha(70);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(ghost);
    QColor line = sel;
    line.setAlpha(150);
    QPen guide(line, 1);
    guide.setCosmetic(true);
    p.setPen(guide);
    p.setBrush(Qt::NoBrush);
    const QRectF b = ghost.boundingRect();
    for (double x : {b.left(), b.right()})
        p.drawLine(QPointF(x, view.top()), QPointF(x, view.bottom()));
    for (double y : {b.top(), b.bottom()})
        p.drawLine(QPointF(view.left(), y), QPointF(view.right(), y));
    QPen edge(sel, 1);
    edge.setCosmetic(true);
    p.setPen(edge);
    p.drawPolygon(ghost);
    p.restore();
}

void drawPicked(QPainter &p, const QPolygonF &poly, const QPointF &turn, bool handles, const QColor &sel)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(sel, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(poly);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!turn.isNull() && poly.size() == 4) {
        const QPointF top = (poly[0] + poly[1]) / 2, up = turn - top;
        const double n = std::max(1e-6, std::hypot(up.x(), up.y()));
        p.setPen(QPen(sel, 1));
        p.drawLine(top, turn - up / n * 8);
        p.setPen(Qt::NoPen);
        p.setBrush(sel);
        p.drawEllipse(turn, 8, 8);
        QPen white(QColor("#FFFFFE"), 1.6);
        white.setCapStyle(Qt::RoundCap);
        p.setPen(white);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(turn - QPointF(4, 4), QSizeF(8, 8)), 50 * 16, 280 * 16);
        const QPointF end = turn + QPointF(4 * std::cos(50 * M_PI / 180), -4 * std::sin(50 * M_PI / 180));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#FFFFFE"));
        p.drawPolygon(QPolygonF({end + QPointF(-2.6, -1.2), end + QPointF(1.9, -1.9), end + QPointF(0.9, 2.6)}));
    }
    if (handles && poly.size() == 4) {
        p.setPen(QPen(sel, 1.2));
        p.setBrush(QColor("#FFFFFE"));
        for (const QPointF &c : handleSpots(poly))
            p.drawEllipse(c, 4, 4);
    }
    p.restore();
}

}  // namespace

void PageView::paintTool(QPainter &p)
{
    const double ppp = pixelsPerPoint();
    const QPoint off = scrollOffset();
    auto origin = [&](int page) { return QPointF(pages_[size_t(page)].topLeft() - off); };
    p.save();
    // what's picked up: a dotted box round each piece of a text mark; round anything else a
    // dashed box with little handles at its corners
    if (pickId_ && pickPage_ >= 0 && pickPage_ < int(pages_.size())) {
        const QColor sel = theme::selection().first;
        for (const Annot &a : doc_->annots(pickPage_)) {
            if (a.id != pickId_)
                continue;
            const QPointF o = origin(pickPage_);
            auto screen = [&](const QRectF &r) {
                return QRectF(o + r.topLeft() * ppp, r.size() * ppp);
            };
            QPen pen(sel, 1, Qt::CustomDashLine);
            pen.setCosmetic(true);
            pen.setDashPattern({2, 2});
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            if (a.markup()) {
                for (const QRectF &r : a.pieces)
                    p.drawRect(screen(r).adjusted(-1, -1, 1, 1));
            } else {
                const QRectF r = screen(a.rect).adjusted(-2, -2, 2, 2);
                p.drawRect(r);
                for (const QPointF &c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()})
                    p.fillRect(QRectF(c.x() - 3, c.y() - 3, 6, 6), sel);
            }
            if (press_ == Press::Move && (moveTo_ - moveFrom_).manhattanLength() >= 3)
                drawMoveGuide(p, QPolygonF(screen(moveRect_).translated(moveTo_ - moveFrom_)), QRectF(viewport()->rect()), sel);
        }
    }
    if (toolPage_ >= 0 && toolPage_ < int(pages_.size()) && press_ != Press::None) {
        const QPointF o = origin(toolPage_);
        auto screen = [&](QPointF q) { return o + q * ppp; };
        p.setRenderHint(QPainter::Antialiasing);
        QColor color = look_.color;
        if (press_ == Press::Ink && stroke_.size() >= 1) {
            // the stroke as it'll be put in: smooth, in its colour and see-through
            std::vector<QPointF> line = stroke_.size() > 2 ? smoothStroke(stroke_, 1 / ppp, 2) : stroke_;
            QPainterPath path(screen(line.front()));
            for (size_t k = 1; k < line.size(); ++k)
                path.lineTo(screen(line[k]));
            if (line.size() == 1)
                path.lineTo(screen(line.front()) + QPointF(0.5, 0));
            color.setAlphaF(look_.opacity);
            QPen pen(color, std::max(1.0, look_.width * ppp), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            if (look_.tool == Tool::Marker)
                p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        } else if (press_ == Press::Box) {
            const QPointF a = screen(boxFrom_), b = screen(boxTo_);
            const QRectF r = QRectF(a, b).normalized();
            if (look_.tool == Tool::Image || look_.tool == Tool::Link) {  // a picture's or link's place: dotted
                QPen dots(Qt::black, 1, Qt::CustomDashLine);
                dots.setCosmetic(true);
                dots.setDashPattern({2, 2});
                p.setPen(dots);
                p.setBrush(Qt::NoBrush);
                p.drawRect(r);
            } else if (look_.tool == Tool::Eraser) {  // white-out: the colour it'll cover with
                p.fillRect(r, look_.color);
                QPen dots(Qt::black, 1, Qt::CustomDashLine);
                dots.setCosmetic(true);
                dots.setDashPattern({1, 1});
                p.setPen(dots);
                p.setBrush(Qt::NoBrush);
                p.drawRect(r);
            } else {
                color.setAlphaF(look_.opacity);
                const double w = std::max(1.0, look_.width * ppp);
                p.setPen(QPen(color, w, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
                p.setBrush(Qt::NoBrush);
                if (look_.variant == QLatin1String("ellipse"))
                    p.drawEllipse(r);
                else if (look_.variant == QLatin1String("line") || look_.variant == QLatin1String("arrow")) {
                    p.drawLine(a, b);
                    if (look_.variant == QLatin1String("arrow")) {  // its head, at the end
                        const QPointF d = b - a;
                        const double len = std::hypot(d.x(), d.y());
                        if (len > 1) {
                            const QPointF u = d / len, n(-u.y(), u.x());
                            const double h = std::max(8.0, 3 * w);
                            QPolygonF head({b, b - u * h + n * h / 2, b - u * h - n * h / 2});
                            p.setBrush(color);
                            p.drawPolygon(head);
                        }
                    }
                } else
                    p.drawRect(r);
            }
        } else if (press_ == Press::Rub && !stroke_.empty()) {  // where the eraser's been: rubbed out already
            paintRubbed(p, stroke_);
        }
    }
    // typing in it: its box, handles and rotate handle still round it (as Acrobat's)
    if (look_.tool == Tool::Edit && lineEditor_ && lineIndex_ >= 0 && linePage_ >= 0 && lineIndex_ < int(linesOf(linePage_).size())) {
        const QPolygonF poly = pickedTextOutline();
        if (poly.size() == 4)
            drawPicked(p, poly, lineTurnHandle(), std::abs(linesOf(linePage_)[size_t(lineIndex_)].angle) < 0.01, theme::selection().first);
    }
    // the Edit tool: a dotted box round the line under the mouse; a solid one with round
    // handles at its ends round the one picked up; where it's going, dotted
    if (look_.tool == Tool::Edit && !lineEditor_) {
        const QColor sel = theme::selection().first;
        p.setRenderHint(QPainter::Antialiasing, false);
        auto screen = [&](int page, const QRectF &r) {
            return QRectF(origin(page) + r.topLeft() * ppp, r.size() * ppp).adjusted(-2, -2, 2, 2);
        };
        if (!nudge_.isNull()) {  // (the arrow keys moving what's picked: where it's going)
            QPen ghost(Qt::black, 1, Qt::CustomDashLine);
            ghost.setCosmetic(true);
            ghost.setDashPattern({1, 1});
            p.setPen(ghost);
            p.setBrush(Qt::NoBrush);
            const QPointF off = nudge_ * ppp;
            if (!group_.empty() && groupPage_ >= 0) {
                for (const GroupItem &g : group_) {
                    const QRectF b = groupBox(groupPage_, g);
                    if (!b.isNull())
                        p.drawRect(screen(groupPage_, b).translated(off));
                }
            } else if (lineIndex_ >= 0 && linePage_ >= 0 && lineIndex_ < int(linesOf(linePage_).size())) {
                p.drawPolygon(pickedTextOutline().translated(off));
            } else if (picIndex_ >= 0 && picPage_ >= 0 && picIndex_ < int(picturesOf(picPage_).size())) {
                QPolygonF poly;
                for (const QPointF &c : picturesOf(picPage_)[size_t(picIndex_)].corners)
                    poly << origin(picPage_) + c * ppp + off;
                p.drawPolygon(poly);
            }
        }
        if (groupPage_ >= 0 && !group_.empty()) {  // (things picked together: each one's box - dragged: where they go)
            QPen solid(sel, 2);
            solid.setCosmetic(true);
            p.setPen(solid);
            p.setBrush(Qt::NoBrush);
            const bool dragged = press_ == Press::MoveGroup && (moveTo_ - moveFrom_).manhattanLength() >= 3;
            std::vector<QRectF> shown;
            for (const GroupItem &g : group_) {
                const QRectF b = groupBox(groupPage_, g);
                if (!b.isNull())
                    shown.push_back(screen(groupPage_, b));
            }
            for (const QRectF &r : shown)
                p.drawRect(r);
            if (dragged) {
                QPen ghost(Qt::black, 1, Qt::CustomDashLine);
                ghost.setCosmetic(true);
                ghost.setDashPattern({1, 1});
                p.setPen(ghost);
                for (const QRectF &r : shown)
                    p.drawRect(r.translated(moveTo_ - moveFrom_));
            }
        }
        if (!showBoxes_ && hoverIndex_ >= 100000 && hoverPage_ >= 0 && !(hoverPage_ == picPage_ && hoverIndex_ - 100000 == picIndex_) &&
            hoverIndex_ - 100000 < int(picturesOf(hoverPage_).size())) {
            QPen dots(sel, 1, Qt::CustomDashLine);
            dots.setCosmetic(true);
            dots.setDashPattern({1, 2});
            p.setPen(dots);
            p.setBrush(Qt::NoBrush);
            QPolygonF poly;
            for (const QPointF &c : picturesOf(hoverPage_)[size_t(hoverIndex_ - 100000)].corners)
                poly << origin(hoverPage_) + c * ppp;
            p.drawPolygon(poly);
        }
        if (cropping_ && picPage_ >= 0) {  // Crop: what's cut off dimmed, the box with its handles
            const QRectF whole = cropBounds(), box = cropRect_;
            auto onView = [&](const QRectF &r) { return QRectF(origin(picPage_) + r.topLeft() * ppp, r.size() * ppp); };
            const QColor dim(0, 0, 0, 110);
            const QRectF w = onView(whole), b = onView(box);
            p.fillRect(QRectF(w.left(), w.top(), w.width(), b.top() - w.top()), dim);
            p.fillRect(QRectF(w.left(), b.bottom(), w.width(), w.bottom() - b.bottom()), dim);
            p.fillRect(QRectF(w.left(), b.top(), b.left() - w.left(), b.height()), dim);
            p.fillRect(QRectF(b.right(), b.top(), w.right() - b.right(), b.height()), dim);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(b);
            p.setPen(QPen(Qt::white, 1, Qt::DashLine));
            p.drawRect(b);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor("#FFFFFE"));
            const QPointF spots[8] = {b.topLeft(), {b.center().x(), b.top()}, b.topRight(), {b.right(), b.center().y()},
                                      b.bottomRight(), {b.center().x(), b.bottom()}, b.bottomLeft(), {b.left(), b.center().y()}};
            for (const QPointF &c : spots)
                p.drawRect(QRectF(c.x() - 3, c.y() - 3, 7, 7));
        } else if (picIndex_ >= 0 && picPage_ >= 0 && picIndex_ < int(picturesOf(picPage_).size())) {
            // the picture picked up: its outline, square handles at its corners, and the
            // rotate handle on a stalk over its top
            const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
            QPolygonF poly;
            for (const QPointF &c : pic.corners)
                poly << origin(picPage_) + c * ppp;
            drawPicked(p, poly, poly.size() == 4 ? pictureTurnSpot() : QPointF(), true, sel);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(Qt::black, 1));
            if ((press_ == Press::MovePic || press_ == Press::SizePic || press_ == Press::TurnPic) &&
                (moveTo_ - moveFrom_).manhattanLength() >= 3) {
                QPen ghost(Qt::black, 1, Qt::CustomDashLine);
                ghost.setCosmetic(true);
                ghost.setDashPattern({1, 1});
                p.setPen(ghost);
                p.setBrush(Qt::NoBrush);
                const QTransform t = pictureMove();
                QPolygonF moved;
                for (const QPointF &c : pic.corners)
                    moved << origin(picPage_) + t.map(c) * ppp;
                if (press_ == Press::MovePic)  // (moved: Acrobat's box and guide lines)
                    drawMoveGuide(p, moved, QRectF(viewport()->rect()), sel);
                else
                    p.drawPolygon(moved);
                if (press_ == Press::TurnPic) {
                    const double a = std::atan2(t.m12(), t.m11()) * 180 / M_PI;
                    p.setPen(theme::selection().first);
                    p.setFont(theme::font());
                    p.drawText(QPointF(moveTo_) + QPointF(14, 14), QStringLiteral("%1°").arg(std::lround(a)));
                }
            }
        }
        if (!showBoxes_ && hoverIndex_ >= 0 && hoverIndex_ < 100000 && hoverPage_ >= 0 && !(hoverPage_ == linePage_ && hoverIndex_ == lineIndex_) &&
            !(hoverPage_ == linePage_ && lineIndex_ >= 0 && lineIndex_ < int(linesOf(linePage_).size()) &&
              frameLines(linePage_, lineIndex_).size() > 1 &&
              pagetext::frameOf(framesOf(linePage_), hoverIndex_) == pagetext::frameOf(framesOf(linePage_), lineIndex_)) &&
            hoverIndex_ < int(linesOf(hoverPage_).size())) {
            QPen dots(sel, 1, Qt::CustomDashLine);
            dots.setCosmetic(true);
            dots.setDashPattern({1, 2});
            p.setPen(dots);
            p.setBrush(Qt::NoBrush);
            const std::vector<int> block = frameLines(hoverPage_, hoverIndex_);
            if (block.size() > 1) {  // (a block of paragraphs: round all of it)
                const int f = pagetext::frameOf(framesOf(hoverPage_), hoverIndex_);
                p.drawRect(screen(hoverPage_, framesOf(hoverPage_)[size_t(f)].rect.adjusted(-1, -1, 1, 1)));
            } else {
                QPolygonF shown;
                for (const QPointF &c : linesOf(hoverPage_)[size_t(hoverIndex_)].shown())
                    shown << QPointF(toView(hoverPage_, c));
                p.drawPolygon(shown);
            }
        }
        if (lineIndex_ >= 0 && linePage_ >= 0 && lineIndex_ < int(linesOf(linePage_).size())) {
            const TextLine &picked = linesOf(linePage_)[size_t(lineIndex_)];
            const bool block = frameLines(linePage_, lineIndex_).size() > 1;
            const QRectF blockRect = block ? framesOf(linePage_)[size_t(pagetext::frameOf(framesOf(linePage_), lineIndex_))].rect.adjusted(-1, -1, 1, 1)
                                           : picked.rect;
            const QRectF r = screen(linePage_, blockRect);
            const bool turned = std::abs(picked.angle) > 0.01;
            // (its 4 corners - a closed QPolygonF(QRectF) has 5, and drew it bare: no handles)
            const QPolygonF shown = pickedTextOutline();
            // (its 8 handles - not turned: its box resized; the rotate handle over its top)
            drawPicked(p, shown, lineTurnHandle(), !turned, sel);
            p.setRenderHint(QPainter::Antialiasing, false);
            if (press_ == Press::SizeText && (moveTo_ - moveFrom_).manhattanLength() >= 3) {  // (where its box goes)
                QPen ghost(Qt::black, 1, Qt::CustomDashLine);
                ghost.setCosmetic(true);
                ghost.setDashPattern({1, 1});
                p.setPen(ghost);
                p.setBrush(Qt::NoBrush);
                // (just what letting go makes: from the box as it's shown, never over its text)
                p.drawRect(shownBox(draggedBox(), block));
            }
            if (press_ == Press::TurnLine && (moveTo_ - moveFrom_).manhattanLength() >= 3) {  // (where it's turned to)
                const double d = lineTurnDegrees();
                const QPointF m = (shown[0] + shown[2]) / 2;
                QTransform t;
                t.translate(m.x(), m.y());
                t.rotate(d);
                t.translate(-m.x(), -m.y());
                QPen ghost(Qt::black, 1, Qt::CustomDashLine);
                ghost.setCosmetic(true);
                ghost.setDashPattern({1, 1});
                p.setPen(ghost);
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(t.map(shown));
                p.setPen(theme::selection().first);
                p.setFont(theme::font());
                p.drawText(QPointF(moveTo_) + QPointF(14, 14), QStringLiteral("%1°").arg(std::lround(std::fmod(picked.angle + d + 540, 360) - 180)));
            }
            if ((press_ == Press::MoveLine || press_ == Press::ScaleLine) && (moveTo_ - moveFrom_).manhattanLength() >= 3) {
                QPen ghost(Qt::black, 1, Qt::CustomDashLine);
                ghost.setCosmetic(true);
                ghost.setDashPattern({1, 1});
                p.setPen(ghost);
                p.setBrush(Qt::NoBrush);
                QRectF g = r;
                if (press_ == Press::MoveLine && (turned || block)) {  // (its outline, moved: Acrobat's box and guide lines)
                    drawMoveGuide(p, shown.translated(moveTo_ - moveFrom_), QRectF(viewport()->rect()), sel);
                    g = QRectF();
                } else if (press_ == Press::MoveLine) {
                    drawMoveGuide(p, QPolygonF(g.translated(moveTo_ - moveFrom_)), QRectF(viewport()->rect()), sel);
                    g = QRectF();
                } else {  // (bigger or smaller from the other end, its shape kept)
                    const double dx = (moveTo_ - moveFrom_).x();
                    const double k = std::max(0.2, (r.width() + (scaleRight_ ? dx : -dx)) / std::max(1.0, r.width()));
                    const double w = r.width() * k, h = r.height() * k;
                    g = scaleRight_ ? QRectF(r.left(), r.bottom() - h, w, h) : QRectF(r.right() - w, r.bottom() - h, w, h);
                }
                if (!g.isNull())
                    p.drawRect(g);
            }
        }
    }
    // the eraser's pointer: a ring its size (black and white: seen on anything)
    if (look_.tool == Tool::Eraser && look_.variant == QLatin1String("rub") && viewport()->rect().contains(hover_) &&
        viewport()->underMouse()) {
        const double r = std::max(2.0, look_.width * ppp / 2);
        const QPointF c(hover_);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawEllipse(c, r + 1, r + 1);
        p.setPen(QPen(Qt::black, 1));
        p.drawEllipse(c, r, r);
        if (r > 6) {
            p.drawLine(c - QPointF(2, 0), c + QPointF(3, 0));
            p.drawLine(c - QPointF(0, 2), c + QPointF(0, 3));
        }
    }
    p.restore();
}

// ---- the little toolbar over selected text (or a mark picked up) ----

void PageView::setFloatBar(QWidget *bar, int page, const std::vector<QRectF> &rects)
{
    if (bar_ && bar_ != bar)
        bar_->deleteLater();
    bar_ = bar;
    barPage_ = page;
    barRects_ = rects;
    if (bar) {
        bar->setParent(viewport());
        bar->adjustSize();
        bar->show();
        placeBar();
    }
}

void PageView::placeBar()
{
    if (editor_ && edit_.page >= 0 && edit_.page < int(pages_.size()))
        editor_->move(toView(edit_.page, edit_.at) - QPoint(2, 2));
    if (!bar_ || barPage_ < 0 || barPage_ >= int(pages_.size()) || barRects_.empty())
        return;
    // just over the boxes (under them when there's no room above); hidden while they're
    // scrolled out of sight
    QRect all;
    for (const QRectF &r : barRects_)
        all |= QRect(toView(barPage_, r.topLeft()), toView(barPage_, r.bottomRight()));
    if (!all.intersects(viewport()->rect())) {
        bar_->hide();
        return;
    }
    const QSize s = bar_->sizeHint();
    int y = all.top() - s.height() - 6;
    if (y < 2)
        y = all.bottom() + 6;
    const int x = std::clamp(all.left(), 2, std::max(2, viewport()->width() - s.width() - 2));
    bar_->resize(s);
    bar_->move(x, y);
    bar_->show();
    bar_->raise();
}

// ---- a text box's words, typed on the page ----

namespace {

// the box typed in: it grows as you type; clicking elsewhere (or Ctrl + Enter) puts the text
// on the page, Esc cancels
class TextBoxEdit : public QPlainTextEdit {
public:
    std::function<void(bool)> done;
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (done)
                done(false);
            return;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier)) {
            if (done)
                done(true);
            return;
        }
        if (const auto ar = keyboard::arabicDigit(e)) {  // (an Arabic keyboard's digits: Arabic's own)
            QPlainTextEdit::keyPressEvent(ar.get());
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        QPlainTextEdit::focusOutEvent(e);
        if (done && e->reason() != Qt::PopupFocusReason) {
            auto d = done;
            QTimer::singleShot(0, this, [d] { d(true); });
        }
    }
};

}  // namespace

void PageView::startTextBox(int page, QPointF at)
{
    openTextBox(page, 0, at, QString(), look_.width, look_.color);
}

void PageView::openTextBox(int page, int id, QPointF at, const QString &text, double size, const QColor &color)
{
    closeTextBox(true);
    edit_ = {page, id, at, size, color, text};
    auto *box = new TextBoxEdit(viewport());
    editor_ = box;
    QFont f(look_.tool == Tool::Text ? look_.family : QStringLiteral("Arial"));
    f.setPixelSize(std::max(6, int(std::lround(size * pixelsPerPoint()))));
    if (look_.tool == Tool::Text) {
        f.setBold(look_.bold);
        f.setItalic(look_.italic);
    }
    box->setFont(f);
    QPalette pal = box->palette();
    pal.setColor(QPalette::Base, QColor("#FFFFFE"));
    pal.setColor(QPalette::Text, color);
    box->setPalette(pal);
    box->setFrameStyle(QFrame::Box | QFrame::Plain);
    box->setLineWidth(1);
    box->setLineWrapMode(QPlainTextEdit::NoWrap);
    box->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    box->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    box->document()->setDocumentMargin(1);
    box->setPlainText(text);
    auto grow = [box] {  // (as big as its text)
        const QFontMetrics fm(box->font());
        int w = fm.horizontalAdvance(QLatin1Char('0')) * 12;
        const QStringList lines = box->toPlainText().split(QLatin1Char('\n'));
        for (const QString &line : lines)
            w = std::max(w, fm.horizontalAdvance(line) + fm.horizontalAdvance(QLatin1Char('0')) * 2);
        box->resize(w + 6, int(lines.size()) * fm.lineSpacing() + 8);
    };
    connect(box, &QPlainTextEdit::textChanged, box, grow);
    grow();
    box->done = [this](bool commit) { closeTextBox(commit); };
    box->move(toView(page, at) - QPoint(2, 2));
    box->show();
    box->setFocus();
    box->moveCursor(QTextCursor::End);
    emit message(tr("Type the text. Click outside the box (or press Ctrl + Enter) when you're done; Esc cancels."));
}

void PageView::closeTextBox(bool commit)
{
    if (!editor_)
        return;
    QPlainTextEdit *box = editor_;
    editor_ = nullptr;
    static_cast<TextBoxEdit *>(box)->done = nullptr;
    QString text = box->toPlainText();
    while (text.endsWith(QLatin1Char(' ')) || text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    box->hide();
    box->deleteLater();
    setFocus();
    const Editing e = edit_;
    edit_ = {};
    if (commit && text != e.was && (!text.isEmpty() || e.id))
        emit textBoxDone(e.page, e.id, e.at, text, e.size, e.color);
}

// ---- the Edit tool: lines picked up, moved, made bigger, retyped ----

const std::vector<TextLine> &PageView::linesOf(int page)
{
    if (page != linesPage_ || linesGen_ != serial_) {
        lines_ = doc_ && page >= 0 && page < doc_->pageCount() ? doc_->lines(page) : std::vector<TextLine>{};
        linesPage_ = page;
        linesGen_ = serial_;
    }
    return lines_;
}

const std::vector<pagetext::TextFrame> &PageView::framesOf(int page)
{
    const std::vector<TextLine> &lines = linesOf(page);
    if (page != framesPage_ || framesGen_ != serial_) {
        // (what's edited as one: each box as Acrobat boxes the text - its paragraphs, top down)
        frames_.clear();
        std::vector<bool> taken(lines.size(), false);
        for (const pagetext::TextBox &b : boxesOf(page)) {
            pagetext::TextFrame f;
            f.shown = b.shown;
            f.hidden = b.hidden;
            for (const std::vector<int> &para : b.paras) {
                f.paras.push_back(para);
                for (int k : para) {
                    f.lines.push_back(k);
                    f.rect |= lines[size_t(k)].rect;
                    taken[size_t(k)] = true;
                }
            }
            if (!f.lines.empty())
                frames_.push_back(f);
        }
        for (size_t k = 0; k < lines.size(); ++k)  // (any line no box holds: its own)
            if (!taken[k])
                frames_.push_back({{int(k)}, lines[k].rect, {{int(k)}}});
        // (turned lines - a block turned: its lines one box again, as the block they were. Each angle's
        // lines straightened round one point and grouped as straight ones are)
        {
            std::vector<double> angles;
            for (const TextLine &l : lines)
                if (std::abs(l.angle) > 0.01 &&
                    std::none_of(angles.begin(), angles.end(), [&](double a) { return std::abs(a - l.angle) < 0.3; }))
                    angles.push_back(l.angle);
            for (double a : angles) {
                QTransform un;  // (straightened round the page's corner)
                un.rotate(-a);
                std::vector<int> ids;
                std::vector<TextLine> straight;
                for (size_t k = 0; k < lines.size(); ++k) {
                    const TextLine &l = lines[k];
                    if (std::abs(l.angle) < 0.01 || std::abs(l.angle - a) >= 0.3)
                        continue;
                    QTransform t;  // (as shown)
                    t.translate(l.pivot.x(), l.pivot.y());
                    t.rotate(l.angle);
                    t.translate(-l.pivot.x(), -l.pivot.y());
                    const QTransform m = t * un;
                    TextLine s = l;
                    s.angle = 0;
                    s.rect = m.mapRect(l.rect);
                    s.baseline = m.map(QPointF(l.rect.left(), l.baseline)).y();
                    for (TextRun &r : s.runs) {
                        r.origin = m.map(r.origin);
                        r.box = m.mapRect(r.box);
                    }
                    ids.push_back(int(k));
                    straight.push_back(s);
                }
                if (ids.size() < 2)
                    continue;
                // (pieces of one line - side by side on one baseline, read apart: with their line)
                std::vector<std::vector<int>> with(ids.size());  // (the pieces each line takes along)
                std::vector<bool> piece(ids.size(), false);
                for (size_t i = 0; i < straight.size(); ++i)
                    for (size_t j = 0; j < straight.size(); ++j) {
                        if (i == j || piece[i] || piece[j])
                            continue;
                        const TextLine &x = straight[i], &y = straight[j];
                        const double sz = std::max(1.0, x.mainRun().size);
                        const double gap = y.rect.left() - x.rect.right();
                        if (std::abs(x.baseline - y.baseline) < 0.3 * sz && gap > -0.5 * sz && gap < 1.3 * sz) {
                            piece[j] = true;
                            with[i].push_back(ids[j]);
                            with[i].insert(with[i].end(), with[j].begin(), with[j].end());
                            with[j].clear();
                        }
                    }
                std::vector<int> mainIds;
                std::vector<TextLine> mains;
                std::vector<std::vector<int>> mainWith;
                for (size_t i = 0; i < straight.size(); ++i)
                    if (!piece[i]) {
                        mainIds.push_back(ids[i]);
                        mains.push_back(straight[i]);
                        mainWith.push_back(with[i]);
                    }
                std::set<int> turnedHere(ids.begin(), ids.end());
                std::vector<pagetext::TextFrame> keep;  // (the frames without these lines)
                for (pagetext::TextFrame &f : frames_) {
                    bool has = false;
                    for (int k : f.lines)
                        has = has || turnedHere.count(k);
                    if (!has)
                        keep.push_back(f);
                }
                for (const pagetext::TextFrame &g : pagetext::frames(mains)) {
                    pagetext::TextFrame f;
                    for (const std::vector<int> &para : g.paras) {
                        std::vector<int> p;
                        for (int i : para) {
                            p.push_back(mainIds[size_t(i)]);
                            p.insert(p.end(), mainWith[size_t(i)].begin(), mainWith[size_t(i)].end());
                        }
                        f.paras.push_back(p);
                        f.lines.insert(f.lines.end(), p.begin(), p.end());
                    }
                    for (int k : f.lines)
                        f.rect |= lines[size_t(k)].rect;
                    keep.push_back(f);
                }
                frames_ = std::move(keep);
            }
        }
        // (a box with lines over each other - a line of the page read in pieces, paper_2's: its finer
        // blocks instead - as one, its pieces came out twice and words were lost)
        std::vector<bool> messy(lines.size(), false);
        for (size_t a = 0; a < lines.size(); ++a)
            for (size_t b = a + 1; b < lines.size(); ++b) {
                if (std::abs(lines[a].angle) > 0.01 || std::abs(lines[b].angle) > 0.01)
                    continue;  // (turned: their straightened boxes aren't where they're shown)
                const QRectF &ra = lines[a].rect, &rb = lines[b].rect;
                const QRectF x = ra & rb;
                if (!x.isEmpty() && x.width() * x.height() > 0.3 * std::min(ra.width() * ra.height(), rb.width() * rb.height()))
                    messy[a] = messy[b] = true;
            }
        std::vector<pagetext::TextFrame> fine;
        bool anyMessy = false;
        for (bool m : messy)
            anyMessy = anyMessy || m;
        if (anyMessy) {
            const std::vector<pagetext::TextFrame> raw = pagetext::frames(lines);
            std::vector<pagetext::TextFrame> out;
            for (const pagetext::TextFrame &f : frames_) {
                bool bad = false;
                for (int k : f.lines)
                    bad = bad || messy[size_t(k)];
                if (!bad) {
                    out.push_back(f);
                    continue;
                }
                std::set<int> mine(f.lines.begin(), f.lines.end());
                for (const pagetext::TextFrame &r : raw) {  // (its lines as the finer blocks group them)
                    pagetext::TextFrame g;
                    for (const std::vector<int> &para : r.paras) {
                        std::vector<int> part;
                        for (int k : para)
                            if (mine.count(k))
                                part.push_back(k);
                        if (part.empty())
                            continue;
                        g.paras.push_back(part);
                        for (int k : part) {
                            g.lines.push_back(k);
                            g.rect |= lines[size_t(k)].rect;
                            mine.erase(k);
                        }
                    }
                    if (!g.lines.empty())
                        out.push_back(g);
                }
                for (int k : mine)
                    out.push_back({{k}, lines[size_t(k)].rect, {{k}}});
            }
            frames_ = std::move(out);
        }
        framesPage_ = page;
        framesGen_ = serial_;
    }
    return frames_;
}

const std::vector<pagetext::TextBox> &PageView::boxesOf(int page)
{
    if (boxesGen_ != serial_) {
        boxes_.clear();
        boxesGen_ = serial_;
    }
    auto it = boxes_.find(page);
    if (it == boxes_.end()) {
        const std::vector<TextLine> lines = linesOf(page);
        const std::vector<pagetext::TextFrame> fs = pagetext::frames(lines);
        std::vector<QLineF> rules;
        const std::vector<pagetext::StructMark> marks = doc_ ? doc_->structureMarks(page, &rules) : std::vector<pagetext::StructMark>{};
        it = boxes_.emplace(page, pagetext::boxes(lines, fs, marks, rules)).first;
    }
    return it->second;
}

std::vector<int> PageView::frameLines(int page, int line)
{
    const std::vector<pagetext::TextFrame> &fs = framesOf(page);
    const int f = pagetext::frameOf(fs, line);
    return f < 0 ? std::vector<int>{line} : fs[size_t(f)].lines;
}

int PageView::lineAt(int page, QPointF at, bool inBox)
{
    const std::vector<TextLine> &lines = linesOf(page);
    const double slack = 2 / pixelsPerPoint();
    for (int n = int(lines.size()); n-- > 0;) {  // (lines over each other: the one written last - on top)
        const TextLine &l = lines[size_t(n)];
        QPointF p = at;
        if (std::abs(l.angle) > 0.01) {  // (turned: the point turned back, as the line is read)
            QTransform back;
            back.translate(l.pivot.x(), l.pivot.y());
            back.rotate(-l.angle);
            back.translate(-l.pivot.x(), -l.pivot.y());
            p = back.map(at);
        }
        if (l.rect.adjusted(-slack, -slack, slack, slack).contains(p))
            return n;
    }
    // (not on a line, but inside a block's box as shown - beside its short lines, between them: the block's nearest
    // line, as Acrobat picks; a picture there first)
    if (!inBox)
        return -1;
    if (const int pic = pictureAt(page, at); pic >= 0) {  // (not a page's background)
        const QRectF r = picturesOf(page)[size_t(pic)].rect;
        const QSizeF s = doc_->pageSize(page);
        if (r.width() * r.height() < 0.5 * s.width() * s.height())
            return -1;
    }
    const std::vector<pagetext::TextFrame> &fs = framesOf(page);
    const double pad = 2 / pixelsPerPoint();
    for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
        const pagetext::TextFrame &f = *it;
        if (f.hidden || f.lines.empty())
            continue;
        QPolygonF box;
        if (std::abs(lines[size_t(f.lines.front())].angle) > 0.01) {
            box = frameOutline(page, f);
        } else {
            const QRectF sized = sizedBox(page, f.rect);
            const QRectF r = (sized == f.rect && !f.shown.isNull() ? f.shown : sized).adjusted(-pad, -pad, pad, pad);
            box = QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
        }
        if (!box.containsPoint(at, Qt::OddEvenFill))
            continue;
        int best = -1;
        double bestFar = 0;
        for (int k : f.lines) {
            const QRectF r = lines[size_t(k)].rect;
            const double dy = std::max({0.0, r.top() - at.y(), at.y() - r.bottom()});
            const double dx = std::max({0.0, r.left() - at.x(), at.x() - r.right()});
            const double far = dy * 1000 + dx;  // (the line beside it, then the nearest across)
            if (best < 0 || far < bestFar)
                best = k, bestFar = far;
        }
        return best;
    }
    return -1;
}

QPointF PageView::lineTurnHandle()
{
    // over the middle of the line's top, as shown, on a stalk (as a picture's)
    if (lineIndex_ < 0 || linePage_ < 0 || lineIndex_ >= int(linesOf(linePage_).size()))
        return {};
    if (frameLines(linePage_, lineIndex_).size() > 1) {  // (a block: over the middle of its box's top)
        const QPolygonF b = pickedTextOutline();
        if (b.size() != 4)
            return {};
        const QPointF top = (b[0] + b[1]) / 2, up = top - (b[3] + b[2]) / 2;  // (straight up from its top, as turned)
        const double len = std::hypot(up.x(), up.y());
        return len < 0.5 ? top - QPointF(0, 22) : top + up / len * 22;
    }
    const QPolygonF c = linesOf(linePage_)[size_t(lineIndex_)].shown();
    const QPointF top = (QPointF(toView(linePage_, c[0])) + QPointF(toView(linePage_, c[1]))) / 2;
    const QPointF bottom = (QPointF(toView(linePage_, c[3])) + QPointF(toView(linePage_, c[2]))) / 2;
    const QPointF up = top - bottom;
    const double len = std::hypot(up.x(), up.y());
    return len < 0.5 ? top - QPointF(0, 22) : top + up / len * 22;
}

double PageView::lineTurnDegrees() const
{
    // (round the line's middle as shown: from where the handle was taken to where it is)
    if (lineIndex_ < 0 || linePage_ < 0)
        return 0;
    // (a block: round its box's middle)
    const QPolygonF box = const_cast<PageView *>(this)->pickedTextOutline();
    const QPolygonF c = const_cast<PageView *>(this)->linesOf(linePage_)[size_t(lineIndex_)].shown();
    const QPointF m = const_cast<PageView *>(this)->frameLines(linePage_, lineIndex_).size() > 1 && box.size() == 4
                          ? (box[0] + box[2]) / 2
                          : QPointF(toView(linePage_, (c[0] + c[2]) / 2));
    const double a0 = std::atan2(moveFrom_.y() - m.y(), moveFrom_.x() - m.x());
    const double a1 = std::atan2(moveTo_.y() - m.y(), moveTo_.x() - m.x());
    double d = (a1 - a0) * 180 / M_PI;
    while (d > 180)
        d -= 360;
    while (d < -180)
        d += 360;
    return d;
}

QPolygonF PageView::pickedTextOutline()
{
    if (lineIndex_ < 0 || linePage_ < 0 || lineIndex_ >= int(linesOf(linePage_).size()))
        return {};
    const TextLine &picked = linesOf(linePage_)[size_t(lineIndex_)];
    if (frameLines(linePage_, lineIndex_).size() > 1 && std::abs(picked.angle) > 0.01) {  // (a turned block: round all of it, turned)
        QPolygonF shown;
        for (const QPointF &c : frameOutline(linePage_, framesOf(linePage_)[size_t(pagetext::frameOf(framesOf(linePage_), lineIndex_))]))
            shown << QPointF(toView(linePage_, c));
        return shown;
    }
    if (frameLines(linePage_, lineIndex_).size() > 1) {  // (a block: round all of it)
        const QRectF b = sizedBox(linePage_, framesOf(linePage_)[size_t(pagetext::frameOf(framesOf(linePage_), lineIndex_))].rect).adjusted(-1, -1, 1, 1);
        const QRectF r = QRectF(QPointF(toView(linePage_, b.topLeft())), QPointF(toView(linePage_, b.bottomRight()))).adjusted(-2, -2, 2, 2);
        return QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
    }
    if (std::abs(picked.angle) > 0.01) {
        QPolygonF shown;
        for (const QPointF &c : picked.shown())
            shown << QPointF(toView(linePage_, c));
        return shown;
    }
    const QRectF b = sizedBox(linePage_, picked.rect);
    const QRectF r = QRectF(QPointF(toView(linePage_, b.topLeft())), QPointF(toView(linePage_, b.bottomRight()))).adjusted(-2, -2, 2, 2);
    return QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
}

QPolygonF PageView::frameOutline(int page, const pagetext::TextFrame &f)
{
    const std::vector<TextLine> &lines = linesOf(page);
    if (f.lines.empty())
        return {};
    const TextLine &first = lines[size_t(f.lines.front())];
    if (std::abs(first.angle) < 0.01) {
        const QRectF r = sizedBox(page, f.rect);
        return QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
    }
    // (turned: its lines as shown, straightened round one point, boxed, turned back)
    QTransform t;
    t.translate(first.pivot.x(), first.pivot.y());
    t.rotate(first.angle);
    t.translate(-first.pivot.x(), -first.pivot.y());
    const QTransform un = t.inverted();
    QRectF r;
    for (int k : f.lines)
        r |= un.map(lines[size_t(k)].shown()).boundingRect();
    r.adjust(-1, -1, 1, 1);
    return t.map(QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}));
}

QRectF PageView::pickedTextBox()
{
    return frameLines(linePage_, lineIndex_).size() > 1 ? framesOf(linePage_)[size_t(pagetext::frameOf(framesOf(linePage_), lineIndex_))].rect
                                                        : linesOf(linePage_)[size_t(lineIndex_)].rect;
}

QRectF PageView::draggedBox()
{
    const QRectF text = pickedTextBox();
    QRectF box = sizedBox(linePage_, text);
    const QPointF d = QPointF(moveTo_ - moveFrom_) / pixelsPerPoint();
    const int h = textHandle_;
    const double least = 3 * linesOf(linePage_)[size_t(lineIndex_)].mainRun().size;  // (a few letters wide, at least)
    if (h == 0 || h == 6 || h == 7)
        box.setLeft(std::min(box.right() - least, box.left() + d.x()));
    if (h == 2 || h == 3 || h == 4)
        box.setRight(std::max(box.left() + least, box.right() + d.x()));
    if (h == 0 || h == 1 || h == 2)  // (taller or shorter - never over its text: it isn't moved)
        box.setTop(std::min(text.top(), box.top() + d.y()));
    if (h == 4 || h == 5 || h == 6)
        box.setBottom(std::max(text.bottom(), box.bottom() + d.y()));
    return box;
}

QRectF PageView::shownBox(const QRectF &box, bool block)
{
    // (as the picked box's outline is drawn: a block's a point out, and 2 pixels more)
    const QRectF b = block ? box.adjusted(-1, -1, 1, 1) : box;
    return QRectF(QPointF(toView(linePage_, b.topLeft())), QPointF(toView(linePage_, b.bottomRight()))).adjusted(-2, -2, 2, 2);
}

QRectF PageView::sizedBox(int page, const QRectF &text) const
{
    const auto it = boxSizes_.find(page);
    if (it != boxSizes_.end())
        for (const auto &[was, now] : it->second)
            if (std::abs(was.left() - text.left()) < 0.5 && std::abs(was.top() - text.top()) < 0.5 &&
                std::abs(was.right() - text.right()) < 0.5 && std::abs(was.bottom() - text.bottom()) < 0.5)
                return now;
    return text;
}

int PageView::edgeAt(QPoint pos, int *page)
{
    // (its outline, a few pixels either side - not a handle)
    auto onEdge = [&](const QRectF &r) {
        return r.adjusted(-4, -4, 4, 4).contains(pos) && !r.adjusted(4, 4, -4, -4).contains(pos);
    };
    {  // (on a line's letters: its text - typed in, never an edge; lines over each other put one's
       // edge across another's words)
        QPointF at;
        const int pg = pageAt(pos, &at);
        if (pg >= 0)
            for (const TextLine &l : linesOf(pg))
                if (std::abs(l.angle) < 0.01 ? l.rect.adjusted(0, l.rect.height() * 0.25, 0, -l.rect.height() * 0.25).contains(at)
                                             : l.shown().containsPoint(at, Qt::OddEvenFill))
                    return -1;  // (its letters' middle band - a box's edge just over a line next to it still an edge)
    }
    if (lineIndex_ >= 0 && linePage_ >= 0 && lineIndex_ < int(linesOf(linePage_).size()) &&
        std::abs(linesOf(linePage_)[size_t(lineIndex_)].angle) < 0.01) {
        const QPolygonF poly = pickedTextOutline();
        if (poly.size() == 4 && onEdge(poly.boundingRect())) {
            *page = linePage_;
            return lineIndex_;
        }
    }
    QPointF at;
    const int pg = pageAt(pos, &at);
    if (pg < 0)
        return -1;
    const std::vector<TextLine> &lines = linesOf(pg);
    for (const pagetext::TextFrame &f : framesOf(pg)) {
        if (f.lines.empty() || std::abs(lines[size_t(f.lines.front())].angle) > 0.01)
            continue;
        const QRectF b = sizedBox(pg, f.rect);
        const QRectF r = QRectF(QPointF(toView(pg, b.topLeft())), QPointF(toView(pg, b.bottomRight()))).adjusted(-3, -3, 3, 3);
        if (onEdge(r)) {
            *page = pg;
            return f.lines.front();
        }
    }
    return -1;
}

int PageView::handleAt(QPoint pos)
{
    if (lineIndex_ < 0 || linePage_ < 0 || lineIndex_ >= int(linesOf(linePage_).size()))
        return -1;
    const QPointF th = lineTurnHandle();
    if (!th.isNull() && (QPointF(pos) - th).manhattanLength() <= 9)
        return 8;
    if (std::abs(linesOf(linePage_)[size_t(lineIndex_)].angle) > 0.01)
        return -1;  // (turned: turned, moved and retyped - not resized)
    const QPolygonF poly = pickedTextOutline();
    if (poly.size() != 4)
        return -1;
    const std::array<QPointF, 8> spots = handleSpots(poly);
    for (int k = 0; k < 8; ++k)
        if ((spots[size_t(k)] - QPointF(pos)).manhattanLength() <= 6)
            return k;
    return -1;
}

void PageView::pickLine(int page, int index)
{
    if (page == linePage_ && index == lineIndex_)
        return;
    linePage_ = index >= 0 ? page : -1;
    lineIndex_ = index;
    viewport()->update();
    emit linePicked(linePage_, lineIndex_);
}

// ---- several things picked at once ----

QRectF PageView::groupBox(int page, const GroupItem &g)
{
    if (g.picture) {
        const std::vector<Picture> &pics = picturesOf(page);
        if (g.index < 0 || g.index >= int(pics.size()))
            return {};
        QPolygonF corners;
        for (const QPointF &c : pics[size_t(g.index)].corners)
            corners << c;
        return corners.isEmpty() ? pics[size_t(g.index)].rect : corners.boundingRect();
    }
    if (g.index < 0 || g.index >= int(linesOf(page).size()))
        return {};
    if (frameLines(page, g.index).size() > 1)  // (a block of paragraphs: all of it)
        return framesOf(page)[size_t(pagetext::frameOf(framesOf(page), g.index))].rect;
    return linesOf(page)[size_t(g.index)].shown().boundingRect();
}

int PageView::groupAt(int page, QPointF at)
{
    if (page != groupPage_)
        return -1;
    for (int k = 0; k < int(group_.size()); ++k)
        if (groupBox(page, group_[size_t(k)]).adjusted(-2, -2, 2, 2).contains(at))
            return k;
    return -1;
}

void PageView::clearGroup()
{
    if (group_.empty() && groupPage_ < 0)
        return;
    group_.clear();
    groupPage_ = -1;
    viewport()->update();
    emit groupChanged(0);
}

bool PageView::groupClick(QPoint pos)
{
    QPointF at;
    const int page = pageAt(pos, &at);
    if (page < 0)
        return false;
    // what's there: a block of text (its first line - the block is one thing), or a picture
    GroupItem it;
    int line = lineAt(page, at);
    if (line >= 0 && overPicture(page, at) >= 0)
        line = -1;  // (a picture drawn over the text: the picture, as a click picks)
    if (line >= 0) {
        it = {false, frameLines(page, line).front()};
    } else {
        const int pic = pictureAt(page, at);
        if (pic < 0)
            return false;
        it = {true, pic};
    }
    if (groupPage_ != page)
        group_.clear();
    if (group_.empty()) {  // (what's picked up already: in it too)
        if (lineIndex_ >= 0 && linePage_ == page && lineIndex_ < int(linesOf(page).size()))
            group_.push_back({false, frameLines(page, lineIndex_).front()});
        else if (picIndex_ >= 0 && picPage_ == page && picIndex_ < int(picturesOf(page).size()))
            group_.push_back({true, picIndex_});
    }
    groupPage_ = page;
    const auto there = std::find(group_.begin(), group_.end(), it);
    if (there != group_.end())
        group_.erase(there);  // (picked already: put down)
    else
        group_.push_back(it);
    pickLine(-1, -1);
    pickPicture(-1, -1);
    if (group_.size() == 1) {  // (just one: picked up as a click picks it)
        const GroupItem one = group_.front();
        group_.clear();
        groupPage_ = -1;
        if (one.picture)
            pickPicture(page, one.index);
        else
            pickLine(page, one.index);
    } else if (group_.empty()) {
        groupPage_ = -1;
    }
    viewport()->update();
    emit groupChanged(int(group_.size()));
    return true;
}

void PageView::pickGroup(int page, const std::vector<QRectF> &texts, const std::vector<QRectF> &pictures)
{
    // (each box: the text or picture nearest it - its middle, its size)
    auto far = [](const QRectF &a, const QRectF &b) {
        return std::abs(a.center().x() - b.center().x()) + std::abs(a.center().y() - b.center().y()) +
               std::abs(a.width() - b.width()) + std::abs(a.height() - b.height());
    };
    group_.clear();
    groupPage_ = page;
    pickLine(-1, -1);
    pickPicture(-1, -1);
    for (const QRectF &r : texts) {
        GroupItem best;
        double d = 12;  // (points: further, it's not there)
        for (int i = 0; i < int(linesOf(page).size()); ++i) {
            const GroupItem g{false, frameLines(page, i).front()};
            const double k = far(groupBox(page, g), r);
            if (k < d && std::find(group_.begin(), group_.end(), g) == group_.end()) {
                d = k;
                best = g;
            }
        }
        if (best.index >= 0)
            group_.push_back(best);
    }
    for (const QRectF &r : pictures) {
        GroupItem best;
        double d = 12;
        for (int i = 0; i < int(picturesOf(page).size()); ++i) {
            const GroupItem g{true, i};
            const double k = far(groupBox(page, g), r);
            if (k < d && std::find(group_.begin(), group_.end(), g) == group_.end()) {
                d = k;
                best = g;
            }
        }
        if (best.index >= 0)
            group_.push_back(best);
    }
    if (group_.size() < 2) {
        const std::vector<GroupItem> one = group_;
        group_.clear();
        groupPage_ = -1;
        if (!one.empty()) {
            if (one.front().picture)
                pickPicture(page, one.front().index);
            else
                pickLine(page, one.front().index);
        }
    }
    viewport()->update();
    emit groupChanged(int(group_.size()));
}

void PageView::editPress(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    // Ctrl / Shift + click: one more thing picked with what's picked (or put down again)
    if (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        groupClick(pos);
        return;
    }
    if (!group_.empty()) {  // (a press on one of them: all of them dragged - anywhere else: they're put down)
        QPointF at;
        const int page = pageAt(pos, &at);
        if (page >= 0 && groupAt(page, at) >= 0) {
            press_ = Press::MoveGroup;
            moveFrom_ = moveTo_ = pos;
            return;
        }
        clearGroup();
    }
    const int ph = pictureHandleAt(pos);
    if (ph >= 0) {  // the picture's corner (resize) or its rotate handle (turn)
        picHandle_ = ph;
        press_ = ph == 8 ? Press::TurnPic : Press::SizePic;
        moveFrom_ = moveTo_ = pos;
        return;
    }
    const int h = handleAt(pos);
    if (h == 8) {  // the rotate handle: turned
        press_ = Press::TurnLine;
        moveFrom_ = moveTo_ = pos;
        return;
    }
    if (h >= 0) {  // a handle round its box: resized (its text wrapped again, or made bigger / smaller)
        press_ = Press::SizeText;
        textHandle_ = h;
        moveFrom_ = moveTo_ = pos;
        return;
    }
    {  // a box's edge: picked up and dragged about (a click there just picks it)
        int edgePage = -1;
        const int edge = edgeAt(pos, &edgePage);
        if (edge >= 0) {
            pickPicture(-1, -1);
            again_ = false;
            pickLine(edgePage, edge);
            press_ = Press::MoveLine;
            edgeMove_ = true;
            moveFrom_ = moveTo_ = pos;
            return;
        }
    }
    QPointF at;
    const int page = pageAt(pos, &at);
    int index = page >= 0 ? lineAt(page, at) : -1;
    if (index >= 0 && overPicture(page, at) >= 0)
        index = -1;  // (a picture drawn over the text: the picture, as Word picks)
    if (index < 0) {
        pickLine(-1, -1);
        const int pic = page >= 0 ? pictureAt(page, at) : -1;
        pickPicture(page, pic);
        if (pic >= 0) {  // a picture: picked up, and dragged about
            press_ = Press::MovePic;
            moveFrom_ = moveTo_ = pos;
        }
        return;
    }
    pickPicture(-1, -1);
    again_ = page == linePage_ && index == lineIndex_;
    pickLine(page, index);
    press_ = Press::MoveLine;
    moveFrom_ = moveTo_ = pos;
}

namespace {

// (the editor's: how far below its top its text is drawn - the part of a pixel its place can't have)
constexpr const char *kDropProp = "drop";

}  // namespace

namespace {

// the caret's colour: the text's where it is (the theme's text colour is light in a dark theme -
// on the page's white it was barely seen)
QColor caretColor(const QTextCursor &c)
{
    const QBrush b = c.charFormat().foreground();
    const QColor col = b.style() != Qt::NoBrush ? b.color() : QColor(Qt::black);
    return col.lightness() > 200 ? QColor(Qt::black) : col;  // (white text: drawn black, to be seen)
}

// its box: whole screen pixels, one or more - a pixel and a quarter (125%) was spread over two, grey
QRectF caretBox(const QTextEdit *box)
{
    const QRect r = box->cursorRect();
    const qreal dpr = box->viewport()->devicePixelRatioF();
    const qreal w = std::max<qreal>(1, std::round(dpr * std::max(1, box->cursorWidth()))) / dpr;
    const qreal x = std::round(r.left() * dpr) / dpr;
    return QRectF(x, r.top(), w, r.height());
}

// the box a line is retyped in: Enter puts it on the page, Esc cancels, clicking away too
// (a block's paragraph in the editor: justified on the page, and its lines after the first - their indent)
const int kReflowJustify = QTextFormat::UserProperty + 77;
const int kReflowRest = QTextFormat::UserProperty + 78;
const int kReflowRtl = QTextFormat::UserProperty + 79;  // (a paragraph right to left: its lines after the first in from the right)

class LineEdit : public QTextEdit {
    bool caretOn_ = true;
    QTimer blink_;

public:
    std::function<void(bool)> done;
    std::function<bool(QWidget *)> keep;
    bool paragraphs = false;  // (a block of paragraphs: Enter starts another, Ctrl+Enter puts it in)
    std::function<void()> followCaret;  // (a key pressed: the view brought to the caret - scrolled away, it's shown again)

    // Its own Undo / Redo: each step what was typed, deleted, restyled in it - the whole text as it was before, taken
    // back to the very first (Qt's own kept the editor's own changes too - its letters laid out again at another zoom,
    // its paragraphs flowing again once typed in: Ctrl+Z took those back, not the typing). A word typed: one step.
    double scale = 1;                      // (the zoom it's laid out at: pixels a point)
    std::function<void(double)> rescaled;  // (a step taken back laid out at another zoom: brought to this one)
    int quiet = 0;                         // (the editor's own changes going on: not steps)
    void startHistory()
    {
        document()->setUndoRedoEnabled(false);
        undo_.clear();
        redo_.clear();
        settled_ = snap();
        connect(document(), &QTextDocument::contentsChange, this, &LineEdit::changed);
    }
    bool step(bool redo)
    {
        auto &from = redo ? redo_ : undo_;
        auto &to = redo ? undo_ : redo_;
        if (from.empty())
            return false;
        to.push_back(snap());
        const Snap s = from.back();
        from.pop_back();
        ++quiet;
        {
            QTextCursor c(document());
            c.select(QTextCursor::Document);
            c.insertFragment(QTextDocumentFragment(s.doc.get()));
        }
        --quiet;
        const int last = document()->characterCount() - 1;
        QTextCursor k(document());
        k.setPosition(std::clamp(s.anchor, 0, last));
        k.setPosition(std::clamp(s.pos, 0, last), QTextCursor::KeepAnchor);
        setTextCursor(k);
        if (std::abs(s.scale - scale) > 1e-9 && rescaled)
            rescaled(s.scale);
        settled_ = snap();
        lastEdit_ = 0;
        return true;
    }
    explicit LineEdit(QWidget *parent) : QTextEdit(parent)
    {
        // (its caret drawn here, blinking as Windows' does - the letters are drawn here too)
        blink_.setInterval(QApplication::cursorFlashTime() > 0 ? QApplication::cursorFlashTime() / 2 : 530);
        connect(&blink_, &QTimer::timeout, this, &LineEdit::blink);
        blink_.start();
        connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
            caretOn_ = true;
            blink_.start();
            viewport()->update();
        });
    }

protected:
    // its letters drawn on a see-through picture first: grey edges, as the page draws them (straight
    // on the window Windows gives them ClearType's coloured edges - they looked redrawn, bolder)
    void paintEvent(QPaintEvent *e) override
    {
        const qreal dpr = viewport()->devicePixelRatioF();
        QImage im((QSizeF(viewport()->size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        im.setDevicePixelRatio(dpr);
        im.fill(Qt::transparent);
        {
            QPainter p(&im);
            p.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value() + property(kDropProp).toDouble());
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.palette = palette();
            ctx.clip = QRectF(e->rect()).translated(horizontalScrollBar()->value(), verticalScrollBar()->value());
            const QTextCursor c = textCursor();
            if (c.hasSelection()) {
                QAbstractTextDocumentLayout::Selection sel;
                sel.cursor = c;
                sel.format.setBackground(palette().brush(QPalette::Highlight));
                sel.format.setForeground(palette().brush(QPalette::HighlightedText));
                ctx.selections << sel;
            }
            // (letters made wider / narrower - a scan's line as wide as the scan's: Windows' fonts in Qt get their spacing
            // made so, not the letters themselves - joined Arabic came apart, white gaps between its letters. Their places
            // left empty here, they're drawn below, each letter made as wide as its spacing is)
            struct Stretched {
                int from, length;
                double scale;
                QColor color;
            };
            std::vector<Stretched> stretched;
            for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
                for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat f = it.fragment().charFormat();
                    const double shownScale = f.hasProperty(kShownScaleProp) ? f.property(kShownScaleProp).toDouble() : 100;
                    if (std::abs(shownScale - 100) > 0.01) {
                        const QBrush fg = f.foreground();
                        stretched.push_back({it.fragment().position(), it.fragment().length(), shownScale / 100.0,
                                             fg.style() != Qt::NoBrush ? fg.color() : palette().color(QPalette::Text)});
                        QAbstractTextDocumentLayout::Selection none;
                        none.cursor = QTextCursor(document());
                        none.cursor.setPosition(it.fragment().position());
                        none.cursor.setPosition(it.fragment().position() + it.fragment().length(), QTextCursor::KeepAnchor);
                        none.format.setForeground(Qt::transparent);
                        ctx.selections << none;
                    }
                }
            document()->documentLayout()->draw(&p, ctx);
            for (const Stretched &s : stretched)
                drawStretched(p, s.from, s.length, s.scale, s.color, c);
        }
        QPainter p(viewport());
        p.drawImage(QPoint(0, 0), im);
        if (hasFocus() && caretOn_ && cursorWidth() > 0 && !textCursor().hasSelection())
            p.fillRect(caretBox(this), caretColor(textCursor()));
    }
    // (the letters from .. from + length, made scale times as wide as the font draws them, each from its own place - as
    // the spacing Qt gave them is: joined, as on the page; the selected ones in the selection's colour)
    void drawStretched(QPainter &p, int from, int length, double scale, const QColor &color, const QTextCursor &sel)
    {
        const QTextBlock block = document()->findBlock(from);
        const QTextLayout *layout = block.layout();
        if (!layout)
            return;
        const QPointF origin = document()->documentLayout()->blockBoundingRect(block).topLeft();
        // (the selected part and the rest: each in its colour)
        std::vector<std::tuple<int, int, QColor>> parts;
        const int a = std::max(from, sel.selectionStart()), z = std::min(from + length, sel.selectionEnd());
        if (sel.hasSelection() && a < z) {
            if (a > from)
                parts.emplace_back(from, a - from, color);
            parts.emplace_back(a, z - a, palette().color(QPalette::HighlightedText));
            if (z < from + length)
                parts.emplace_back(z, from + length - z, color);
        } else {
            parts.emplace_back(from, length, color);
        }
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const auto &[at, n, col] : parts) {
            p.setPen(col);
            for (const QGlyphRun &run : layout->glyphRuns(at - block.position(), n)) {
                const QList<quint32> glyphs = run.glyphIndexes();
                const QList<QPointF> places = run.positions();
                for (qsizetype i = 0; i < glyphs.size(); ++i) {
                    QGlyphRun one;
                    one.setRawFont(run.rawFont());
                    one.setGlyphIndexes({glyphs[i]});
                    one.setPositions({QPointF(0, 0)});
                    p.save();
                    p.translate(origin + places[i]);
                    p.scale(scale, 1);
                    p.drawGlyphRun(QPointF(0, 0), one);
                    p.restore();
                }
            }
        }
        p.restore();
    }
    void blink()
    {
        caretOn_ = !caretOn_;
        viewport()->update(cursorRect().adjusted(-2, -2, 2, 2));
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->matches(QKeySequence::Undo) || e->matches(QKeySequence::Redo)) {
            undoInPlace(this, e->matches(QKeySequence::Redo));
            return;
        }
        if (e->key() == Qt::Key_Escape && done) {
            done(false);
            return;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && paragraphs && !(e->modifiers() & Qt::ControlModifier)) {
            QTextEdit::keyPressEvent(e);
            follow();
            return;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && done) {
            done(true);
            return;
        }
        if (const auto ar = keyboard::arabicDigit(e)) {  // (an Arabic keyboard's digits: Arabic's own)
            QTextEdit::keyPressEvent(ar.get());
            follow();
            return;
        }
        QTextEdit::keyPressEvent(e);
        follow();
    }
    void follow()
    {
        if (followCaret)  // (once it's laid out again, in its place)
            QTimer::singleShot(0, this, [this] {
                if (followCaret)
                    followCaret();
            });
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        QTextEdit::focusOutEvent(e);
        // (a menu, a dialog - the colour's -, or the text bar: still being typed)
        if (!done || e->reason() == Qt::PopupFocusReason || e->reason() == Qt::ActiveWindowFocusReason)
            return;
        auto d = done;
        auto k = keep;
        QTimer::singleShot(0, this, [this, d, k] {
            if (hasFocus() || (k && k(QApplication::focusWidget())))  // (focus back on it: a click on its turned text)
                return;
            d(true);
        });
    }

private:
    struct Snap {
        std::shared_ptr<QTextDocument> doc;  // (its text and looks, as they were)
        int anchor = 0, pos = 0;             // (the caret, the selection)
        double scale = 1;
    };
    std::vector<Snap> undo_, redo_;
    Snap settled_;  // (as it is now - once the editor's own changes after a step are done too: the step's "before")
    qint64 lastEdit_ = 0;
    int lastEnd_ = -1;
    bool settling_ = false;
    Snap snap() const
    {
        Snap s;
        s.doc.reset(document()->clone());
        s.anchor = textCursor().anchor();
        s.pos = textCursor().position();
        s.scale = scale;
        return s;
    }
    void changed(int pos, int removed, int added)
    {
        if (quiet)
            return;
        // (a letter typed right after the last, quickly, within a word: the same step - a word, a step)
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool sameWord = removed == 0 && added == 1 && pos == lastEnd_ && now - lastEdit_ < 1500 && !undo_.empty() &&
                              !document()->characterAt(pos).isSpace() && !document()->characterAt(pos - 1).isSpace();
        if (!sameWord) {
            undo_.push_back(settled_);
            undo_.back().anchor = pos;  // (taken back: the caret where it was typed - deleted: after what comes back)
            undo_.back().pos = pos + removed;
        }
        redo_.clear();
        lastEdit_ = now;
        lastEnd_ = pos + added;
        if (!settling_) {  // (after the editor's own changes it set going: its paragraph flowing again)
            settling_ = true;
            QTimer::singleShot(0, this, [this] {
                settling_ = false;
                settled_ = snap();
            });
        }
    }
};

}  // namespace

void undoInPlace(QTextEdit *box, bool redo)
{
    if (auto *edit = dynamic_cast<LineEdit *>(box)) {  // (the page's text being typed in: its own steps)
        edit->step(redo);
        return;
    }
    // (another box: Qt's - a look taken back - bold, a font -: Qt put the caret at the box's start; it's kept where it
    // was. Letters taken back: the caret where Qt puts it, by them)
    const QString text = box->toPlainText();
    const QTextCursor was = box->textCursor();
    if (redo)
        box->redo();
    else
        box->undo();
    if (box->toPlainText() == text) {
        QTextCursor c(box->document());
        c.setPosition(std::min(was.anchor(), box->document()->characterCount() - 1));
        c.setPosition(std::min(was.position(), box->document()->characterCount() - 1), QTextCursor::KeepAnchor);
        box->setTextCursor(c);
    }
}

namespace {

// a letter's look in the editor <-> a run's (its size in points and its font's own family kept
// as properties: the editor shows them at the view's zoom)
constexpr int kSizeProp = QTextFormat::UserProperty + 1, kFamilyProp = QTextFormat::UserProperty + 2;
// (its letters' width %, more room after each - 1/1000 of its size -, superscript 1 / subscript -1)
constexpr int kScaleProp = QTextFormat::UserProperty + 3, kTrackProp = QTextFormat::UserProperty + 4,
              kRiseProp = QTextFormat::UserProperty + 5;
// (shown only, never written: points more after each letter, so a word as wide as the page draws it -
// the page's letter spacing, or a stand-in font a little narrower: its letters don't creep over)
constexpr int kFitProp = QTextFormat::UserProperty + 6;
// (bold by its outline - as the page draws it: the outline's width, part of its size)
constexpr int kOutlineProp = QTextFormat::UserProperty + 9;

// a font's own width, as Qt measures stretch (100: normal) - read from its face's name: "Condensed", "Narrow",
// "Expanded"... (asked for at another width, Qt makes its letters that wide itself)
int ownStretch(const QFont &font)
{
    const QFontInfo info(font);
    const QString name = (info.styleName() + QLatin1Char(' ') + info.family()).toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    if (name.contains(QLatin1String("ultracondensed")) || name.contains(QLatin1String("ultracompressed")))
        return QFont::UltraCondensed;
    if (name.contains(QLatin1String("extracondensed")) || name.contains(QLatin1String("extracompressed")))
        return QFont::ExtraCondensed;
    if (name.contains(QLatin1String("semicondensed")) || name.contains(QLatin1String("seminarrow")))
        return QFont::SemiCondensed;
    if (name.contains(QLatin1String("condensed")) || name.contains(QLatin1String("narrow")) || name.contains(QLatin1String("compressed")) ||
        name.endsWith(QLatin1String("cond")))
        return QFont::Condensed;
    if (name.contains(QLatin1String("ultraexpanded")))
        return QFont::UltraExpanded;
    if (name.contains(QLatin1String("extraexpanded")))
        return QFont::ExtraExpanded;
    if (name.contains(QLatin1String("semiexpanded")))
        return QFont::SemiExpanded;
    if (name.contains(QLatin1String("expanded")) || name.contains(QLatin1String("extended")) || name.endsWith(QLatin1String("wide")))
        return QFont::Expanded;
    return QFont::Unstretched;
}

// how the letters' width, spacing and rise show at the view's zoom
void shownForm(QTextCharFormat &f, double ppp)
{
    const double scale = f.hasProperty(kScaleProp) ? f.property(kScaleProp).toDouble() : 100;
    // (the font at its own width - a condensed one asked for as condensed: not made wider - the letters' width as the page
    // has them drawn by the editor)
    f.setProperty(kShownScaleProp, scale);
    QFont probe = f.font();
    probe.setStretch(QFont::AnyStretch);
    f.setFontStretch(int(std::lround(std::clamp(ownStretch(probe) * scale / 100, 1.0, 4000.0))));
    f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    f.setFontLetterSpacing((f.property(kTrackProp).toDouble() / 1000 * f.property(kSizeProp).toDouble() + f.property(kFitProp).toDouble()) * ppp);
    const int rise = f.property(kRiseProp).toInt();
    f.setVerticalAlignment(rise > 0 ? QTextCharFormat::AlignSuperScript : rise < 0 ? QTextCharFormat::AlignSubScript
                                                                        : QTextCharFormat::AlignNormal);
    if (const double outline = f.property(kOutlineProp).toDouble(); outline > 0)  // (its outline, as thick at this zoom)
        f.setTextOutline(QPen(f.foreground().color(), outline * f.property(kSizeProp).toDouble() * ppp));
}

// (a family whose name says its weight - "Calibri Light", "Segoe UI Semilight": Windows keeps those in the
// family itself - "Calibri" - by weight, so the weight asked for picks it: asked for as normal, the regular,
// heavier, face is shown)
int weightOf(const QString &family)
{
    const QString f = family.toLower();
    if (f.contains(QLatin1String("extralight")) || f.contains(QLatin1String("ultralight")) || f.contains(QLatin1String("extra light")))
        return QFont::ExtraLight;
    if (f.contains(QLatin1String("semilight")) || f.contains(QLatin1String("semi light")))
        return 350;
    if (f.contains(QLatin1String("thin")) || f.contains(QLatin1String("hairline")))
        return QFont::Thin;
    if (f.contains(QLatin1String("light")))
        return QFont::Light;
    if (f.contains(QLatin1String("medium")))
        return QFont::Medium;
    return QFont::Normal;
}

// (the family as Qt knows it: "Calibri Light" isn't one of its families - Windows keeps it as Calibri's light
// face - and asked for by that name, a stand-in was shown (Tahoma: wider, heavier); the weight word taken off,
// the family itself, the weight picking the face)
QString qtFamily(const QString &family)
{
    if (family.isEmpty() || QFontDatabase::hasFamily(family))
        return family;
    static const char *words[] = {" ExtraLight", " Extra Light", " UltraLight", " SemiLight", " Semilight", " Light", " Thin",
                                  " Medium", " SemiBold", " Semibold", " DemiBold", " Demibold", " ExtraBold", " Black", " Heavy"};
    for (const char *w : words)
        if (family.endsWith(QLatin1String(w), Qt::CaseInsensitive)) {
            const QString base = family.left(family.size() - int(qstrlen(w)));
            if (QFontDatabase::hasFamily(base))
                return base;
        }
    return family;
}

QTextCharFormat formatOf(const Document::StyledRun &r, double ppp, const QString &shown = QString())
{
    // (shown: the font its letters are drawn in, in the editor - the PDF's own, if it's not installed)
    QTextCharFormat f;
    f.setFontFamilies({qtFamily(shown.isEmpty() ? r.family : shown)});
    f.setFontHintingPreference(QFont::PreferNoHinting);  // (as wide as the page draws it)
    f.setFontStyleStrategy(QFont::NoSubpixelAntialias);  // (grey edges, as the page's letters - not ClearType's colours)
    f.setFontPointSize(std::max(1.0, r.size * ppp * 0.75));  // (pixels as points: 96 dpi)
    f.setFontWeight(r.bold && r.outline <= 0 ? QFont::Bold : weightOf(r.family));
    if (r.bold && r.outline > 0) {  // (bold by its outline, as the page draws it: the font itself, outlined)
        f.setProperty(kOutlineProp, r.outline);
        f.setTextOutline(QPen(r.color, r.outline * r.size * ppp));
    }
    f.setFontItalic(r.italic);
    f.setFontUnderline(r.underline);
    f.setFontStrikeOut(r.strike);
    f.setForeground(r.color);
    f.setProperty(kSizeProp, r.size);
    f.setProperty(kFamilyProp, r.family);
    f.setProperty(kScaleProp, r.scale);
    f.setProperty(kTrackProp, r.track);
    f.setProperty(kRiseProp, r.rise);
    shownForm(f, ppp);
    return f;
}

// how wide the page draws a run: to where the next one starts (its trailing space's box may be
// narrower than its advance), the last to its letters' end
double runWidth(const TextLine &l, const TextRun &tr)
{
    const auto it = std::find_if(l.runs.begin(), l.runs.end(), [&](const TextRun &x) { return &x == &tr; });
    if (it != l.runs.end() && it + 1 != l.runs.end() && !l.rtl && (it + 1)->origin.x() > tr.origin.x() &&
        (it + 1)->origin.x() - tr.box.right() < 0.6 * tr.size)  // (not over a wide gap)
        return (it + 1)->origin.x() - tr.origin.x();
    return tr.box.width();
}

// a run's look in the editor, its letters spaced to be as wide as the page has them (pageWidth points)
// (spread: a line whose spaces may be stretched to fill it - only a run of one word fitted there)
QTextCharFormat fittedOf(const Document::StyledRun &r, double ppp, const QString &shown, const QString &piece, double pageWidth,
                         bool spread = false)
{
    QTextCharFormat f = formatOf(r, ppp, shown);
    if (piece.trimmed().size() < 2 || (spread && piece.trimmed().contains(QLatin1Char(' '))) || pageWidth <= 0)
        return f;  // (words spread over a line: the line's spacing does it)
    QFont font = f.font();
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    const double natural = QFontMetricsF(font).horizontalAdvance(piece) / ppp;
    // (Arabic: as the font draws it - its letters joined, never spaced: a letter's spacing pulled them apart, or into each
    // other - the font looked different while it was typed in; nor its spaces: the page's width of a word leaves out the
    // space after it, made all but none. As it's written when it's done)
    if (std::any_of(piece.begin(), piece.end(), [](QChar c) { return c.script() == QChar::Script_Arabic && c.isLetter(); }))
        return f;
    const double fit = (pageWidth - natural) / piece.size();
    if (std::abs(fit) > 0.01 && std::abs(fit) < 0.08 * r.size) {
        f.setProperty(kFitProp, fit);
        shownForm(f, ppp);
    }
    return f;
}

Document::StyledRun runOf(const QString &text, const QTextCharFormat &f)
{
    Document::StyledRun r;
    r.text = text;
    r.text.replace(QString(QChar::LineSeparator) + QLatin1Char('	'), QString(QChar::LineSeparator));  // (a line's indent: none)
    r.text.replace(QLatin1Char('	'), QLatin1Char(' '));  // (a tab shown as the page's gap: a space, written)
    r.family = f.property(kFamilyProp).toString();
    if (r.family.isEmpty() && !f.fontFamilies().toStringList().isEmpty())
        r.family = f.fontFamilies().toStringList().first();
    r.size = f.property(kSizeProp).toDouble();
    r.outline = f.property(kOutlineProp).toDouble();
    r.bold = f.fontWeight() >= QFont::DemiBold || r.outline > 0;
    r.italic = f.fontItalic();
    r.underline = f.fontUnderline();
    r.strike = f.fontStrikeOut();
    r.color = f.foreground().color();
    r.scale = f.hasProperty(kScaleProp) ? f.property(kScaleProp).toDouble() : 100;
    r.track = f.property(kTrackProp).toDouble();
    r.rise = f.property(kRiseProp).toInt();
    return r;
}

bool sameLook(const Document::StyledRun &a, const Document::StyledRun &b)
{
    return a.family == b.family && a.bold == b.bold && a.italic == b.italic && a.underline == b.underline && a.strike == b.strike &&
           std::abs(a.size - b.size) < 0.05 && a.color.rgb() == b.color.rgb() && std::abs(a.scale - b.scale) < 0.05 &&
           std::abs(a.track - b.track) < 0.05 && a.rise == b.rise;
}

std::vector<Document::StyledRun> runsIn(const QTextEdit *box)
{
    std::vector<Document::StyledRun> out;
    for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            Document::StyledRun r = runOf(f.text(), f.charFormat());
            if (!out.empty() && sameLook(out.back(), r))
                out.back().text += r.text;
            else
                out.push_back(r);
        }
    return out;
}

// each paragraph's runs (a block of the editor's: a paragraph)
std::vector<std::vector<Document::StyledRun>> paragraphRunsIn(const QTextEdit *box)
{
    std::vector<std::vector<Document::StyledRun>> out;
    for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next()) {
        out.emplace_back();
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            Document::StyledRun r = runOf(f.text(), f.charFormat());
            if (!out.back().empty() && sameLook(out.back().back(), r))
                out.back().back().text += r.text;
            else
                out.back().push_back(r);
        }
    }
    return out;
}

}  // namespace

// the font a run of the PDF is shown in while typed: the one found installed if it's the very font,
// else the PDF's own (embedded) one, made known to Qt - its letters as the page has them
QString PageView::shownFamily(const QString &pdfFont, const QString &found)
{
    QString base = pdfFont.section(QLatin1Char('+'), -1);  // (a subset's tag left out)
    base = base.section(QLatin1Char('-'), 0, 0).section(QLatin1Char(','), 0, 0);
    auto squash = [](QString x) { return x.remove(QLatin1Char(' ')).toLower(); };
    if ((squash(found).startsWith(squash(base)) || squash(base).startsWith(squash(found))) &&
        !fonts::isEmbedded(fonts::findFont(pdfFont, 0))) {
        // (installed: all its letters - the PDF's own isn't known to Qt by its name. One the app found but Windows didn't
        // register - Office's cloud fonts: Aptos... - its files given to Qt first: asked for by name, Qt drew another)
        static QSet<QString> given;
        if (!QFontDatabase::hasFamily(found) && !given.contains(found)) {
            given.insert(found);
            for (const auto &[bold, italic] : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
                const fonts::Found f = fonts::findFamily(found, bold, italic);
                if (!f.path.isEmpty())
                    QFontDatabase::addApplicationFont(f.path);
            }
        }
        if (QFontDatabase::hasFamily(found))
            return found;
    }
    static QHash<QString, QString> made;
    auto it = made.find(pdfFont);
    if (it == made.end()) {
        QString family;
        const QByteArray program = doc_ ? doc_->fontProgram(pdfFont) : QByteArray();
        if (!program.isEmpty()) {
            const int id = QFontDatabase::addApplicationFontFromData(program);
            if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty())
                family = QFontDatabase::applicationFontFamilies(id).front();
        }
        it = made.insert(pdfFont, family);
    }
    return it->isEmpty() ? found : *it;
}

// the page under the text being edited, drawn without its letters
void PageView::holeFor(int page, const std::vector<QRectF> &letters)
{
    holePage_ = page;
    holeLetters_ = letters;
    holeRect_ = QRectF();
    for (const QRectF &r : letters)
        holeRect_ |= r;
    holeRect_.adjust(-3, -3, 3, 3);
    const double scale = pixelsPerPoint() * devicePixelRatioF();
    holeScale_ = scale;
    // (whole pixels of the page as drawn - and its place: just those pixels, so it lies exactly on them)
    const QRect px = QRectF(holeRect_.topLeft() * scale, holeRect_.size() * scale).toAlignedRect();
    holeRect_ = QRectF(QPointF(px.topLeft()) / scale, QSizeF(px.size()) / scale);
    hole_ = doc_->renderHiding(page, scale, px, letters);
    hole_.setDevicePixelRatio(devicePixelRatioF());
}

namespace {

// (a turned line's editor: nothing of its own drawn - the view draws its text turned)
class NoPaint : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *, QEvent *e) override { return e->type() == QEvent::Paint; }
};

// a box to type in over the page: nothing of its own drawn - the letters only, where they are
void seeThrough(QTextEdit *box)
{
    box->setAttribute(Qt::WA_TranslucentBackground);
    box->viewport()->setAutoFillBackground(false);
    box->setAutoFillBackground(false);
    QPalette pal = box->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    box->setPalette(pal);
    box->viewport()->setPalette(pal);
    box->setStyleSheet(QStringLiteral("QTextEdit { background: transparent; border: none; }"));
    box->document()->setDocumentMargin(0);
    box->document()->setUseDesignMetrics(true);  // (the fonts' own widths, as the page has them - not the screen's hinted ones)
}

// where its top goes for its first baseline to be at `baseline` (view pixels): the whole pixels
// in the box's place, the part of a pixel left as room over its text - so it lands exactly
int topFor(QTextEdit *box, double baseline)
{
    box->document()->size();  // (laid out)
    const QTextBlock b = box->document()->begin();
    const QTextLayout *l = b.layout();
    // (a line of a fixed height has its baseline where Qt puts it in it - not just its ascent down)
    const double under = l && l->lineCount() > 0
                             ? box->document()->documentLayout()->blockBoundingRect(b).top() + l->lineAt(0).y() + l->lineAt(0).ascent()
                             : box->fontMetrics().ascent();
    const int top = int(std::floor(baseline - under));
    // (the part of a pixel left: its text drawn that much lower - a margin of a part of a pixel, the
    // root frame's or its first paragraph's, isn't laid out by Qt: the text sat that much too high).
    // And a quarter of a screen pixel more: Qt puts letters on the nearest whole pixel, the page's
    // reach into the pixel their baseline is in - nearest a little lower matched the most lines
    box->setProperty(kDropProp, baseline - under - top + 0.25 / box->devicePixelRatioF());
    return top;
}

// the caret where it was clicked (none: at the end)
void caretAt(QTextEdit *box, QWidget *view, QPoint at)
{
    QTextCursor c = box->textCursor();
    if (at.x() >= 0) {
        c = box->cursorForPosition(box->viewport()->mapFrom(view, at));
    } else {
        c.movePosition(QTextCursor::End);
    }
    box->setTextCursor(c);
}

}  // namespace

void PageView::rescaleEditor()
{
    if (!lineEditor_ || editorPage_ < 0)
        return;
    const double ppp = pixelsPerPoint();
    const double k = ppp / editorPpp_;
    if (std::abs(k - 1) < 1e-6)
        return;
    editorPpp_ = ppp;
    QTextDocument *doc = lineEditor_->document();
    const QSignalBlocker quiet(doc);  // (not "changed": nothing typed)
    QTextCursor c(doc);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat f = b.blockFormat();
        if (f.lineHeightType() == QTextBlockFormat::FixedHeight)
            f.setLineHeight(f.lineHeight() * k, QTextBlockFormat::FixedHeight);
        f.setTopMargin(f.topMargin() * k);
        f.setTextIndent(f.textIndent() * k);
        f.setLeftMargin(f.leftMargin() * k);  // (a paragraph typed in, flowing: its lines after the first where they were)
        f.setRightMargin(f.rightMargin() * k);
        if (f.hasProperty(kReflowRest))
            f.setProperty(kReflowRest, f.property(kReflowRest).toDouble() * k);
        QList<QTextOption::Tab> tabs = f.tabPositions();
        for (QTextOption::Tab &t : tabs)
            t.position *= k;
        f.setTabPositions(tabs);
        c.setPosition(b.position());
        c.setBlockFormat(f);
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {  // (each letter's look at the new zoom)
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            QTextCharFormat cf = frag.charFormat();
            if (cf.hasProperty(kSizeProp))
                cf.setFontPointSize(std::max(1.0, cf.property(kSizeProp).toDouble() * ppp * 0.75));
            if (cf.hasProperty(QTextFormat::FontWordSpacing))
                cf.setFontWordSpacing(cf.fontWordSpacing() * k);
            shownForm(cf, ppp);
            QTextCursor fc(doc);
            fc.setPosition(frag.position());
            fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            fc.setCharFormat(cf);
        }
    }
    if (lineEditor_->lineWrapMode() == QTextEdit::FixedPixelWidth) {
        // (a block: as wide as it is on the page at this zoom - worked out again, not the last width made bigger: setting the
        // wrap width sets the text's too, made bigger once more after it - wider than the block, its lines past its right)
        const int width = toView(editorPage_, QPointF(editorRight_, 0)).x() - toView(editorPage_, QPointF(editorLeft_, 0)).x() + 1;
        lineEditor_->setLineWrapColumnOrWidth(width);
        doc->setTextWidth(width);
    } else if (doc->textWidth() > 0) {
        doc->setTextWidth(doc->textWidth() * k);
    }
    if (auto *edit = dynamic_cast<LineEdit *>(lineEditor_))
        edit->scale = ppp;
    editorTop_ = topFor(lineEditor_, toView(editorPage_, QPointF(0, 0)).y() + editorBase_ * ppp);
    if (editorFit_)
        editorFit_();
    viewport()->update();
}

void PageView::showEditorCaret()
{
    if (!lineEditor_ || editorTurned_)
        return;
    const QRect c = lineEditor_->cursorRect();
    QRect r(lineEditor_->viewport()->mapTo(viewport(), c.topLeft()), c.size());
    r.adjust(-48, -48, 48, 48);  // (some room round it)
    const QRect v = viewport()->rect();
    const int dy = r.top() < v.top() ? r.top() - v.top() : r.bottom() > v.bottom() ? r.bottom() - v.bottom() : 0;
    const int dx = r.left() < v.left() ? r.left() - v.left() : r.right() > v.right() ? r.right() - v.right() : 0;
    if (dy)
        verticalScrollBar()->setValue(verticalScrollBar()->value() + dy);
    if (dx)
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + dx);
}

QTransform PageView::editorTurn() const
{
    // (turned clockwise round its pivot, as the page shows it - in the view's pixels)
    QTransform t;
    if (!editorTurned_)
        return t;
    const QPointF c = toView(editorPage_, editorPivot_);
    t.translate(c.x(), c.y());
    t.rotate(editorAngle_);
    t.translate(-c.x(), -c.y());
    return t;
}

bool PageView::turnedEditorMouse(QMouseEvent *e)
{
    // a press on the turned text (and the drag / release after it): to the editor, turned back
    if (!lineEditor_ || !editorTurned_)
        return false;
    const QPointF back = editorTurn().inverted().map(e->position());
    const bool inside = lineEditor_->geometry().adjusted(-3, -3, 3, 3).contains(back.toPoint());
    const QEvent::Type type = e->type();
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
        turnedDrag_ = inside && e->button() == Qt::LeftButton;
        if (!turnedDrag_)
            return false;
        lineEditor_->setFocus(Qt::MouseFocusReason);
    } else if (!turnedDrag_) {
        if (type == QEvent::MouseMove)
            viewport()->setCursor(inside ? Qt::IBeamCursor : Qt::ArrowCursor);
        return false;
    }
    QWidget *vp = lineEditor_->viewport();
    const QPointF local = vp->mapFrom(viewport(), back);
    QMouseEvent m(type, local, vp->mapToGlobal(local), e->button(), e->buttons(), e->modifiers());
    QApplication::sendEvent(vp, &m);
    if (type == QEvent::MouseButtonRelease)
        turnedDrag_ = false;
    viewport()->update();
    return true;
}

void PageView::paintTurnedEditor(QPainter &p, int page)
{
    // its text as the editor lays it out, turned as the line is - its caret and selection too
    if (!lineEditor_ || !editorTurned_ || page != editorPage_)
        return;
    QTextEdit *box = lineEditor_;
    p.save();
    p.setTransform(editorTurn(), true);
    p.translate(box->geometry().topLeft() + box->viewport()->pos());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = box->palette();
    ctx.palette.setColor(QPalette::Text, caretColor(box->textCursor()));  // (its caret: as the text's, not the theme's)
    const QTextCursor cur = box->textCursor();
    ctx.cursorPosition = box->hasFocus() ? cur.position() : -1;
    if (cur.hasSelection()) {
        QAbstractTextDocumentLayout::Selection s;
        s.cursor = cur;
        QColor c = theme::selection().first;
        c.setAlpha(110);
        s.format.setBackground(c);
        ctx.selections.append(s);
    }
    ctx.clip = QRectF(QPointF(0, 0), box->viewport()->size());
    p.translate(0, box->property(kDropProp).toDouble());
    box->document()->documentLayout()->draw(&p, ctx);
    p.restore();
}

// (spread: a justified line, its spaces stretched - only gaps far wider than its others: a bullet's)
std::vector<std::pair<int, double>> PageView::wideGaps(int page, const TextLine &l, bool spread)
{
    std::vector<std::pair<int, double>> out;
    if (!doc_ || l.rtl || std::abs(l.angle) > 0.01)
        return out;
    const auto chars = doc_->text(page);
    std::vector<const TextChar *> cs;
    for (const TextChar &ch : *chars)
        if (std::abs(ch.origin.y() - l.baseline) < 0.6 && ch.box.center().x() >= l.rect.left() - 1 && ch.box.center().x() <= l.rect.right() + 1)
            cs.push_back(&ch);
    std::sort(cs.begin(), cs.end(), [](const TextChar *a, const TextChar *b) { return a->box.left() < b->box.left(); });
    std::vector<double> gaps;  // (between its words)
    {
        bool in = false;
        double right = 0;
        for (const TextChar *ch : cs) {
            if (QChar::isSpace(uint(ch->c)) || ch->c == 0xA0) {
                in = false;
                continue;
            }
            if (!in && right > 0)
                gaps.push_back(ch->box.left() - right);
            in = true;
            right = ch->box.right();
        }
    }
    double usual = 0;
    if (spread && !gaps.empty()) {
        std::vector<double> g = gaps;
        std::sort(g.begin(), g.end());
        usual = g[g.size() / 2];
    }
    int word = -1;
    bool inWord = false;
    double prevRight = 0;
    for (const TextChar *ch : cs) {
        const bool space = QChar::isSpace(uint(ch->c)) || ch->c == 0xA0;
        const double wide = std::max(0.6 * std::max(1.0f, ch->size), spread ? usual + 0.25 * std::max(1.0f, ch->size) : 0.0);
        if (space) {
            inWord = false;
            continue;
        }
        if (!inWord || ch->box.left() - prevRight > wide) {
            if (word >= 0 && ch->box.left() - prevRight > wide)
                out.emplace_back(word, ch->box.left());
            ++word;
            inWord = true;
        }
        prevRight = ch->box.right();
    }
    return out;
}

namespace {

// a line just put in the editor (from..to): the spaces after the words wideGaps names made tabs
// - each tab's stop where the page has the word after it (px from the block's left: x0)
void tabsIn(QTextDocument *doc, int from, int to, const std::vector<std::pair<int, double>> &gaps, double x0, double ppp,
            QList<QTextOption::Tab> *stops)
{
    if (gaps.empty())
        return;
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    const QString t = c.selectedText();
    // (its words: runs of what isn't a space - the boundary after word w: the spaces after it)
    std::vector<std::pair<int, int>> after;  // (each word's following spaces: start, end)
    int i = 0;
    while (i < t.size()) {
        while (i < t.size() && t[i].isSpace())
            ++i;
        if (i >= t.size())
            break;
        while (i < t.size() && !t[i].isSpace())
            ++i;
        int j = i;
        while (j < t.size() && t[j] == QLatin1Char(' '))
            ++j;
        after.emplace_back(i, j);
        i = j;
    }
    for (auto it = gaps.rbegin(); it != gaps.rend(); ++it) {  // (from the end: the places before stay put)
        const int w = it->first;
        if (w < 0 || w >= int(after.size()) || after[size_t(w)].first >= after[size_t(w)].second)
            continue;
        QTextCursor s(doc);
        s.setPosition(from + after[size_t(w)].first);
        s.setPosition(from + after[size_t(w)].second, QTextCursor::KeepAnchor);
        const QTextCharFormat f = s.charFormat();
        s.insertText(QStringLiteral("	"), f);
        stops->append(QTextOption::Tab((it->second - x0) * ppp, QTextOption::LeftTab));
    }
}

}  // namespace

void PageView::editLine(int page, int index, QPoint at)
{
    closeLineEditor(true);
    const std::vector<TextLine> &lines = linesOf(page);
    if (index < 0 || index >= int(lines.size()))
        return;
    editorFrame_ = false;
    if (frameLines(page, index).size() > 1 && std::abs(lines[size_t(index)].angle) < 0.01) {  // a block of paragraphs: all of it, one paragraph to a line
        const pagetext::TextFrame frame = framesOf(page)[size_t(pagetext::frameOf(framesOf(page), index))];
        QStringList paras;
        for (const std::vector<int> &para : frame.paras) {
            QStringList words;
            for (int k : para)
                words << lines[size_t(k)].text;
            paras << words.join(QLatin1Char(' '));
        }
        const TextLine &first = lines[size_t(frame.lines.front())];
        auto *box = new LineEdit(viewport());
        box->paragraphs = true;
        lineEditor_ = box;
        connect(box, &QTextEdit::cursorPositionChanged, this, &PageView::editorCaretMoved);  // (the text bar: the caret's look)
        editorFrame_ = true;
        editorTurned_ = false;
        editorPage_ = page;
        editorIndex_ = index;
        box->setAcceptRichText(false);
        box->setFrameShape(QFrame::NoFrame);
        box->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        box->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        seeThrough(box);
        if (first.rtl) {
            box->setLayoutDirection(Qt::RightToLeft);
            QTextOption o = box->document()->defaultTextOption();
            o.setAlignment(Qt::AlignRight | Qt::AlignAbsolute);  // (at the right - not flipped to the left)
            o.setTextDirection(Qt::RightToLeft);
            box->document()->setDefaultTextOption(o);
        }
        // its paragraphs, each letter in its own look (as the line editor's), one to a line
        const double ppp = pixelsPerPoint();
        const std::vector<Annot> marks = doc_->annots(page);
        QTextCursor c(box->document());
        double frameLeft = 1e9;
        for (int k : frame.lines)
            frameLeft = std::min(frameLeft, lines[size_t(k)].rect.left());
        std::vector<QList<QTextOption::Tab>> paraTabs(frame.paras.size());
        // (a justified paragraph's lines: their words spread to fill them, below - any other's letters
        // spaced as wide as the page has them, here)
        std::vector<bool> paraJustified(frame.paras.size());
        {
            double fl = 1e9, fr = -1e9;
            for (int k : frame.lines) {
                fl = std::min(fl, lines[size_t(k)].rect.left());
                fr = std::max(fr, lines[size_t(k)].rect.right());
            }
            for (size_t n = 0; n < frame.paras.size(); ++n)
                paraJustified[n] = pagetext::alignOf(lines, frame.paras[n], fl, fr) == QLatin1String("justify");
        }
        for (size_t n = 0; n < frame.paras.size(); ++n) {
            if (n > 0)
                c.insertBlock();
            for (size_t m = 0; m < frame.paras[n].size(); ++m) {
                const int k = frame.paras[n][m];
                bool lineStarted = false;
                int lineFrom = -1;
                for (const TextRun &tr : lines[size_t(k)].runs) {
                    // (a line's leading spaces kept: its letters start where the page has them)
                    QString piece = tr.text;
                    if (piece.isEmpty())
                        continue;
                    const fonts::Found f = fonts::findFont(tr.font, tr.flags);
                    Document::StyledRun r;
                    r.text = piece;
                    r.family = f.family;
                    r.bold = f.bold;
                    r.outline = (tr.flags & 16) && !f.realBold ? tr.outline : 0;
                    r.italic = f.italic;
                    r.size = tr.size;
                    r.scale = tr.scale;  // (its letters' width and spacing, as the page has them)
                    r.track = tr.track;
                    r.color = QColor::fromRgb(tr.color);
                    const QRectF mid = tr.box.adjusted(1, tr.box.height() * 0.3, -1, -tr.box.height() * 0.3);
                    for (const Annot &a : marks)
                        for (const QRectF &q : a.pieces)
                            if (q.intersects(mid)) {
                                r.underline = r.underline || a.kind == QLatin1String("Underline");
                                r.strike = r.strike || a.kind == QLatin1String("StrikeOut");
                            }
                    const QString shown = shownFamily(tr.font, f.family);
                    if (!lineStarted && m > 0) {  // (the page's own line break: the line goes on where it did)
                        c.insertText(QString(QChar::LineSeparator), formatOf(r, ppp, shown));
                        // (a line in from the block's left - a contents list's entry - a tab to where it starts)
                        const TextLine &ln = lines[size_t(k)];
                        const double in = ln.rect.left() - frameLeft;
                        if (in > 1.5 && !ln.rtl && std::abs(ln.rect.center().x() - frame.rect.center().x()) > 2) {
                            c.insertText(QStringLiteral("	"), formatOf(r, ppp, shown));
                            paraTabs[n].append(QTextOption::Tab(in * ppp, QTextOption::LeftTab));
                        }
                    }
                    if (lineFrom < 0)
                        lineFrom = c.position();
                    c.insertText(piece, fittedOf(r, ppp, shown, piece, runWidth(lines[size_t(k)], tr), paraJustified[n] && m + 1 < frame.paras[n].size()));
                    lineStarted = true;
                }
                // (its tabs, as the page's - not a justified line's spaces, stretched to fill it)
                const bool spreadOut = m + 1 < frame.paras[n].size() && std::abs(lines[size_t(k)].rect.right() - frame.rect.right()) <= 2;
                if (lineFrom >= 0) {
                    tabsIn(box->document(), lineFrom, c.position(), wideGaps(page, lines[size_t(k)], spreadOut), frameLeft, ppp, &paraTabs[n]);
                    c.movePosition(QTextCursor::End);
                }
            }
        }
        // laid out as the page has it: its line spacing, the gap between its paragraphs, their
        // first-line indents, its alignment (as the block is retyped)
        std::vector<double> pitches, gaps, indents;
        double left = 1e9, right = -1e9;
        for (int k : frame.lines) {
            left = std::min(left, lines[size_t(k)].rect.left());
            right = std::max(right, lines[size_t(k)].rect.right());
        }
        bool justified = false, notJustified = false, centered = true, rightAligned = true, leftAligned = true;
        for (size_t n = 0; n < frame.paras.size(); ++n) {
            const std::vector<int> &para = frame.paras[n];
            for (size_t m = 0; m < para.size(); ++m) {
                const TextLine &l = lines[size_t(para[m])];
                if (m > 0)
                    pitches.push_back(l.baseline - lines[size_t(para[m - 1])].baseline);
                if (m + 1 < para.size()) {
                    if (std::abs(l.rect.right() - right) > 2 || (m > 0 && std::abs(l.rect.left() - left) > 2))
                        notJustified = true;
                    else
                        justified = true;
                } else if (para.size() > 1 && (l.rtl ? std::abs(l.rect.right() - right) > 2 : std::abs(l.rect.left() - left) > 2)) {
                    notJustified = true;  // (its last line not at its start: centred / right - not justified)
                }
                centered = centered && std::abs(l.rect.center().x() - (left + right) / 2) <= 2;
                rightAligned = rightAligned && std::abs(l.rect.right() - right) <= 2;
                leftAligned = leftAligned && std::abs(l.rect.left() - left) <= 2;
            }
            if (n > 0)
                gaps.push_back(lines[size_t(para.front())].baseline - lines[size_t(frame.paras[n - 1].back())].baseline);
            const TextLine &f0 = lines[size_t(para.front())];
            indents.push_back(f0.rtl ? std::max(0.0, right - f0.rect.right()) : std::max(0.0, f0.rect.left() - left));
        }
        auto median = [](std::vector<double> v, double otherwise) {
            if (v.empty())
                return otherwise;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double size0 = first.mainRun().size;
        const double pitch = median(pitches, size0 * 1.2), paraGap = median(gaps, pitch * 1.5);
        justified = justified && !notJustified;
        const Qt::Alignment align = justified ? Qt::AlignJustify
                                    : (centered && !leftAligned && !rightAligned) ? Qt::AlignHCenter
                                    : (first.rtl || (rightAligned && !leftAligned)) ? Qt::AlignRight : Qt::AlignLeft;
        // (justified: each line's words spread to fill it, below - the lines broken as the page's)
        const Qt::Alignment blockAlign = align == Qt::AlignJustify ? (first.rtl ? Qt::AlignRight : Qt::AlignLeft) : align;
        // (each paragraph as the page has it - a block may hold a title, a byline, body text: its own
        // line spacing, its own gap above it, its own alignment)
        std::vector<double> ownPitch, ownGap;
        std::vector<Qt::Alignment> ownAlign;
        for (size_t n = 0; n < frame.paras.size(); ++n) {
            const std::vector<int> &para = frame.paras[n];
            std::vector<double> steps;
            const double psize = lines[size_t(para.front())].mainRun().size;
            for (size_t m = 1; m < para.size(); ++m) {  // (pieces of one line read apart - no step: left out)
                const double step = lines[size_t(para[m])].baseline - lines[size_t(para[m - 1])].baseline;
                if (step > 0.5 * psize)
                    steps.push_back(step);
            }
            ownPitch.push_back(steps.empty() ? pitch / std::max(0.1, size0) * psize : median(steps, pitch));
            ownGap.push_back(n > 0 ? lines[size_t(para.front())].baseline - lines[size_t(frame.paras[n - 1].back())].baseline : 0);
            const QString pa = pagetext::alignOf(lines, para, left, right);
            ownAlign.push_back(pa == QLatin1String("justify") ? Qt::AlignJustify : pa == QLatin1String("center") ? Qt::AlignHCenter
                                                     : pa == QLatin1String("right") ? Qt::AlignRight : Qt::AlignLeft);
        }
        // (a paragraph of one line: its line as tall as the step to the next one, no more - a guessed
        // height bigger than that would need room taken back, and pushed the next line down)
        for (size_t n = 0; n < ownPitch.size(); ++n)  // (... or than the step from the line above it)
            if (frame.paras[n].size() == 1) {
                if (n + 1 < ownPitch.size() && ownGap[n + 1] > 0)
                    ownPitch[n] = std::min(ownPitch[n], ownGap[n + 1]);
                if (n > 0 && ownGap[n] > 0)
                    ownPitch[n] = std::min(ownPitch[n], ownGap[n]);
            }
        // (where its baseline is in its line: Qt puts it 4/5 down a line of a fixed height, less its
        // font's leading)
        auto ascentAt = [&](const QTextBlock &b, double height) {
            QTextCursor c(box->document());
            c.setPosition(b.position() + (b.length() > 1 ? 1 : 0));
            return height * 0.8 - std::max(0.0, QFontMetricsF(c.charFormat().font()).leading());
        };
        QTextCursor bc(box->document());
        int n = 0;
        double prevPitch = pitch, prevAscent = 0;
        for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next(), ++n) {
            QTextBlockFormat f = b.blockFormat();
            const double own = n < int(ownPitch.size()) ? ownPitch[size_t(n)] : pitch;
            const double gap = n < int(ownGap.size()) ? ownGap[size_t(n)] : paraGap;
            const Qt::Alignment a = n < int(ownAlign.size()) ? ownAlign[size_t(n)] : align;
            const double ascent = ascentAt(b, own * ppp);
            f.setLineHeight(own * ppp, QTextBlockFormat::FixedHeight);
            // (from the last baseline above to its first: its gap on the page)
            f.setTopMargin(n > 0 ? std::max(0.0, gap * ppp - prevPitch * ppp + prevAscent - ascent) : 0);
            // (right to left - Arabic: the paragraph's own way too, not just the box's - justified as it's typed in, its
            // last line ends at the right, as it starts; its indents on the right)
            const bool rtl = n < int(frame.paras.size()) ? lines[size_t(frame.paras[size_t(n)].front())].rtl : first.rtl;
            f.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
            // (where the page has it - not mirrored: Qt flips a left / right alignment in a right-to-left paragraph)
            f.setAlignment((a == Qt::AlignJustify ? (rtl ? Qt::AlignRight : Qt::AlignLeft) : a) | Qt::AlignAbsolute);
            const bool leading = a == Qt::AlignJustify || a == (rtl ? Qt::AlignRight : Qt::AlignLeft);  // (aligned where its lines start)
            f.setTextIndent(leading && n < int(indents.size()) ? indents[size_t(n)] * ppp : 0);
            // (kept for when it's typed in and flows again: justified, its lines after the first where the page has them)
            f.setProperty(kReflowJustify, a == Qt::AlignJustify);
            f.setProperty(kReflowRtl, rtl);
            if (n < int(frame.paras.size()) && frame.paras[size_t(n)].size() > 1 && leading) {
                const QRectF &second = lines[size_t(frame.paras[size_t(n)][1])].rect;
                f.setProperty(kReflowRest, std::max(0.0, rtl ? right - second.right() : second.left() - left) * ppp);
            }
            if (n < int(paraTabs.size()) && !paraTabs[size_t(n)].isEmpty()) {  // (in order, as Qt reads them)
                QList<QTextOption::Tab> stops = paraTabs[size_t(n)];
                std::sort(stops.begin(), stops.end(), [](const QTextOption::Tab &a, const QTextOption::Tab &b) { return a.position < b.position; });
                f.setTabPositions(stops);
            }
            bc.setPosition(b.position());
            bc.setBlockFormat(f);
            prevPitch = own;
            prevAscent = ascent;
        }
        (void)blockAlign;
        // where its text is: as wide as the block, its first baseline on the page's
        const int x0 = toView(page, QPointF(left, 0)).x(), x1 = toView(page, QPointF(right, 0)).x();
        box->setLineWrapMode(QTextEdit::FixedPixelWidth);  // (its width: the block's...)
        box->setLineWrapColumnOrWidth(x1 - x0 + 1);
        box->setWordWrapMode(QTextOption::NoWrap);  // (... its lines broken where the page breaks them)
        box->document()->setTextWidth(x1 - x0 + 1);
        {  // each line but a paragraph's last (a justified paragraph's): its words spread to where the page's ends
            box->document()->size();  // (laid out)
            const double originX = toView(page, QPointF(0, 0)).x();
            int bn = 0;
            for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next(), ++bn) {
                QTextLayout *l = b.layout();
                if (!l || bn >= int(ownAlign.size()) || ownAlign[size_t(bn)] != Qt::AlignJustify)
                    continue;
                for (int i = 0; i + 1 < l->lineCount(); ++i) {
                    const QTextLine ln = l->lineAt(i);
                    const QString part = b.text().mid(ln.textStart(), ln.textLength());
                    QString words = part;
                    words.remove(QChar::LineSeparator);
                    const int spaces = int(words.trimmed().count(QLatin1Char(' ')));
                    // (to the block's right edge from where its letters end now - past an indent, a tab)
                    int end = ln.textStart() + ln.textLength();
                    while (end > ln.textStart() && (b.text()[end - 1] == QChar::LineSeparator || b.text()[end - 1] == QLatin1Char(' ')))
                        --end;
                    // (the page's line: the block's i-th line of this paragraph - one that ends short of the
                    // block's edge isn't stretched to it)
                    const std::vector<int> &para = frame.paras[size_t(bn)];
                    const double target = l->lineCount() == int(para.size()) ? originX + lines[size_t(para[size_t(i)])].rect.right() * ppp - x0
                                                                             : double(x1 - x0);
                    const double extra = (target - ln.cursorToX(end)) / std::max(1, spaces);
                    // (wider or narrower spaces: a stand-in font a little wider than the page's fits too)
                    if (spaces == 0 || std::abs(extra) < 0.01 || extra > 4 * ppp * size0 || extra < -0.2 * ppp * size0)
                        continue;
                    QTextCursor sel(box->document());
                    sel.setPosition(b.position() + ln.textStart());
                    sel.setPosition(b.position() + ln.textStart() + ln.textLength(), QTextCursor::KeepAnchor);
                    QTextCharFormat spread;
                    spread.setFontWordSpacing(extra);
                    sel.mergeCharFormat(spread);
                }
            }
        }
        {  // each paragraph's first baseline where the page has it (the gaps above them, worked out from
           // the fonts' metrics, were a little off - and it added up down the block)
            auto baseOf = [&](const QTextBlock &b) {
                const QTextLayout *l = b.layout();
                return box->document()->documentLayout()->blockBoundingRect(b).top() +
                       (l && l->lineCount() > 0 ? l->lineAt(0).y() + l->lineAt(0).ascent() : 0);
            };
            box->document()->size();  // (laid out)
            const double base0 = baseOf(box->document()->begin());
            int pn = 0;
            for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next(), ++pn) {
                if (pn == 0 || pn >= int(frame.paras.size()) || frame.paras[size_t(pn)].empty())
                    continue;
                const double want = base0 + (lines[size_t(frame.paras[size_t(pn)].front())].baseline - first.baseline) * ppp;
                const double off = want - baseOf(b);
                if (std::abs(off) < 0.01)
                    continue;
                QTextBlockFormat bf = b.blockFormat();
                bf.setTopMargin(std::max(0.0, bf.topMargin() + off));
                bc.setPosition(b.position());
                bc.setBlockFormat(bf);
                box->document()->size();  // (laid out again: the ones below it moved)
            }
        }
        const double baseline = toView(page, QPointF(0, 0)).y() + first.baseline * ppp;  // (exact: not a whole pixel)
        editorLeft_ = left;
        editorRight_ = right;
        editorBase_ = first.baseline;
        editorPpp_ = ppp;
        editorTop_ = topFor(box, baseline);
        (void)x0;
        (void)x1;
        const bool rtlBlock = first.rtl;
        auto fit = [this, box, page, rtlBlock] {  // (as tall as its text - and as wide as its longest line: never scrolled across)
            const int a = toView(page, QPointF(editorLeft_, 0)).x(), b = toView(page, QPointF(editorRight_, 0)).x();
            if (rtlBlock) {
                // (right to left: a line kept as the page breaks it, a little wider in the font than on the page, past the
                // block's left - its right where the block's is: Qt ran it past the right, the whole line moved over)
                double widest = 0;
                for (QTextBlock bl = box->document()->begin(); bl.isValid(); bl = bl.next())
                    if (bl.blockFormat().nonBreakableLines() && bl.layout())
                        for (int i = 0; i < bl.layout()->lineCount(); ++i)
                            widest = std::max(widest, bl.layout()->lineAt(i).naturalTextWidth() + bl.blockFormat().leftMargin() +
                                                          bl.blockFormat().rightMargin() + std::abs(bl.blockFormat().textIndent()));
                const int width = std::max(b - a + 1, int(std::ceil(widest)));
                if (width != box->lineWrapColumnOrWidth()) {
                    box->setLineWrapColumnOrWidth(width);
                    box->document()->setTextWidth(width);
                }
                const QSizeF size = box->document()->size();
                const int w = std::max(width, int(std::ceil(size.width()))) + 2;
                box->setGeometry(b + 1 - width, editorTop_, w, int(std::ceil(size.height())) + 2);
                return;
            }
            const QSizeF size = box->document()->size();
            box->setGeometry(a, editorTop_, std::max(b - a + 2, int(std::ceil(size.width())) + 2), int(std::ceil(size.height())) + 2);
        };
        editorFit_ = fit;
        // (each paragraph's lines broken where the page breaks them - till it's typed in: then it flows again in
        // the block's width, as Acrobat's does, its spread spaces Qt's justifying now)
        for (QTextBlock b = box->document()->begin(); b.isValid(); b = b.next()) {
            QTextBlockFormat f = b.blockFormat();
            f.setNonBreakableLines(true);
            bc.setPosition(b.position());
            bc.setBlockFormat(f);
        }
        box->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        auto reflowing = std::make_shared<bool>(false);
        connect(box->document(), &QTextDocument::contentsChange, box, [box, reflowing](int pos, int, int added) {
            if (*reflowing || box->quiet)  // (the editor's own changes - a step taken back: not typed)
                return;
            std::vector<int> touched;
            const QTextBlock last = box->document()->findBlock(pos + added);
            for (QTextBlock b = box->document()->findBlock(pos); b.isValid(); b = b.next()) {
                if (b.blockFormat().nonBreakableLines())
                    touched.push_back(b.blockNumber());
                if (b == last)
                    break;
            }
            if (touched.empty())
                return;
            QTimer::singleShot(0, box, [box, reflowing, touched] {
                *reflowing = true;
                ++box->quiet;
                QTextCursor edit(box->document());
                edit.joinPreviousEditBlock();  // (one Undo with the typing that set it flowing)
                for (int n : touched) {
                    QTextBlock b = box->document()->findBlockByNumber(n);
                    if (!b.isValid() || !b.blockFormat().nonBreakableLines())
                        continue;
                    const QString t = b.text();
                    for (int i = int(t.size()) - 1; i >= 0; --i) {  // (the page's line breaks: spaces)
                        if (t[i] != QChar::LineSeparator)
                            continue;
                        const bool spaced = (i > 0 && t[i - 1] == QLatin1Char(' ')) || (i + 1 < t.size() && t[i + 1] == QLatin1Char(' '));
                        QTextCursor s(box->document());
                        s.setPosition(b.position() + i);
                        s.setPosition(b.position() + i + 1, QTextCursor::KeepAnchor);
                        s.insertText(spaced ? QString() : QStringLiteral(" "));
                    }
                    b = box->document()->findBlockByNumber(n);
                    QTextBlockFormat f = b.blockFormat();
                    f.setNonBreakableLines(false);
                    if (f.property(kReflowJustify).toBool())
                        f.setAlignment(Qt::AlignJustify);
                    if (f.hasProperty(kReflowRest)) {  // (its lines after the first: where they were - right to left, from the right)
                        const double rest = f.property(kReflowRest).toDouble();
                        f.setTextIndent(f.textIndent() - rest);
                        if (f.property(kReflowRtl).toBool())
                            f.setRightMargin(rest);
                        else
                            f.setLeftMargin(rest);
                    }
                    QTextCursor bc(b);
                    bc.setBlockFormat(f);
                    QTextCursor all(b);  // (the page's spread spaces off: Qt justifies it now)
                    all.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    QTextCharFormat plain;
                    plain.setFontWordSpacing(0);
                    all.mergeCharFormat(plain);
                }
                edit.endEditBlock();
                *reflowing = false;
                --box->quiet;
            });
        });
        // (as it is now - laid out, its lines spread: put back only if it's changed)
        editorParas_ = paragraphRunsIn(box);
        editorWas_ = box->toPlainText();
        fit();
        connect(box->document(), &QTextDocument::contentsChanged, box, fit);
        std::vector<QRectF> letters;
        for (int k : frame.lines)
            letters.push_back(lines[size_t(k)].rect);
        holeFor(page, letters);
        box->done = [this](bool commit) { closeLineEditor(commit); };
        box->keep = editorKeeper_;
        // (its Undo: just what's done in it from here - not the steps it was filled in with, taken back
        // one by one: its letters' looks and spacing undone a piece at a time)
        box->document()->clearUndoRedoStacks();
        box->scale = editorPpp_;  // (its own Undo / Redo, from here)
        box->rescaled = [this](double from) {
            editorPpp_ = from;
            rescaleEditor();
        };
        box->followCaret = [this] { showEditorCaret(); };
        box->startHistory();
        box->show();
        box->setFocus();
        caretAt(box, viewport(), at);
        viewport()->update();
        emit message(tr("Type where the text is (Enter: a new paragraph), then click away or press Ctrl+Enter (Esc: as it was)."));
        return;
    }
    const TextLine &line = lines[size_t(index)];
    const double ppp = pixelsPerPoint();
    // its runs as they are: each letter's font, size, colour - and the underline / strike-out
    // marks over them
    std::vector<Document::StyledRun> runs;
    std::vector<QString> shown;  // (the font each is shown in)
    std::vector<double> widths;  // (and how wide the page draws it)
    QString text;
    const std::vector<Annot> annots = doc_->annots(page);
    for (const TextRun &tr : line.runs) {
        QString piece = tr.text;
        if (!text.isEmpty() && text.endsWith(QLatin1Char(' ')))  // (one space between words; its leading ones kept:
            while (piece.startsWith(QLatin1Char(' ')))           // its letters start where the page has them)
                piece.remove(0, 1);
        if (piece.isEmpty())
            continue;
        const fonts::Found f = fonts::findFont(tr.font, tr.flags);
        Document::StyledRun r;
        r.text = piece;
        r.family = f.family;
        r.bold = f.bold;
        r.outline = (tr.flags & 16) && !f.realBold ? tr.outline : 0;
        r.italic = f.italic;
        r.size = tr.size;
        r.scale = tr.scale;
        r.track = tr.track;
        r.color = QColor::fromRgb(tr.color);
        const QRectF mid = tr.box.adjusted(1, tr.box.height() * 0.3, -1, -tr.box.height() * 0.3);
        for (const Annot &a : annots)
            for (const QRectF &q : a.pieces)
                if (q.intersects(mid)) {
                    r.underline = r.underline || a.kind == QLatin1String("Underline");
                    r.strike = r.strike || a.kind == QLatin1String("StrikeOut");
                }
        text += piece;
        runs.push_back(r);
        shown.push_back(shownFamily(tr.font, f.family));
        widths.push_back(piece == tr.text ? runWidth(line, tr) : 0);
    }
    while (!runs.empty() && runs.back().text.endsWith(QLatin1Char(' '))) {
        runs.back().text.chop(1);
        if (runs.back().text.isEmpty())
            runs.pop_back();
    }
    if (runs.empty())
        return;
    auto *box = new LineEdit(viewport());
    lineEditor_ = box;
    connect(box, &QTextEdit::cursorPositionChanged, this, &PageView::editorCaretMoved);  // (the text bar: the caret's look)
    editorPage_ = page;
    editorIndex_ = index;
    box->setLineWrapMode(QTextEdit::NoWrap);
    box->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    box->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    box->setAcceptRichText(false);
    box->setFrameShape(QFrame::NoFrame);
    const bool turned = std::abs(line.angle) > 0.01;
    // (typed where it is - the page under it drawn without its letters; turned: the box unseen, its
    // text drawn turned by the view)
    seeThrough(box);
    editorTurned_ = turned;
    editorAngle_ = line.angle;
    editorPivot_ = line.pivot;
    if (turned) {
        box->viewport()->installEventFilter(new NoPaint(box));
        box->setAttribute(Qt::WA_TransparentForMouseEvents);
        box->viewport()->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto again = [this] { viewport()->update(); };
        connect(box, &QTextEdit::cursorPositionChanged, this, again);
        connect(box, &QTextEdit::selectionChanged, this, again);
        connect(box->document(), &QTextDocument::contentsChanged, this, again);
    }
    QTextCursor c(box->document());
    for (size_t k = 0; k < runs.size(); ++k)
        c.insertText(runs[k].text, fittedOf(runs[k], ppp, k < shown.size() ? shown[k] : QString(), runs[k].text, k < widths.size() ? widths[k] : 0));
    {  // (its tabs, as the page's - from its first letter's pen)
        QList<QTextOption::Tab> stops;
        tabsIn(box->document(), 0, c.position(), wideGaps(page, line), line.runs.front().origin.x(), ppp, &stops);
        if (!stops.isEmpty()) {
            QTextCursor b(box->document());
            QTextBlockFormat f = b.blockFormat();
            f.setTabPositions(stops);
            b.setBlockFormat(f);
        }
    }
    if (line.rtl) {
        box->setLayoutDirection(Qt::RightToLeft);
        QTextOption o = box->document()->defaultTextOption();
        o.setAlignment(Qt::AlignRight | Qt::AlignAbsolute);  // (at the right - not flipped to the left)
        o.setTextDirection(Qt::RightToLeft);
        box->document()->setDefaultTextOption(o);
    }
    editorRuns_ = runsIn(box);
    editorWas_ = box->toPlainText();
    const QPoint br = toView(page, line.rect.bottomRight());
    {
        // its letters where they are: its first letter's start (right to left: its last's end) and
        // its baseline on the page's
        const double baseline = toView(page, QPointF(0, 0)).y() + line.baseline * ppp;  // (exact: not a whole pixel)
        // (its pen's start, not its first letter's ink: the editor's letters start from the pen too)
        editorLeft_ = line.runs.front().origin.x();
        editorRight_ = line.rect.right();
        editorBase_ = line.baseline;
        editorPpp_ = ppp;
        editorTop_ = topFor(box, baseline);
        (void)br;
        const bool rtl = line.rtl;
        auto fit = [this, box, page, rtl] {
            const int w = int(std::ceil(box->document()->idealWidth())) + 4;
            const int h = int(std::ceil(box->document()->size().height())) + 2;
            const int start = toView(page, QPointF(editorLeft_, 0)).x(), end = toView(page, QPointF(editorRight_, 0)).x();
            box->setGeometry(rtl ? end + 2 - w : start, editorTop_, w, h);
        };
        editorFit_ = fit;
        fit();
        connect(box->document(), &QTextDocument::contentsChanged, box, fit);
        if (turned) {  // (its letters as shown, turned)
            std::vector<QRectF> letters;
            for (const QPolygonF &q : line.quads)
                letters.push_back(q.boundingRect().adjusted(-0.5, -0.5, 0.5, 0.5));
            holeFor(page, letters.empty() ? std::vector<QRectF>{line.shown().boundingRect()} : letters);
        } else {
            holeFor(page, {line.rect});
        }
    }
    box->done = [this](bool commit) { closeLineEditor(commit); };
    box->keep = editorKeeper_;
    box->document()->clearUndoRedoStacks();  // (its Undo: just what's done in it from here)
    box->scale = editorPpp_;  // (its own Undo / Redo, from here)
    box->rescaled = [this](double from) {
        editorPpp_ = from;
        rescaleEditor();
    };
    box->followCaret = [this] { showEditorCaret(); };
    box->startHistory();
    box->show();
    box->setFocus();
    // (turned: the click turned back, onto its straight place)
    caretAt(box, viewport(), turned && at.x() >= 0 ? editorTurn().inverted().map(QPointF(at)).toPoint() : at);
    viewport()->update();
    emit message(tr("Type where the text is, then press Enter (Esc: as it was). Select letters to make just them bold, "
                    "another size or colour..."));
}

void PageView::styleEditor(const std::function<void(QTextCharFormat &)> &change)
{
    if (!lineEditor_)
        return;
    QTextCursor sel = lineEditor_->textCursor();
    int a = sel.selectionStart(), b = sel.selectionEnd();
    const double ppp = pixelsPerPoint();
    if (a == b) {  // (nothing selected: the word the caret's in - between words, what's typed next)
        QTextCursor word = sel;
        word.select(QTextCursor::WordUnderCursor);
        a = word.selectionStart();
        b = word.selectionEnd();
        if (a == b) {
            QTextCharFormat f = lineEditor_->currentCharFormat();
            change(f);
            f.setFontPointSize(std::max(1.0, f.property(kSizeProp).toDouble() * ppp * 0.75));
            shownForm(f, ppp);
            lineEditor_->setCurrentCharFormat(f);
            lineEditor_->setFocus();
            return;
        }
    }
    // (letter by letter: each keeps the rest of its own look - all one change, so Ctrl+Z takes it back
    // at once, not a letter at a time)
    QTextCursor one(lineEditor_->document());
    one.beginEditBlock();
    for (int k = a; k < b; ++k) {
        one.setPosition(k);
        one.setPosition(k + 1, QTextCursor::KeepAnchor);
        QTextCharFormat f = one.charFormat();
        change(f);
        f.setFontPointSize(std::max(1.0, f.property(kSizeProp).toDouble() * ppp * 0.75));
        shownForm(f, ppp);
        one.setCharFormat(f);
    }
    one.endEditBlock();
    lineEditor_->setFocus();
}

void PageView::editorSet(const QString &key, const QVariant &value, const QStringList &sizes)
{
    styleEditor([&](QTextCharFormat &f) {
        if (key == QLatin1String("family")) {
            f.setFontFamilies({qtFamily(value.toString())});  // ("Calibri Light": Calibri, light - not a stand-in)
            f.setProperty(kFamilyProp, value.toString());
            if (f.fontWeight() < QFont::DemiBold)
                f.setFontWeight(weightOf(value.toString()));
        } else if (key == QLatin1String("size")) {
            f.setProperty(kSizeProp, value.toDouble());
        } else if (key == QLatin1String("step")) {
            const double now = f.property(kSizeProp).toDouble();
            double to = now;
            if (value.toInt() > 0) {
                for (const QString &s : sizes)
                    if (s.toDouble() > now + 0.05) {
                        to = s.toDouble();
                        break;
                    }
            } else {
                for (int k = int(sizes.size()) - 1; k >= 0; --k)
                    if (sizes[k].toDouble() < now - 0.05) {
                        to = sizes[k].toDouble();
                        break;
                    }
            }
            f.setProperty(kSizeProp, to);
        } else if (key == QLatin1String("bold")) {
            f.setFontWeight(value.toBool() ? QFont::Bold : QFont::Normal);
            f.clearProperty(kOutlineProp);  // (bold by its outline no more: as chosen now)
            f.setTextOutline(QPen(Qt::NoPen));
        } else if (key == QLatin1String("italic")) {
            f.setFontItalic(value.toBool());
        } else if (key == QLatin1String("underline")) {
            f.setFontUnderline(value.toBool());
        } else if (key == QLatin1String("strike")) {
            f.setFontStrikeOut(value.toBool());
        } else if (key == QLatin1String("color")) {
            f.setForeground(value.value<QColor>());
        } else if (key == QLatin1String("rise")) {  // (1 superscript, -1 subscript, 0 neither)
            f.setProperty(kRiseProp, value.toInt());
        } else if (key == QLatin1String("scale")) {
            f.setProperty(kScaleProp, value.toDouble());
        } else if (key == QLatin1String("track")) {
            f.setProperty(kTrackProp, value.toDouble());
        }
    });
}

bool PageView::editorHas(const QString &key) const
{
    return editorAll([&](const QTextCharFormat &f) {
        return key == QLatin1String("super") ? f.property(kRiseProp).toInt() > 0
               : key == QLatin1String("sub") ? f.property(kRiseProp).toInt() < 0
               : key == QLatin1String("bold") ? f.fontWeight() >= QFont::DemiBold || f.property(kOutlineProp).toDouble() > 0
               : key == QLatin1String("italic") ? f.fontItalic()
               : key == QLatin1String("underline") ? f.fontUnderline() : f.fontStrikeOut();
    });
}

QVariant PageView::editorValue(const QString &key) const
{
    if (!lineEditor_)
        return {};
    // (the first letter selected's - nothing selected: the letter before the caret's, what's typed takes;
    // at the very start, the one after it)
    QTextCursor one = lineEditor_->textCursor();
    if (one.hasSelection()) {
        const int a = one.selectionStart();
        one.setPosition(a);
        one.setPosition(std::min(a + 1, lineEditor_->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    } else if (one.atBlockStart() && !one.atBlockEnd()) {
        one.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    }
    const QTextCharFormat f = one.charFormat();
    if (key == QLatin1String("scale"))
        return f.hasProperty(kScaleProp) ? f.property(kScaleProp).toDouble() : 100.0;
    if (key == QLatin1String("track"))
        return f.property(kTrackProp).toDouble();
    if (key == QLatin1String("family")) {
        const QString fam = f.property(kFamilyProp).toString();
        return fam.isEmpty() && !f.fontFamilies().toStringList().isEmpty() ? f.fontFamilies().toStringList().first() : fam;
    }
    if (key == QLatin1String("size"))
        return f.property(kSizeProp).toDouble();
    if (key == QLatin1String("bold"))
        return f.fontWeight() >= QFont::DemiBold || f.property(kOutlineProp).toDouble() > 0;
    if (key == QLatin1String("italic"))
        return f.fontItalic();
    if (key == QLatin1String("underline"))
        return f.fontUnderline();
    if (key == QLatin1String("strike"))
        return f.fontStrikeOut();
    if (key == QLatin1String("color"))
        return f.foreground().style() != Qt::NoBrush ? f.foreground().color() : QColor(Qt::black);
    return {};
}

std::vector<int> PageView::editorParagraphs() const
{
    if (!lineEditor_ || !editorFrame_)
        return {};
    const QTextCursor c = lineEditor_->textCursor();
    const int a = lineEditor_->document()->findBlock(c.selectionStart()).blockNumber();
    const int b = lineEditor_->document()->findBlock(c.selectionEnd()).blockNumber();
    std::vector<int> out;
    for (int n = std::max(0, a); n <= b; ++n)
        out.push_back(n);
    return out;
}

bool PageView::editorAll(const std::function<bool(const QTextCharFormat &)> &test) const
{
    if (!lineEditor_)
        return false;
    const QTextCursor sel = lineEditor_->textCursor();
    int a = sel.selectionStart(), b = sel.selectionEnd();
    if (a == b) {  // (nothing selected: the word the caret's in - as styleEditor changes)
        QTextCursor word = sel;
        word.select(QTextCursor::WordUnderCursor);
        a = word.selectionStart();
        b = word.selectionEnd();
        if (a == b)
            return test(lineEditor_->currentCharFormat());
    }
    for (int k = a; k < b; ++k) {
        QTextCursor one(lineEditor_->document());
        one.setPosition(k);
        one.setPosition(k + 1, QTextCursor::KeepAnchor);
        if (!test(one.charFormat()))
            return false;
    }
    return true;
}

void PageView::closeLineEditor(bool commit)
{
    if (!lineEditor_)
        return;
    auto *box = static_cast<LineEdit *>(lineEditor_);
    lineEditor_ = nullptr;
    box->done = nullptr;
    editorTurned_ = false;
    turnedDrag_ = false;
    hole_ = QImage();  // (the page as it is again)
    holePage_ = -1;
    const QString text = box->toPlainText();
    if (editorFrame_) {  // (a block of paragraphs)
        editorFrame_ = false;
        box->hide();
        box->deleteLater();
        setFocus();
        viewport()->update();
        if (commit) {
            std::vector<std::vector<Document::StyledRun>> paras = paragraphRunsIn(box);
            for (std::vector<Document::StyledRun> &para : paras)  // (no look of their own - pasted...: the block's)
                for (Document::StyledRun &r : para)
                    if (!editorParas_.empty() && !editorParas_.front().empty() && (r.size <= 0 || r.family.isEmpty())) {
                        const Document::StyledRun &f0 = editorParas_.front().front();
                        r.size = r.size > 0 ? r.size : f0.size;
                        r.family = r.family.isEmpty() ? f0.family : r.family;
                    }
            bool same = text == editorWas_ && paras.size() == editorParas_.size();
            for (size_t n = 0; same && n < paras.size(); ++n) {
                same = paras[n].size() == editorParas_[n].size();
                for (size_t m = 0; same && m < paras[n].size(); ++m)
                    same = paras[n][m].text == editorParas_[n][m].text && sameLook(paras[n][m], editorParas_[n][m]);
            }
            auto alike = [&](size_t n, size_t o) {
                bool a = paras[n].size() == editorParas_[o].size();
                for (size_t m = 0; a && m < paras[n].size(); ++m)
                    a = paras[n][m].text == editorParas_[o][m].text && sameLook(paras[n][m], editorParas_[o][m]);
                return a;
            };
            // (the first paragraph changed: the ones above it left as they are)
            int from = 0;
            for (; from < int(paras.size()) && from < int(editorParas_.size()); ++from)
                if (!alike(size_t(from), size_t(from)))
                    break;
            from = std::min(from, int(paras.size()) - 1);
            // (and the ones after the last changed one: as they were - just moved up or down with it)
            int keep = 0;
            while (keep < int(paras.size()) - from - 1 && keep < int(editorParas_.size()) - from - 1 &&
                   alike(paras.size() - 1 - size_t(keep), editorParas_.size() - 1 - size_t(keep)))
                ++keep;
            if (!same)
                emit frameRestyled(editorPage_, editorIndex_, paras, from, keep);
        }
        return;
    }
    std::vector<Document::StyledRun> runs = runsIn(box);
    for (Document::StyledRun &r : runs)  // (letters with no look of their own - pasted...: the line's)
        if (!editorRuns_.empty() && (r.size <= 0 || r.family.isEmpty())) {
            const Document::StyledRun &first = editorRuns_.front();
            r.size = r.size > 0 ? r.size : first.size;
            r.family = r.family.isEmpty() ? first.family : r.family;
        }
    box->hide();
    box->deleteLater();
    setFocus();
    viewport()->update();
    if (!commit)
        return;
    bool same = runs.size() == editorRuns_.size();
    for (size_t k = 0; same && k < runs.size(); ++k)
        same = runs[k].text == editorRuns_[k].text && sameLook(runs[k], editorRuns_[k]);
    if (same)
        return;  // (as it was)
    // one look all through, as it was: retyped as before (a paragraph's words flowed again);
    // letters in looks of their own: written run by run
    bool oneLook = true;
    for (const Document::StyledRun &r : runs)
        oneLook = oneLook && sameLook(r, runs.front());
    bool wasOneLook = true;
    for (const Document::StyledRun &r : editorRuns_)
        wasOneLook = wasOneLook && sameLook(r, editorRuns_.front());
    if (oneLook && wasOneLook && !runs.empty() && sameLook(runs.front(), editorRuns_.front()))
        emit lineEdited(editorPage_, editorIndex_, text);
    else
        emit lineRestyled(editorPage_, editorIndex_, runs);
}

// ---- the Edit tool's pictures: moved, resized, turned ----

const std::vector<Picture> &PageView::picturesOf(int page)
{
    if (page != picsPage_ || picsGen_ != serial_) {
        pics_ = doc_ && page >= 0 && page < doc_->pageCount() ? doc_->pictures(page) : std::vector<Picture>{};
        picsPage_ = page;
        picsGen_ = serial_;
    }
    return pics_;
}

int PageView::pictureAt(int page, QPointF at)
{
    const std::vector<Picture> &pics = picturesOf(page);
    int clear = -1;  // (the topmost see-through there: picked if no text's under it - anywhere in its box moves it)
    for (int n = int(pics.size()) - 1; n >= 0; --n) {  // (the topmost: drawn last)
        const Picture &p = pics[size_t(n)];
        if (p.vector) {  // (a drawing: by its lines and boxes themselves - a few pixels round them, a thin rule too)
            const double t = 4 / pixelsPerPoint();
            for (const Picture::Piece &pc : p.pieces)
                if (pc.box.adjusted(-t, -t, t, t).contains(at))
                    return n;
            continue;
        }
        QPolygonF poly;
        for (const QPointF &c : p.shape.empty() ? p.corners : p.shape)  // (a shape on a background: picked by its shape)
            poly << c;
        if (poly.containsPoint(at, Qt::OddEvenFill)) {
            if (!doc_->seeThroughAt(p, at))
                return n;
            if (clear < 0)  // (see-through there: clicks go through - to text under it)
                clear = n;
        }
    }
    return clear >= 0 && lineAt(page, at, false) < 0 ? clear : -1;
}

int PageView::pictureHandleAt(QPoint pos)
{
    if (picIndex_ < 0 || picPage_ < 0 || picIndex_ >= int(picturesOf(picPage_).size()))
        return -1;
    const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
    if (pic.corners.size() != 4)
        return -1;
    if ((pictureTurnSpot() - QPointF(pos)).manhattanLength() <= 9)
        return 8;
    QPolygonF poly;
    for (const QPointF &c : pic.corners)
        poly << QPointF(toView(picPage_, c));
    const std::array<QPointF, 8> spots = handleSpots(poly);
    for (int k = 0; k < 8; ++k)
        if ((spots[size_t(k)] - QPointF(pos)).manhattanLength() <= 6)
            return k;
    return -1;
}

QPointF PageView::pictureTurnSpot()
{
    // the rotate handle: 22 px out from the middle of its top side, as shown (turned: turned)
    const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
    if (pic.corners.size() != 4)
        return QPointF(toView(picPage_, QPointF(pic.rect.center().x(), pic.rect.top()))) - QPointF(0, 22);
    QPointF c[4];
    for (int k = 0; k < 4; ++k)
        c[k] = QPointF(toView(picPage_, pic.corners[size_t(k)]));
    const QPointF top = (c[0] + c[1]) / 2, middle = (c[0] + c[1] + c[2] + c[3]) / 4;
    const QPointF up = top - middle;
    const double n = std::max(1e-6, std::hypot(up.x(), up.y()));
    return top + up / n * 22;
}

QRectF PageView::cropBounds()
{
    if (picPage_ < 0 || picIndex_ < 0 || picIndex_ >= int(picturesOf(picPage_).size()))
        return {};
    const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
    QRectF r;
    for (const QPointF &c : pic.whole.empty() ? pic.corners : pic.whole)
        r = r.isNull() ? QRectF(c, QSizeF(0.001, 0.001)) : r.united(QRectF(c, QSizeF(0.001, 0.001)));
    return r;
}

bool PageView::startCrop()
{
    // (upright pictures - or turned a quarter: its box is a box on the page)
    if (picPage_ < 0 || picIndex_ < 0 || picIndex_ >= int(picturesOf(picPage_).size()))
        return false;
    const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
    const std::vector<QPointF> &w = pic.whole.empty() ? pic.corners : pic.whole;
    if (w.size() != 4 || (std::abs(w[1].y() - w[0].y()) > 0.5 && std::abs(w[1].x() - w[0].x()) > 0.5))
        return false;
    QRectF shown;
    for (const QPointF &c : pic.corners)
        shown = shown.isNull() ? QRectF(c, QSizeF(0.001, 0.001)) : shown.united(QRectF(c, QSizeF(0.001, 0.001)));
    cropping_ = true;
    cropRect_ = cropWas_ = shown;
    cropHandle_ = -1;
    viewport()->update();
    return true;
}

void PageView::finishCrop(bool apply)
{
    if (!cropping_)
        return;
    cropping_ = false;
    cropHandle_ = -1;
    viewport()->update();
    if (!apply || picPage_ < 0 || picIndex_ < 0 || picIndex_ >= int(picturesOf(picPage_).size()))
        return;
    const QRectF r = cropRect_.normalized();
    if (std::abs(r.left() - cropWas_.left()) < 0.05 && std::abs(r.top() - cropWas_.top()) < 0.05 &&
        std::abs(r.right() - cropWas_.right()) < 0.05 && std::abs(r.bottom() - cropWas_.bottom()) < 0.05)
        return;  // (as it was)
    // its corners in the picture's own square (whole[0]: (0, 1), whole[1]: (1, 1), whole[3]: (0, 0))
    const Picture &pic = picturesOf(picPage_)[size_t(picIndex_)];
    const std::vector<QPointF> &w = pic.whole.empty() ? pic.corners : pic.whole;
    const QPointF ex = w[1] - w[0], ey = w[3] - w[0];
    const double lx = QPointF::dotProduct(ex, ex), ly = QPointF::dotProduct(ey, ey);
    if (lx < 1e-9 || ly < 1e-9)
        return;
    double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
    for (const QPointF &c : {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}) {
        const double u = QPointF::dotProduct(c - w[0], ex) / lx;
        const double v = 1 - QPointF::dotProduct(c - w[0], ey) / ly;
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    emit pictureCropped(picPage_, picIndex_, QRectF(QPointF(u0, v0), QPointF(u1, v1)));
}

bool PageView::cropMouse(QMouseEvent *e)
{
    if (!cropping_ || picPage_ < 0)
        return false;
    const QPoint pos = e->position().toPoint();
    const double ppp = pixelsPerPoint();
    const QPointF at = QPointF(pos + scrollOffset() - pages_[size_t(picPage_)].topLeft()) / ppp;
    const QRectF b = cropRect_;
    const QPointF spots[8] = {b.topLeft(), {b.center().x(), b.top()}, b.topRight(), {b.right(), b.center().y()},
                              b.bottomRight(), {b.center().x(), b.bottom()}, b.bottomLeft(), {b.left(), b.center().y()}};
    auto handleAt = [&]() {
        for (int k = 0; k < 8; ++k)
            if ((QPointF(toView(picPage_, spots[k])) - QPointF(pos)).manhattanLength() <= 7)
                return k;
        return b.contains(at) ? 8 : -1;
    };
    static const Qt::CursorShape shapes[9] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                              Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                              Qt::SizeAllCursor};
    if (e->type() == QEvent::MouseButtonPress) {
        if (e->button() != Qt::LeftButton)
            return true;
        cropHandle_ = handleAt();
        if (cropHandle_ < 0) {  // (a click outside: put in)
            finishCrop(true);
            return true;
        }
        cropFrom_ = at;
        cropWas_ = cropWas_.isNull() ? b : cropWas_;
        moveRect_ = b;
        return true;
    }
    if (e->type() == QEvent::MouseMove) {
        if (cropHandle_ < 0) {
            const int h = handleAt();
            viewport()->setCursor(h >= 0 ? shapes[h] : Qt::ArrowCursor);
            return true;
        }
        const QRectF bounds = cropBounds();
        const QPointF d = at - cropFrom_;
        QRectF r = moveRect_;
        const int h = cropHandle_;
        if (h == 8) {
            r.translate(d);
            r.moveLeft(std::clamp(r.left(), bounds.left(), bounds.right() - r.width()));
            r.moveTop(std::clamp(r.top(), bounds.top(), bounds.bottom() - r.height()));
        } else {
            const double minimum = 4;
            if (h == 0 || h == 6 || h == 7)
                r.setLeft(std::clamp(r.left() + d.x(), bounds.left(), r.right() - minimum));
            if (h == 2 || h == 3 || h == 4)
                r.setRight(std::clamp(r.right() + d.x(), r.left() + minimum, bounds.right()));
            if (h == 0 || h == 1 || h == 2)
                r.setTop(std::clamp(r.top() + d.y(), bounds.top(), r.bottom() - minimum));
            if (h == 4 || h == 5 || h == 6)
                r.setBottom(std::clamp(r.bottom() + d.y(), r.top() + minimum, bounds.bottom()));
        }
        cropRect_ = r;
        viewport()->update();
        return true;
    }
    if (e->type() == QEvent::MouseButtonRelease) {
        cropHandle_ = -1;
        return true;
    }
    return false;
}

void PageView::setShowBoxes(bool on)
{
    showBoxes_ = on;
    viewport()->update();
}

void PageView::pickPicture(int page, int index)
{
    if (page == picPage_ && index == picIndex_)
        return;
    if (cropping_)
        finishCrop(false);
    picPage_ = index >= 0 ? page : -1;
    picIndex_ = index;
    viewport()->update();
    emit picturePicked(picPage_, picIndex_);
}

QTransform PageView::pictureMove() const
{
    // in the page's points: moved by the drag; resized from the opposite corner, its shape
    // kept; turned round its middle (Shift: in steps of 15 degrees; near 0, 15, 30... it
    // settles on them)
    if (picIndex_ < 0 || picPage_ < 0 || picIndex_ >= int(pics_.size()) || picsPage_ != picPage_)
        return {};
    const Picture &pic = pics_[size_t(picIndex_)];
    const double ppp = pixelsPerPoint();
    const QPointF d = QPointF(moveTo_ - moveFrom_) / ppp;
    if (press_ == Press::MovePic)
        return QTransform::fromTranslate(d.x(), d.y());
    const QPointF c = pic.rect.center();
    if (press_ == Press::TurnPic) {
        const QPointF a = QPointF(moveFrom_) / ppp, b = QPointF(moveTo_) / ppp;
        const QPointF cv = (QPointF(toView(picPage_, c)) ) / ppp;
        double angle = (std::atan2(b.y() - cv.y(), b.x() - cv.x()) - std::atan2(a.y() - cv.y(), a.x() - cv.x())) * 180 / M_PI;
        angle = std::fmod(angle + 540, 360) - 180;
        if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
            angle = std::round(angle / 15) * 15;
        else {
            const double near15 = std::round(angle / 15) * 15;
            angle = std::abs(angle - near15) <= 2 ? near15 : std::round(angle);
        }
        QTransform r;
        r.rotate(angle);
        return QTransform::fromTranslate(-c.x(), -c.y()) * r * QTransform::fromTranslate(c.x(), c.y());
    }
    // a corner: from the opposite one
    // (a corner: from the opposite one, its shape kept; a side's middle: stretched from the
    // opposite side, along the picture's own side - a turned one isn't slanted)
    const std::array<QPointF, 8> spots = handleSpots(QPolygonF(QList<QPointF>(pic.corners.begin(), pic.corners.end())));
    const QPointF corner = spots[size_t(picHandle_ % 8)];
    const QPointF anchor = spots[size_t((picHandle_ + 4) % 8)];
    if (picHandle_ % 2) {
        const std::vector<QPointF> &w = pic.whole.size() == 4 ? pic.whole : pic.corners;
        const QPointF way = corner - anchor;
        QPointF axis = w[1] - w[0];
        const QPointF other = w[3] - w[0];
        auto along = [](QPointF a, QPointF b) { return std::abs(QPointF::dotProduct(a, b)) / std::max(1e-9, std::hypot(a.x(), a.y())); };
        if (along(other, way) > along(axis, way))
            axis = other;
        const double n = std::max(1e-9, std::hypot(axis.x(), axis.y()));
        const QPointF u = axis / n;
        const double len = std::max(1e-3, std::abs(QPointF::dotProduct(way, u)));
        const double grow = QPointF::dotProduct(d, u) * (QPointF::dotProduct(way, u) >= 0 ? 1 : -1);
        const double k = std::max(0.05, (len + grow) / len);
        const QTransform s(1 + (k - 1) * u.x() * u.x(), (k - 1) * u.x() * u.y(), (k - 1) * u.x() * u.y(), 1 + (k - 1) * u.y() * u.y(), 0, 0);
        return QTransform::fromTranslate(-anchor.x(), -anchor.y()) * s * QTransform::fromTranslate(anchor.x(), anchor.y());
    }
    const QPointF moved = corner + d;
    const double w0 = std::max(1e-3, std::hypot(corner.x() - anchor.x(), corner.y() - anchor.y()));
    const double w1 = std::hypot(moved.x() - anchor.x(), moved.y() - anchor.y());
    const double k = std::max(0.05, w1 / w0);
    return QTransform::fromTranslate(-anchor.x(), -anchor.y()) * QTransform::fromScale(k, k) *
           QTransform::fromTranslate(anchor.x(), anchor.y());
}

const std::vector<PdfLink> &PageView::linksOf(int page)
{
    const int rev = doc_ ? doc_->revision() : -1;  // (another PDF, or this one changed: read again)
    if (linksGen_ != serial_ || linksRev_ != rev) {
        links_.clear();
        linksGen_ = serial_;
        linksRev_ = rev;
    }
    auto it = links_.find(page);
    if (it == links_.end())
        it = links_.emplace(page, doc_ && page >= 0 && page < doc_->pageCount() ? doc_->links(page)
                                                                                 : std::vector<PdfLink>{}).first;
    return it->second;
}

int PageView::linkAt(int page, QPointF at)
{
    const std::vector<PdfLink> &links = linksOf(page);
    for (int i = 0; i < int(links.size()); ++i)
        if (links[size_t(i)].rect.contains(at))
            return i;
    return -1;
}

int PageView::overPicture(int page, QPointF at)
{
    // a picture over the text there: drawn after all the page's text, smaller than half the
    // page (not its background)
    const int pic = pictureAt(page, at);
    if (pic < 0)
        return -1;
    const Picture &p = picturesOf(page)[size_t(pic)];
    if (p.vector) {  // (a drawing round text - a title's frame: the text inside it, unless it's on its lines)
        const double t = 4 / pixelsPerPoint();
        for (const Picture::Piece &pc : p.pieces) {
            const QRectF out = pc.box.adjusted(-t, -t, t, t), in = pc.box.adjusted(t, t, -t, -t);
            if (out.contains(at) && (in.width() <= 0 || in.height() <= 0 || !in.contains(at)))
                return pic;
        }
        return -1;
    }
    const QSizeF s = doc_->pageSize(page);
    return !p.textAfter && p.rect.width() * p.rect.height() < 0.5 * s.width() * s.height() ? pic : -1;
}

void PageView::paintRubbed(QPainter &p, const std::vector<QPointF> &path)
{
    // the page without the drawings, inside the eraser's path (a round brush, its size)
    if (rubClean_.isNull() || rubPage_ < 0 || path.empty())
        return;
    const double ppp = pixelsPerPoint();
    QPainterPath line(QPointF(toView(rubPage_, path.front())));
    for (size_t k = 1; k < path.size(); ++k)
        line.lineTo(QPointF(toView(rubPage_, path[k])));
    if (path.size() == 1)
        line.lineTo(QPointF(toView(rubPage_, path.front())) + QPointF(0.01, 0));
    QPainterPathStroker brush;
    brush.setWidth(std::max(1.0, look_.width * ppp));
    brush.setCapStyle(Qt::RoundCap);
    brush.setJoinStyle(Qt::RoundJoin);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipPath(brush.createStroke(line));
    // (on the page's pixels exactly, as its tiles are: from the page's corner, not each corner rounded -
    // a part of a pixel off, the letters under it doubled, grey)
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPointF corner(toView(rubPage_, QPointF(0, 0)));
    p.drawImage(QRectF(corner + rubRect_.topLeft() * ppp, rubRect_.size() * ppp), rubClean_);
    p.restore();
}
