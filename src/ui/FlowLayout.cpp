#include "ui/FlowLayout.h"

#include "ui/ToolButton.h"

#include <QBoxLayout>
#include <QWidget>

#include <vector>

namespace {
constexpr int kRowGap = 2;  // (between one row and the next)
}

FlowLayout::FlowLayout(QWidget *parent) : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    items_.append(item);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

int FlowLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}

QSize FlowLayout::minimumSize() const
{
    // (as wide as its widest item - each can go on a row of its own)
    QSize size;
    for (QLayoutItem *item : items_)
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    // (all of it on one row)
    int w = 0, h = 0;
    for (QLayoutItem *item : items_)
        if (!item->isEmpty()) {
            w += item->sizeHint().width();
            h = std::max(h, item->sizeHint().height());
        }
    const QMargins m = contentsMargins();
    return {w + m.left() + m.right(), h + m.top() + m.bottom()};
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect &rect, bool place) const
{
    const QRect r = rect.marginsRemoved(contentsMargins());
    struct Row {
        std::vector<QLayoutItem *> items;
        int height = 0;
    };
    std::vector<Row> rows(1);
    std::vector<int> starts{0};  // (each row's left)
    int x = 0, indent = 0;
    for (QLayoutItem *item : items_) {
        if (item->isEmpty())
            continue;
        const QSize s = item->sizeHint();
        if (x > starts.back() && x + s.width() > r.width()) {  // (no room left on this row: the next one)
            rows.emplace_back();
            x = indent + s.width() <= r.width() ? indent : 0;  // (under the item it's set to start under)
            starts.push_back(x);
        }
        if (rows.size() == 1 && under_ && item->widget() == under_)
            indent = x;
        rows.back().items.push_back(item);
        rows.back().height = std::max(rows.back().height, s.height());
        x += s.width();
    }
    int y = r.top();
    for (size_t n = 0; n < rows.size(); ++n) {
        const Row &row = rows[n];
        if (place) {
            int left = r.left() + starts[n];
            for (QLayoutItem *item : row.items) {
                const QSize s = item->sizeHint();
                item->setGeometry(QRect(QPoint(left, y + (row.height - s.height()) / 2), s));
                left += s.width();
            }
        }
        y += row.height + (n + 1 < rows.size() ? kRowGap : 0);
    }
    const QMargins m = contentsMargins();
    return y - r.top() + m.top() + m.bottom();
}

FlowLayout *flowInGroups(QWidget *bar)
{
    auto *old = qobject_cast<QBoxLayout *>(bar->layout());
    if (!old)
        return nullptr;
    QList<QLayoutItem *> items;
    while (QLayoutItem *item = old->takeAt(0))
        items << item;
    delete old;
    auto *flow = new FlowLayout(bar);
    QWidget *group = nullptr;
    QHBoxLayout *gl = nullptr;
    for (QLayoutItem *item : items) {
        QWidget *w = item->widget();
        if (!group || dynamic_cast<ToolSeparator *>(w)) {  // (a new group at each separator, it first)
            group = new QWidget(bar);
            gl = new QHBoxLayout(group);
            gl->setContentsMargins(0, 0, 0, 0);
            gl->setSpacing(0);
            flow->addWidget(group);
        }
        if (w) {
            gl->addWidget(w);
            delete item;
        } else {
            gl->addItem(item);  // (its spacing, as it was)
        }
    }
    QSizePolicy p = bar->sizePolicy();
    p.setHeightForWidth(true);
    bar->setSizePolicy(p);
    return flow;
}
