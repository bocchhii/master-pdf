#include "app/OrganizeView.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QThreadPool>
#include <QVBoxLayout>

namespace {
constexpr int kThumb = 150;  // (each page's picture: in a square this big)
}

// the pages' grid: dragged to a new place as the Pages panel's are - the other pages sliding aside to open the gap
// where they'll go, the ones moved dotted there, a see-through copy under the mouse, every page numbered as it'll be.
// Not moved by the list itself, but said where (the window moves them in the PDF, then they're drawn again)
class PageGrid : public QListWidget {
public:
    explicit PageGrid(QWidget *parent) : QListWidget(parent)
    {
        setViewMode(QListView::IconMode);
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setDragEnabled(false);  // (dragged here, not by Qt's drag and drop)
        setIconSize(QSize(kThumb, kThumb));
        setGridSize(QSize(kThumb + 40, kThumb + 44));
        setSpacing(8);
        setWordWrap(false);
        setContextMenuPolicy(Qt::CustomContextMenu);
        slide_.setInterval(16);
        connect(&slide_, &QTimer::timeout, this, &PageGrid::step);
    }
    std::function<void(int)> dropped;  // (before which page)

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        press_ = -1;
        QListWidgetItem *it = itemAt(e->position().toPoint());
        if (e->button() == Qt::LeftButton && it) {
            press_ = row(it);
            pressAt_ = e->position().toPoint();
            // (a picked page held, others picked too: they're all dragged - the others left picked till it's let go)
            if (it->isSelected() && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) && selectedItems().size() > 1) {
                keepPicked_ = true;
                setCurrentItem(it, QItemSelectionModel::NoUpdate);
                return;
            }
        }
        keepPicked_ = false;
        QListWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (press_ < 0 || !(e->buttons() & Qt::LeftButton))
            return QListWidget::mouseMoveEvent(e);
        const QPoint at = e->position().toPoint();
        if (!drag_) {
            if ((at - pressAt_).manhattanLength() < QApplication::startDragDistance())
                return;
            if (!item(press_)->isSelected())
                return QListWidget::mouseMoveEvent(e);  // (a drag from no picked page: picking by a band, as ever)
            start();
        }
        // (near the ends: the grid scrolls along)
        if (at.y() < 16)
            verticalScrollBar()->setValue(verticalScrollBar()->value() - 20);
        else if (at.y() > viewport()->height() - 16)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + 20);
        drag_->mouse = at + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        // where they'd go: before the first of the other pages - as they're shown now, the gap holding steady - that's
        // after the mouse, reading the grid's rows left to right
        int k = 0;
        for (int q : drag_->rest) {
            const QRectF r = drag_->shown[size_t(q)];
            const bool sameRow = std::abs(r.center().y() - drag_->mouse.y()) < r.height() / 2 + 4;
            if (r.center().y() < drag_->mouse.y() - r.height() / 2 - 4 || (sameRow && r.center().x() < drag_->mouse.x()))
                ++k;
        }
        if (k != drag_->k) {
            drag_->k = k;
            drag_->from = drag_->shown;
            const std::vector<int> order = drag_->order();
            for (size_t n = 0; n < order.size(); ++n)
                drag_->to[size_t(order[n])] = drag_->places[n];
            drag_->frame = 0;
            slide_.start();
        }
        viewport()->update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && drag_) {  // dropped: the pages go where the gap was
            std::unique_ptr<Drag> d = std::move(drag_);
            slide_.stop();
            press_ = -1;
            keepPicked_ = false;
            viewport()->update();
            const std::vector<int> order = d->order();
            bool same = true;
            for (size_t n = 0; n < order.size(); ++n)
                same = same && order[n] == int(n);
            if (!same && dropped)
                dropped(d->k < int(d->rest.size()) ? d->rest[size_t(d->k)] : count());
            return;
        }
        if (e->button() == Qt::LeftButton && keepPicked_ && press_ >= 0) {  // (a picked page clicked, not dragged: just it)
            keepPicked_ = false;
            clearSelection();
            item(press_)->setSelected(true);
            setCurrentItem(item(press_));
        }
        press_ = -1;
        QListWidget::mouseReleaseEvent(e);
    }
    void paintEvent(QPaintEvent *e) override
    {
        if (!drag_)
            return QListWidget::paintEvent(e);
        // every page where it's going; the ones moved as dotted gaps there; the copy under the mouse
        QPainter p(viewport());
        p.fillRect(viewport()->rect(), viewport()->palette().brush(QPalette::Base));
        const QPoint scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const std::vector<int> order = drag_->order();
        std::vector<int> number(static_cast<size_t>(count()), 0);
        for (size_t n = 0; n < order.size(); ++n)
            number[size_t(order[n])] = int(n) + 1;
        const auto [selBg, selText] = theme::selection();
        for (int q = 0; q < count(); ++q) {
            const QRectF cell = drag_->shown[size_t(q)].translated(-scroll);
            if (cell.bottom() < 0 || cell.top() > viewport()->height())
                continue;
            const QRectF icon = iconIn(cell);
            if (std::count(drag_->moving.begin(), drag_->moving.end(), q)) {
                QPen pen(selBg, 1, Qt::CustomDashLine);
                pen.setDashPattern({2, 2});
                pen.setCosmetic(true);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawRect(icon.adjusted(4, 4, -4, -4));
            } else {
                p.drawPixmap(icon.topLeft(), item(q)->icon().pixmap(iconSize(), devicePixelRatioF()));
            }
            p.setPen(theme::text());
            p.setFont(font());
            p.drawText(QRectF(cell.left(), icon.bottom() + 2, cell.width(), cell.bottom() - icon.bottom() - 2), Qt::AlignHCenter | Qt::AlignTop,
                       QString::number(number[size_t(q)]));
        }
        p.setOpacity(150 / 255.0);
        p.drawImage(QPointF(drag_->mouse - scroll - drag_->grab), drag_->ghost);
    }

