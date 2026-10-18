#pragma once

#include "ui/ClassicWindow.h"

#include <QLabel>
#include <QMenu>
#include <QPushButton>

#include <functional>

// The small parts of the Python app's window, drawn the same way.

// A status bar panel: a 1 px sunken edge round a face-coloured inside
class StatusPanel : public QLabel {
public:
    explicit StatusPanel(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
};

// Greyed-out words engraved into the face (what the tool does)
class EngravedLabel : public QWidget {
public:
    explicit EngravedLabel(QWidget *parent = nullptr);
    void setText(const QString &text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }  // (never squeezed: its words cut off)

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString text_;
};

// A flat button with a name and a little arrow ("Pages ▾"), pressed in while its list is open
class ChooserButton : public QWidget {
    Q_OBJECT
public:
    explicit ChooserButton(QWidget *parent = nullptr);
    void setText(const QString &text);
    void setPressed(bool on);
    QSize sizeHint() const override;

signals:
    void pressed();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QString text_;
    bool pressed_ = false;
};

// A list that stays open while its choices are tried (the Theme menu); it closes when
// something else is clicked, or Esc
class StayOpenMenu : public QMenu {
public:
    using QMenu::QMenu;
    std::function<void()> refill;

protected:
    void mouseReleaseEvent(QMouseEvent *) override;
};

// The app's raised button (Open..., OK)
class ClassicButton : public QPushButton {
public:
    explicit ClassicButton(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
};

// With no PDF open, over the middle of the view: a raised box saying how to start
class StartPanel : public QWidget {
    Q_OBJECT
public:
    explicit StartPanel(QWidget *parent = nullptr);
    QPushButton *openButton = nullptr;
    QPushButton *createButton = nullptr;

protected:
    void paintEvent(QPaintEvent *) override;
};

// A Windows 98 dialog box over its window: the classic title bar and border, not resizable
// Windows 98's progress bar: a sunken box, darker than the face (a dark theme's), filling left to right block by
// block in the title bar's colour (the scan's box, the update's)
class ClassicProgress : public QWidget {
public:
    explicit ClassicProgress(QWidget *parent);
    void setValue(double f);  // (0..1)

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int room() const;
    int blocks(double f) const;
    static constexpr int kBlock = 8, kGap = 2;
    double value_ = 0;
};

class ClassicDialog : public ClassicWindow {
public:
    explicit ClassicDialog(QWidget *owner, const QString &title);
    void exec(bool placed = false);  // centred over its window (placed: where it is); waits until it's closed

protected:
    void keyPressEvent(QKeyEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    QWidget *owner_;
    class QEventLoop *loop_ = nullptr;
};

// A message in the app's own style; returns the button clicked ("" if closed)
QString classicMessage(QWidget *owner, const QString &title, const QString &message,
                       const QStringList &buttons = {QStringLiteral("OK")});
// (the self-tests: the messages answered by code instead of shown; nullptr: shown again)
void setAutoAnswer(std::function<QString(const QString &message, const QStringList &buttons)> answer);

// The colour picker - Windows 98's "Edit Colors" box, as the Python app's (ColorDialog.cpp). Invalid if
// cancelled. (beside: opened off to the right of its window, not centred over it)
QColor colorDialog(QWidget *owner, const QString &title, const QColor &initial, bool beside = false);
// Words typed in a box (a sticky note's); null if cancelled
QString textDialog(QWidget *owner, const QString &title, const QString &prompt, const QString &text);

// Help > About Master PDF
void aboutBox(QWidget *owner);
// a file shown in its folder: Windows' Explorer with it picked; macOS' Finder; Linux: its folder opened
void showInFolder(const QString &path);
