#pragma once

#include <QImage>
#include <QWidget>

// A Windows 98 toolbar button, as the Python app's: flat, just its picture; under the mouse it
// rises (a 1 px raised edge), held down it sinks in. A tool in use (latched) stays sunk in on
// a checkered face. Greyed out, its picture is engraved. With a list (split), a narrow part
// at its right with a little arrow opens it. Its name shows when the mouse rests on it.
class ToolButton : public QWidget {
    Q_OBJECT
public:
    static constexpr int W = 23, H = 22, ARROW_W = 11;

    ToolButton(const QString &icon, const QString &tip, const QString &note = {},
               bool split = false, const QString &label = {}, QWidget *parent = nullptr);

    void setIcon(const QString &name);
    void setImage(const QImage &image);  // (a picture made by the app: a colour swatch...)
    void setLatched(bool on);
    bool latched() const { return latched_; }
    void setWhiten(int rows) { whiten_ = rows; update(); }  // its black parts white (dark theme)
    void setGreyBlues(bool on) { greyBlues_ = on; update(); }  // its dark blues grey (every theme)

    QSize sizeHint() const override;

signals:
    void clicked();
    void arrowClicked();  // (the list's arrow; the button stays pressed in while it's open)

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override;

private:
    QString icon_;
    bool greyBlues_ = false;
    QImage image_;
    QString label_;
    bool split_ = false;
    bool hot_ = false, down_ = false, latched_ = false;
    int whiten_ = 0;
};

// The etched line between groups of toolbar buttons (2 px, 4 px of room either side)
class ToolSeparator : public QWidget {
public:
    explicit ToolSeparator(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {10, ToolButton::H}; }

protected:
    void paintEvent(QPaintEvent *) override;
};

// The grip at a toolbar's start, as Windows 98's: two raised bars side by side
class ToolGrip : public QWidget {
public:
    explicit ToolGrip(QWidget *parent = nullptr);
    void paint(QPainter &p, int height) const;  // (drawn in another widget too: the Home toolbar's)

protected:
    void paintEvent(QPaintEvent *) override;
};

// An etched line across (shadow over light): under the toolbars
class EtchedLine : public QWidget {
public:
    explicit EtchedLine(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
};