private:
    struct Drag {
        std::vector<int> moving, rest;
        int k = 0;                              // (the moving pages go before rest[k])
        std::vector<QRectF> places;             // (the grid's places, in order - the content's coordinates)
        std::vector<QRectF> shown, from, to;    // (each page's place now, and while sliding)
        int frame = 0;
        QPointF mouse, grab;
        QImage ghost;
        std::vector<int> order() const
        {
            std::vector<int> o(rest.begin(), rest.begin() + k);
            o.insert(o.end(), moving.begin(), moving.end());
            o.insert(o.end(), rest.begin() + k, rest.end());
            return o;
        }
    };
    // (the page's picture in its place in the grid: across the top, in the middle)
    QRectF iconIn(const QRectF &cell) const
    {
        return QRectF(cell.center().x() - iconSize().width() / 2.0, cell.top(), iconSize().width(), iconSize().height());
    }
    void start()
    {
        auto d = std::make_unique<Drag>();
        const QPoint scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
        for (int q = 0; q < count(); ++q) {
            d->places.push_back(QRectF(visualItemRect(item(q))).translated(scroll));
            (item(q)->isSelected() ? d->moving : d->rest).push_back(q);
        }
        d->shown = d->places;
        d->to = d->places;
        d->k = int(std::count_if(d->rest.begin(), d->rest.end(), [&](int q) { return q < d->moving.front(); }));
        const QRectF held = d->places[size_t(press_)];
        d->grab = QPointF(pressAt_ + scroll) - iconIn(held).topLeft();
        // (the copy under the mouse: the page's picture, and with several, how many in a little badge)
        const double dpr = devicePixelRatioF();
        QImage ghost(iconSize() * dpr, QImage::Format_ARGB32_Premultiplied);
        ghost.setDevicePixelRatio(dpr);
        ghost.fill(Qt::transparent);
        QPainter g(&ghost);
        g.drawPixmap(QPointF(0, 0), item(press_)->icon().pixmap(iconSize(), dpr));
        if (d->moving.size() > 1) {
            const auto [bg, fg] = theme::selection();
            const QRectF badge(iconSize().width() - 30, 4, 26, 17);
            g.fillRect(badge, bg);
            g.setPen(fg);
            g.setFont(theme::font());
            g.drawText(badge, Qt::AlignCenter, QString::number(d->moving.size()));
        }
        g.end();
        d->ghost = ghost;
        drag_ = std::move(d);
    }
    void step()  // (the pages slide to their new places over a few frames - about 0.1 s)
    {
        if (!drag_) {
            slide_.stop();
            return;
        }
        constexpr int steps = 6;
        ++drag_->frame;
        for (size_t q = 0; q < drag_->shown.size(); ++q) {
            const QRectF &a = drag_->from[q], &b = drag_->to[q];
            const double t = double(drag_->frame) / steps;
            drag_->shown[q] = QRectF(a.topLeft() + (b.topLeft() - a.topLeft()) * t, b.size());
        }
        if (drag_->frame >= steps)
            slide_.stop();
        viewport()->update();
    }

    std::unique_ptr<Drag> drag_;
    QTimer slide_;
    int press_ = -1;
    QPoint pressAt_;
    bool keepPicked_ = false;
};

