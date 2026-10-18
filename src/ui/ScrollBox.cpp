#include "ui/ScrollBox.h"

#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QAbstractScrollArea>
#include <QGridLayout>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>

namespace {

// the sunken 2 px edge round the area
class SunkenFrame : public QWidget {
public:
    explicit SunkenFrame(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        classic::sunkenEdge(p, rect(), QColor());
    }
};

void mirror(QScrollBar *in, QScrollBar *out)
{
    QObject::connect(in, &QScrollBar::rangeChanged, out, [in, out] {
        out->setPageStep(in->pageStep());
        out->setSingleStep(in->singleStep());
        out->setRange(in->minimum(), in->maximum());
        out->setValue(in->value());
    });
    QObject::connect(in, &QScrollBar::valueChanged, out, [in, out](int v) {
        if (out->pageStep() != in->pageStep())
            out->setPageStep(in->pageStep());
        out->setValue(v);
    });
    QObject::connect(out, &QScrollBar::valueChanged, in, &QScrollBar::setValue);
    out->setRange(in->minimum(), in->maximum());
    out->setPageStep(in->pageStep());
    out->setValue(in->value());
}

}  // namespace

ScrollBox::ScrollBox(QAbstractScrollArea *area, bool horizontal, QWidget *parent)
    : QWidget(parent), area_(area), horizontal_(horizontal)
{
    area->setFrameShape(QFrame::NoFrame);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    frame_ = new SunkenFrame(this);
    auto *inner = new QVBoxLayout(frame_);
    inner->setContentsMargins(2, 2, 2, 2);
    inner->addWidget(area);
    grid->addWidget(frame_, 0, 0);
    vsb_ = new QScrollBar(Qt::Vertical, this);
    grid->addWidget(vsb_, 0, 1);
    mirror(area->verticalScrollBar(), vsb_);
    if (horizontal) {
        hsb_ = new QScrollBar(Qt::Horizontal, this);
        grid->addWidget(hsb_, 1, 0);
        corner_ = new QWidget(this);
        corner_->setFixedSize(vsb_->sizeHint().width(), hsb_->sizeHint().height());
        grid->addWidget(corner_, 1, 1);
        mirror(area->horizontalScrollBar(), hsb_);
        connect(area->horizontalScrollBar(), &QScrollBar::rangeChanged, this, &ScrollBox::sync);
        sync();
    }
}

void ScrollBox::sync()
{
    // (sideways: only while the pages are wider than the window)
    const bool show = area_->horizontalScrollBar()->maximum() > 0;
    if (hsb_->isVisibleTo(this) != show) {
        hsb_->setVisible(show);
        corner_->setVisible(show);
    }
}

void ScrollBox::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
}
