#pragma once

#include <QWidget>

#include <functional>

class TitleBar;

// A window with a Windows 98 title bar instead of Windows' own, as the Python app's: the
// palette's gradient, the title, small raised ? _ [] X buttons, and a 3D border. Windows does
// the moving (dragging the bar) and resizing (dragging the border) - smooth, and dragged to a
// screen's edge it snaps there, like the Python app: the left / right edge, half the screen;
// the top, maximized (its left / right third: as tall as the screen). Put the contents in
// body().
class ClassicWindow : public QWidget {
    Q_OBJECT
public:
    explicit ClassicWindow(bool resizable = true, QWidget *parent = nullptr);

    QWidget *body() const { return body_; }
    void setTitle(const QString &title);
    QString title() const { return title_; }
    void setHelpButton(bool on);  // the ? (What's This?) button
    // the update button: a download arrow left of _, a little apart (a newer version: install it from there) - nullptr: none
    void setUpdateButton(std::function<void()> onClick);
    void setTitleIcon(const QImage &icon);  // (none: just the title)
    void setShownActive(bool on) { shownActive_ = on; }  // (its title bar in the active colours - a box shown, not clicked)

    bool isMaximizedClassic() const { return maximized_; }
    void toggleMaximize();
    void minimizeWindow();
    TitleBar *titleBar() const { return bar_; }

signals:
    void helpClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *) override;
    void showEvent(QShowEvent *) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;

private:
    friend class TitleBar;
    void startMove(QPoint pressGlobal);
    void snapAt(QPoint cursor);

    TitleBar *bar_ = nullptr;
    bool shownActive_ = false;
    QWidget *body_ = nullptr;
    QString title_;
    bool resizable_ = true;
    bool maximized_ = false;
    QRect normal_;  // the size and place before it was maximized / snapped
    bool moving_ = false;
    bool styled_ = false;
};

class TitleBar : public QWidget {
    Q_OBJECT
public:
    static constexpr int H = 26, BTN_W = 21, BTN_H = 20;
    explicit TitleBar(ClassicWindow *window);

    void setHelp(bool on) { help_ = on; update(); }
    void setUpdate(std::function<void()> onClick) { update_ = std::move(onClick); update(); }
    void setIcon(const QImage &icon) { icon_ = icon; update(); }
    QString buttonAt(QPoint p) const;
    // (the maximize box: pressed through Windows - it's Windows' maximize box, for Snap Layouts)
    void pressMax(bool down)
    {
        pressed_ = down ? (win_->isMaximized() ? QStringLiteral("restore") : QStringLiteral("max")) : QString();
        down_ = down;
        update();
    }
    bool maxPressed() const { return pressed_ == QLatin1String("max") || pressed_ == QLatin1String("restore"); }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    bool event(QEvent *) override;

private:
    std::vector<std::pair<QString, QRect>> buttons() const;
    void drawButton(QPainter &p, const QString &kind, const QRect &r, bool down);

    ClassicWindow *win_;
    bool help_ = false;
    std::function<void()> update_;
    QImage icon_;
    QString pressed_;
    bool down_ = false;
    QPoint pressAt_;
    bool barPress_ = false;
};