OrganizeView::OrganizeView(QWidget *parent) : QWidget(parent)
{
    setWhatsThis(tr("Organize Pages: every page of the PDF - click to pick one (Ctrl, Shift: several), drag them to "
                    "a new place; the toolbar turns, deletes, takes out, puts in and replaces pages."));
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(2, 2, 4, 2);
    bar->setSpacing(1);
    bar->addWidget(new ToolGrip(this));
    title_ = new QLabel(tr("Organize Pages"), this);
    title_->setFont(theme::font(8, true));
    bar->addWidget(title_);
    bar->addSpacing(10);
    auto *pageLabel = new QLabel(tr("Page:"), this);
    pageLabel->setFont(theme::font());
    bar->addWidget(pageLabel);
    bar->addSpacing(3);
    pageBox_ = new QSpinBox(this);
    pageBox_->setFont(theme::font());
    pageBox_->setFixedHeight(20);
    pageBox_->setMinimum(1);
    pageBox_->setToolTip(tr("Pick a page by its number"));
    connect(pageBox_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int n) {
        if (!grid_ || n < 1 || n > grid_->count())
            return;
        if (selection() == std::vector<int>{n - 1})
            return;
        select({n - 1});
        grid_->scrollToItem(grid_->item(n - 1));
    });
    bar->addWidget(pageBox_);
    count_ = new QLabel(this);
    count_->setFont(theme::font());
    bar->addSpacing(3);
    bar->addWidget(count_);
    bar->addWidget(new ToolSeparator(this));
    auto button = [this, bar](const QString &icon, const QString &tip, const QString &label, bool pick, std::function<void()> fn,
                              bool split = false) {
        auto *b = new ToolButton(icon, tip, {}, split, label, this);
        b->setWhiten(label.isEmpty() ? 16 : 0);
        connect(b, &ToolButton::clicked, this, fn);
        bar->addWidget(b);
        if (pick)
            needPick_.push_back(b);
        return b;
    };
    button(QStringLiteral("rotate_ccw"), tr("Turn the picked pages left"), {}, true, [this] {
        if (rotate)
            rotate(selection(), -90);
    });
    button(QStringLiteral("rotate_cw"), tr("Turn the picked pages right"), {}, true, [this] {
        if (rotate)
            rotate(selection(), 90);
    });
    button({}, tr("Delete the picked pages (Delete)"), tr("Delete"), true, [this] {
        if (remove)
            remove(selection());
    });
    bar->addWidget(new ToolSeparator(this));
    button({}, tr("Save the picked pages as a PDF of their own"), tr("Extract..."), true, [this] {
        if (extract)
            extract(selection());
    });
    ToolButton *insert = button({}, tr("Put pages from a file after the picked ones (the arrow: a blank page)"), tr("Insert"), false,
                                [this] {
                                    if (insertFile)
                                        insertFile(insertAt());
                                },
                                true);
    connect(insert, &ToolButton::arrowClicked, this, [this, insert] {
        QMenu menu(this);
        menu.addAction(tr("&Blank page"), this, [this] {
            if (insertBlank)
                insertBlank(insertAt());
        });
        menu.addAction(tr("Pages from a &file..."), this, [this] {
            if (insertFile)
                insertFile(insertAt());
        });
        menu.exec(insert->mapToGlobal(QPoint(0, insert->height())));
    });
    button({}, tr("Replace the picked pages with pages from another file"), tr("Replace..."), true, [this] {
        if (replace)
            replace(selection());
    });
    button({}, tr("Split the PDF into several"), tr("Split..."), false, [this] {
        if (split)
            split();
    });
    bar->addWidget(new ToolSeparator(this));
    button({}, tr("Pick every page (Ctrl+A)"), tr("Select all"), false, [this] { grid_->selectAll(); });
    bar->addStretch(1);
    // (Create PDF: the new PDF's name - it can't be made without one - and Create)
    nameRow_ = new QWidget(this);
    auto *row = new QHBoxLayout(nameRow_);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(4);
    row->addWidget(new ToolSeparator(nameRow_));
    auto *nameLabel = new QLabel(tr("File name:"), nameRow_);
    nameLabel->setFont(theme::font());
    row->addWidget(nameLabel);
    nameBox_ = new QLineEdit(nameRow_);
    nameBox_->setFont(theme::font());
    nameBox_->setFixedHeight(20);
    nameBox_->setFixedWidth(180);
    nameBox_->setPlaceholderText(tr("Name the new PDF"));
    nameBox_->setToolTip(tr("The new PDF's name - needed before it can be created"));
    row->addWidget(nameBox_);
    createB_ = new QPushButton(tr("Create"), nameRow_);
    createB_->setFont(theme::font());
    createB_->setToolTip(tr("Make the PDF of these pages, in this order"));
    createB_->setEnabled(false);
    row->addWidget(createB_);
    auto named = [this] {  // (a name with letters in it - not just spaces, nor only the signs a file's name can't have)
        QString n = nameBox_->text();
        n.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
        return n.trimmed();
    };
    connect(nameBox_, &QLineEdit::textChanged, this, [this, named] { createB_->setEnabled(!named().isEmpty()); });
    auto make = [this, named] {
        if (!named().isEmpty() && create)
            create(named());
    };
    connect(createB_, &QPushButton::clicked, this, make);
    connect(nameBox_, &QLineEdit::returnPressed, this, make);
    nameRow_->hide();
    bar->addWidget(nameRow_);
    closeB_ = new QPushButton(tr("Close"), this);
    closeB_->setFont(theme::font());
    closeB_->setToolTip(tr("Back to the pages (Esc)"));
    connect(closeB_, &QPushButton::clicked, this, [this] {
        if (close)
            close();
    });
    bar->addWidget(closeB_);
    box->addLayout(bar);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(2, 0, 2, 0);
    line->addWidget(new EtchedLine(this));
    box->addLayout(line);
    grid_ = new PageGrid(this);
    grid_->setFont(theme::font());
    grid_->dropped = [this](int to) {
        const std::vector<int> pages = selection();
        if (!pages.empty() && move)
            move(pages, to);
    };
    connect(grid_, &QListWidget::itemSelectionChanged, this, &OrganizeView::picked);
    connect(grid_, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
        if (!grid_->itemAt(at))
            return;
        if (!grid_->itemAt(at)->isSelected())
            select({grid_->row(grid_->itemAt(at))});
        QMenu menu(this);
        menu.addAction(tr("Turn &left"), this, [this] { rotate(selection(), -90); });
        menu.addAction(tr("Turn &right"), this, [this] { rotate(selection(), 90); });
        menu.addSeparator();
        menu.addAction(tr("&Extract..."), this, [this] { extract(selection()); });
        menu.addAction(tr("Re&place..."), this, [this] { replace(selection()); });
        menu.addAction(tr("Insert a &blank page after"), this, [this] { insertBlank(insertAt()); });
        menu.addAction(tr("Insert pages from a &file after..."), this, [this] { insertFile(insertAt()); });
        menu.addSeparator();
        menu.addAction(tr("&Delete"), this, [this] { remove(selection()); });
        menu.exec(grid_->viewport()->mapToGlobal(at));
    });
    auto *mid = new QHBoxLayout;
    mid->setContentsMargins(2, 2, 2, 2);
    mid->addWidget(grid_);
    box->addLayout(mid, 1);
    picked();
}

