#pragma once

#include <QWidget>

// The Python app's slim Windows 98 slider for a toolbar: a sunken groove, tick marks (when
// there are few stops), and a thumb with a point at the bottom. Click or drag it, or use the
// mouse wheel or the arrow keys; it moves in steps.
class TrackBar : public QWidget {
    Q_OBJECT
public:
    explicit TrackBar(QWidget *parent = nullptr, int length = 110);
    void setRange(int lo, int hi, int step);
    void setValue(int v);
    int value() const { return value_; }
    QSize sizeHint() const override { return {length_, 22}; }

signals:
    void valueChanged(int v);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    double xOf(int v) const;
    void setFromX(double x);
    int length_, lo_ = 0, hi_ = 100, step_ = 10, value_ = 0;
};
