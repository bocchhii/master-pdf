#pragma once

#include <QWidget>

class QAbstractScrollArea;
class QScrollBar;

// A scrolled area in a sunken box with its scrollbars outside it, beside and under the box -
// as the Python app lays out the pages, the thumbnails and Find's results. (The area's own
// scrollbars still keep the numbers; these show them and move them.) The scrollbar along
// the bottom only shows while there's something to scroll sideways.
class ScrollBox : public QWidget {
    Q_OBJECT
public:
    ScrollBox(QAbstractScrollArea *area, bool horizontal, QWidget *parent = nullptr);
    QScrollBar *vertical() const { return vsb_; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void sync();
    QAbstractScrollArea *area_;
    QScrollBar *vsb_ = nullptr;
    QScrollBar *hsb_ = nullptr;
    QWidget *frame_ = nullptr;
    QWidget *corner_ = nullptr;
    bool horizontal_;
};