void OrganizeView::setDocument(std::shared_ptr<Document> doc)
{
    doc_ = std::move(doc);
    drawn_.clear();
    asked_.clear();
    fill();
}

void OrganizeView::setCreating(bool on)
{
    if (on && !creating_)
        nameBox_->clear();
    creating_ = on;
    nameRow_->setVisible(on);
    title_->setText(on ? tr("Create PDF") : tr("Organize Pages"));
    closeB_->setText(on ? tr("Cancel") : tr("Close"));
    closeB_->setToolTip(on ? tr("Don't make this PDF (Esc)") : tr("Back to the pages (Esc)"));
    createB_->setEnabled(on && !nameBox_->text().trimmed().isEmpty());
}

void OrganizeView::refresh()
{
    const std::vector<int> keep = selection();
    fill();
    select(keep);
}

std::vector<int> OrganizeView::selection() const
{
    std::vector<int> out;
    for (int i = 0; i < grid_->count(); ++i)
        if (grid_->item(i)->isSelected())
            out.push_back(i);
    return out;
}

void OrganizeView::select(const std::vector<int> &pages)
{
    grid_->clearSelection();
    for (int p : pages)
        if (p >= 0 && p < grid_->count())
            grid_->item(p)->setSelected(true);
    if (!pages.empty() && pages.front() >= 0 && pages.front() < grid_->count())
        grid_->setCurrentItem(grid_->item(pages.front()), QItemSelectionModel::NoUpdate);
}

