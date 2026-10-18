#pragma once

#include <QLayout>
#include <QList>

// Items in a row, left to right - what doesn't fit goes on to a row under it (a toolbar on a narrow
// window: its groups put under each other, not squeezed over each other). Each row as tall as its
// tallest item, the others centred in it.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override { return int(items_.size()); }
    QLayoutItem *itemAt(int index) const override { return items_.value(index); }
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;
    // (the rows under the first start under this item, where it is on the first row - if they fit there)
    void setIndentUnder(QWidget *item) { under_ = item; invalidate(); }

private:
    int arrange(const QRect &rect, bool place) const;  // (the height it takes)
    QList<QLayoutItem *> items_;
    QWidget *under_ = nullptr;
};

// A bar's row (its QHBoxLayout) made a flowing one: its items in groups - a new one at each
// separator - the groups put under each other when the bar's too narrow for them
FlowLayout *flowInGroups(QWidget *bar);