int OrganizeView::insertAt() const
{
    const std::vector<int> sel = selection();
    return sel.empty() ? grid_->count() : sel.back() + 1;
}

void OrganizeView::fill()
{
    grid_->clear();
    if (!doc_)
        return;
    const int n = doc_->pageCount();
    const int rev = doc_->revision();
    for (int i = 0; i < n; ++i) {
        auto *it = new QListWidgetItem(grid_);
        it->setText(QString::number(i + 1));
        it->setTextAlignment(Qt::AlignHCenter);
        const QString key = QStringLiteral("%1/%2").arg(rev).arg(i);
        const QImage im = drawn_.value(key);
        if (!im.isNull()) {
            it->setIcon(QPixmap::fromImage(im));
        } else {  // (till it's drawn: an empty square as big - the grid laid out for the pictures to come)
            static QPixmap blank;
            if (blank.isNull()) {
                blank = QPixmap(kThumb, kThumb);
                blank.fill(Qt::transparent);
            }
            it->setIcon(blank);
            drawPage(i, key);
        }
    }
    pageBox_->setMaximum(std::max(1, n));
    count_->setText(QStringLiteral("/ %1").arg(n));
    picked();
}

void OrganizeView::picked()
{
    const std::vector<int> sel = selection();
    const bool can = doc_ && doc_->editable();
    for (ToolButton *b : needPick_)
        b->setEnabled(can && !sel.empty());
    if (sel.size() == 1) {
        const QSignalBlocker hold(pageBox_);
        pageBox_->setValue(sel.front() + 1);
    }
}

// (a page's picture: drawn in the background, on a sheet with a thin line and a shadow, in the grid's square)
void OrganizeView::drawPage(int page, const QString &key)
{
    if (asked_.contains(key) || !doc_)
        return;
    asked_.insert(key, true);
    auto doc = doc_;
    QPointer<OrganizeView> self(this);
    const double dpr = devicePixelRatioF();
    const QColor shadow = theme::shadow(), edge = theme::c("#404040", theme::Edge);
    QThreadPool::globalInstance()->start([self, doc, page, key, dpr, shadow, edge] {
        const QSizeF s = doc->pageSize(page);
        const double room = (kThumb - 8) * dpr;
        const double scale = room / std::max(1.0, std::max(s.width(), s.height()));
        const QImage im = doc->render(page, scale, QRect(0, 0, int(s.width() * scale), int(s.height() * scale)));
        QImage out(QSize(kThumb, kThumb) * dpr, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        if (!im.isNull()) {
            QPainter p(&out);
            const QPoint at((out.width() - im.width()) / 2, (out.height() - im.height()) / 2);
            const int k = std::max(1, int(dpr + 0.25));
            p.fillRect(QRect(at + QPoint(2 * k, 2 * k), im.size()), shadow);  // (its shadow, right and below)
            p.fillRect(QRect(at - QPoint(k, k), im.size() + QSize(2 * k, 2 * k)), edge);
            p.drawImage(at, im);
        }
        out.setDevicePixelRatio(dpr);
        QMetaObject::invokeMethod(
            qApp,
            [self, key, page, out] {
                if (!self)
                    return;
                self->drawn_.insert(key, out);
                if (page < self->grid_->count() && self->doc_ && key.startsWith(QString::number(self->doc_->revision()) + QLatin1Char('/')))
                    self->grid_->item(page)->setIcon(QPixmap::fromImage(out));
            },
            Qt::QueuedConnection);
    });
}

void OrganizeView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete && remove && !selection().empty()) {
        remove(selection());
        return;
    }
    if (e->key() == Qt::Key_Escape && close) {
        close();
        return;
    }
    if (e->key() == Qt::Key_A && (e->modifiers() & Qt::ControlModifier)) {
        grid_->selectAll();
        return;
    }
    QWidget::keyPressEvent(e);
}
